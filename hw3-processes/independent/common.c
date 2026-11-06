#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <time.h>
#include <unistd.h>
#include <sys/mman.h>
#include "common.h"

Shared *shared;
sem_t *mutex;
int log_fd = -1;
void (*on_interrupt)(void);

static int is_creator; // этот процесс первым пришёл и создал память и семафоры
static int world_ready; // создатель закончил подготовку
static int is_joined; // участник занял номер и учтён в shared->alive
static int my_slot = -1;
static volatile sig_atomic_t got_sig; // номер сигнала завершения (SIGINT, SIGTERM, SIGHUP)
static volatile sig_atomic_t want_dump; // SIGUSR1: вывести состояние
static volatile sig_atomic_t want_toggle; // SIGUSR2: переключить режим «без ограничения по времени»
static sem_t *volatile waiting_on; // семафор, на котором участник сейчас ждёт

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

// аварийный выход: уже запущенный участник останавливает всю модель,
// а создатель, не успевший закончить подготовку, удаляет за собой всё созданное
static void abort_run(void) __attribute__((noreturn));
static void abort_run(void) {
    if (is_joined) {
        shared->bad_pid = getpid();
        request_stop(FAILURE);
        leave();
    }

    if (is_creator && !world_ready) unlink_all(MAX_CHAMBERS, MAX_SHIPS);
    _exit(1);
}

void die(const char *msg) {
    perror(msg);
    abort_run();
}

void fail(const char *msg) {
    fprintf(stderr, "%s\n", msg);
    abort_run();
}

// номер из аргумента командной строки (position-й аргумент после имени программы)
int arg_number(int argc, char **argv, int position, const char *what) {
    char *end;

    if (position >= argc) {
        fprintf(stderr, "Не указан аргумент: %s\n", what);
        _exit(1);
    }

    long value = strtol(argv[position], &end, 10);
    if (*end != '\0' || value < 0 || value > 100000) {
        fprintf(stderr, "Неверный аргумент (%s): %s\n", what, argv[position]);
        _exit(1);
    }

    return (int)value;
}

// sem_wait, который не прерывается сигналами
void sem_wait_retry(sem_t *sem) {
    while (sem_wait(sem) < 0 && errno == EINTR);
}

// ожидание семафора с проверкой остановки: сигнал или остановка модели прерывают ожидание,
// и участник завершается. Обработчик сигнала увеличивает waiting_on, поэтому сигнал,
// пришедший прямо перед sem_wait, не теряется.
void wait_sem(sem_t *sem) {
    waiting_on = sem;
    poll_signals();

    while (sem_wait(sem) < 0) {
        if (errno != EINTR) die("sem_wait");
        if (on_interrupt) on_interrupt();
        poll_signals();
    }

    waiting_on = NULL;
    poll_signals();
}

