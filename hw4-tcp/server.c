// Сервер-диспетчер шлюзовой системы. Хранит состояние модели, выбирает группы
// и пересылает сообщения между камерами, воротами и судами, проверяя инварианты.
// Один процесс, все соединения обслуживаются через select().
// Запуск: server [<файл_параметров>|-] [<лог>] [<порт>]
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

#include "common.h"
#include "model.h"

#define ROLE_UNKNOWN -1

typedef struct {
    int in_use;
    int closing; // соединение разорвано или отклонено; клиент будет удалён после обработки событий
    int role, id, side; // id – номер камеры (для ворот тоже) или номер судна
    unsigned char inbox[WIRE_MSG_SIZE]; // TCP – поток байтов: сообщение может прийти по частям
    size_t inbox_used;
} Client;

static Client clients[FD_SETSIZE];
static int listen_sock;
static int highest_client_sock = -1;
static int signal_pipe[2]; // self-pipe: обработчик сигнала пишет байт, select просыпается

static volatile sig_atomic_t stop_signal, want_dump, want_toggle;

// отправка:

static void send_to(int sock, int type, int a, int b, int c) {
    if (sock < 0 || !clients[sock].in_use || clients[sock].closing) return;
    if (send_msg(sock, type, a, b, c, time_now()) < 0) clients[sock].closing = 1;
}

static void reject(int sock, int reason) {
    send_to(sock, M_REJECT, reason, 0, 0);
    clients[sock].closing = 1;
}

static void request_stop(int reason) {
    if (world.stop_reason != RUNNING) return;

    world.stop_reason = reason;
    if (world.end_time < 0) world.end_time = time_now();
}

static void stop_on_failure(const char *note) {
    snprintf(world.failure_note, sizeof(world.failure_note), "%s", note);
    request_stop(FAILURE);
}

// диспетчер, назначение групп:

static void dispatch_groups(void) {
    if (!world.locking_started || world.stop_reason != RUNNING) return;

    for (int chamber_id = 0; chamber_id < world.chambers_count; chamber_id++) {
        Chamber *chamber = &world.chambers[chamber_id];
        if (chamber->busy) continue;

        int side = choose_side(chamber_id);
        if (side < 0) continue;

        chamber->busy = 1, chamber->entry_side = side;
        world.groups++;

        if (side != chamber->last_dir) {
            event("диспетчер: камера %d, направление работы -> %s", chamber_id, dir_name(side));
            chamber->last_dir = side;
        }

        event("диспетчер: камера %d пропускает группу (%s), в очереди с этой стороны: %d", chamber_id,
              dir_name(side), queue_len(chamber_id, side));
        send_to(chamber->chamber_client, M_START, side, chamber->water, 0);
    }
}

static void check_finish(void) {
    if (all_ships_handled() && all_chambers_idle()) request_stop(ALL_DONE);
}

// суда:

static void put_in_queue(Ship *ship) {
    ship->state = SHIP_QUEUED;
    ship->queued_at = time_now();
    ship->chamber = ship->dir == DIR_FORWARD ? ship->pos : ship->pos - 1;

    event("судно %d встало в очередь к камере %d", ship->id, ship->chamber);
    send_to(ship->client, M_QUEUED, ship->chamber, ship->pos, 0);
}

static void finish_entering(Ship *ship) {
    Chamber *chamber = &world.chambers[ship->chamber];
    int waited = time_now() - ship->queued_at;

    world.entries++, world.wait_sum += waited;
    if (waited > world.wait_max) world.wait_max = waited;

    if (chamber->gate[ship->dir] != GATE_IS_OPEN || chamber->water != ship->dir)
        violation("судно входит при несовпадающем уровне или закрытых воротах");
    if (chamber->load > world.capacity) violation("вместимость камеры превышена");

    ship->state = SHIP_INSIDE;
    event("судно %d (размер %d%s%s) вошло в камеру %d, занято %d/%d", ship->id, ship->size,
          ship->prio ? ", приоритетное" : "", ship->client < 0 ? ", без связи" : "", ship->chamber, chamber->load,
          world.capacity);
    send_to(chamber->chamber_client, M_SHIP_DONE, ship->id, 0, 0);
}

