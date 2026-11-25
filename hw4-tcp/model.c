#include "model.h"

#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "common.h"

#define PARAMS_COUNT 12

World world;
static FILE *log_file;

void out(const char *format, ...) {
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

void event(const char *format, ...) {
    char text[512];
    va_list args;

    va_start(args, format);
    vsnprintf(text, sizeof(text), format, args);
    va_end(args);

    out("[t=%4d] %s\n", time_now(), text);
}

int time_now(void) {
    return (int)((now_ms() - world.start_ms) / world.tick);
}

int level(int section) {
    return (world.chambers_count - section) * world.step;
}

int is_last_section(const Ship *ship) {
    return ship->pos == (ship->dir == DIR_FORWARD ? world.chambers_count : 0);
}

// нарушение инварианта модели; в нормальной работе не должно появляться
void violation(const char *what) {
    world.violations++;
    event("НАРУШЕНИЕ ИНВАРИАНТА: %s", what);
}

// читает до 12 чисел параметров из файла; недостающие остаются значениями по умолчанию
static void read_params(const char *path, int *params) {
    FILE *file = fopen(path, "r");
    if (!file) die(path);

    for (int i = 0; i < PARAMS_COUNT; i++)
        if (fscanf(file, "%d", &params[i]) != 1) break;

    fclose(file);
}

static int params_are_valid(const int *params) {
    for (int i = 0; i < PARAMS_COUNT; i++)
        if (params[i] < 0) return 0;

    return params[0] >= 1 && params[0] <= MAX_CHAMBERS && params[1] >= 1 && params[2] <= MAX_SHIPS &&
           params[6] <= 1 && params[11] >= 1;
}

static void print_header(void) {
    out("Шлюзов: %d, вместимость: %d, стратегия: %d, макс. ожидание: %d\n", world.chambers_count, world.capacity,
        world.strategy, world.max_wait);

    if (world.ships_expected) out("Судов в модели: %d\n", world.ships_expected);
    else out("Число судов не ограничено\n");

    if (world.limit) out("Время моделирования ограничено: %d\n", world.limit);
    else if (!world.ships_expected) out("Время моделирования не ограничено, остановка по Ctrl+C\n");

    for (int section = 0; section <= world.chambers_count; section++)
        out("участок %d: уровень %d\n", section, level(section));
}

void model_init(const char *params_path, const char *log_path) {
    // значения по умолчанию в порядке чисел в файле параметров
    int params[PARAMS_COUNT] = {3, 5, 10, 3, 2, 4, 0, 15, 500, 1, 2, 500};

    if (params_path && strcmp(params_path, "-") != 0) read_params(params_path, params);
    if (!params_are_valid(params))
        fail("Неверные параметры: камер 1..100, судов 0..1000 (0 – без ограничения), стратегия 0 или 1, "
             "вместимость и тик не меньше 1, остальное неотрицательно");

    world.chambers_count = params[0], world.capacity = params[1], world.ships_expected = params[2];
    world.arrival_interval = params[3], world.gate_time = params[4], world.level_time = params[5];
    world.strategy = params[6], world.max_wait = params[7], world.limit = params[8], world.seed = params[9];
    world.step = params[10], world.tick = params[11];
    world.end_at = world.limit, world.end_time = -1;
    world.start_ms = now_ms();

    for (int chamber_id = 0; chamber_id < world.chambers_count; chamber_id++) {
        Chamber *chamber = &world.chambers[chamber_id];
        chamber->last_dir = -1;
        chamber->chamber_client = chamber->gate_client[0] = chamber->gate_client[1] = -1;
    }

    log_file = fopen(log_path, "w");
    if (!log_file) die(log_path);

    print_header();
}

void model_close_log(void) {
    if (log_file) fclose(log_file);
    log_file = NULL;
}

static int is_better_candidate(const Ship *candidate, const Ship *best) {
    if (!best) return 1;
    if (candidate->prio != best->prio) return candidate->prio > best->prio;
    return candidate->queued_at < best->queued_at;
}

// лучшее ожидающее судно со стороны side, которое помещается в room
Ship *find_ship_to_enter(int chamber_id, int side, int room) {
    Ship *best = NULL;

    for (int i = 0; i < world.ships_registered; i++) {
        Ship *ship = &world.ships[i];
        if (ship->state != SHIP_QUEUED || ship->chamber != chamber_id || ship->dir != side || ship->size > room)
            continue;
        if (is_better_candidate(ship, best)) best = ship;
    }

    return best;
}

Ship *find_ship_to_exit(int chamber_id) {
    for (int i = 0; i < world.ships_registered; i++) {
        Ship *ship = &world.ships[i];
        if (ship->state == SHIP_INSIDE && ship->chamber == chamber_id) return ship;
    }

    return NULL;
}

int queue_len(int chamber_id, int side) {
    int count = 0;

    for (int i = 0; i < world.ships_registered; i++) {
        Ship *ship = &world.ships[i];
        if (ship->state == SHIP_QUEUED && ship->chamber == chamber_id && ship->dir == side) count++;
    }

    return count;
}

// выбор стороны, с которой камера пропустит следующую группу; -1 – очередей нет
int choose_side(int chamber_id) {
    const int any_room = 1 << 30;
    Ship *forward = find_ship_to_enter(chamber_id, DIR_FORWARD, any_room);
    Ship *backward = find_ship_to_enter(chamber_id, DIR_BACKWARD, any_room);

    if (!forward && !backward) return -1;
    if (!backward) return DIR_FORWARD;
    if (!forward) return DIR_BACKWARD;

    int now = time_now();
    int wait_forward = now - forward->queued_at, wait_backward = now - backward->queued_at;

    // защита от бесконечного ожидания одного направления
    if (wait_forward > world.max_wait && wait_forward >= wait_backward) return DIR_FORWARD;
    if (wait_backward > world.max_wait) return DIR_BACKWARD;

    if (forward->prio != backward->prio) return backward->prio > forward->prio ? DIR_BACKWARD : DIR_FORWARD;
    if (world.strategy == 1) return world.chambers[chamber_id].water; // по направлению: без смены уровня

    return wait_backward > wait_forward ? DIR_BACKWARD : DIR_FORWARD; // самое долгое ожидание
}

int all_chambers_idle(void) {
    for (int chamber_id = 0; chamber_id < world.chambers_count; chamber_id++)
        if (world.chambers[chamber_id].busy) return 0;

    return 1;
}

// все сформированные суда прошли систему или ушли из неё
int all_ships_handled(void) {
    return world.ships_expected > 0 && world.ships_registered == world.ships_expected &&
           world.passed + world.left == world.ships_expected;
}

static int gate_is_open(int state) {
    return state != GATE_CLOSED;
}

static void print_chambers(void) {
    for (int chamber_id = 0; chamber_id < world.chambers_count; chamber_id++) {
        Chamber *chamber = &world.chambers[chamber_id];

        out("камера %d: %s, направление %s, вода на уровне %d, занято %d/%d, ворота: верхние %s, нижние %s\n",
            chamber_id, chamber->busy ? "работает" : "свободна", chamber->busy ? dir_name(chamber->entry_side) : "-",
            level(chamber_id + chamber->water), chamber->load, world.capacity,
            gate_is_open(chamber->gate[0]) ? "открыты" : "закрыты", gate_is_open(chamber->gate[1]) ? "открыты" : "закрыты");
    }
}

static void print_ships_where(int chamber_id, int side, int inside) {
    for (int i = 0; i < world.ships_registered; i++) {
        Ship *ship = &world.ships[i];
        if (ship->chamber != chamber_id) continue;

        int ship_inside = ship->state == SHIP_ENTERING || ship->state == SHIP_INSIDE || ship->state == SHIP_EXITING;
        int ship_waiting = ship->state == SHIP_QUEUED && ship->dir == side;
        if (inside ? ship_inside : ship_waiting) out(" %d%s", ship->id, ship->client < 0 ? "(без связи)" : "");
    }
}

static void print_queues(void) {
    for (int chamber_id = 0; chamber_id < world.chambers_count; chamber_id++) {
        for (int side = 0; side < 2; side++) {
            out("камера %d, %s:", chamber_id, dir_name(side));
            print_ships_where(chamber_id, side, 0);
            out("\n");
        }

        out("внутри камеры %d:", chamber_id);
        print_ships_where(chamber_id, 0, 1);
        out("\n");
    }

    if (world.ships_expected) out("ещё не прибыли: %d\n", world.ships_expected - world.ships_registered);
}

void print_state(void) {
    out("--- состояние по SIGUSR1, t=%d, прошло %d, покинули систему %d ---\n", time_now(), world.passed, world.left);
    print_chambers();
    print_queues();
}

void print_final_report(void) {
    int end_time = world.end_time >= 0 ? world.end_time : time_now();

    if (world.stop_reason == ALL_DONE && world.left == 0) {
        out("Все %d судов прошли систему за %d\n", world.passed, end_time);
    } else if (world.stop_reason == ALL_DONE) {
        out("Все суда обработаны за %d: прошли систему %d, покинули её %d\n", end_time, world.passed, world.left);
    } else {
        if (world.stop_reason == TIME_OUT) out("Время моделирования вышло.");
        else if (world.stop_reason == INTERRUPT) out("Прервано пользователем (сигнал %d).", world.stop_signal);
        else out("Аварийная остановка: %s.", world.failure_note);

        out(" Прошли %d, покинули систему %d. Очереди:\n", world.passed, world.left);
        print_chambers();
        print_queues();
    }

    out("Статистика: время %d, прошли %d, покинули %d, групп %d, смен уровня %d, среднее ожидание %d, "
        "максимальное %d, нарушений инвариантов %d\n",
        end_time, world.passed, world.left, world.groups, world.level_changes,
        world.entries ? world.wait_sum / world.entries : 0, world.wait_max, world.violations);
}
