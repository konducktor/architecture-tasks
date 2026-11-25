// Общее для сервера-диспетчера и клиентов (камера, ворота, судно):
// формат сообщений по TCP, работа с сокетами, время и завершение с ошибкой.
#ifndef COMMON_H
#define COMMON_H

#include <stdint.h>

#define MAX_CHAMBERS 100
#define MAX_SHIPS 1000

// роли клиентов в сообщении M_HELLO
enum { ROLE_CHAMBER, ROLE_GATE, ROLE_SHIP, ROLE_MONITOR };

enum {
    SHIP_CONNECTING, // подключилось, но ещё не сообщило о прибытии
    SHIP_QUEUED,
    SHIP_ENTERING, // диспетчер позвал судно в камеру
    SHIP_INSIDE,
    SHIP_EXITING,
    SHIP_PASSED,
    SHIP_LEFT // ушло из системы, не пройдя её (отключилось или отклонено)
};

enum { GATE_CLOSED, GATE_OPENING, GATE_IS_OPEN, GATE_CLOSING };

// стороны камеры и ворот: 0 – верхние (у участка с номером камеры), 1 – нижние
enum { SIDE_UPPER, SIDE_LOWER };

// направление судна: вперёд (участок 0 -> N) или назад
enum { DIR_FORWARD, DIR_BACKWARD };

enum { GATE_OPEN, GATE_CLOSE };
enum { MOVE_ENTER, MOVE_EXIT };

// типы сообщений; в комментарии – кто кому отправляет и что лежит в полях a, b, c
enum {
    // подключение
    M_HELLO, // клиент -> сервер: a = роль; камера: b = номер; ворота: b = камера, c = сторона
    M_WELCOME, // сервер -> клиент: c = длина тика (мс); камера и ворота: a = время действия (тики); судно: a = выданный номер, b = вместимость камер
    M_REJECT, // сервер -> клиент: подключение отклонено, a = причина (REJECT_*)

    // судно
    M_ARRIVED, // судно -> сервер: прибытие, a = размер, b = направление, c = приоритет (0/1)
    M_QUEUED, // сервер -> судно: стоит в очереди, a = камера, b = участок
    M_GO, // сервер -> судно: a = камера, b = MOVE_ENTER или MOVE_EXIT
    M_MOVED, // судно -> сервер: вход или выход закончен, a = MOVE_ENTER или MOVE_EXIT
    M_PASSED, // сервер -> судно: прошло всю систему, a = участок выхода

    // камера
    M_START, // сервер -> камера: группа назначена, a = сторона входа, b = сторона, под которую сейчас вода
    M_LEVEL_BEGIN, // камера -> сервер: начинаю менять уровень, a = сторона, под которую меняется вода
    M_LEVEL_END, // камера -> сервер: уровень изменён, a = сторона
    M_GATE_CMD, // камера -> сервер -> ворота: a = сторона, b = GATE_OPEN или GATE_CLOSE
    M_GATE_DONE, // ворота -> сервер -> камера: команда выполнена, a = сторона, b = GATE_OPEN или GATE_CLOSE
    M_NEXT_SHIP, // камера -> сервер: позвать следующее судно на вход (a = MOVE_ENTER) или выход (a = MOVE_EXIT)
    M_SHIP_DONE, // сервер -> камера: судно a вошло или вышло
    M_NO_SHIP, // сервер -> камера: больше звать некого
    M_FREE, // камера -> сервер: цикл закончен, камера свободна

    // остановка
    M_STOP, // сервер -> клиент: модель остановлена, a = причина остановки (ALL_DONE, TIME_OUT, ...)

    // сервер -> монитор: параметры и снимок состояния при подключении
    M_INFO, // a = INFO_*, b = значение
    M_SNAPSHOT_BEGIN, // дальше идут события, описывающие текущее состояние (монитор их не печатает)
    M_SNAPSHOT_END,
    M_COUNTS, // a = прошли систему, b = покинули её, c = сколько судов подключалось

