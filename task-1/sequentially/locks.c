// Система шлюзов на канале: последовательно.
#include <stdio.h>
#include <stdlib.h>

#define MAX_SHIPS 1000
#define MAX_CHAMBERS 100

// состояния судна
enum { NOT_ARRIVED, QUEUED, INSIDE, DONE };

// состояния камеры
enum { IDLE, PREPARE, OPEN_IN, LOADING, CLOSE_IN, LEVELING, OPEN_OUT, UNLOADING, CLOSE_OUT };

typedef struct {
    int id, size, dir; // dir: 0 – вперёд (участок 0 -> N), 1 - назад
    int prio, arrive; // приоритет и время прибытия
    int state, pos, ch; // участок канала и камера, к которой судно стоит в очереди
    int queued_at;
} Ship;

typedef struct {
    int state, timer;
    int dir; // сторона входа текущей группы
    int water; // сторона, с уровнем которой совпадает вода в камере
    int load; // занятая вместимость
    int gate[2]; // 1 - ворота открыты
    int last_dir;
} Chamber;

Ship ships[MAX_SHIPS];
Chamber ch[MAX_CHAMBERS];
int N, cap, n_ships, max_int, gate_time, level_time, strategy, max_wait, limit, step;
int t, passed;

int level(int section) { 
    return (N - section) * step; 
}

void say(int c, const char *s) { 
    printf("[t=%4d] камера %d: %s\n", t, c, s); 
}

// лучшее ожидающее судно со стороны d, которое помещается в room
int pick(int c, int d, int room) {
    int best = -1;
    
    for (int i = 0; i < n_ships; i++) {
        Ship *s = &ships[i];
        if (s->state != QUEUED || s->ch != c || s->dir != d || s->size > room) continue;
        if (best < 0 || s->prio > ships[best].prio ||
            (s->prio == ships[best].prio && s->queued_at < ships[best].queued_at))
            best = i;
    }

    return best;
}

// диспетчер: выбор стороны, с которой камера пропустит следующую группу
int choose(int c) {
    int a = pick(c, 0, 1 << 30), b = pick(c, 1, 1 << 30);
    if (a < 0 && b < 0) return -1;
    if (b < 0) return 0;
    if (a < 0) return 1;

    int wa = t - ships[a].queued_at, wb = t - ships[b].queued_at;

    if (wa > max_wait && wa >= wb) return 0; // защита от бесконечного ожидания
    if (wb > max_wait) return 1;
    if (ships[a].prio != ships[b].prio) return ships[b].prio > ships[a].prio;
    if (strategy == 1) return ch[c].water; // без смены уровня

    return wb > wa; // самое долгое ожидание
}

void ship_exit(Ship *s, int c) {
    s->pos = s->dir == 0 ? c + 1 : c;
    printf("[t=%4d] судно %d вышло из камеры %d на участок %d (уровень %d)\n", t, s->id, c, s->pos, level(s->pos));

    if (s->pos == (s->dir == 0 ? N : 0)) {
        s->state = DONE;
        passed++;
        printf("[t=%4d] судно %d прошло всю систему\n", t, s->id);
    } else {
        s->state = QUEUED;
        s->queued_at = t;
        s->ch = s->dir == 0 ? s->pos : s->pos - 1;
        printf("[t=%4d] судно %d встало в очередь к камере %d\n", t, s->id, s->ch);
    }
}

