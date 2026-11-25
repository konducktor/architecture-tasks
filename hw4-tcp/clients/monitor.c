// Монитор: отдельный клиент, который показывает работу всей шлюзовой системы.
// При подключении получает от сервера параметры и снимок состояния, затем – событие
// на каждое изменение модели. Монитор ведёт собственную копию модели и по ней
// печатает журнал всей системы и итоговый отчёт. Мониторов может быть сколько угодно,
// подключаться можно в любой момент, на ход модели монитор не влияет.
// Запуск: monitor <ip_сервера> <порт> [<лог>]
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

#include "client.h"

typedef struct {
    int known; // монитор уже знает об этом судне
    int size, dir, prio;
    int state, pos;
    int without_link;
} ShipView;

typedef struct {
    int connected, gate_connected[2];
    int busy, entry_side, water, level_changing, last_dir;
    int gate[2];
} ChamberView;

typedef struct {
    int passed, left, groups, end_time, average_wait, max_wait, level_changes, violations, stop_signal;
} FinalStats;

static struct {
    int chambers_count, capacity, step, ships_expected, time_limit, strategy, max_wait;
    int in_snapshot; // события снимка только обновляют состояние, без печати
    int locking_started;
    int passed, left, ships_seen;
    int event_time;
    char failure_note[128];
    FinalStats final;
    ShipView ships[MAX_SHIPS + 1]; // по номеру судна, номера начинаются с 1
    ChamberView chambers[MAX_CHAMBERS];
} view;

static FILE *log_file;

// вывод:

static void show(const char *format, ...) __attribute__((format(printf, 1, 2)));
static void show(const char *format, ...) {
    va_list args;

    va_start(args, format);
    vprintf(format, args);
    va_end(args);
    fflush(stdout);

    if (!log_file) return;
    va_start(args, format);
    vfprintf(log_file, format, args);
    va_end(args);
    fflush(log_file);
}

static void show_event(const char *format, ...) __attribute__((format(printf, 1, 2)));
static void show_event(const char *format, ...) {
    char text[512];
    va_list args;

    if (view.in_snapshot) return;

    va_start(args, format);
    vsnprintf(text, sizeof(text), format, args);
    va_end(args);

    show("[t=%4d] %s\n", view.event_time, text);
}

// модель глазами монитора:

static int level(int section) {
    return (view.chambers_count - section) * view.step;
}

static int valid_chamber(int chamber_id) {
    return chamber_id >= 0 && chamber_id < view.chambers_count;
}

static ShipView *ship_by_id(int ship_id) {
    if (ship_id < 1 || ship_id > MAX_SHIPS) return NULL;
    if (ship_id > view.ships_seen) view.ships_seen = ship_id;
    return &view.ships[ship_id];
}

static int chamber_of(const ShipView *ship) {
    return ship->dir == DIR_FORWARD ? ship->pos : ship->pos - 1;
}

static int is_in_chamber(const ShipView *ship) {
    return ship->state == SHIP_ENTERING || ship->state == SHIP_INSIDE || ship->state == SHIP_EXITING;
}

static int chamber_load(int chamber_id) {
    int load = 0;

    for (int ship_id = 1; ship_id <= view.ships_seen; ship_id++) {
        ShipView *ship = &view.ships[ship_id];
        if (ship->known && is_in_chamber(ship) && chamber_of(ship) == chamber_id) load += ship->size;
    }

    return load;
}

static int queue_len(int chamber_id, int side) {
    int count = 0;

    for (int ship_id = 1; ship_id <= view.ships_seen; ship_id++) {
        ShipView *ship = &view.ships[ship_id];
        if (ship->known && ship->state == SHIP_QUEUED && ship->dir == side && chamber_of(ship) == chamber_id) count++;
    }

    return count;
}

static const char *gate_role(int chamber_id, int side) {
    return side == view.chambers[chamber_id].entry_side ? "входа" : "выхода";
}

// печать состояния:

