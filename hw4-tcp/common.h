// Общее для сервера-диспетчера и клиентов (камера, ворота, судно):
// формат сообщений по TCP, работа с сокетами, время и завершение с ошибкой.
#ifndef COMMON_H
#define COMMON_H

#include <stdint.h>

#define DEFAULT_PORT 6767

// роли клиентов в сообщении M_HELLO
enum { ROLE_CHAMBER, ROLE_GATE, ROLE_SHIP };

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
    M_STOP // сервер -> клиент: модель остановлена, a = причина остановки (ALL_DONE, TIME_OUT, ...)
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
int create_server_socket(int port);
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

#endif
