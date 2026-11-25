// Камера шлюза: получает от диспетчера группу и проводит её через шлюз.
// Все команды воротам и вызовы судов идут через сервер, который проверяет инварианты.
// Запуск: chamber <хост> <порт> <номер_камеры>
#include "client.h"

static int level_time;

static void change_water(int to_side, const char *reason) {
    client_say("%s", reason);
    client_send(M_LEVEL_BEGIN, to_side, 0, 0);
    client_sleep_ticks(level_time);
    client_send(M_LEVEL_END, to_side, 0, 0);
    client_say("уровень воды изменён под %s ворота", side_name(to_side));
}

static void command_gate(int side, int command) {
    client_send(M_GATE_CMD, side, command, 0);
    client_expect(M_GATE_DONE);
}

// зовёт суда по одному, пока серверу есть кого звать; возвращает число прошедших через ворота
static int move_ships(int move) {
    int moved = 0;

    while (1) {
        client_send(M_NEXT_SHIP, move, 0, 0);
        Msg reply = client_receive();

        if (reply.type == M_NO_SHIP) return moved;
        if (reply.type != M_SHIP_DONE) fail("Камера: неожиданное сообщение при загрузке или выгрузке");

        client_say("судно %d %s", reply.a, move == MOVE_ENTER ? "вошло" : "вышло");
        moved++;
    }
}

static void pass_group(int entry_side, int water_side) {
    int exit_side = 1 - entry_side;

    client_say("группа %s, вход через %s ворота", dir_name(entry_side), side_name(entry_side));
    if (water_side != entry_side) change_water(entry_side, "подготовка: меняю уровень воды под сторону входа");

    command_gate(entry_side, GATE_OPEN);
    int loaded = move_ships(MOVE_ENTER);
    command_gate(entry_side, GATE_CLOSE);

    // все ушли из очереди, пока ворота открывались: шлюзовать некого
    if (loaded > 0) {
        change_water(exit_side, "подготовка к выходу: меняю уровень воды под сторону выхода");
        command_gate(exit_side, GATE_OPEN);
        move_ships(MOVE_EXIT);
        command_gate(exit_side, GATE_CLOSE);
    }

    client_send(M_FREE, 0, 0, 0);
    client_say("свободна");
}

int main(int argc, char **argv) {
    if (argc != 4) fail("Использование: chamber хост порт номер_камеры");

    int port = parse_port(argv[2]);
    int chamber_id = parse_int(argv[3], 0, 1000, "номер камеры");

    client_set_name("камера %d", chamber_id);
    level_time = client_join(argv[1], port, ROLE_CHAMBER, chamber_id, 0).a;
    client_say("подключена к серверу");

    while (1) {
        Msg start = client_expect(M_START);
        pass_group(start.a, start.b);
    }
}
