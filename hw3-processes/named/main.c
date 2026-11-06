// Супервизор
// Создаёт общую память и семафоры, порождает всех участников, ведёт модельные часы,
// обрабатывает сигналы и в конце останавливает участников и удаляет всё созданное.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include "common.h"
#ifdef __linux__
#include <sys/prctl.h>
#endif

// роли порождаемых процессов
enum { R_DISPATCHER, R_CHAMBER, R_GATE, R_SHIP };

// причины завершения
enum { ALL_DONE, TIME_OUT, INTERRUPT, FAILURE };

pid_t pids[1 + 3 * MAX_CHAMBERS + MAX_SHIPS]; // pid порождённых участников (0 = уже завершился и подобран)
int n_pids;
volatile sig_atomic_t got_sig; // номер сигнала завершения (SIGINT, SIGTERM, SIGHUP)
volatile sig_atomic_t want_dump; // SIGUSR1: вывести состояние
volatile sig_atomic_t want_toggle; // SIGUSR2: переключить режим «без ограничения по времени»
sigset_t old_mask; // маска сигналов до запуска участников

// обработчики только выставляют флаги, всю работу делает основной цикл
void on_signal(int sig) {
    if (sig == SIGUSR1) want_dump = 1;
    else if (sig == SIGUSR2) want_toggle = 1;
    else got_sig = sig;
}

// читает до 12 чисел параметров из файла в params, недостающие остаются значениями по умолчанию
void read_params(const char *path, int *params) {
    char buf[512] = {0};
    char *cursor = buf, *end;
    int fd = open(path, O_RDONLY);

    if (fd < 0) die(path);
    read(fd, buf, sizeof(buf) - 1);
    close(fd);

    for (int i = 0; i < 12; i++) {
        long value = strtol(cursor, &end, 10);
        if (end == cursor) break;
        params[i] = value, cursor = end;
    }
}

// порождает участника, который выполняет свою роль и завершается;
// id – номер камеры или судна, side – сторона ворот
pid_t spawn(int role, int id, int side) {
    pid_t pid = fork();
    if (pid != 0) return pid;

    // ребёнок: Ctrl+C и служебные сигналы адресованы супервизору, участника останавливает только SIGTERM
    signal(SIGINT, SIG_IGN);
    signal(SIGUSR1, SIG_IGN);
    signal(SIGUSR2, SIG_IGN);
    signal(SIGTERM, SIG_DFL);
    signal(SIGHUP, SIG_DFL);
    sigprocmask(SIG_SETMASK, &old_mask, NULL);

#ifdef __linux__
    prctl(PR_SET_PDEATHSIG, SIGTERM); // супервизор убит через kill -9 – участники не остаются сиротами
    if (getppid() != owner) _exit(1);
#endif

    mutex = sem_get(SEM_MX, 0, 0); // у каждого процесса свой дескриптор мьютекса

    switch (role) {
        case R_DISPATCHER: dispatcher_main(); break;
        case R_CHAMBER: chamber_main(id); break;
        case R_GATE: gate_main(id, side); break;
        case R_SHIP: ship_main(id); break;
    }

    _exit(0);
}

int start(int role, int id, int side) {
    pid_t pid = spawn(role, id, side);

    if (pid < 0) {
        perror("fork");
        return -1;
    }

    pids[n_pids++] = pid;
    return 0;
}

// подбирает завершившиеся процессы; 1 = кто-то завершился аварийно (суда завершаются штатно с кодом 0)
int reap(pid_t *bad) {
    int status, failed = 0;
    pid_t child;

    while ((child = waitpid(-1, &status, WNOHANG)) > 0) {
        for (int k = 0; k < n_pids; k++)
            if (pids[k] == child) pids[k] = 0;

        if (!(WIFEXITED(status) && WEXITSTATUS(status) == 0)) *bad = child, failed = 1;
    }

    return failed;
}

// все камеры закончили цикл
int all_idle(void) {
    for (int c = 0; c < shared->N; c++)
        if (shared->chambers[c].busy) return 0;

    return 1;
}

// состояние по SIGUSR1 без остановки модели
void dump(void) {
    int got = lock_try(1000);

    out("--- состояние по SIGUSR1, t=%d, прошло %d из %d ---\n", shared->now, shared->passed, shared->n_ships);
    print_chambers();
    print_queues();
    if (got) unlock();
}

