// Диспетчер: для каждой свободной камеры выбирает сторону следующей группы
// и сообщает об этом камере; сам суда и ворота не трогает.
// Запуск: dispatcher [файл_параметров|-] [лог]    или    dispatcher -c (удалить остатки аварийного запуска)
// Кроме того, диспетчер следит за временем модели и за тем, что остальные участники живы.
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <signal.h>
#include <unistd.h>
#include <sys/time.h>
#include "common.h"

// все суда прошли и все камеры закончили цикл: модель завершена
static void check_finish(void) {
    if (shared->passed >= shared->n_ships && all_idle()) request_stop(ALL_DONE);
}

// участник, который исчез, не выйдя штатно (например, kill -9), останавливает всю модель
static void check_members(void) {
    for (int slot = 0; slot < shared->total; slot++) {
        pid_t pid = shared->pids[slot];

        if (pid <= 0 || pid == getpid()) continue;
        if (kill(pid, 0) == 0 || errno != ESRCH) continue;
        if (shared->pids[slot] != pid) continue; // успел выйти штатно

        int got = lock_try(200);
        shared->pids[slot] = 0, shared->alive--, shared->bad_pid = pid;
        if (got) unlock();

        request_stop(FAILURE);
    }
}

// по таймеру (SIGALRM прерывает ожидание): ограничение времени, живость участников, завершение
static void on_timer(void) {
    if (shared->end_at && time_now() >= shared->end_at) request_stop(TIME_OUT);
    check_members();
    check_finish();
}

int main(int argc, char **argv) {
    // служебный режим: удалить остатки аварийно завершённого запуска
    if (argc > 1 && strcmp(argv[1], "-c") == 0) {
        unlink_all(MAX_CHAMBERS, MAX_SHIPS);
        printf("Семафоры и разделяемая память удалены\n");
        return 0;
    }

    attach(argc > 1 ? argv[1] : "-", argc > 2 ? argv[2] : "locks.log");
    join(0);

    // таймер раз в тик: обработчик сигнала кладёт M_TICK в ящик диспетчера, и вызывается on_timer
    long interval = shared->tick < 5 ? 5 : shared->tick;
    struct itimerval timer = {{interval / 1000, (interval % 1000) * 1000}, {interval / 1000, (interval % 1000) * 1000}};
    on_tick = on_timer;
    setitimer(ITIMER_REAL, &timer, NULL);

    while (1) {
        recv_expect(M_WAKE); // что-то изменилось: судно встало в очередь или камера освободилась
        lock();

        for (int c = 0; c < shared->N; c++) {
            Chamber *chamber = &shared->chambers[c];
            if (chamber->busy) continue;

            int dir = choose(c);
            if (dir < 0) continue; // очередей нет

            chamber->busy = 1, chamber->dir = dir;
            shared->groups++;

            if (dir != chamber->last_dir) {
                out("[t=%4d] диспетчер: камера %d, направление работы -> %s\n", time_now(), c, dname(dir));
                chamber->last_dir = dir;
            }

            out("[t=%4d] диспетчер: камера %d пропускает группу (%s), в очереди с этой стороны: %d\n", time_now(), c,
                dname(dir), queue_len(c, dir));
            send_msg(mb_chamber(c), M_START, dir); // камере c назначена группа
        }

        unlock();
        check_finish();
    }
}