    // сервер -> монитор: события модели
    M_EV_CHAMBER_CONNECTED, // a = камера
    M_EV_GATE_CONNECTED, // a = камера, b = сторона
    M_EV_LOCKING_STARTED,
    M_EV_CLIENT_REJECTED, // a = причина (REJECT_*)
    M_EV_SHIP_CONNECTED, // a = судно
    M_EV_SHIP_PRIORITY, // a = судно; приходит перед M_EV_SHIP_ARRIVED приоритетного судна
    M_EV_SHIP_ARRIVED, // a = судно, b = размер, c = направление
    M_EV_SHIP_STATE, // только в снимке: a = судно, b = SHIP_*, c = участок
    M_EV_SHIP_QUEUED, // a = судно, b = камера, c = участок
    M_EV_GROUP_STARTED, // a = камера, b = сторона входа
    M_EV_LEVEL_BEGIN, // a = камера, b = сторона, под которую меняется вода
    M_EV_LEVEL_END, // a = камера, b = сторона
    M_EV_GATE_STATE, // a = камера, b = сторона, c = GATE_CLOSED .. GATE_CLOSING
    M_EV_SHIP_CALLED, // a = судно, b = камера, c = MOVE_ENTER или MOVE_EXIT
    M_EV_SHIP_ENTERED, // a = судно, b = камера
    M_EV_SHIP_EXITED, // a = судно, b = камера, c = участок
    M_EV_SHIP_PASSED, // a = судно
    M_EV_SHIP_LEFT, // a = судно, b = причина (LEFT_*)
    M_EV_SHIP_LOST, // a = судно: связь потеряна, судно остаётся в модели до выгрузки
    M_EV_CHAMBER_FREE, // a = камера
    M_EV_PARTICIPANT_LOST, // a = роль, b = камера, c = сторона (для ворот)
    M_EV_VIOLATION, // a = VIOLATION_*
    M_EV_TIME_LIMIT, // a = момент остановки по времени (0 – без ограничения)
    M_EV_FINAL_COUNTS, // a = прошли, b = покинули, c = групп
    M_EV_FINAL_TIMES, // a = время завершения, b = среднее ожидание, c = максимальное
    M_EV_FINAL_OTHER // a = смен уровня, b = нарушений инвариантов, c = сигнал остановки
};

// параметры модели, которые монитор получает в M_INFO
enum { INFO_CHAMBERS, INFO_CAPACITY, INFO_STEP, INFO_SHIPS_EXPECTED, INFO_TIME_LIMIT, INFO_STRATEGY, INFO_MAX_WAIT };

// почему судно покинуло систему, не пройдя её
enum { LEFT_DISCONNECTED, LEFT_UNLOADED_WITHOUT_LINK, LEFT_REJECTED };

enum {
    VIOLATION_ENTER_UNSAFE,
    VIOLATION_CAPACITY,
    VIOLATION_EXIT_UNSAFE,
    VIOLATION_LEVEL_WITH_OPEN_GATES,
    VIOLATION_GATE_OPEN_UNSAFE,
    VIOLATION_OPPOSITE_GATES_OPEN
};

// причины отказа в подключении
enum { REJECT_BAD_ID, REJECT_DUPLICATE, REJECT_TOO_MANY_SHIPS, REJECT_BAD_SHIP, REJECT_STOPPING };

// причины остановки модели
enum { RUNNING, ALL_DONE, TIME_OUT, INTERRUPT, FAILURE };

// Сообщение фиксированного размера: тип, три поля и модельное время отправки.
// По сети каждое поле идёт как 32-битное число в сетевом порядке байт.
typedef struct {
    int32_t type, a, b, c, time;
} Msg;

#define WIRE_MSG_SIZE (5 * sizeof(int32_t))

void die(const char *what) __attribute__((noreturn)); // ошибка системного вызова (errno)
void fail(const char *what) __attribute__((noreturn)); // ошибка без errno

long now_ms(void);
void msleep(long ms);

// разбор аргументов командной строки
int parse_int(const char *text, int min, int max, const char *what);
int parse_port(const char *text);

// сокеты
int create_server_socket(const char *ip, int port); // ip – адрес интерфейса, 0.0.0.0 – все интерфейсы
int connect_to_server(const char *host, int port); // с повторными попытками, пока сервер не запустится

// отправка целого сообщения; 0 при успехе, -1 если соединение разорвано
int send_msg(int sock, int type, int a, int b, int c, int time);

// сериализация для сервера, который собирает сообщения из кусков сам
void decode_msg(const unsigned char *wire, Msg *msg);

// блокирующее чтение целого сообщения; 0 при успехе, -1 если сервер закрыл соединение
int recv_msg(int sock, Msg *msg);

const char *dir_name(int dir);
const char *side_name(int side);
const char *reject_reason(int reason);
const char *stop_reason(int reason);
const char *violation_text(int code);

#endif
