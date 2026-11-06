#ifndef COMMON_H
#define COMMON_H

#include <semaphore.h>
#include <sys/types.h>

#define MAX_SHIPS 1000
#define MAX_CHAMBERS 100

// имя объекта POSIX shm (не длиннее 31 символа); семафоры лежат внутри него и своих имён не имеют
#define SHM_NAME "/lk_shm" // разделяемая память с общим состоянием и семафорами модели

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
    // неименованные семафоры: sem_init с pshared = 1, лежат в разделяемой памяти
    sem_t mutex; // мьютекс: защищает остальное состояние и вывод
    sem_t wake; // будит диспетчера: появилась очередь или освободилась камера
    sem_t start[MAX_CHAMBERS]; // диспетчер -> камера: группа назначена
    sem_t ack[MAX_CHAMBERS]; // судно -> камера: вход или выход закончен
    sem_t token[MAX_CHAMBERS]; // ключ камеры: его держат открытые ворота и смена уровня
    sem_t cmd[MAX_CHAMBERS][2]; // камера -> ворота (по стороне): команда открыть или закрыть
    sem_t done[MAX_CHAMBERS][2]; // ворота (по стороне) -> камера: команда выполнена
    sem_t go[MAX_SHIPS]; // камера -> судно: войти, а затем выйти
    Ship ships[MAX_SHIPS];
    Chamber chambers[MAX_CHAMBERS];
} Shared;

extern Shared *shared; // разделяемая память
extern int log_fd; // дескриптор файла лога
extern pid_t owner; // pid супервизора

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

// неименованные семафоры в разделяемой памяти
void init_sem(sem_t *sem, int value);
void destroy_semaphores(int nch, int nships);

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
