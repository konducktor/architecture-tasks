#include "common.h"

// диспетчер: для каждой свободной камеры выбирает сторону следующей группы
// и сообщает об этом камере; сам суда и ворота не трогает
void dispatcher_main(void) {
    sem_t *wake = sem_get(SEM_WAKE, 0, 0); // что-то изменилось: судно встало в очередь или камера освободилась
    sem_t *start[MAX_CHAMBERS]; // start[c]: камере c назначена группа

    for (int c = 0; c < shared->N; c++) start[c] = sem_get(SEM_START, c, 0);

    while (1) {
        sem_wait_retry(wake);
        lock();

        for (int c = 0; c < shared->N; c++) {
            Chamber *chamber = &shared->chambers[c];
            if (chamber->busy) continue;

            int dir = choose(c);
            if (dir < 0) continue; // очередей нет

            chamber->busy = 1, chamber->dir = dir;
            shared->groups++;

            if (dir != chamber->last_dir) {
                out("[t=%4d] диспетчер: камера %d, направление работы -> %s\n", shared->now, c, dname(dir));
                chamber->last_dir = dir;
            }

            out("[t=%4d] диспетчер: камера %d пропускает группу (%s), в очереди с этой стороны: %d\n", shared->now, c,
                dname(dir), queue_len(c, dir));
            sem_post(start[c]);
        }

        unlock();
    }
}
