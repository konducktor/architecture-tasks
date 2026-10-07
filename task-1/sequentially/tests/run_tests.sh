#!/bin/sh
# Сборка и запуск всех наборов данных; логи сохраняются в tests/logs
cd "$(dirname "$0")"
mkdir -p logs
gcc -Wall -o logs/locks ../locks.c

for f in *.txt; do
    name="${f%.txt}"
    ./logs/locks "$f" "logs/$name.log" > /dev/null

    # набор timeout должен завершиться по лимиту времени, остальные – прохождением всех судов
    if [ "$name" = "timeout" ]; then expected="Время моделирования вышло"; else expected="Все .* судов прошли"; fi

    if grep -q "$expected" "logs/$name.log"; then echo "OK   $name"; else echo "FAIL $name"; fi
    tail -n 1 "logs/$name.log"
done
