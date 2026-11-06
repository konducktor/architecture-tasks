#include "common.h"

// судно i: ждёт прибытия, встаёт в очередь и по зову камеры сначала входит, потом выходит;
// после выхода либо встаёт в очередь к следующей камере, либо завершает проход
void ship_main(int i) {
    Ship *ship = &shared->ships[i];
    sem_t *go = &shared->go[i]; // зов камеры: первый раз войти, второй раз выйти

    // ждём время прибытия по модельным часам, просыпаясь всё ближе к нему
    while (shared->now < ship->arrive) msleep((long)(ship->arrive - shared->now) * shared->tick / 2 + 1);

    lock();
    out("[t=%4d] судно %d (размер %d, %s%s) прибыло на участок %d\n", shared->now, ship->id, ship->size,
        dname(ship->dir), ship->prio ? ", приоритетное" : "", ship->pos);
    ship->state = QUEUED, ship->queued_at = shared->now;
    out("[t=%4d] судно %d встало в очередь к камере %d\n", shared->now, ship->id, ship->chamber);
    unlock();

    sem_post(&shared->wake);

    while (1) {
        sem_wait_retry(go);

        lock();
        int chamber_id = ship->chamber; // камера, которая позвала
        Chamber *chamber = &shared->chambers[chamber_id];

        if (ship->state == ENTERING) {
            int waited = shared->now - ship->queued_at;

            shared->entries++, shared->wait_sum += waited;
            if (waited > shared->wait_max) shared->wait_max = waited;

            // проверка инвариантов входа: открыты ворота своей стороны, уровень совпадает, вместимость не превышена
            if (!chamber->gate[ship->dir] || chamber->water != ship->dir)
                violation("судно входит при несовпадающем уровне или закрытых воротах");
            if (chamber->load > shared->cap) violation("вместимость камеры превышена");

            ship->state = INSIDE;
            out("[t=%4d] судно %d (размер %d%s) вошло в камеру %d, занято %d/%d\n", shared->now, ship->id,
                ship->size, ship->prio ? ", приоритетное" : "", chamber_id, chamber->load, shared->cap);
            unlock();

            sem_post(&shared->ack[chamber_id]);
        } else { // EXITING
            if (!chamber->gate[1 - ship->dir] || chamber->water != 1 - ship->dir)
                violation("судно выходит при несовпадающем уровне или закрытых воротах");

            ship->pos = ship->dir == 0 ? chamber_id + 1 : chamber_id;
            out("[t=%4d] судно %d вышло из камеры %d на участок %d (уровень %d)\n", shared->now, ship->id,
                chamber_id, ship->pos, level(ship->pos));

            if (ship->pos == (ship->dir == 0 ? shared->N : 0)) {
                ship->state = DONE;
                out("[t=%4d] судно %d прошло всю систему\n", shared->now, ship->id);
                shared->passed++;
                unlock();

                sem_post(&shared->ack[chamber_id]);
                return;
            }

            ship->state = QUEUED;
            ship->queued_at = shared->now;
            ship->chamber = ship->dir == 0 ? ship->pos : ship->pos - 1;
            out("[t=%4d] судно %d встало в очередь к камере %d\n", shared->now, ship->id, ship->chamber);
            unlock();

            sem_post(&shared->wake);
            sem_post(&shared->ack[chamber_id]);
        }
    }
}
