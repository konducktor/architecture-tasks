#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <errno.h>
#include <fcntl.h>
#include <time.h>
#include <unistd.h>
#include <sys/mman.h>
#include "common.h"

Shared *shared;
sem_t *mutex;
int log_fd = -1;
pid_t owner; // только супервизор удаляет объекты при аварии
int clean_chambers = MAX_CHAMBERS, clean_ships = MAX_SHIPS; // по умолчанию удаляем всё, что могло быть создано

// вывод одновременно на экран и в лог; вызывать с захваченным мьютексом,
// иначе строки разных процессов перемешаются
void out(const char *fmt, ...) {
    va_list ap;

    va_start(ap, fmt);
    vdprintf(STDOUT_FILENO, fmt, ap);
    va_end(ap);

    if (log_fd >= 0) {
        va_start(ap, fmt);
        vdprintf(log_fd, fmt, ap);
        va_end(ap);
    }
}

// ошибка системного вызова: участник завершается с кодом 1 (супервизор это заметит),
// а супервизор перед этим удаляет за собой семафоры и память
void die(const char *msg) {
    perror(msg);
    if (getpid() == owner) unlink_all(clean_chambers, clean_ships);
    _exit(1);
}

// sem_wait, который не прерывается сигналами
void sem_wait_retry(sem_t *sem) {
    while (sem_wait(sem) < 0 && errno == EINTR);
}

void lock(void) {
    sem_wait_retry(mutex);
}

void unlock(void) {
    sem_post(mutex);
}

// захват мьютекса не дольше ms миллисекунд: 1 = захвачен
int lock_try(int ms) {
    for (int i = 0; i <= ms; i++) {
        if (sem_trywait(mutex) == 0) return 1;
        msleep(1);
    }

    return 0;
}

// пауза на ms миллисекунд, сигналы её не сокращают
void msleep(long ms) {
    struct timespec ts = {ms / 1000, (ms % 1000) * 1000000L};

    while (nanosleep(&ts, &ts) < 0 && errno == EINTR);
}

// пауза на count тиков модельного времени
void ticks(int count) {
    msleep((long)count * shared->tick);
}

// открыть существующий именованный семафор; имя собирается из формата и номеров
sem_t *sem_get(const char *fmt, int first, int second) {
    char name[32];

    snprintf(name, sizeof(name), fmt, first, second);
    sem_t *sem = sem_open(name, 0);
    if (sem == SEM_FAILED) die(name);

    return sem;
}

// создать семафор с начальным значением; остатки прошлого запуска удаляются
sem_t *sem_make(const char *fmt, int first, int second, int value) {
    char name[32];

    snprintf(name, sizeof(name), fmt, first, second);
    sem_unlink(name);
    sem_t *sem = sem_open(name, O_CREAT | O_EXCL, 0600, (unsigned)value);
    if (sem == SEM_FAILED) die(name);

    return sem;
}

// разбудить чужой семафор: открыть, увеличить и сразу закрыть
void post_named(const char *fmt, int first, int second) {
    sem_t *sem = sem_get(fmt, first, second);

    sem_post(sem);
    sem_close(sem);
}

static void unlink_named(const char *fmt, int first, int second) {
    char name[32];

    snprintf(name, sizeof(name), fmt, first, second);
    sem_unlink(name);
}

// удалить все семафоры и разделяемую память
void unlink_all(int nch, int nships) {
    unlink_named(SEM_MX, 0, 0);
    unlink_named(SEM_WAKE, 0, 0);

    for (int c = 0; c < nch; c++) {
        unlink_named(SEM_START, c, 0);
        unlink_named(SEM_ACK, c, 0);
        unlink_named(SEM_TOKEN, c, 0);
        for (int side = 0; side < 2; side++) {
            unlink_named(SEM_CMD, c, side);
            unlink_named(SEM_DONE, c, side);
        }
    }

    for (int i = 0; i < nships; i++) unlink_named(SEM_GO, i, 0);

    shm_unlink(SHM_NAME);
}

// уровень воды на участке канала
int level(int section) {
    return (shared->N - section) * shared->step;
}

