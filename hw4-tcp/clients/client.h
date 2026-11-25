// Общая часть клиентов (камера, ворота, судно): подключение к серверу,
// ожидание сообщений, модельное время и вывод собственных событий.
#ifndef CLIENT_H
#define CLIENT_H

#include "../common.h"

// подключиться и представиться; при отказе сервера программа завершается
Msg client_join(const char *host, int port, int role, int b, int c);

void client_set_name(const char *format, ...) __attribute__((format(printf, 1, 2)));
void client_send(int type, int a, int b, int c);

// ждать сообщение нужного типа; M_STOP и разрыв соединения завершают программу
Msg client_expect(int type);
Msg client_receive(void);

// вызывается при M_STOP вместо стандартного сообщения, перед завершением программы
void client_on_stop(void (*handler)(int reason));

void client_say(const char *format, ...) __attribute__((format(printf, 1, 2)));
void client_sleep_ticks(int count);
int client_time_now(void);

#endif
