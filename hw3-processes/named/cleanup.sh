#!/bin/sh
# Аварийная очистка после kill -9: останавливает оставшиеся процессы, удаляет семафоры и память
cd "$(dirname "$0")"
pkill -x locks 2>/dev/null

if [ -x ./locks ]; then ./locks -c; else echo "Сначала соберите программу: gcc -Wall -pthread -o locks *.c"; fi