static void print_header(void) {
    show("Шлюзов: %d, вместимость: %d, стратегия: %d, макс. ожидание: %d\n", view.chambers_count, view.capacity,
         view.strategy, view.max_wait);

    if (view.ships_expected) show("Судов в модели: %d\n", view.ships_expected);
    else show("Число судов не ограничено\n");

    if (view.time_limit) show("Время моделирования ограничено: %d\n", view.time_limit);
    else if (!view.ships_expected) show("Время моделирования не ограничено, остановка по Ctrl+C на сервере\n");

    for (int section = 0; section <= view.chambers_count; section++)
        show("участок %d: уровень %d\n", section, level(section));
}

static void print_chambers(void) {
    for (int chamber_id = 0; chamber_id < view.chambers_count; chamber_id++) {
        ChamberView *chamber = &view.chambers[chamber_id];

        show("камера %d: %s, направление %s, вода на уровне %d, занято %d/%d, ворота: верхние %s, нижние %s\n",
             chamber_id, chamber->busy ? "работает" : "свободна", chamber->busy ? dir_name(chamber->entry_side) : "-",
             level(chamber_id + chamber->water), chamber_load(chamber_id), view.capacity,
             chamber->gate[0] != GATE_CLOSED ? "открыты" : "закрыты",
             chamber->gate[1] != GATE_CLOSED ? "открыты" : "закрыты");
    }
}

static void print_ships_where(int chamber_id, int side, int inside) {
    for (int ship_id = 1; ship_id <= view.ships_seen; ship_id++) {
        ShipView *ship = &view.ships[ship_id];
        if (!ship->known || chamber_of(ship) != chamber_id) continue;

        int waiting = ship->state == SHIP_QUEUED && ship->dir == side;
        if (inside ? is_in_chamber(ship) : waiting) show(" %d%s", ship_id, ship->without_link ? "(без связи)" : "");
    }
}

static void print_queues(void) {
    for (int chamber_id = 0; chamber_id < view.chambers_count; chamber_id++) {
        for (int side = 0; side < 2; side++) {
            show("камера %d, %s:", chamber_id, dir_name(side));
            print_ships_where(chamber_id, side, 0);
            show("\n");
        }

        show("внутри камеры %d:", chamber_id);
        print_ships_where(chamber_id, 0, 1);
        show("\n");
    }

    if (view.ships_expected) show("ещё не прибыли: %d\n", view.ships_expected - view.ships_seen);
}

// монитор подключился посреди работы: к системе уже кто-то подключён
static int anything_happened(void) {
    for (int chamber_id = 0; chamber_id < view.chambers_count; chamber_id++) {
        ChamberView *chamber = &view.chambers[chamber_id];
        if (chamber->connected || chamber->gate_connected[0] || chamber->gate_connected[1]) return 1;
    }

    return view.ships_seen > 0;
}

static void print_joined_state(void) {
    if (!anything_happened()) return;

    show("--- состояние системы на момент подключения монитора, t=%d, прошли %d, покинули систему %d ---\n",
         view.event_time, view.passed, view.left);
    print_chambers();
    print_queues();
    show("---\n");
}

static void print_final_report(int reason) {
    FinalStats *final = &view.final;

    if (reason == ALL_DONE && final->left == 0) {
        show("Все %d судов прошли систему за %d\n", final->passed, final->end_time);
    } else if (reason == ALL_DONE) {
        show("Все суда обработаны за %d: прошли систему %d, покинули её %d\n", final->end_time, final->passed,
             final->left);
    } else {
        if (reason == TIME_OUT) show("Время моделирования вышло.");
        else if (reason == INTERRUPT) show("Прервано пользователем (сигнал %d).", final->stop_signal);
        else show("Аварийная остановка: %s.", view.failure_note[0] ? view.failure_note : "причина неизвестна");

        show(" Прошли %d, покинули систему %d. Очереди:\n", final->passed, final->left);
        print_chambers();
        print_queues();
    }

    show("Статистика: время %d, прошли %d, покинули %d, групп %d, смен уровня %d, среднее ожидание %d, "
         "максимальное %d, нарушений инвариантов %d\n",
         final->end_time, final->passed, final->left, final->groups, final->level_changes, final->average_wait,
         final->max_wait, final->violations);
}

