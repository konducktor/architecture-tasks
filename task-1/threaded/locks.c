// Система шлюзов на канале: многопоточно
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <fcntl.h>
#include <unistd.h>
#include <pthread.h>
#include <semaphore.h>

#define MAX_SHIPS 1000
#define MAX_CHAMBERS 100

// состояния судна
enum { NOT_ARRIVED, QUEUED, INSIDE, DONE };

typedef struct {
    int id, size, dir; // dir: 0 = вперёд (участок 0 -> N), 1 = назад
    int prio, arrive; // приоритет и время прибытия
    int state, pos, ch; // участок канала и камера, к которой судно стоит в очереди
    int queued_at;
    sem_t go; // камера отпускает судно: после входа и после выхода
} Ship;

typedef struct {
    int dir; // сторона входа текущей группы
    int water; // сторона, с уровнем которой совпадает вода в камере
    int load; // занятая вместимость
    int gate[2]; // 1 = ворота открыты
    int last_dir;
} Chamber;

Ship ships[MAX_SHIPS];
Chamber ch[MAX_CHAMBERS];
sem_t wake[MAX_CHAMBERS]; // судно разбудило камеру: в очереди появился новый корабль
pthread_mutex_t m = PTHREAD_MUTEX_INITIALIZER; // защищает всё общее состояние и вывод
int N, cap, n_ships, max_int, gate_time, level_time, strategy, max_wait, limit, step, tick;
int t, passed;
int log_fd; // дескриптор файла лога
int groups, level_changes, entries, wait_sum, wait_max; // статистика

// вывод одновременно на экран и в лог
void out(const char *fmt, ...) {
    va_list ap;

    va_start(ap, fmt);
    vdprintf(STDOUT_FILENO, fmt, ap);
    va_end(ap);

    va_start(ap, fmt);
    vdprintf(log_fd, fmt, ap);
    va_end(ap);
}

int level(int section) {
    return (N - section) * step;
}

void say(int c, const char *s) {
    out("[t=%4d] камера %d: %s\n", t, c, s);
}