int main(int argc, char **argv) {
    // служебный режим: удалить остатки аварийно завершённого запуска (kill -9)
    if (argc > 1 && strcmp(argv[1], "-c") == 0) {
        unlink_all(MAX_CHAMBERS, MAX_SHIPS);
        out("Семафоры и разделяемая память удалены\n");
        return 0;
    }

    // значения по умолчанию в порядке чисел в файле параметров
    int params[] = {3, 5, 10, 3, 2, 4, 0, 15, 500, 1, 2, 500};

    // параметры из файла, путь к которому передан первым аргументом (вторым - путь к логу)
    if (argc > 1) read_params(argv[1], params);

    if (params[0] < 1 || params[0] > MAX_CHAMBERS || params[1] < 1 || params[2] < 1 || params[2] > MAX_SHIPS ||
        params[3] < 0 || params[4] < 0 || params[5] < 0 || params[7] < 0 || params[8] < 0 || params[10] < 0 ||
        params[11] < 1) {
        fprintf(stderr, "Неверные параметры: камер 1..%d, судов 1..%d, вместимость и тик не меньше 1\n", MAX_CHAMBERS,
                MAX_SHIPS);
        return 1;
    }

    log_fd = open(argc > 2 ? argv[2] : "locks.log", O_WRONLY | O_CREAT | O_TRUNC | O_APPEND, 0644);
    if (log_fd < 0) die("лог");

    // с этого момента супервизор отвечает за удаление созданных объектов
    owner = getpid();
    clean_chambers = params[0], clean_ships = params[2];

    // закрытый канал вывода (например, | head) не должен убивать супервизора до очистки
    signal(SIGPIPE, SIG_IGN);

    // обработчики сигналов без SA_RESTART: пауза прерывается и флаг виден сразу
    struct sigaction sa = {0};
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGHUP, &sa, NULL);
    sigaction(SIGUSR1, &sa, NULL);
    sigaction(SIGUSR2, &sa, NULL);

    // разделяемая память: ftruncate вызывается один раз
    shm_unlink(SHM_NAME);
    int fd = shm_open(SHM_NAME, O_CREAT | O_EXCL | O_RDWR, 0600);
    if (fd < 0) die("shm_open");
    if (ftruncate(fd, sizeof(Shared)) < 0) die("ftruncate");
    shared = mmap(0, sizeof(Shared), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (shared == MAP_FAILED) die("mmap");
    close(fd);

    shared->N = params[0], shared->cap = params[1], shared->n_ships = params[2], shared->max_int = params[3], shared->gate_time = params[4];
    shared->level_time = params[5], shared->strategy = params[6], shared->max_wait = params[7], shared->limit = params[8], shared->seed = params[9];
    shared->step = params[10], shared->tick = params[11];
    int N = shared->N, n_ships = shared->n_ships;

    out("Шлюзов: %d, вместимость: %d, судов: %d, стратегия: %d, макс. ожидание: %d\n", N, shared->cap, n_ships, shared->strategy,
        shared->max_wait);
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

    // запуск участников; сигналы на это время отложены, чтобы они не попали в недозапущенных детей
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGINT), sigaddset(&set, SIGTERM), sigaddset(&set, SIGHUP);
    sigaddset(&set, SIGUSR1), sigaddset(&set, SIGUSR2);
    sigprocmask(SIG_BLOCK, &set, &old_mask);

    int ok = start(R_DISPATCHER, 0, 0) == 0;
    for (int c = 0; c < N; c++)
        ok = ok && start(R_CHAMBER, c, 0) == 0 && start(R_GATE, c, 0) == 0 && start(R_GATE, c, 1) == 0;
    for (int i = 0; i < n_ships; i++) ok = ok && start(R_SHIP, i, 0) == 0;

    sigprocmask(SIG_SETMASK, &old_mask, NULL);
    mutex = sem_get(SEM_MX, 0, 0);

    // часы моделирования; end_at = 0 – без ограничения по времени
    int end_at = shared->limit, reason = ok ? -1 : FAILURE;
    pid_t bad = 0;

    while (reason < 0) {
        if (got_sig) reason = INTERRUPT;
        else if (shared->passed >= n_ships) reason = ALL_DONE;
        else if (end_at && shared->now >= end_at) reason = TIME_OUT;
        else {
            msleep(shared->tick);
            shared->now++;

            if (reap(&bad)) reason = FAILURE;

            if (want_dump) {
                want_dump = 0;
                dump();
            }

            if (want_toggle) {
                want_toggle = 0;
                end_at = end_at ? 0 : shared->now + (shared->limit ? shared->limit : 1000);

                int got = lock_try(1000);
                if (end_at) out("[t=%4d] супервизор: время моделирования ограничено, остановка при t=%d\n", shared->now, end_at);
                else out("[t=%4d] супервизор: время моделирования не ограничено\n", shared->now);
                if (got) unlock();
            }
        }
    }

    int end_time = shared->now;

    // все суда прошли: даём камерам закончить цикл, чтобы в логе не было оборванных операций
    if (reason == ALL_DONE)
        for (int w = 0; w < 200 && !all_idle() && !got_sig; w++) msleep(shared->tick), shared->now++;

    // мьютекс остаётся захваченным: никто не оборвёт строку вывода, пока участников останавливают
    lock_try(1000);
    for (int k = 0; k < n_pids; k++)
        if (pids[k] > 0) kill(pids[k], SIGTERM);
    for (int k = 0; k < n_pids; k++)
        if (pids[k] > 0) waitpid(pids[k], NULL, 0);

    // итог: участники остановлены, вывод больше никому не мешает
    if (reason == ALL_DONE) {
        out("Все %d судов прошли систему за %d\n", shared->passed, end_time);
    } else {
        if (reason == TIME_OUT) out("Время моделирования вышло.");
        else if (reason == INTERRUPT) out("Прервано пользователем (сигнал %d).", (int)got_sig);
        else out("Аварийное завершение участника (pid %d).", (int)bad);

        out(" Прошло %d из %d. Очереди:\n", shared->passed, n_ships);
        print_chambers();
        print_queues();
    }

    out("Статистика: время %d, прошло %d из %d, групп %d, смен уровня %d, среднее ожидание %d, максимальное %d, нарушений инвариантов %d\n",
        end_time, shared->passed, n_ships, shared->groups, shared->level_changes, shared->entries ? shared->wait_sum / shared->entries : 0,
        shared->wait_max, shared->violations);

    // всё созданное удаляется при любом способе завершения
    sem_close(mutex);
    unlink_all(N, n_ships);
    munmap(shared, sizeof(Shared));
    close(log_fd);

    return reason == FAILURE;
}
