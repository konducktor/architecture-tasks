#!/bin/sh
# Аварийная очистка после kill -9: останавливает оставшиеся процессы, удаляет семафоры и память
dir=$(cd "$(dirname "$0")" && pwd)
pkill -x dispatcher 2> /dev/null
pkill -x chamber 2> /dev/null
pkill -x gate 2> /dev/null
pkill -x ship 2> /dev/null

make -s -C "$dir" && "$dir/bin/dispatcher" -c
