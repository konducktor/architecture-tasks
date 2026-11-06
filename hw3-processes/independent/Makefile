# Сборка четырёх независимых программ: make; результат в bin/
CC = gcc
CFLAGS = -Wall -Wextra -pthread
PROGRAMS = dispatcher chamber gate ship

all: $(addprefix bin/, $(PROGRAMS))

bin/%: %.c common.c common.h
	@mkdir -p bin
	$(CC) $(CFLAGS) -o $@ $< common.c

clean:
	rm -rf bin
