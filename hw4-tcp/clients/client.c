#include "client.h"

#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

static int server_sock = -1;
static int tick_ms = 1;
static char client_name[64] = "клиент";
static void (*stop_handler)(int reason);

// модельное время: последнее время от сервера плюс прошедшее с тех пор локально
static int synced_time;
static long synced_at_ms;

static void sync_time(int server_time) {
    synced_time = server_time;
    synced_at_ms = now_ms();
}

int client_time_now(void) {
    return synced_time + (int)((now_ms() - synced_at_ms) / tick_ms);
}

void client_set_name(const char *format, ...) {
    va_list args;

    va_start(args, format);
    vsnprintf(client_name, sizeof(client_name), format, args);
    va_end(args);
}

void client_on_stop(void (*handler)(int reason)) {
    stop_handler = handler;
}

void client_say(const char *format, ...) {
    va_list args;

    printf("[t=%4d] %s: ", client_time_now(), client_name);
    va_start(args, format);
    vprintf(format, args);
    va_end(args);
    printf("\n");
    fflush(stdout);
}

Msg client_join(const char *host, int port, int role, int arg2, int arg3) {
    Msg reply;

    signal(SIGPIPE, SIG_IGN);
    server_sock = connect_to_server(host, port);

    if (send_msg(server_sock, M_HELLO, role, arg2, arg3, 0) < 0 || recv_msg(server_sock, &reply) < 0)
        fail("Сервер закрыл соединение при подключении");

    if (reply.type == M_REJECT) {
        fprintf(stderr, "%s: сервер отказал в подключении: %s\n", client_name, reject_reason(reply.arg1));
        exit(1);
    }
    if (reply.type != M_WELCOME) fail("Неожиданный ответ сервера при подключении");

    tick_ms = reply.arg3 > 0 ? reply.arg3 : 1;
    sync_time(reply.time);
    return reply;
}

void client_send(int type, int arg1, int arg2, int arg3) {
    if (send_msg(server_sock, type, arg1, arg2, arg3, client_time_now()) < 0) {
        client_say("соединение с сервером потеряно");
        exit(1);
    }
}

Msg client_receive(void) {
    Msg msg;

    if (recv_msg(server_sock, &msg) < 0) {
        client_say("соединение с сервером потеряно");
        exit(1);
    }

    sync_time(msg.time);

    if (msg.type == M_STOP) {
        if (stop_handler) stop_handler(msg.arg1);
        else client_say("модель остановлена: %s", stop_reason(msg.arg1));
        close(server_sock);
        exit(0);
    }

    return msg;
}

Msg client_expect(int type) {
    Msg msg = client_receive();

    if (msg.type != type) {
        fprintf(stderr, "%s: неожиданное сообщение %d (ждали %d)\n", client_name, msg.type, type);
        exit(1);
    }

    return msg;
}

void client_sleep_ticks(int count) {
    msleep((long)count * tick_ms);
}
