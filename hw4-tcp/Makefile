# Сборка сервера и четырёх клиентов: make; результат в bin/
CC = gcc
CFLAGS = -Wall -Wextra -std=gnu11

all: bin/server bin/chamber bin/gate bin/ship bin/monitor

bin/server: server.c model.c model.h common.c common.h
	@mkdir -p bin
	$(CC) $(CFLAGS) -o $@ server.c model.c common.c

bin/%: clients/%.c clients/client.c clients/client.h common.c common.h
	@mkdir -p bin
	$(CC) $(CFLAGS) -o $@ $< clients/client.c common.c

clean:
	rm -rf bin
