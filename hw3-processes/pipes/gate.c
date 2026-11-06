// Одни ворота камеры; side: 0 = верхние (у участка chamber_id), 1 = нижние (у участка chamber_id + 1).
// Пока ворота открыты, они держат ключ камеры token. Поэтому противоположные ворота
// не могут открыться, а камера не может менять уровень воды.
// Запуск: gate файл_параметров|- лог номер_камеры сторона
// Команды приходят сообщениями M_CMD, ответ – M_DONE; ключ камеры по-прежнему семафор.
#include "common.h"

int main(int argc, char **argv) {
    int chamber_id = arg_number(argc, argv, 3, "номер камеры");
    int side = arg_number(argc, argv, 4, "сторона ворот (0 или 1)");

    attach(argv[1], argv[2]);
    if (chamber_id >= shared->N || side > 1) fail("Такой камеры или стороны нет в модели");
    join(1 + shared->N + 2 * chamber_id + side);

    Chamber *chamber = &shared->chambers[chamber_id];
    sem_t *token = sem_get(SEM_TOKEN, chamber_id, 0); // ключ камеры

    while (1) {
        int command = recv_expect(M_CMD).arg; // команда от камеры: открыть или закрыть

        if (command == GATE_OPEN) {
            wait_sem(token); // ждём, пока другие ворота закрыты, а уровень воды не меняется
            ticks(shared->gate_time);

            lock();
            chamber->gate[side] = 1;
            if (chamber->gate[1 - side]) violation("противоположные ворота открыты одновременно");
            out("[t=%4d] камера %d: открыты ворота %s (%s)\n", time_now(), chamber_id,
                side == chamber->dir ? "входа" : "выхода", side == 0 ? "верхние" : "нижние");
            unlock();
        } else {
            ticks(shared->gate_time);

            lock();
            chamber->gate[side] = 0;
            out("[t=%4d] камера %d: ворота %s (%s) закрыты\n", time_now(), chamber_id,
                side == chamber->dir ? "входа" : "выхода", side == 0 ? "верхние" : "нижние");
            unlock();
            sem_post(token);
        }

        send_msg(mb_chamber(chamber_id), M_DONE, side); // команда выполнена
    }
}