// мьютекс не должен зависать навсегда, если его держатель погиб: пока ждём, смотрим на остановку
void lock(void) {
    while (sem_trywait(mutex) < 0) {
        if (errno != EAGAIN && errno != EINTR) die("sem_trywait");
        poll_signals();
        msleep(1);
    }
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

// пауза на count тиков модельного времени короткими порциями: остановка модели прерывает её
void ticks(int count) {
    long left = (long)count * shared->tick;

    while (left > 0) {
        long part = left < 50 ? left : 50;

        msleep(part);
        left -= part;
        poll_signals();
    }
}

// единые часы без центрального процесса: время считается от общего момента старта
long now_ms(void) {
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

int time_now(void) {
    if (shared->start_ms == 0) return 0;

    long elapsed = now_ms() - shared->start_ms;
    return elapsed < 0 ? 0 : (int)(elapsed / shared->tick);
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

// то же, но без ошибки, если семафора уже нет (при остановке модели)
static void post_if_exists(const char *fmt, int first, int second) {
    char name[32];

    snprintf(name, sizeof(name), fmt, first, second);
    sem_t *sem = sem_open(name, 0);
    if (sem == SEM_FAILED) return;

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
    unlink_named(SEM_INIT, 0, 0);
    unlink_named(SEM_BAR, 0, 0);
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

// разбудить всех: каждый участник ждёт на каком-то из этих семафоров,
// после пробуждения он увидит shared->stop и завершится
static void wake_everyone(void) {
    post_if_exists(SEM_BAR, 0, 0);
    post_if_exists(SEM_WAKE, 0, 0);

    for (int c = 0; c < shared->N; c++) {
        post_if_exists(SEM_START, c, 0);
        post_if_exists(SEM_ACK, c, 0);
        post_if_exists(SEM_TOKEN, c, 0);
        for (int side = 0; side < 2; side++) {
            post_if_exists(SEM_CMD, c, side);
            post_if_exists(SEM_DONE, c, side);
        }
    }

    for (int i = 0; i < shared->n_ships; i++) post_if_exists(SEM_GO, i, 0);
}

// остановить модель: первая причина остаётся, остальные игнорируются
void request_stop(int reason) {
    if (__sync_bool_compare_and_swap(&shared->stop, RUNNING, reason)) {
        if (reason == INTERRUPT) shared->stop_signal = got_sig;
        if (shared->end_time < 0) shared->end_time = time_now();
    }

    wake_everyone();
}

// обработчики только выставляют флаги; сигнал завершения ещё и будит ожидающий семафор
static void on_signal(int sig) {
    if (sig == SIGUSR1) want_dump = 1;
    else if (sig == SIGUSR2) want_toggle = 1;
    else if (sig != SIGALRM) {
        got_sig = sig;
        if (waiting_on) sem_post(waiting_on);
    }
}

// состояние по SIGUSR1 без остановки модели
static void dump(void) {
    int got = lock_try(1000);

    out("--- состояние по SIGUSR1, t=%d, прошло %d из %d ---\n", time_now(), shared->passed, shared->n_ships);
    print_chambers();
    print_queues();
    if (got) unlock();
}

// SIGUSR2: включить или выключить ограничение времени
static void toggle_limit(void) {
    int got = lock_try(1000);

    shared->end_at = shared->end_at ? 0 : time_now() + (shared->limit ? shared->limit : 1000);
    if (shared->end_at) out("[t=%4d] время моделирования ограничено, остановка при t=%d\n", time_now(), shared->end_at);
    else out("[t=%4d] время моделирования не ограничено\n", time_now());
    if (got) unlock();
}

// вызывается участниками при каждом ожидании и паузе: реакция на сигналы и на остановку модели
void poll_signals(void) {
    if (!shared) return;

    if (got_sig) request_stop(INTERRUPT);
    if (shared->stop) leave();

    if (want_dump) {
        want_dump = 0;
        dump();
    }

    if (want_toggle) {
        want_toggle = 0;
        toggle_limit();
    }
}

// читает до 12 чисел параметров из файла в params, недостающие остаются значениями по умолчанию
static void read_params(const char *path, int *params) {
    char buf[512] = {0};
    char *cursor = buf, *end;
    int fd = open(path, O_RDONLY);

    if (fd < 0) die(path);
    if (read(fd, buf, sizeof(buf) - 1) < 0) die(path);
    close(fd);

    for (int i = 0; i < 12; i++) {
        long value = strtol(cursor, &end, 10);
        if (end == cursor) break;
        params[i] = value, cursor = end;
    }
}

// подготовка модели создателем: параметры, судна, разделяемая память и все семафоры
static void create_world(const char *params_path) {
    // значения по умолчанию в порядке чисел в файле параметров
    int params[] = {3, 5, 10, 3, 2, 4, 0, 15, 500, 1, 2, 500};

    if (params_path && strcmp(params_path, "-") != 0) read_params(params_path, params);

    if (params[0] < 1 || params[0] > MAX_CHAMBERS || params[1] < 1 || params[2] < 1 || params[2] > MAX_SHIPS ||
        params[3] < 0 || params[4] < 0 || params[5] < 0 || params[7] < 0 || params[8] < 0 || params[10] < 0 ||
        params[11] < 1)
        fail("Неверные параметры: камер 1..100, судов 1..1000, вместимость и тик не меньше 1");

    // разделяемая память: ftruncate вызывается один раз
    shm_unlink(SHM_NAME);
    int fd = shm_open(SHM_NAME, O_CREAT | O_EXCL | O_RDWR, 0600);
    if (fd < 0) die("shm_open");
    if (ftruncate(fd, sizeof(Shared)) < 0) die("ftruncate");
    shared = mmap(0, sizeof(Shared), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (shared == MAP_FAILED) die("mmap");
    close(fd);

    shared->N = params[0], shared->cap = params[1], shared->n_ships = params[2], shared->max_int = params[3];
    shared->gate_time = params[4], shared->level_time = params[5], shared->strategy = params[6];
    shared->max_wait = params[7], shared->limit = params[8], shared->seed = params[9], shared->step = params[10];
    shared->tick = params[11];
    shared->end_at = shared->limit, shared->end_time = -1;
    int N = shared->N, n_ships = shared->n_ships;
    shared->total = 1 + 3 * N + n_ships;

    out("Шлюзов: %d, вместимость: %d, судов: %d, стратегия: %d, макс. ожидание: %d\n", N, shared->cap, n_ships,
        shared->strategy, shared->max_wait);
    if (shared->limit) out("Время моделирования ограничено: %d\n", shared->limit);
    else out("Время моделирования не ограничено, остановка по Ctrl+C\n");
    for (int i = 0; i <= N; i++) out("участок %d: уровень %d\n", i, level(i));

    // суда: время прибытия, размер, направление и приоритет
    srand(shared->seed);
    int arrive_time = 0;
    for (int i = 0; i < n_ships; i++) {
        Ship *ship = &shared->ships[i];
        arrive_time += rand() % (shared->max_int + 1);
        ship->id = i + 1, ship->size = rand() % shared->cap + 1, ship->dir = rand() % 2, ship->prio = rand() % 5 == 0;
        ship->arrive = arrive_time;
        ship->pos = ship->dir == 0 ? 0 : N;
        ship->chamber = ship->dir == 0 ? 0 : N - 1;
    }
    for (int c = 0; c < N; c++) shared->chambers[c].last_dir = -1;

    // семафоры создаются и сразу закрываются: дальше каждый участник открывает нужные ему по имени
    sem_close(sem_make(SEM_BAR, 0, 0, 0));
    sem_close(sem_make(SEM_MX, 0, 0, 1));
    sem_close(sem_make(SEM_WAKE, 0, 0, 0));
    for (int c = 0; c < N; c++) {
        sem_close(sem_make(SEM_START, c, 0, 0));
        sem_close(sem_make(SEM_ACK, c, 0, 0));
        sem_close(sem_make(SEM_TOKEN, c, 0, 1));
        for (int side = 0; side < 2; side++) {
            sem_close(sem_make(SEM_CMD, c, side, 0));
            sem_close(sem_make(SEM_DONE, c, side, 0));
        }
    }
    for (int i = 0; i < n_ships; i++) sem_close(sem_make(SEM_GO, i, 0, 0));
}

// Присоединение к модели. Первый запущенный участник становится создателем:
// он подготавливает всё общее и открывает остальным вход через SEM_INIT.
// Остальные ждут на SEM_INIT, потом подключаются к готовой памяти.
void attach(const char *params_path, const char *log_path) {
    // закрытый канал вывода не должен убивать участника
    signal(SIGPIPE, SIG_IGN);

    // обработчики без SA_RESTART: ожидание прерывается и флаг виден сразу
    struct sigaction sa = {0};
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGHUP, &sa, NULL);
    sigaction(SIGUSR1, &sa, NULL);
    sigaction(SIGUSR2, &sa, NULL);
    sigaction(SIGALRM, &sa, NULL);

    // кто успел создать SEM_INIT, тот создатель
    sem_t *init = sem_open(SEM_INIT, O_CREAT | O_EXCL, 0600, 0);
    if (init != SEM_FAILED) {
        is_creator = 1;
    } else if (errno == EEXIST) {
        init = sem_open(SEM_INIT, 0);
        if (init == SEM_FAILED) die(SEM_INIT);

        // турникет: создатель увеличил семафор один раз, каждый прошедший пропускает следующего
        while (sem_wait(init) < 0) {
            if (errno != EINTR) die("sem_wait");
            if (got_sig) _exit(1);
        }
        sem_post(init);
        sem_close(init);
    } else {
        die(SEM_INIT);
    }

    // лог общий: создатель начинает его заново, остальные дописывают
    log_fd = open(log_path, O_WRONLY | O_CREAT | O_APPEND | (is_creator ? O_TRUNC : 0), 0644);
    if (log_fd < 0) die(log_path);

    if (is_creator) {
        create_world(params_path);
        world_ready = 1;
        sem_post(init);
        sem_close(init);
    } else {
        int fd = shm_open(SHM_NAME, O_RDWR, 0600);
        if (fd < 0) die("shm_open");
        shared = mmap(0, sizeof(Shared), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        if (shared == MAP_FAILED) die("mmap");
        close(fd);
    }

    mutex = sem_get(SEM_MX, 0, 0);
}

// занять номер участника и дождаться остальных: работа начинается одновременно
// who: 0 – диспетчер, 1 + c – камера c, 1 + N + 2c + side – ворота, 1 + 3N + i – судно i
void join(int who) {
    sem_t *barrier = sem_get(SEM_BAR, 0, 0);

    lock();
    if (who < 0 || who >= shared->total || shared->joined[who]) {
        unlock();
        fail("Такой участник уже запущен");
    }

    shared->joined[who] = 1, shared->pids[who] = getpid();
    shared->alive++, shared->registered++;
    if (shared->registered == shared->total) {
        shared->start_ms = now_ms(); // общий момент старта модельных часов
        sem_post(barrier);
    }
    unlock();
    my_slot = who, is_joined = 1;

    // турникет: последний пришедший открывает, каждый прошедший пропускает следующего
    wait_sem(barrier);
    sem_post(barrier);
    sem_close(barrier);
}

// итог, его печатает последний уходящий участник (с захваченным мьютексом)
static void final_report(void) {
    int reason = shared->stop, n_ships = shared->n_ships;
    int end_time = shared->end_time >= 0 ? shared->end_time : time_now();

    if (reason == ALL_DONE || (reason == RUNNING && shared->passed >= n_ships)) {
        out("Все %d судов прошли систему за %d\n", shared->passed, end_time);
    } else {
        if (reason == TIME_OUT) out("Время моделирования вышло.");
        else if (reason == INTERRUPT) out("Прервано пользователем (сигнал %d).", shared->stop_signal);
        else out("Аварийное завершение участника (pid %d).", (int)shared->bad_pid);

        out(" Прошло %d из %d. Очереди:\n", shared->passed, n_ships);
        print_chambers();
        print_queues();
    }

    out("Статистика: время %d, прошло %d из %d, групп %d, смен уровня %d, среднее ожидание %d, максимальное %d, нарушений инвариантов %d\n",
        end_time, shared->passed, n_ships, shared->groups, shared->level_changes,
        shared->entries ? shared->wait_sum / shared->entries : 0, shared->wait_max, shared->violations);
}

// выход участника; последний ушедший печатает итог и удаляет семафоры и память
void leave(void) {
    if (!is_joined) _exit(1);
    is_joined = 0;

    int got = lock_try(500); // погибший держатель мьютекса не должен задержать выход
    int code = shared->stop == FAILURE;

    shared->pids[my_slot] = 0;
    int last = --shared->alive == 0;
    if (last) final_report();
    if (got) unlock();

    if (last) unlink_all(shared->N, shared->n_ships);
    close(log_fd);
    _exit(code);
}

// уровень воды на участке канала
int level(int section) {
    return (shared->N - section) * shared->step;
}

const char *dname(int dir) {
    return dir == 0 ? "вперёд" : "назад";
}

void say(int chamber_id, const char *text) {
    out("[t=%4d] камера %d: %s\n", time_now(), chamber_id, text);
}

// нарушение инварианта модели; в нормальной работе не должно появляться
void violation(const char *what) {
    shared->violations++;
    out("[t=%4d] НАРУШЕНИЕ ИНВАРИАНТА: %s\n", time_now(), what);
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
    int now = time_now();
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

// все камеры закончили цикл
int all_idle(void) {
    for (int c = 0; c < shared->N; c++)
        if (shared->chambers[c].busy) return 0;

    return 1;
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
