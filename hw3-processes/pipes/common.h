#ifndef COMMON_H
#define COMMON_H

#include <semaphore.h>
#include <sys/types.h>

#define MAX_SHIPS 1000
#define MAX_CHAMBERS 100
#define MAX_PROCESSES (1 + 3 * MAX_CHAMBERS + MAX_SHIPS) // диспетчер, камеры, по двое ворот на камеру и суда

// имена объектов POSIX (не длиннее 31 символа)
#define SHM_NAME "/lp_shm" // разделяемая память с общим состоянием модели
#define SEM_INIT "/lp_in" // кто первым его создал, тот становится создателем модели; остальные ждут его готовности
#define SEM_BAR "/lp_br" // турникет: все участники начинают работу одновременно
#define SEM_MX "/lp_mx" // мьютекс: защищает разделяемое состояние и вывод
#define SEM_TOKEN "/lp_tk%d" // ключ камеры (номер камеры): его держат открытые ворота и смена уровня
#define FIFO_PATH "/tmp/lkp_%d" // «почтовый ящик» участника: именованный канал FIFO (номер участника)

// состояния судна
enum { NOT_ARRIVED, QUEUED, ENTERING, INSIDE, EXITING, DONE };

// команды воротам
enum { GATE_OPEN, GATE_CLOSE };

// сообщения между участниками; каждое занимает ровно sizeof(Msg) байт, а запись такого размера в канал атомарна
enum {
    M_WAKE,   // судно или камера -> диспетчер: очереди изменились или камера свободна
    M_START,  // диспетчер -> камера: группа назначена (arg = сторона входа)
    M_GO,     // камера -> судно: войти, а затем выйти
    M_ACK,    // судно -> камера: вход или выход закончен
    M_CMD,    // камера -> ворота: открыть или закрыть (arg = GATE_OPEN или GATE_CLOSE)
    M_DONE,   // ворота -> камера: команда выполнена (arg = сторона)
    M_TICK,   // служебное: таймер диспетчера (приходит самому себе)
    M_SIGNAL, // служебное: пришёл сигнал (приходит самому себе, чтобы прервать ожидание)
    M_STOP    // служебное: модель остановлена, участнику пора завершаться
};

typedef struct {
    int type, arg;
} Msg;

// причины остановки модели
enum { RUNNING, ALL_DONE, TIME_OUT, INTERRUPT, FAILURE };

typedef struct {
    int id, size, dir; // dir: 0 = вперёд (участок 0 -> N), 1 = назад
    int prio, arrive; // приоритет и время прибытия
    int state, pos, chamber; // участок канала и камера, к которой судно стоит в очереди
    int queued_at;
} Ship;

typedef struct {
    int dir; // сторона входа текущей группы
    int water; // сторона, с уровнем которой совпадает вода в камере
    int load; // занятая вместимость (с учётом судов, которым уже разрешён вход)
    int gate[2]; // 1 = ворота открыты; сторона 0 – верхние, сторона 1 – нижние
    int busy; // 1 = диспетчер назначил группу, камера ещё не закончила цикл
    int last_dir; // направление предыдущей группы
} Chamber;

// всё общее состояние модели лежит в одном объекте разделяемой памяти
typedef struct {
    // параметры из файла
    int N, cap, n_ships, max_int, gate_time, level_time, strategy, max_wait, limit, seed, step, tick;
    // сбор участников и часы: супервизора нет, время считается от общего момента старта
    int total; // сколько участников должно собраться: диспетчер, камеры, ворота и суда
    int registered; // сколько участников уже ждёт на барьере
    int alive; // сколько участников ещё работает
    char joined[MAX_PROCESSES]; // 1 = участник с таким номером уже запущен (защита от повторного запуска)
    long start_ms; // момент старта модельных часов (CLOCK_MONOTONIC, мс)
    pid_t pids[MAX_PROCESSES]; // pid работающих участников (0 = вышел), нужны для проверки живости
    pid_t bad_pid; // pid участника, который погиб или остановил модель из-за ошибки
    // остановка
    volatile int stop; // RUNNING, пока модель работает; иначе причина остановки
    int stop_signal; // сигнал, прервавший работу
    int end_at; // момент остановки по времени (0 = без ограничения)
    int end_time; // момент завершения для итогов (-1, пока не определён)
    volatile int passed; // сколько судов прошло всю систему
    int groups, level_changes, entries, wait_sum, wait_max, violations; // статистика
    Ship ships[MAX_SHIPS];
    Chamber chambers[MAX_CHAMBERS];
} Shared;

extern Shared *shared; // разделяемая память
extern sem_t *mutex; // мьютекс (свой дескриптор в каждом процессе)
extern int log_fd; // дескриптор файла лога

// вывод (вызывать с захваченным мьютексом) и завершение с ошибкой
void out(const char *fmt, ...);
void die(const char *msg) __attribute__((noreturn)); // ошибка системного вызова
void fail(const char *msg) __attribute__((noreturn)); // ошибка без errno

// синхронизация и время
void sem_wait_retry(sem_t *sem);
void wait_sem(sem_t *sem); // ждёт семафор (ключ камеры, барьер); при остановке модели участник сам завершается
void lock(void);
void unlock(void);
int lock_try(int ms);
void msleep(long ms);
void ticks(int count);
long now_ms(void);
int time_now(void);

// именованные семафоры; имя собирается из формата и двух номеров
sem_t *sem_get(const char *fmt, int first, int second);
sem_t *sem_make(const char *fmt, int first, int second, int value);
void unlink_all(int nch, int nships);

// каналы: у каждого участника свой входящий FIFO, писать в него может кто угодно, читает только хозяин
int mb_dispatcher(void);
int mb_chamber(int chamber_id);
int mb_gate(int chamber_id, int side);
int mb_ship(int i);
void send_msg(int to, int type, int arg); // отправить сообщение в ящик участника to
Msg recv_expect(int type); // ждать сообщение; служебные обрабатываются внутри, другой тип – ошибка

// жизненный цикл участника: attach -> join -> работа -> leave
// Супервизора нет: общее состояние создаёт самый первый участник, а последний уходящий всё удаляет.
void attach(const char *params_path, const char *log_path);
void join(int who); // участник занимает свой номер и ждёт на барьере остальных
void poll_signals(void); // обработка сигналов и остановки модели
void request_stop(int reason); // остановить модель и разбудить всех участников
void leave(void) __attribute__((noreturn));
int arg_number(int argc, char **argv, int position, const char *what);

extern void (*on_tick)(void); // вызывается при каждом сообщении M_TICK (таймер диспетчера)

// модель
int level(int section);
const char *dname(int dir);
void say(int chamber_id, const char *text);
void violation(const char *what);
int pick(int chamber_id, int dir, int room);
int queue_len(int chamber_id, int dir);
int choose(int chamber_id);
int all_idle(void);
void print_chambers(void);
void print_queues(void);

#endif