static void finish_exiting(Ship *ship) {
    int chamber_id = ship->chamber;
    Chamber *chamber = &world.chambers[chamber_id];

    if (chamber->gate[1 - ship->dir] != GATE_IS_OPEN || chamber->water != 1 - ship->dir)
        violation("судно выходит при несовпадающем уровне или закрытых воротах");

    chamber->load -= ship->size;
    ship->pos = ship->dir == DIR_FORWARD ? chamber_id + 1 : chamber_id;
    event("судно %d вышло из камеры %d на участок %d (уровень %d)", ship->id, chamber_id, ship->pos, level(ship->pos));

    if (ship->client < 0) {
        ship->state = SHIP_LEFT;
        world.left++;
        event("судно %d без связи выведено из камеры и покинуло систему", ship->id);
    } else if (is_last_section(ship)) {
        ship->state = SHIP_PASSED;
        world.passed++;
        event("судно %d прошло всю систему", ship->id);
        send_to(ship->client, M_PASSED, ship->pos, 0, 0);
    } else {
        put_in_queue(ship);
    }

    send_to(chamber->chamber_client, M_SHIP_DONE, ship->id, 0, 0);
    dispatch_groups();
}

static void handle_ship_arrived(Ship *ship, int sock, const Msg *msg) {
    if (ship->state != SHIP_CONNECTING) return;

    if (msg->a < 1 || msg->a > world.capacity || (msg->b != DIR_FORWARD && msg->b != DIR_BACKWARD) ||
        (msg->c != 0 && msg->c != 1)) {
        ship->state = SHIP_LEFT;
        world.left++;
        event("судно %d отклонено: неверные параметры", ship->id);
        reject(sock, REJECT_BAD_SHIP);
        return;
    }

    ship->size = msg->a, ship->dir = msg->b, ship->prio = msg->c;
    ship->pos = ship->dir == DIR_FORWARD ? 0 : world.chambers_count;

    event("судно %d (размер %d, %s%s) прибыло на участок %d", ship->id, ship->size, dir_name(ship->dir),
          ship->prio ? ", приоритетное" : "", ship->pos);
    put_in_queue(ship);
    dispatch_groups();
}

static void handle_ship_message(int sock, const Msg *msg) {
    Ship *ship = &world.ships[clients[sock].id - 1];

    if (msg->type == M_ARRIVED) {
        handle_ship_arrived(ship, sock, msg);
    } else if (msg->type == M_MOVED && msg->a == MOVE_ENTER && ship->state == SHIP_ENTERING) {
        finish_entering(ship);
    } else if (msg->type == M_MOVED && msg->a == MOVE_EXIT && ship->state == SHIP_EXITING) {
        finish_exiting(ship);
    } else {
        event("судно %d прислало неожиданное сообщение %d, соединение закрыто", ship->id, msg->type);
        clients[sock].closing = 1;
    }
}

// камеры:

static int gates_closed(const Chamber *chamber) {
    return chamber->gate[0] == GATE_CLOSED && chamber->gate[1] == GATE_CLOSED;
}

static void handle_level(int chamber_id, const Msg *msg) {
    Chamber *chamber = &world.chambers[chamber_id];
    int to_side = msg->a == SIDE_LOWER ? SIDE_LOWER : SIDE_UPPER;

    if (!gates_closed(chamber)) {
        violation("уровень воды меняется при открытых воротах");
        stop_on_failure("камера пыталась менять уровень при открытых воротах");
        return;
    }

    if (msg->type == M_LEVEL_BEGIN) {
        chamber->level_changing = 1;
        event("камера %d: %s", chamber_id,
              to_side == chamber->entry_side ? "подготовка: уровень воды меняется под сторону входа"
                                             : "подготовка к выходу: уровень воды меняется под сторону выхода");
        return;
    }

    chamber->level_changing = 0;
    chamber->water = to_side;
    world.level_changes++;
    event("камера %d: уровень воды изменён до %d", chamber_id, level(chamber_id + to_side));
}

static void handle_gate_command(int chamber_id, const Msg *msg) {
    Chamber *chamber = &world.chambers[chamber_id];
    int side = msg->a == SIDE_LOWER ? SIDE_LOWER : SIDE_UPPER;

    if (msg->b == GATE_OPEN) {
        if (chamber->gate[1 - side] != GATE_CLOSED || chamber->level_changing || chamber->water != side) {
            violation("ворота открываются при открытых противоположных или несовпадающем уровне");
            stop_on_failure("камера пыталась открыть ворота с нарушением инварианта");
            return;
        }
        chamber->gate[side] = GATE_OPENING;
    } else {
        chamber->gate[side] = GATE_CLOSING;
    }

    send_to(chamber->gate_client[side], M_GATE_CMD, side, msg->b, 0);
}

