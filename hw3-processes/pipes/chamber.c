// Камера: получает группу от диспетчера и проводит её через шлюз,
// командуя воротам и зовя суда по одному.
// Запуск: chamber файл_параметров|- лог номер_камеры
// Все «разбуди» и «готово» – сообщения в каналы; ключ камеры остаётся семафором.
#include <stdio.h>
#include "common.h"

// команда воротам стороны side и ожидание, пока они её выполнят
static void gate_cmd(int chamber_id, int side, int command) {
    send_msg(mb_gate(chamber_id, side), M_CMD, command);
    recv_expect(M_DONE); // ждём, пока ворота выполнят команду
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
    sem_t *token = sem_get(SEM_TOKEN, chamber_id, 0); // ключ камеры
    int i;

    while (1) {
        recv_expect(M_START); // группа назначена диспетчером

        lock();
        int dir = chamber->dir; // сторона входа выбрана диспетчером
        unlock();

        if (chamber->water != dir)
            change_water(chamber_id, chamber, token, dir, "подготовка: уровень воды меняется под сторону входа");

        gate_cmd(chamber_id, dir, GATE_OPEN);

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

            send_msg(mb_ship(i), M_GO, 0); // судно входит само и сообщает об этом
            recv_expect(M_ACK); // судно закончило вход или выход
            ticks(1);
        }

        gate_cmd(chamber_id, dir, GATE_CLOSE);

        change_water(chamber_id, chamber, token, 1 - dir,
                     "подготовка к выходу: уровень воды меняется под сторону выхода");

        gate_cmd(chamber_id, 1 - dir, GATE_OPEN);

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

            send_msg(mb_ship(i), M_GO, 0); // судно выходит само
            recv_expect(M_ACK); // судно закончило вход или выход
            ticks(1);
        }

        gate_cmd(chamber_id, 1 - dir, GATE_CLOSE);

        // цикл закончен: камера свободна, диспетчер решит, что делать дальше
        lock();
        chamber->busy = 0;
        say(chamber_id, "свободна");
        unlock();

        send_msg(mb_dispatcher(), M_WAKE, 0);
    }
}