static void on_model_stopped(int reason) {
    show_event("модель остановлена: %s", stop_reason(reason));
    print_final_report(reason);
    if (log_file) fclose(log_file);
}

// события судов:

static void apply_ship_arrived(ShipView *ship, int ship_id, int size, int dir) {
    ship->known = 1, ship->size = size, ship->dir = dir;
    ship->pos = dir == DIR_FORWARD ? 0 : view.chambers_count;
    ship->state = SHIP_QUEUED;

    show_event("судно %d (размер %d, %s%s) прибыло на участок %d", ship_id, size, dir_name(dir),
               ship->prio ? ", приоритетное" : "", ship->pos);
}

static void apply_ship_entered(ShipView *ship, int ship_id, int chamber_id) {
    ship->state = SHIP_INSIDE;
    show_event("судно %d (размер %d%s%s) вошло в камеру %d, занято %d/%d", ship_id, ship->size,
               ship->prio ? ", приоритетное" : "", ship->without_link ? ", без связи" : "", chamber_id,
               chamber_load(chamber_id), view.capacity);
}

static void apply_ship_left(ShipView *ship, int ship_id, int reason) {
    if (reason == LEFT_DISCONNECTED)
        show_event("судно %d покинуло систему (отключилось) с участка %d", ship_id, ship->pos);
    else if (reason == LEFT_UNLOADED_WITHOUT_LINK)
        show_event("судно %d без связи выведено из камеры и покинуло систему", ship_id);
    else
        show_event("судно %d отклонено сервером: неверные параметры", ship_id);

    ship->state = SHIP_LEFT;
    view.left++;
}

static void apply_ship_lost(ShipView *ship, int ship_id) {
    ship->without_link = 1;

    if (ship->state == SHIP_INSIDE)
        show_event("судно %d потеряло связь, остаётся в камере %d до выгрузки", ship_id, chamber_of(ship));
    else if (ship->state == SHIP_ENTERING)
        show_event("судно %d потеряло связь при входе, вход завершается без связи", ship_id);
    else if (ship->state == SHIP_EXITING)
        show_event("судно %d потеряло связь при выходе, выход завершается без связи", ship_id);
}

static void apply_ship_event(const Msg *msg) {
    ShipView *ship = ship_by_id(msg->a);
    if (!ship) return;

    switch (msg->type) {
    case M_EV_SHIP_CONNECTED:
        ship->known = 1, ship->state = SHIP_CONNECTING;
        show_event("к системе подключилось судно %d", msg->a);
        break;
    case M_EV_SHIP_PRIORITY:
        ship->prio = 1;
        break;
    case M_EV_SHIP_ARRIVED:
        apply_ship_arrived(ship, msg->a, msg->b, msg->c);
        break;
    case M_EV_SHIP_STATE:
        ship->state = msg->b, ship->pos = msg->c;
        break;
    case M_EV_SHIP_QUEUED:
        ship->state = SHIP_QUEUED, ship->pos = msg->c;
        show_event("судно %d встало в очередь к камере %d", msg->a, msg->b);
        break;
    case M_EV_SHIP_CALLED:
        ship->state = msg->c == MOVE_ENTER ? SHIP_ENTERING : SHIP_EXITING;
        show_event("диспетчер зовёт судно %d %s камеры %d", msg->a, msg->c == MOVE_ENTER ? "на вход в" : "на выход из",
                   msg->b);
        break;
    case M_EV_SHIP_ENTERED:
        apply_ship_entered(ship, msg->a, msg->b);
        break;
    case M_EV_SHIP_EXITED:
        // судно уже вне камеры; следом придёт очередь к следующей камере, прохождение или уход
        ship->state = SHIP_QUEUED, ship->pos = msg->c;
        show_event("судно %d вышло из камеры %d на участок %d (уровень %d)", msg->a, msg->b, msg->c, level(msg->c));
        break;
    case M_EV_SHIP_PASSED:
        ship->state = SHIP_PASSED;
        view.passed++;
        show_event("судно %d прошло всю систему", msg->a);
        break;
    case M_EV_SHIP_LEFT:
        apply_ship_left(ship, msg->a, msg->b);
        break;
    case M_EV_SHIP_LOST:
        apply_ship_lost(ship, msg->a);
        break;
    }
}