static void call_next_ship(int chamber_id, int sock, int move) {
    Chamber *chamber = &world.chambers[chamber_id];
    Ship *ship = move == MOVE_ENTER ? find_ship_to_enter(chamber_id, chamber->entry_side, world.capacity - chamber->load)
                                    : find_ship_to_exit(chamber_id);

    if (!ship) {
        send_to(sock, M_NO_SHIP, 0, 0, 0);
        return;
    }

    // судно без связи не может ответить: сервер выводит его сам
    if (move == MOVE_EXIT && ship->client < 0) {
        finish_exiting(ship);
        return;
    }

    if (move == MOVE_ENTER) {
        ship->state = SHIP_ENTERING;
        chamber->load += ship->size; // вместимость занимается сразу, до фактического входа
    } else {
        ship->state = SHIP_EXITING;
    }

    send_to(ship->client, M_GO, chamber_id, move, 0);
}

static void handle_chamber_message(int sock, const Msg *msg) {
    int chamber_id = clients[sock].id;
    Chamber *chamber = &world.chambers[chamber_id];

    switch (msg->type) {
    case M_LEVEL_BEGIN:
    case M_LEVEL_END:
        handle_level(chamber_id, msg);
        break;
    case M_GATE_CMD:
        handle_gate_command(chamber_id, msg);
        break;
    case M_NEXT_SHIP:
        call_next_ship(chamber_id, sock, msg->a);
        break;
    case M_FREE:
        chamber->busy = 0;
        event("камера %d: свободна", chamber_id);
        dispatch_groups();
        break;
    default:
        event("камера %d прислала неожиданное сообщение %d", chamber_id, msg->type);
        stop_on_failure("протокол камеры нарушен");
    }
}

// ворота:

static void handle_gate_message(int sock, const Msg *msg) {
    int chamber_id = clients[sock].id, side = clients[sock].side;
    Chamber *chamber = &world.chambers[chamber_id];
    const char *role = side == chamber->entry_side ? "входа" : "выхода";

    if (msg->type != M_GATE_DONE) {
        event("ворота камеры %d прислали неожиданное сообщение %d", chamber_id, msg->type);
        stop_on_failure("протокол ворот нарушен");
        return;
    }

    if (msg->b == GATE_OPEN) {
        chamber->gate[side] = GATE_IS_OPEN;
        if (chamber->gate[1 - side] != GATE_CLOSED) violation("противоположные ворота открыты одновременно");
        event("камера %d: открыты ворота %s (%s)", chamber_id, role, side_name(side));
    } else {
        chamber->gate[side] = GATE_CLOSED;
        event("камера %d: ворота %s (%s) закрыты", chamber_id, role, side_name(side));
    }

    send_to(chamber->chamber_client, M_GATE_DONE, side, msg->b, 0);
}

// подключение участников:

static void start_locking_if_ready(void) {
    if (world.locking_started) return;

    for (int chamber_id = 0; chamber_id < world.chambers_count; chamber_id++) {
        Chamber *chamber = &world.chambers[chamber_id];
        if (chamber->chamber_client < 0 || chamber->gate_client[0] < 0 || chamber->gate_client[1] < 0) return;
    }

    world.locking_started = 1;
    event("все камеры и ворота подключены, шлюзование начинается");
    dispatch_groups();
}

static void register_chamber(int sock, int chamber_id) {
    if (chamber_id < 0 || chamber_id >= world.chambers_count) {
        reject(sock, REJECT_BAD_ID);
        return;
    }
    if (world.chambers[chamber_id].chamber_client >= 0) {
        reject(sock, REJECT_DUPLICATE);
        return;
    }

    clients[sock].role = ROLE_CHAMBER, clients[sock].id = chamber_id;
    world.chambers[chamber_id].chamber_client = sock;
    send_to(sock, M_WELCOME, world.level_time, 0, world.tick);
    event("камера %d подключилась", chamber_id);
    start_locking_if_ready();
}

static void register_gate(int sock, int chamber_id, int side) {
    if (chamber_id < 0 || chamber_id >= world.chambers_count || (side != SIDE_UPPER && side != SIDE_LOWER)) {
        reject(sock, REJECT_BAD_ID);
        return;
    }
    if (world.chambers[chamber_id].gate_client[side] >= 0) {
        reject(sock, REJECT_DUPLICATE);
        return;
    }

    clients[sock].role = ROLE_GATE, clients[sock].id = chamber_id, clients[sock].side = side;
    world.chambers[chamber_id].gate_client[side] = sock;
    send_to(sock, M_WELCOME, world.gate_time, 0, world.tick);
    event("ворота камеры %d (%s) подключились", chamber_id, side_name(side));
    start_locking_if_ready();
}