void step_chamber(int c) {
    Chamber *k = &ch[c];
    char buf[100];
    int i;

    switch (k->state) {
    case IDLE:
        k->dir = choose(c);
        if (k->dir < 0) break;
        if (k->dir != k->last_dir) {
            printf("[t=%4d] камера %d: направление работы -> %s\n", t, c, k->dir == 0 ? "вперёд" : "назад");
            k->last_dir = k->dir;
        }
        if (k->water != k->dir) {
            say(c, "подготовка: уровень воды меняется под сторону входа");
            k->state = PREPARE, k->timer = level_time;
        } else {
            k->state = OPEN_IN, k->timer = gate_time;
        }
        break;
    
    case PREPARE:
        if (--k->timer > 0) break;
        k->water = k->dir;
        sprintf(buf, "уровень воды изменён до %d", level(c + k->water));
        say(c, buf);
        k->state = OPEN_IN, k->timer = gate_time;
        break;
    
    case OPEN_IN:
        if (--k->timer > 0) break;
        k->gate[k->dir] = 1;
        sprintf(buf, "открыты ворота %s", k->dir == 0 ? "нижние" : "верхние");
        say(c, buf);
        k->state = LOADING;
        break;
    
    case LOADING:
        i = pick(c, k->dir, cap - k->load);
        if (i >= 0) {
            ships[i].state = INSIDE;
            k->load += ships[i].size;
            printf("[t=%4d] судно %d (размер %d%s) вошло в камеру %d, занято %d/%d\n", t, ships[i].id,
                   ships[i].size, ships[i].prio ? ", приоритетное" : "", c, k->load, cap);
        } else {
            k->state = CLOSE_IN, k->timer = gate_time;
        }
        break;
    
    case CLOSE_IN:
        if (--k->timer > 0) break;
        k->gate[k->dir] = 0;
        say(c, "ворота входа закрыты");
        k->state = LEVELING, k->timer = level_time;
        break;
    
    case LEVELING:
        if (--k->timer > 0) break;
        k->water = 1 - k->dir;
        sprintf(buf, "уровень воды изменён до %d", level(c + k->water));
        say(c, buf);
        k->state = OPEN_OUT, k->timer = gate_time;
        break;
    
    case OPEN_OUT:
        if (--k->timer > 0) break;
        k->gate[1 - k->dir] = 1;
        say(c, "открыты ворота выхода");
        k->state = UNLOADING;
        break;
    
    case UNLOADING:
        for (i = 0; i < n_ships; i++)
            if (ships[i].state == INSIDE && ships[i].ch == c) break;
        if (i < n_ships) {
            k->load -= ships[i].size;
            ship_exit(&ships[i], c);
        } else {
            k->state = CLOSE_OUT, k->timer = gate_time;
        }
        break;
    
    case CLOSE_OUT:
        if (--k->timer > 0) break;
        k->gate[1 - k->dir] = 0;
        say(c, "ворота выхода закрыты");
        k->state = IDLE;
        break;
    }
}

int main(int argc, char **argv) {
    // значения по умолчанию в порядке аргументов
    int a[] = {3, 5, 10, 3, 2, 4, 0, 15, 500, 1, 2};
    for (int i = 1; i < argc && i <= 11; i++) a[i - 1] = atoi(argv[i]);
    N = a[0], cap = a[1], n_ships = a[2], max_int = a[3], gate_time = a[4], level_time = a[5];
    strategy = a[6], max_wait = a[7], limit = a[8], step = a[10];
    srand(a[9]);

    printf("Шлюзов: %d, вместимость: %d, судов: %d, стратегия: %d, макс. ожидание: %d\n", N, cap, n_ships, strategy, max_wait);
    for (int i = 0; i <= N; i++) printf("участок %d: уровень %d\n", i, level(i));

    int time = 0;
    for (int i = 0; i < n_ships; i++) {
        Ship *s = &ships[i];
        time += rand() % (max_int + 1);
        s->id = i + 1, s->size = rand() % cap + 1, s->dir = rand() % 2, s->prio = rand() % 5 == 0;
        s->arrive = time;
        s->pos = s->dir == 0 ? 0 : N;
        s->ch = s->dir == 0 ? 0 : N - 1;
    }

    for (int c = 0; c < N; c++) ch[c].last_dir = -1;

    for (t = 0; t < limit && passed < n_ships; t++) {
        for (int i = 0; i < n_ships; i++) {
            Ship *s = &ships[i];
            if (s->arrive != t) continue;
            printf("[t=%4d] судно %d (размер %d, %s%s) прибыло на участок %d\n", t, s->id, s->size,
                   s->dir == 0 ? "вперёд" : "назад", s->prio ? ", приоритетное" : "", s->pos);
            s->state = QUEUED, s->queued_at = t;
            printf("[t=%4d] судно %d встало в очередь к камере %d\n", t, s->id, s->ch);
        }

        for (int c = 0; c < N; c++) step_chamber(c);
    }

    if (passed == n_ships) {
        printf("Все %d судов прошли систему за %d\n", passed, t);
    } else {
        printf("Время моделирования вышло. Прошло %d из %d. Очереди:\n", passed, n_ships);

        for (int c = 0; c < N; c++)
            for (int d = 0; d < 2; d++) {
                printf("камера %d, %s:", c, d == 0 ? "вперёд" : "назад");
                for (int i = 0; i < n_ships; i++)
                    if (ships[i].state == QUEUED && ships[i].ch == c && ships[i].dir == d) printf(" %d", ships[i].id);
                printf("\n");
            }
    }
    return 0;
}
