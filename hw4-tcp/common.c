#include "common.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define CONNECT_ATTEMPTS 250
#define CONNECT_RETRY_MS 20

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

void die(const char *what) {
    perror(what);
    exit(1);
}

void fail(const char *what) {
    fprintf(stderr, "%s\n", what);
    exit(1);
}

long now_ms(void) {
    struct timespec moment;

    clock_gettime(CLOCK_MONOTONIC, &moment);
    return moment.tv_sec * 1000 + moment.tv_nsec / 1000000;
}

void msleep(long ms) {
    struct timespec pause = {ms / 1000, (ms % 1000) * 1000000};

    while (nanosleep(&pause, &pause) < 0 && errno == EINTR) {
    }
}

int parse_int(const char *text, int min, int max, const char *what) {
    char *end;
    long value = strtol(text, &end, 10);

    if (*text == '\0' || *end != '\0' || value < min || value > max) {
        fprintf(stderr, "Неверное значение «%s» для параметра «%s» (допустимо %d..%d)\n", text, what, min, max);
        exit(1);
    }

    return (int)value;
}

int parse_port(const char *text) {
    return parse_int(text, 1, 65535, "порт");
}

int create_server_socket(int port) {
    int server_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (server_sock < 0) die("socket");

    // повторный запуск сразу после остановки не должен падать на занятом порте (TIME_WAIT)
    int reuse = 1;
    setsockopt(server_sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    struct sockaddr_in address = {0};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons((unsigned short)port);

    if (bind(server_sock, (struct sockaddr *)&address, sizeof(address)) < 0) {
        fprintf(stderr, "Не удалось занять порт %d: %s\n", port, strerror(errno));
        if (errno == EADDRINUSE)
            fprintf(stderr, "Порт занят другой программой. Укажите другой: server <файл_параметров> <лог> <порт>\n");
        exit(1);
    }
    if (listen(server_sock, SOMAXCONN) < 0) die("listen");

    return server_sock;
}

static int try_connect(const char *host, int port) {
    char port_text[16];
    struct addrinfo hints = {0}, *found;

    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    snprintf(port_text, sizeof(port_text), "%d", port);

    int error = getaddrinfo(host, port_text, &hints, &found);
    if (error != 0) {
        fprintf(stderr, "Не удалось найти адрес %s: %s\n", host, gai_strerror(error));
        exit(1);
    }

    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) die("socket");

    if (connect(sock, found->ai_addr, found->ai_addrlen) < 0) {
        close(sock);
        sock = -1;
    }

    freeaddrinfo(found);
    return sock;
}

int connect_to_server(const char *host, int port) {
    for (int attempt = 0; attempt < CONNECT_ATTEMPTS; attempt++) {
        int sock = try_connect(host, port);
        if (sock >= 0) return sock;
        msleep(CONNECT_RETRY_MS);
    }

    fprintf(stderr, "Сервер %s:%d недоступен\n", host, port);
    exit(1);
}

int send_msg(int sock, int type, int a, int b, int c, int time) {
    int32_t wire[5] = {htonl(type), htonl(a), htonl(b), htonl(c), htonl(time)};
    const char *cursor = (const char *)wire;
    size_t left = WIRE_MSG_SIZE;

    while (left > 0) {
        ssize_t written = send(sock, cursor, left, MSG_NOSIGNAL);

        if (written < 0 && errno == EINTR) continue;
        if (written <= 0) return -1;

        cursor += written, left -= (size_t)written;
    }

    return 0;
}

void decode_msg(const unsigned char *wire, Msg *msg) {
    int32_t fields[5];

    memcpy(fields, wire, WIRE_MSG_SIZE);
    msg->type = (int32_t)ntohl(fields[0]);
    msg->a = (int32_t)ntohl(fields[1]);
    msg->b = (int32_t)ntohl(fields[2]);
    msg->c = (int32_t)ntohl(fields[3]);
    msg->time = (int32_t)ntohl(fields[4]);
}

int recv_msg(int sock, Msg *msg) {
    unsigned char wire[WIRE_MSG_SIZE];
    size_t have = 0;

    while (have < WIRE_MSG_SIZE) {
        ssize_t got = recv(sock, wire + have, WIRE_MSG_SIZE - have, 0);

        if (got < 0 && errno == EINTR) continue;
        if (got <= 0) return -1;

        have += (size_t)got;
    }

    decode_msg(wire, msg);
    return 0;
}

const char *dir_name(int dir) {
    return dir == DIR_FORWARD ? "вперёд" : "назад";
}

const char *side_name(int side) {
    return side == SIDE_UPPER ? "верхние" : "нижние";
}

const char *reject_reason(int reason) {
    switch (reason) {
    case REJECT_BAD_ID: return "такого участника нет в модели";
    case REJECT_DUPLICATE: return "такой участник уже подключён";
    case REJECT_TOO_MANY_SHIPS: return "все суда модели уже сформированы";
    case REJECT_BAD_SHIP: return "неверные параметры судна";
    case REJECT_STOPPING: return "модель останавливается";
    default: return "неизвестная причина";
    }
}

const char *stop_reason(int reason) {
    switch (reason) {
    case ALL_DONE: return "все суда обработаны";
    case TIME_OUT: return "время моделирования вышло";
    case INTERRUPT: return "прервано пользователем";
    case FAILURE: return "аварийная остановка";
    default: return "неизвестная причина";
    }
}