// события камер и ворот:

static void apply_group_started(ChamberView *chamber, int chamber_id, int side) {
    chamber->busy = 1, chamber->entry_side = side;

    if (side != chamber->last_dir) {
        show_event("камера %d: направление работы -> %s", chamber_id, dir_name(side));
        chamber->last_dir = side;
    }

    show_event("камера %d пропускает группу (%s), в очереди с этой стороны: %d", chamber_id, dir_name(side),
               queue_len(chamber_id, side));
}

static void apply_gate_state(ChamberView *chamber, int chamber_id, int side, int state) {
    chamber->gate[side] = state;

    if (state == GATE_IS_OPEN)
        show_event("камера %d: открыты ворота %s (%s)", chamber_id, gate_role(chamber_id, side), side_name(side));
    else if (state == GATE_CLOSED)
        show_event("камера %d: ворота %s (%s) закрыты", chamber_id, gate_role(chamber_id, side), side_name(side));
}

static void apply_level(ChamberView *chamber, int chamber_id, int type, int side) {
    if (type == M_EV_LEVEL_BEGIN) {
        chamber->level_changing = 1;
        show_event("камера %d: %s", chamber_id,
                   side == chamber->entry_side ? "подготовка: уровень воды меняется под сторону входа"
                                               : "подготовка к выходу: уровень воды меняется под сторону выхода");
        return;
    }

    chamber->level_changing = 0, chamber->water = side;
    show_event("камера %d: уровень воды изменён до %d", chamber_id, level(chamber_id + side));
}

static void apply_participant_lost(int role, int chamber_id, int side) {
    char participant[64];

    if (role == ROLE_CHAMBER) {
        view.chambers[chamber_id].connected = 0;
        snprintf(participant, sizeof(participant), "камера %d", chamber_id);
    } else {
        view.chambers[chamber_id].gate_connected[side] = 0;
        snprintf(participant, sizeof(participant), "ворота %d (%s)", chamber_id, side_name(side));
    }

    if (!view.locking_started) {
        show_event("отключился участник «%s» до начала шлюзования", participant);
        return;
    }

    snprintf(view.failure_note, sizeof(view.failure_note), "отключился участник «%s»", participant);
    show_event("%s, работа шлюза невозможна", view.failure_note);
}

static void apply_chamber_event(const Msg *msg) {
    if (!valid_chamber(msg->a)) return;
    ChamberView *chamber = &view.chambers[msg->a];
    int side = msg->b == SIDE_LOWER ? SIDE_LOWER : SIDE_UPPER;

    switch (msg->type) {
    case M_EV_CHAMBER_CONNECTED:
        chamber->connected = 1;
        show_event("камера %d подключилась", msg->a);
        break;
    case M_EV_GATE_CONNECTED:
        chamber->gate_connected[side] = 1;
        show_event("ворота камеры %d (%s) подключились", msg->a, side_name(side));
        break;
    case M_EV_GROUP_STARTED:
        apply_group_started(chamber, msg->a, side);
        break;
    case M_EV_LEVEL_BEGIN:
    case M_EV_LEVEL_END:
        apply_level(chamber, msg->a, msg->type, side);
        break;
    case M_EV_GATE_STATE:
        apply_gate_state(chamber, msg->a, side, msg->c);
        break;
    case M_EV_CHAMBER_FREE:
        chamber->busy = 0;
        show_event("камера %d: свободна", msg->a);
        break;
    }
}

// остальные события:

static void apply_info(int key, int value) {
    switch (key) {
    case INFO_CHAMBERS: view.chambers_count = value > MAX_CHAMBERS ? MAX_CHAMBERS : value; break;
    case INFO_CAPACITY: view.capacity = value; break;
    case INFO_STEP: view.step = value; break;
    case INFO_SHIPS_EXPECTED: view.ships_expected = value; break;
    case INFO_TIME_LIMIT: view.time_limit = value; break;
    case INFO_STRATEGY: view.strategy = value; break;
    case INFO_MAX_WAIT: view.max_wait = value; break;
    }
}

