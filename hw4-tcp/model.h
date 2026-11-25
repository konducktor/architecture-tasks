// Состояние шлюзовой системы, которое хранит сервер-диспетчер:
// суда, камеры, параметры, статистика, стратегия выбора групп и вывод.
#ifndef MODEL_H
#define MODEL_H

#include "common.h"

typedef struct {
    int id, size, dir, prio;
    int state, pos, chamber; // участок канала и камера, к которой судно стоит в очереди или в которой находится
    int queued_at;
    int client; // сокет судна; -1 после потери связи
} Ship;

typedef struct {
    int busy; // диспетчер назначил группу, камера ещё не закончила цикл
    int entry_side; // сторона входа текущей группы
    int water; // сторона, с уровнем которой совпадает вода в камере
    int load; // занятая вместимость (с учётом судов, которым уже разрешён вход)
    int gate[2]; // GATE_CLOSED .. GATE_CLOSING для верхних и нижних ворот
    int level_changing;
    int last_dir; // направление предыдущей группы (-1 – ещё не было)
    int chamber_client, gate_client[2]; // сокеты участников; -1 – не подключены
} Chamber;

typedef struct {
    // параметры из файла (тот же порядок чисел, что и в прошлых версиях)
    int chambers_count, capacity, ships_expected, arrival_interval, gate_time, level_time, strategy, max_wait,
        limit, seed, step, tick;

    long start_ms;
    int locking_started; // все камеры и ворота подключились
    int stop_reason, stop_signal;
    char failure_note[128];
    int end_at; // момент остановки по времени (0 – без ограничения)
    int end_time; // момент завершения для итогов (-1, пока не определён)

    int ships_registered;
    int passed, left, groups, level_changes, entries, wait_sum, wait_max, violations;

    Ship ships[MAX_SHIPS];
    Chamber chambers[MAX_CHAMBERS];
} World;

extern World world;

void model_init(const char *params_path, const char *log_path);
void model_close_log(void);

void out(const char *format, ...) __attribute__((format(printf, 1, 2)));
void event(const char *format, ...) __attribute__((format(printf, 1, 2))); // строка с временем [t=...]
void event_received(const char *format, ...) __attribute__((format(printf, 1, 2))); // "<- от кого: что пришло"
void event_sent(const char *format, ...) __attribute__((format(printf, 1, 2))); // "-> кому: что отправлено"
void event_decision(const char *format, ...) __attribute__((format(printf, 1, 2))); // "диспетчер: собственное решение"
int time_now(void);

int level(int section);
int is_last_section(const Ship *ship);
void violation(int code); // VIOLATION_*

Ship *find_ship_to_enter(int chamber_id, int side, int room);
Ship *find_ship_to_exit(int chamber_id);
int queue_len(int chamber_id, int side);
int choose_side(int chamber_id);
int all_chambers_idle(void);
int all_ships_handled(void);

void print_state(void);
void print_final_report(void);

#endif
