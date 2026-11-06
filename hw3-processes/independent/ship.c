// Судно i: ждёт прибытия, встаёт в очередь и по зову камеры сначала входит, потом выходит;
// после выхода либо встаёт в очередь к следующей камере, либо завершает проход.
// Запуск: ship файл_параметров|- лог номер_судна
#include "common.h"

int main(int argc, char **argv) {
    int i = arg_number(argc, argv, 3, "номер судна");

    attach(argv[1], argv[2]);
    if (i >= shared->n_ships) fail("Такого судна нет в модели");
    join(1 + 3 * shared->N + i);

    Ship *ship = &shared->ships[i];
    sem_t *go = sem_get(SEM_GO, i, 0); // зов камеры: первый раз войти, второй раз выйти

    // ждём время прибытия по общим часам, просыпаясь всё ближе к нему (и не дольше 100 мс, чтобы заметить остановку)
    while (time_now() < ship->arrive) {
        long pause = (long)(ship->arrive - time_now()) * shared->tick / 2 + 1;

        msleep(pause > 100 ? 100 : pause);
        poll_signals();
    }

    lock();
    out("[t=%4d] судно %d (размер %d, %s%s) прибыло на участок %d\n", time_now(), ship->id, ship->size,
        dname(ship->dir), ship->prio ? ", приоритетное" : "", ship->pos);
    ship->state = QUEUED, ship->queued_at = time_now();
    out("[t=%4d] судно %d встало в очередь к камере %d\n", time_now(), ship->id, ship->chamber);
    unlock();

    post_named(SEM_WAKE, 0, 0);

    while (1) {
        wait_sem(go);

        lock();
        int chamber_id = ship->chamber; // камера, которая позвала
        Chamber *chamber = &shared->chambers[chamber_id];

        if (ship->state == ENTERING) {
            int waited = time_now() - ship->queued_at;

            shared->entries++, shared->wait_sum += waited;
            if (waited > shared->wait_max) shared->wait_max = waited;

            // проверка инвариантов входа: открыты ворота своей стороны, уровень совпадает, вместимость не превышена
            if (!chamber->gate[ship->dir] || chamber->water != ship->dir)
                violation("судно входит при несовпадающем уровне или закрытых воротах");
            if (chamber->load > shared->cap) violation("вместимость камеры превышена");

            ship->state = INSIDE;
            out("[t=%4d] судно %d (размер %d%s) вошло в камеру %d, занято %d/%d\n", time_now(), ship->id,
                ship->size, ship->prio ? ", приоритетное" : "", chamber_id, chamber->load, shared->cap);
            unlock();

            post_named(SEM_ACK, chamber_id, 0);
        } else { // EXITING
            if (!chamber->gate[1 - ship->dir] || chamber->water != 1 - ship->dir)
                violation("судно выходит при несовпадающем уровне или закрытых воротах");

            ship->pos = ship->dir == 0 ? chamber_id + 1 : chamber_id;
            out("[t=%4d] судно %d вышло из камеры %d на участок %d (уровень %d)\n", time_now(), ship->id,
                chamber_id, ship->pos, level(ship->pos));

            if (ship->pos == (ship->dir == 0 ? shared->N : 0)) {
                ship->state = DONE;
                out("[t=%4d] судно %d прошло всю систему\n", time_now(), ship->id);
                shared->passed++;
                if (shared->passed >= shared->n_ships) shared->end_time = time_now();
                unlock();

                post_named(SEM_ACK, chamber_id, 0);
                leave();
            }

            ship->state = QUEUED;
            ship->queued_at = time_now();
            ship->chamber = ship->dir == 0 ? ship->pos : ship->pos - 1;
            out("[t=%4d] судно %d встало в очередь к камере %d\n", time_now(), ship->id, ship->chamber);
            unlock();

            post_named(SEM_WAKE, 0, 0);
            post_named(SEM_ACK, chamber_id, 0);
        }
    }
}
