#include "common.h"

// одни ворота камеры; side: 0 = верхние (у участка chamber_id), 1 = нижние (у участка chamber_id + 1)
// Пока ворота открыты, они держат ключ камеры token. Поэтому противоположные ворота
// не могут открыться, а камера не может менять уровень воды.
void gate_main(int chamber_id, int side) {
    Chamber *chamber = &shared->chambers[chamber_id];
    sem_t *cmd = sem_get(SEM_CMD, chamber_id, side); // команда от камеры
    sem_t *done = sem_get(SEM_DONE, chamber_id, side); // ответ камере
    sem_t *token = sem_get(SEM_TOKEN, chamber_id, 0); // ключ камеры

    while (1) {
        sem_wait_retry(cmd);
        lock();
        int command = chamber->cmd[side];
        unlock();

        if (command == GATE_OPEN) {
            sem_wait_retry(token); // ждём, пока другие ворота закрыты, а уровень воды не меняется
            ticks(shared->gate_time);

            lock();
            chamber->gate[side] = 1;
            if (chamber->gate[1 - side]) violation("противоположные ворота открыты одновременно");
            out("[t=%4d] камера %d: открыты ворота %s (%s)\n", shared->now, chamber_id,
                side == chamber->dir ? "входа" : "выхода", side == 0 ? "верхние" : "нижние");
            unlock();
        } else {
            ticks(shared->gate_time);

            lock();
            chamber->gate[side] = 0;
            out("[t=%4d] камера %d: ворота %s (%s) закрыты\n", shared->now, chamber_id,
                side == chamber->dir ? "входа" : "выхода", side == 0 ? "верхние" : "нижние");
            unlock();
            sem_post(token);
        }

        sem_post(done);
    }
}