const char *dname(int dir) {
    return dir == 0 ? "вперёд" : "назад";
}

void say(int chamber_id, const char *text) {
    out("[t=%4d] камера %d: %s\n", shared->now, chamber_id, text);
}

// нарушение инварианта модели; в нормальной работе не должно появляться
void violation(const char *what) {
    shared->violations++;
    out("[t=%4d] НАРУШЕНИЕ ИНВАРИАНТА: %s\n", shared->now, what);
}

// лучшее ожидающее судно со стороны dir, которое помещается в room (с захваченным мьютексом)
int pick(int chamber_id, int dir, int room) {
    Ship *ships = shared->ships;
    int best = -1;

    for (int i = 0; i < shared->n_ships; i++) {
        Ship *ship = &ships[i];
        if (ship->state != QUEUED || ship->chamber != chamber_id || ship->dir != dir || ship->size > room) continue;
        if (best < 0 || ship->prio > ships[best].prio ||
            (ship->prio == ships[best].prio && ship->queued_at < ships[best].queued_at))
            best = i;
    }

    return best;
}

// сколько судов стоит в очереди к камере со стороны dir
int queue_len(int chamber_id, int dir) {
    int count = 0;

    for (int i = 0; i < shared->n_ships; i++) {
        Ship *ship = &shared->ships[i];
        if (ship->state == QUEUED && ship->chamber == chamber_id && ship->dir == dir) count++;
    }

    return count;
}

// выбор стороны, с которой камера пропустит следующую группу (с захваченным мьютексом)
int choose(int chamber_id) {
    Ship *ships = shared->ships;
    int now = shared->now;
    int forward = pick(chamber_id, 0, 1 << 30), backward = pick(chamber_id, 1, 1 << 30); // лучшие суда сторон

    if (forward < 0 && backward < 0) return -1;
    if (backward < 0) return 0;
    if (forward < 0) return 1;

    int wait_forward = now - ships[forward].queued_at, wait_backward = now - ships[backward].queued_at;

    if (wait_forward > shared->max_wait && wait_forward >= wait_backward) return 0; // защита от бесконечного ожидания
    if (wait_backward > shared->max_wait) return 1;
    if (ships[forward].prio != ships[backward].prio) return ships[backward].prio > ships[forward].prio;
    if (shared->strategy == 1) return shared->chambers[chamber_id].water; // без смены уровня

    return wait_backward > wait_forward; // самое долгое ожидание
}

// состояние всех камер
void print_chambers(void) {
    for (int c = 0; c < shared->N; c++) {
        Chamber *chamber = &shared->chambers[c];

        out("камера %d: %s, направление %s, вода на уровне %d, занято %d/%d, ворота: верхние %s, нижние %s\n",
            c, chamber->busy ? "работает" : "свободна", chamber->busy ? dname(chamber->dir) : "-",
            level(c + chamber->water), chamber->load, shared->cap, chamber->gate[0] ? "открыты" : "закрыты",
            chamber->gate[1] ? "открыты" : "закрыты");
    }
}

// очереди к камерам, суда внутри камер и число ещё не прибывших
void print_queues(void) {
    int waiting = 0;

    for (int c = 0; c < shared->N; c++) {
        for (int dir = 0; dir < 2; dir++) {
            out("камера %d, %s:", c, dname(dir));
            for (int i = 0; i < shared->n_ships; i++) {
                Ship *ship = &shared->ships[i];
                if (ship->state == QUEUED && ship->chamber == c && ship->dir == dir) out(" %d", ship->id);
            }
            out("\n");
        }

        out("внутри камеры %d:", c);
        for (int i = 0; i < shared->n_ships; i++) {
            Ship *ship = &shared->ships[i];
            if ((ship->state == ENTERING || ship->state == INSIDE || ship->state == EXITING) && ship->chamber == c)
                out(" %d", ship->id);
        }
        out("\n");
    }

    for (int i = 0; i < shared->n_ships; i++)
        if (shared->ships[i].state == NOT_ARRIVED) waiting++;
    out("ещё не прибыли: %d\n", waiting);
}
