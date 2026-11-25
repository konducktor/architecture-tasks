// Судно: подключение к серверу – это прибытие к шлюзовой системе.
// Дальше судно стоит в очередях и по зову диспетчера входит в камеры и выходит из них.
// Запуск: ship <хост> <порт> [--size <N>] [--dir forward|backward] [--prio 0|1] [--seed <S>]
// Не заданные параметры выбираются случайно.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "client.h"

#define MOVE_TICKS 1 // время входа в камеру или выхода из неё
#define PRIORITY_CHANCE 5 // приоритетным становится примерно каждое пятое случайное судно

typedef struct {
    int size, dir, prio; // -1 – выбрать случайно
    unsigned seed;
    int seed_given;
} ShipOptions;

static void print_usage_and_exit(void) {
    fail("Использование: ship <хост> <порт> [--size N] [--dir forward|backward] [--prio 0|1] [--seed S]");
}

static int parse_dir(const char *text) {
    if (strcmp(text, "forward") == 0 || strcmp(text, "0") == 0) return DIR_FORWARD;
    if (strcmp(text, "backward") == 0 || strcmp(text, "1") == 0) return DIR_BACKWARD;
    fail("Направление: forward (0) или backward (1)");
}

static ShipOptions parse_options(int argc, char **argv) {
    ShipOptions options = {-1, -1, -1, 0, 0};

    for (int i = 3; i < argc; i += 2) {
        if (i + 1 >= argc) print_usage_and_exit();
        const char *value = argv[i + 1];

        if (strcmp(argv[i], "--size") == 0) options.size = parse_int(value, 1, 1000000, "размер");
        else if (strcmp(argv[i], "--dir") == 0) options.dir = parse_dir(value);
        else if (strcmp(argv[i], "--prio") == 0) options.prio = parse_int(value, 0, 1, "приоритет");
        else if (strcmp(argv[i], "--seed") == 0) {
            options.seed = (unsigned)parse_int(value, 0, 2147483647, "seed");
            options.seed_given = 1;
        } else print_usage_and_exit();
    }

    return options;
}

static void fill_random_options(ShipOptions *options, int capacity) {
    srand(options->seed_given ? options->seed : (unsigned)time(NULL) ^ (unsigned)getpid());

    // случайные числа берутся всегда в одном порядке, чтобы одно и то же seed давало одно и то же судно
    int random_size = rand() % capacity + 1;
    int random_dir = rand() % 2;
    int random_prio = rand() % PRIORITY_CHANCE == 0;

    if (options->size < 0) options->size = random_size;
    if (options->dir < 0) options->dir = random_dir;
    if (options->prio < 0) options->prio = random_prio;
}

static void move_through_chamber(int chamber_id, int move) {
    client_say(move == MOVE_ENTER ? "вхожу в камеру %d" : "выхожу из камеры %d", chamber_id);
    client_sleep_ticks(MOVE_TICKS);
    client_send(M_MOVED, move, 0, 0);
}

int main(int argc, char **argv) {
    if (argc < 3) print_usage_and_exit();

    int port = parse_port(argv[2]);
    ShipOptions options = parse_options(argc, argv);

    Msg welcome = client_join(argv[1], port, ROLE_SHIP, 0, 0);
    int ship_id = welcome.a, capacity = welcome.b;
    client_set_name("судно %d", ship_id);

    fill_random_options(&options, capacity);
    if (options.size > capacity) {
        client_say("размер %d больше вместимости камер %d, ухожу", options.size, capacity);
        return 1;
    }

    client_say("прибыло: размер %d, %s%s", options.size, dir_name(options.dir), options.prio ? ", приоритетное" : "");
    client_send(M_ARRIVED, options.size, options.dir, options.prio);

    while (1) {
        Msg msg = client_receive();

        switch (msg.type) {
        case M_QUEUED:
            client_say("встало в очередь к камере %d (участок %d)", msg.a, msg.b);
            break;
        case M_GO:
            move_through_chamber(msg.a, msg.b);
            break;
        case M_PASSED:
            client_say("прошло всю систему, участок %d", msg.a);
            return 0;
        case M_REJECT:
            client_say("сервер отказал: %s", reject_reason(msg.a));
            return 1;
        default:
            fprintf(stderr, "судно %d: неожиданное сообщение %d\n", ship_id, msg.type);
            return 1;
        }
    }
}