static void apply_system_event(const Msg *msg) {
    switch (msg->type) {
    case M_INFO:
        apply_info(msg->a, msg->b);
        break;
    case M_SNAPSHOT_BEGIN:
        print_header();
        view.in_snapshot = 1;
        break;
    case M_COUNTS:
        view.passed = msg->a, view.left = msg->b;
        if (msg->c > view.ships_seen) view.ships_seen = msg->c;
        break;
    case M_SNAPSHOT_END:
        view.in_snapshot = 0;
        print_joined_state();
        break;
    case M_EV_LOCKING_STARTED:
        view.locking_started = 1;
        show_event("все камеры и ворота подключены, шлюзование начинается");
        break;
    case M_EV_CLIENT_REJECTED:
        show_event("сервер отказал новому клиенту: %s", reject_reason(msg->a));
        break;
    case M_EV_PARTICIPANT_LOST:
        if (valid_chamber(msg->b)) apply_participant_lost(msg->a, msg->b, msg->c == SIDE_LOWER);
        break;
    case M_EV_VIOLATION:
        snprintf(view.failure_note, sizeof(view.failure_note), "нарушение инварианта");
        show_event("НАРУШЕНИЕ ИНВАРИАНТА: %s", violation_text(msg->a));
        break;
    case M_EV_TIME_LIMIT:
        view.time_limit = msg->a;
        if (msg->a) show_event("время моделирования ограничено, остановка при t=%d", msg->a);
        else show_event("время моделирования не ограничено");
        break;
    case M_EV_FINAL_COUNTS:
        view.final.passed = msg->a, view.final.left = msg->b, view.final.groups = msg->c;
        break;
    case M_EV_FINAL_TIMES:
        view.final.end_time = msg->a, view.final.average_wait = msg->b, view.final.max_wait = msg->c;
        break;
    case M_EV_FINAL_OTHER:
        view.final.level_changes = msg->a, view.final.violations = msg->b, view.final.stop_signal = msg->c;
        break;
    default:
        show_event("неизвестное сообщение от сервера: %d", msg->type);
    }
}

static int is_ship_event(int type) {
    return type == M_EV_SHIP_CONNECTED || type == M_EV_SHIP_PRIORITY || type == M_EV_SHIP_ARRIVED ||
           type == M_EV_SHIP_STATE || type == M_EV_SHIP_QUEUED || type == M_EV_SHIP_CALLED ||
           type == M_EV_SHIP_ENTERED || type == M_EV_SHIP_EXITED || type == M_EV_SHIP_PASSED ||
           type == M_EV_SHIP_LEFT || type == M_EV_SHIP_LOST;
}

static int is_chamber_event(int type) {
    return type == M_EV_CHAMBER_CONNECTED || type == M_EV_GATE_CONNECTED || type == M_EV_GROUP_STARTED ||
           type == M_EV_LEVEL_BEGIN || type == M_EV_LEVEL_END || type == M_EV_GATE_STATE || type == M_EV_CHAMBER_FREE;
}

int main(int argc, char **argv) {
    if (argc < 3 || argc > 4) fail("Использование: monitor <ip_сервера> <порт> [<лог>]");

    int port = parse_port(argv[2]);
    if (argc == 4) {
        log_file = fopen(argv[3], "w");
        if (!log_file) die(argv[3]);
    }

    for (int chamber_id = 0; chamber_id < MAX_CHAMBERS; chamber_id++) view.chambers[chamber_id].last_dir = -1;

    client_set_name("монитор");
    client_on_stop(on_model_stopped);
    client_join(argv[1], port, ROLE_MONITOR, 0, 0);
    show("Монитор подключён к серверу %s:%d\n", argv[1], port);

    while (1) {
        Msg msg = client_receive();
        view.event_time = msg.time;

        if (is_ship_event(msg.type)) apply_ship_event(&msg);
        else if (is_chamber_event(msg.type)) apply_chamber_event(&msg);
        else apply_system_event(&msg);
    }
}