// пауза на n тиков, мьютекс на это время отпускается
void wait_ticks(int n) {
    pthread_mutex_unlock(&m);
    usleep(n * tick * 1000);
    pthread_mutex_lock(&m);
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

// вызывается с захваченным мьютексом
void ship_exit(Ship *s, int c) {
    s->pos = s->dir == 0 ? c + 1 : c;
    out("[t=%4d] судно %d вышло из камеры %d на участок %d (уровень %d)\n", t, s->id, c, s->pos, level(s->pos));

    if (s->pos == (s->dir == 0 ? N : 0)) {
        s->state = DONE;
        passed++;
        out("[t=%4d] судно %d прошло всю систему\n", t, s->id);
    } else {
        s->state = QUEUED;
        s->queued_at = t;
        s->ch = s->dir == 0 ? s->pos : s->pos - 1;
        out("[t=%4d] судно %d встало в очередь к камере %d\n", t, s->id, s->ch);
        sem_post(&wake[s->ch]);
    }

    sem_post(&s->go);
}

// поток судна: приходит, встаёт в очередь и ждёт, пока камеры его проведут
void *ship_thread(void *arg) {
    Ship *s = arg;

    usleep(s->arrive * tick * 1000);

    pthread_mutex_lock(&m);
    out("[t=%4d] судно %d (размер %d, %s%s) прибыло на участок %d\n", t, s->id, s->size,
        s->dir == 0 ? "вперёд" : "назад", s->prio ? ", приоритетное" : "", s->pos);
    s->state = QUEUED, s->queued_at = t;
    out("[t=%4d] судно %d встало в очередь к камере %d\n", t, s->id, s->ch);
    sem_post(&wake[s->ch]);
    pthread_mutex_unlock(&m);

    while (1) {
        sem_wait(&s->go); // вошло в камеру
        sem_wait(&s->go); // вышло из камеры

        pthread_mutex_lock(&m);
        int done = s->state == DONE;
        pthread_mutex_unlock(&m);

        if (done) break;
    }

    return NULL;
}

// поток камеры (она же ворота): один цикл пропуска группы за итерацию
void *chamber_thread(void *arg) {
    int c = (long)arg;
    Chamber *k = &ch[c];
    char buf[100];
    int i;

    pthread_mutex_lock(&m);

    while (1) {
        k->dir = choose(c);

        // очередей нет – спим, пока судно не встанет в очередь
        if (k->dir < 0) {
            pthread_mutex_unlock(&m);
            sem_wait(&wake[c]);
            pthread_mutex_lock(&m);
            continue;
        }

        groups++;
        if (k->dir != k->last_dir) {
            out("[t=%4d] камера %d: направление работы -> %s\n", t, c, k->dir == 0 ? "вперёд" : "назад");
            k->last_dir = k->dir;
        }

        if (k->water != k->dir) {
            say(c, "подготовка: уровень воды меняется под сторону входа");
            wait_ticks(level_time);
            k->water = k->dir;
            level_changes++;
            sprintf(buf, "уровень воды изменён до %d", level(c + k->water));
            say(c, buf);
        }

        wait_ticks(gate_time);
        k->gate[k->dir] = 1;
        sprintf(buf, "открыты ворота %s", k->dir == 0 ? "нижние" : "верхние");
        say(c, buf);

        // загрузка: одно судно за тик
        while ((i = pick(c, k->dir, cap - k->load)) >= 0) {
            int w = t - ships[i].queued_at;

            entries++, wait_sum += w;
            if (w > wait_max) wait_max = w;

            ships[i].state = INSIDE;
            k->load += ships[i].size;
            out("[t=%4d] судно %d (размер %d%s) вошло в камеру %d, занято %d/%d\n", t, ships[i].id,
                ships[i].size, ships[i].prio ? ", приоритетное" : "", c, k->load, cap);
            sem_post(&ships[i].go);
            wait_ticks(1);
        }

        wait_ticks(gate_time);
        k->gate[k->dir] = 0;
        say(c, "ворота входа закрыты");

        wait_ticks(level_time);
        k->water = 1 - k->dir;
        level_changes++;
        sprintf(buf, "уровень воды изменён до %d", level(c + k->water));
        say(c, buf);

        wait_ticks(gate_time);
        k->gate[1 - k->dir] = 1;
        say(c, "открыты ворота выхода");

        // выгрузка: одно судно за тик
        while (1) {
            for (i = 0; i < n_ships; i++)
                if (ships[i].state == INSIDE && ships[i].ch == c) break;
            if (i == n_ships) break;

            k->load -= ships[i].size;
            ship_exit(&ships[i], c);
            wait_ticks(1);
        }

        wait_ticks(gate_time);
        k->gate[1 - k->dir] = 0;
        say(c, "ворота выхода закрыты");
    }

    return NULL;
}

int main(int argc, char **argv) {
    // значения по умолчанию в порядке чисел в файле параметров
    int a[] = {3, 5, 10, 3, 2, 4, 0, 15, 500, 1, 2, 50};

    // параметры из файла, путь к которому передан первым аргументом (вторым – путь к логу)
    if (argc > 1) {
        char buf[256] = {0};
        char *p = buf;
        int fd = open(argv[1], O_RDONLY);

        read(fd, buf, sizeof(buf) - 1);
        close(fd);

        for (int i = 0; i < 12; i++) a[i] = strtol(p, &p, 10);
    }

    log_fd = open(argc > 2 ? argv[2] : "locks.log", O_WRONLY | O_CREAT | O_TRUNC, 0644);

    N = a[0], cap = a[1], n_ships = a[2], max_int = a[3], gate_time = a[4], level_time = a[5];
    strategy = a[6], max_wait = a[7], limit = a[8], step = a[10], tick = a[11];
    srand(a[9]);

    out("Шлюзов: %d, вместимость: %d, судов: %d, стратегия: %d, макс. ожидание: %d\n", N, cap, n_ships, strategy, max_wait);
    for (int i = 0; i <= N; i++) out("участок %d: уровень %d\n", i, level(i));

    int time = 0;
    for (int i = 0; i < n_ships; i++) {
        Ship *s = &ships[i];
        time += rand() % (max_int + 1);
        s->id = i + 1, s->size = rand() % cap + 1, s->dir = rand() % 2, s->prio = rand() % 5 == 0;
        s->arrive = time;
        s->pos = s->dir == 0 ? 0 : N;
        s->ch = s->dir == 0 ? 0 : N - 1;
        sem_init(&s->go, 0, 0);
    }

    for (int c = 0; c < N; c++) {
        ch[c].last_dir = -1;
        sem_init(&wake[c], 0, 0);
    }

    // запуск камер и судов
    pthread_t th;

    for (long c = 0; c < N; c++) pthread_create(&th, NULL, chamber_thread, (void *)c);
    for (int i = 0; i < n_ships; i++) pthread_create(&th, NULL, ship_thread, &ships[i]);

    // главный поток – часы моделирования
    pthread_mutex_lock(&m);
    while (t < limit && passed < n_ships) {
        pthread_mutex_unlock(&m);
        usleep(tick * 1000);
        pthread_mutex_lock(&m);
        t++;
    }

    // мьютекс остаётся захваченным: остальные потоки замирают и завершаются вместе с процессом
    if (passed == n_ships) {
        out("Все %d судов прошли систему за %d\n", passed, t);
    } else {
        out("Время моделирования вышло. Прошло %d из %d. Очереди:\n", passed, n_ships);

        for (int c = 0; c < N; c++)
            for (int d = 0; d < 2; d++) {
                out("камера %d, %s:", c, d == 0 ? "вперёд" : "назад");
                for (int i = 0; i < n_ships; i++)
                    if (ships[i].state == QUEUED && ships[i].ch == c && ships[i].dir == d) out(" %d", ships[i].id);
                out("\n");
            }
    }

    out("Статистика: время %d, прошло %d из %d, групп %d, смен уровня %d, среднее ожидание %d, максимальное %d\n",
        t, passed, n_ships, groups, level_changes, entries ? wait_sum / entries : 0, wait_max);
    close(log_fd);

    return 0;
}
