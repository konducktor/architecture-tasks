# Сборка сервера и трёх клиентов: make; результат в bin/
CC = gcc
CFLAGS = -Wall -Wextra -std=gnu11

all: bin/server bin/chamber bin/gate bin/ship

bin/server: server.c model.c model.h common.c common.h
	@mkdir -p bin
	$(CC) $(CFLAGS) -o $@ server.c model.c common.c

bin/%: %.c client.c client.h common.c common.h
	@mkdir -p bin
	$(CC) $(CFLAGS) -o $@ $< client.c common.c

clean:
	rm -rf bin
