// Камера: получает группу от диспетчера и проводит её через шлюз,
// командуя воротам и зовя суда по одному.
// Запуск: chamber файл_параметров|- лог номер_камеры
#include <stdio.h>
#include "common.h"

// команда воротам стороны side и ожидание, пока они её выполнят
static void gate_cmd(Chamber *chamber, sem_t *cmd, sem_t *done, int side, int command) {
    lock();
    chamber->cmd[side] = command;
    unlock();

    sem_post(cmd);
    wait_sem(done); // ждём, пока ворота выполнят команду
}

// смена уровня воды под сторону to_side: нужен ключ камеры, то есть все ворота закрыты
static void change_water(int chamber_id, Chamber *chamber, sem_t *token, int to_side, const char *reason) {
    char buf[100];

    wait_sem(token);

    lock();
    if (chamber->gate[0] || chamber->gate[1]) violation("уровень воды меняется при открытых воротах");
    say(chamber_id, reason);
    unlock();

    ticks(shared->level_time);

    lock();
    chamber->water = to_side;
    shared->level_changes++;
    snprintf(buf, sizeof(buf), "уровень воды изменён до %d", level(chamber_id + to_side));
    say(chamber_id, buf);
    unlock();

    sem_post(token);
}

int main(int argc, char **argv) {
    int chamber_id = arg_number(argc, argv, 3, "номер камеры");

    attach(argv[1], argv[2]);
    if (chamber_id >= shared->N) fail("Такой камеры нет в модели");
    join(1 + chamber_id);

    Chamber *chamber = &shared->chambers[chamber_id];
    sem_t *start = sem_get(SEM_START, chamber_id, 0); // группа назначена диспетчером
    sem_t *ack = sem_get(SEM_ACK, chamber_id, 0); // судно закончило вход или выход
    sem_t *token = sem_get(SEM_TOKEN, chamber_id, 0); // ключ камеры
    sem_t *cmd[2], *done[2]; // связь с верхними и нижними воротами
    int i;

    for (int side = 0; side < 2; side++) {
        cmd[side] = sem_get(SEM_CMD, chamber_id, side);
        done[side] = sem_get(SEM_DONE, chamber_id, side);
    }

    while (1) {
        wait_sem(start);

        lock();
        int dir = chamber->dir; // сторона входа выбрана диспетчером
        unlock();

        if (chamber->water != dir)
            change_water(chamber_id, chamber, token, dir, "подготовка: уровень воды меняется под сторону входа");

        gate_cmd(chamber, cmd[dir], done[dir], dir, GATE_OPEN);

        // загрузка: одно судно за тик
        while (1) {
            lock();
            i = pick(chamber_id, dir, shared->cap - chamber->load);
            if (i >= 0) {
                shared->ships[i].state = ENTERING; // вместимость занимается сразу
                chamber->load += shared->ships[i].size;
            }
            unlock();

            if (i < 0) break;

            post_named(SEM_GO, i, 0); // судно входит само и сообщает об этом
            wait_sem(ack);
            ticks(1);
        }

        gate_cmd(chamber, cmd[dir], done[dir], dir, GATE_CLOSE);

        change_water(chamber_id, chamber, token, 1 - dir,
                     "подготовка к выходу: уровень воды меняется под сторону выхода");

        gate_cmd(chamber, cmd[1 - dir], done[1 - dir], 1 - dir, GATE_OPEN);

        // выгрузка: одно судно за тик
        while (1) {
            lock();
            for (i = 0; i < shared->n_ships; i++)
                if (shared->ships[i].state == INSIDE && shared->ships[i].chamber == chamber_id) break;

            if (i == shared->n_ships) {
                unlock();
                break;
            }

            shared->ships[i].state = EXITING;
            chamber->load -= shared->ships[i].size;
            unlock();

            post_named(SEM_GO, i, 0); // судно выходит само
            wait_sem(ack);
            ticks(1);
        }

        gate_cmd(chamber, cmd[1 - dir], done[1 - dir], 1 - dir, GATE_CLOSE);

        // цикл закончен: камера свободна, диспетчер решит, что делать дальше
        lock();
        chamber->busy = 0;
        say(chamber_id, "свободна");
        unlock();

        post_named(SEM_WAKE, 0, 0);
    }
}