static void register_ship(int sock) {
    int limit = world.ships_expected ? world.ships_expected : MAX_SHIPS;
    if (world.ships_registered >= limit) {
        reject(sock, REJECT_TOO_MANY_SHIPS);
        return;
    }

    Ship *ship = &world.ships[world.ships_registered];
    world.ships_registered++;

    memset(ship, 0, sizeof(*ship));
    ship->id = world.ships_registered;
    ship->state = SHIP_CONNECTING;
    ship->client = sock;

    clients[sock].role = ROLE_SHIP, clients[sock].id = ship->id;
    send_to(sock, M_WELCOME, ship->id, world.capacity, world.tick);
}

static void handle_hello(int sock, const Msg *msg) {
    if (msg->type != M_HELLO) {
        clients[sock].closing = 1;
        return;
    }

    if (msg->a == ROLE_CHAMBER) register_chamber(sock, msg->b);
    else if (msg->a == ROLE_GATE) register_gate(sock, msg->b, msg->c);
    else if (msg->a == ROLE_SHIP) register_ship(sock);
    else clients[sock].closing = 1;
}

static void handle_message(int sock, const Msg *msg) {
    switch (clients[sock].role) {
    case ROLE_UNKNOWN: handle_hello(sock, msg); break;
    case ROLE_CHAMBER: handle_chamber_message(sock, msg); break;
    case ROLE_GATE: handle_gate_message(sock, msg); break;
    case ROLE_SHIP: handle_ship_message(sock, msg); break;
    }
}

// отключение участников:

static void forget_ship(Ship *ship) {
    switch (ship->state) {
    case SHIP_CONNECTING:
        ship->state = SHIP_LEFT;
        world.left++;
        event("судно %d отключилось, не сообщив о прибытии", ship->id);
        break;
    case SHIP_QUEUED:
        ship->state = SHIP_LEFT;
        world.left++;
        event("судно %d покинуло систему (отключилось) с участка %d", ship->id, ship->pos);
        break;
    case SHIP_ENTERING:
        event("судно %d потеряло связь при входе, вход завершается без связи", ship->id);
        finish_entering(ship);
        break;
    case SHIP_INSIDE:
        event("судно %d потеряло связь, остаётся в камере %d до выгрузки", ship->id, ship->chamber);
        break;
    case SHIP_EXITING:
        event("судно %d потеряло связь при выходе, выход завершается без связи", ship->id);
        finish_exiting(ship);
        break;
    }
}

static void forget_infrastructure(int sock) {
    Client *client = &clients[sock];
    Chamber *chamber = &world.chambers[client->id];
    char note[128];

    if (client->role == ROLE_CHAMBER) {
        chamber->chamber_client = -1;
        snprintf(note, sizeof(note), "отключилась камера %d", client->id);
    } else {
        chamber->gate_client[client->side] = -1;
        snprintf(note, sizeof(note), "отключились ворота камеры %d (%s)", client->id, side_name(client->side));
    }

    // до начала шлюзования участник может просто переподключиться
    if (!world.locking_started) {
        event("%s до начала шлюзования", note);
        return;
    }

    event("%s, работа шлюза невозможна", note);
    stop_on_failure(note);
}

static void drop_client(int sock) {
    Client *client = &clients[sock];

    client->in_use = 0; // сначала, чтобы ответы при обработке отключения ему уже не отправлялись
    close(sock);

    if (client->role == ROLE_SHIP) {
        Ship *ship = &world.ships[client->id - 1];
        ship->client = -1;
        forget_ship(ship);
    } else if (client->role == ROLE_CHAMBER || client->role == ROLE_GATE) {
        forget_infrastructure(sock);
    }
}

// удаление клиента может вызвать отправки другим, а неудачная отправка помечает следующего
static void drop_closing_clients(void) {
    int dropped_any = 1;

    while (dropped_any) {
        dropped_any = 0;
        for (int sock = 0; sock <= highest_client_sock; sock++) {
            if (clients[sock].in_use && clients[sock].closing) {
                drop_client(sock);
                dropped_any = 1;
            }
        }
    }
}

// сеть:

static void accept_client(void) {
    int sock = accept(listen_sock, NULL, NULL);
    if (sock < 0) return;

    if (sock >= FD_SETSIZE) {
        close(sock); // select не умеет следить за таким дескриптором
        return;
    }

    if (sock > highest_client_sock) highest_client_sock = sock;
    memset(&clients[sock], 0, sizeof(Client));
    clients[sock].in_use = 1;
    clients[sock].role = ROLE_UNKNOWN;
}

