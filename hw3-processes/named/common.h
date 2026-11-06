#ifndef COMMON_H
#define COMMON_H

#include <semaphore.h>
#include <sys/types.h>

#define MAX_SHIPS 1000
#define MAX_CHAMBERS 100

// имена объектов POSIX (не длиннее 31 символа)
#define SHM_NAME "/lk_shm" // разделяемая память с общим состоянием модели
#define SEM_MX "/lk_mx" // мьютекс: защищает разделяемое состояние и вывод
#define SEM_WAKE "/lk_dw" // будит диспетчера: появилась очередь или освободилась камера
#define SEM_START "/lk_st%d" // диспетчер -> камера (номер камеры): группа назначена
#define SEM_ACK "/lk_ak%d" // судно -> камера (номер камеры): вход или выход закончен
#define SEM_TOKEN "/lk_tk%d" // ключ камеры (номер камеры): его держат открытые ворота и смена уровня
#define SEM_CMD "/lk_gc%d_%d" // камера -> ворота (номер камеры, сторона): команда открыть или закрыть
#define SEM_DONE "/lk_gd%d_%d" // ворота (номер камеры, сторона) -> камера: команда выполнена
#define SEM_GO "/lk_go%d" // камера -> судно (номер судна): войти, а затем выйти

// состояния судна
enum { NOT_ARRIVED, QUEUED, ENTERING, INSIDE, EXITING, DONE };

// команды воротам
enum { GATE_OPEN, GATE_CLOSE };

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
    int cmd[2]; // последняя команда воротам стороны
    int busy; // 1 = диспетчер назначил группу, камера ещё не закончила цикл
    int last_dir; // направление предыдущей группы
} Chamber;

// всё общее состояние модели лежит в одном объекте разделяемой памяти
typedef struct {
    // параметры из файла
    int N, cap, n_ships, max_int, gate_time, level_time, strategy, max_wait, limit, seed, step, tick;
    volatile int now; // модельное время, его увеличивает только супервизор
    volatile int passed; // сколько судов прошло всю систему
    int groups, level_changes, entries, wait_sum, wait_max, violations; // статистика
    Ship ships[MAX_SHIPS];
    Chamber chambers[MAX_CHAMBERS];
} Shared;

extern Shared *shared; // разделяемая память
extern sem_t *mutex; // мьютекс (свой дескриптор в каждом процессе)
extern int log_fd; // дескриптор файла лога
extern pid_t owner; // pid супервизора
extern int clean_chambers, clean_ships; // сколько камер и судов могло создать семафоры

// вывод (вызывать с захваченным мьютексом) и завершение с ошибкой
void out(const char *fmt, ...);
void die(const char *msg);

// синхронизация и время
void sem_wait_retry(sem_t *sem);
void lock(void);
void unlock(void);
int lock_try(int ms);
void msleep(long ms);
void ticks(int count);

// именованные семафоры; имя собирается из формата и двух номеров
sem_t *sem_get(const char *fmt, int first, int second);
sem_t *sem_make(const char *fmt, int first, int second, int value);
void post_named(const char *fmt, int first, int second);
void unlink_all(int nch, int nships);

// модель
int level(int section);
const char *dname(int dir);
void say(int chamber_id, const char *text);
void violation(const char *what);
int pick(int chamber_id, int dir, int room);
int queue_len(int chamber_id, int dir);
int choose(int chamber_id);
void print_chambers(void);
void print_queues(void);

// роли участников, по файлу на каждую
void dispatcher_main(void);
void chamber_main(int chamber_id);
void gate_main(int chamber_id, int side);
void ship_main(int i);

#endif
