// Ворота камеры: по команде, пришедшей через сервер, открываются или закрываются
// за время gate_time и сообщают серверу, что команда выполнена.
// Запуск: gate <хост> <порт> <номер_камеры> <сторона> (сторона: 0 – верхние, 1 – нижние)
#include "client.h"

int main(int argc, char **argv) {
    if (argc != 5) fail("Использование: gate хост порт номер_камеры сторона(0 – верхние, 1 – нижние)");

    int port = parse_port(argv[2]);
    int chamber_id = parse_int(argv[3], 0, 1000, "номер камеры");
    int side = parse_int(argv[4], SIDE_UPPER, SIDE_LOWER, "сторона");

    client_set_name("ворота %d (%s)", chamber_id, side_name(side));
    Msg welcome = client_join(argv[1], port, ROLE_GATE, chamber_id, side);
    int gate_time = welcome.a;
    client_say("подключены к серверу");

    while (1) {
        int command = client_expect(M_GATE_CMD).b;

        client_say(command == GATE_OPEN ? "открываются" : "закрываются");
        client_sleep_ticks(gate_time);
        client_say(command == GATE_OPEN ? "открыты" : "закрыты");

        client_send(M_GATE_DONE, side, command, 0);
    }
}