static void read_from_client(int sock) {
    Client *client = &clients[sock];
    ssize_t got = recv(sock, client->inbox + client->inbox_used, WIRE_MSG_SIZE - client->inbox_used, 0);

    if (got < 0 && errno == EINTR) return;
    if (got <= 0) {
        client->closing = 1;
        return;
    }

    client->inbox_used += (size_t)got;
    if (client->inbox_used < WIRE_MSG_SIZE) return;

    Msg msg;
    decode_msg(client->inbox, &msg);
    client->inbox_used = 0;
    handle_message(sock, &msg);
}

static int fill_watch_set(fd_set *watched) {
    int max_sock = listen_sock > signal_pipe[0] ? listen_sock : signal_pipe[0];

    FD_ZERO(watched);
    FD_SET(listen_sock, watched);
    FD_SET(signal_pipe[0], watched);

    for (int sock = 0; sock <= highest_client_sock; sock++) {
        if (!clients[sock].in_use) continue;
        FD_SET(sock, watched);
        if (sock > max_sock) max_sock = sock;
    }

    return max_sock;
}

// сигналы:

static void on_signal(int sig) {
    int saved_errno = errno;

    if (sig == SIGUSR1) want_dump = 1;
    else if (sig == SIGUSR2) want_toggle = 1;
    else stop_signal = sig;

    ssize_t written = write(signal_pipe[1], "s", 1);
    (void)written;
    errno = saved_errno;
}

static void setup_signals(void) {
    if (pipe(signal_pipe) < 0) die("pipe");
    fcntl(signal_pipe[0], F_SETFL, O_NONBLOCK);
    fcntl(signal_pipe[1], F_SETFL, O_NONBLOCK);

    signal(SIGPIPE, SIG_IGN);

    struct sigaction action = {0};
    action.sa_handler = on_signal;
    sigemptyset(&action.sa_mask);
    sigaction(SIGINT, &action, NULL);
    sigaction(SIGTERM, &action, NULL);
    sigaction(SIGHUP, &action, NULL);
    sigaction(SIGUSR1, &action, NULL);
    sigaction(SIGUSR2, &action, NULL);
}

static void toggle_time_limit(void) {
    world.end_at = world.end_at ? 0 : time_now() + (world.limit ? world.limit : 1000);

    if (world.end_at) event("время моделирования ограничено, остановка при t=%d", world.end_at);
    else event("время моделирования не ограничено");
}

static void handle_signals(void) {
    char drained[64];
    while (read(signal_pipe[0], drained, sizeof(drained)) > 0) {
    }

    if (stop_signal) {
        world.stop_signal = stop_signal;
        request_stop(INTERRUPT);
    }

    if (want_dump) {
        want_dump = 0;
        print_state();
    }

    if (want_toggle) {
        want_toggle = 0;
        toggle_time_limit();
    }
}

// главный цикл:

static void serve_one_round(void) {
    fd_set watched;
    int max_sock = fill_watch_set(&watched);
    struct timeval timeout = {world.tick / 1000, (world.tick % 1000) * 1000};

    if (select(max_sock + 1, &watched, NULL, NULL, &timeout) < 0) {
        if (errno == EINTR) return;
        die("select");
    }

    if (FD_ISSET(signal_pipe[0], &watched)) handle_signals();
    if (FD_ISSET(listen_sock, &watched)) accept_client();

    for (int sock = 0; sock <= highest_client_sock && world.stop_reason == RUNNING; sock++)
        if (clients[sock].in_use && !clients[sock].closing && FD_ISSET(sock, &watched)) read_from_client(sock);
    drop_closing_clients();

    if (world.end_at && time_now() >= world.end_at) request_stop(TIME_OUT);
    check_finish();
}

static void shut_down(void) {
    for (int sock = 0; sock <= highest_client_sock; sock++) {
        if (!clients[sock].in_use) continue;
        if (!clients[sock].closing) send_msg(sock, M_STOP, world.stop_reason, 0, 0, time_now());
        close(sock);
    }

    close(listen_sock);
    print_final_report();
    model_close_log();
}

int main(int argc, char **argv) {
    const char *params_path = argc > 1 ? argv[1] : "-";
    const char *log_path = argc > 2 ? argv[2] : "locks.log";
    int port = argc > 3 ? parse_port(argv[3]) : DEFAULT_PORT;

    // порт занимается первым: если он занят, лог прошлого запуска не затирается
    listen_sock = create_server_socket(port);
    setup_signals();
    model_init(params_path, log_path);
    out("Сервер слушает порт %d, ждёт %d камер и %d ворот\n", port, world.chambers_count, 2 * world.chambers_count);

    while (world.stop_reason == RUNNING) serve_one_round();

    shut_down();
    return world.stop_reason == FAILURE;
}
