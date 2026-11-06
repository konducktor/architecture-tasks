#!/bin/sh
# Сборка и запуск всех наборов данных и тестов сигналов для одной версии программы:
#   ./run_tests.sh named | unnamed | independent | pipes
# Логи сохраняются в tests/logs/<версия>
cd "$(dirname "$0")"
version=${1:-named}
case "$version" in
    named | unnamed | independent | pipes) ;;
    *) echo "Использование: $0 named|unnamed|independent|pipes"; exit 1 ;;
esac

# независимые программы (independent и pipes) запускаются скриптом, а не одним бинарником
case "$version" in independent | pipes) multi=1 ;; *) multi=0 ;; esac

mkdir -p "logs/$version"
logs="logs/$version"

# запуск модели: $runcmd <параметры> <лог>
if [ "$multi" = 1 ]; then
    make -s -C ../$version || exit 1
    runcmd=../$version/run.sh
    cleanup() { ../$version/cleanup.sh > /dev/null 2>&1; }
    process=dispatcher # к нему идут сигналы SIGUSR1 и SIGINT
else
    gcc -Wall -pthread -o "$logs/locks" ../$version/*.c || exit 1
    runcmd="$logs/locks"
    cleanup() { "$logs/locks" -c > /dev/null 2>&1; }
    process=locks
fi
cleanup

# после завершения не должно остаться семафоров и памяти (список виден только на Linux)
clean() {
    if [ -d /dev/shm ] && ls /dev/shm | grep -q 'lk_\|lp_'; then return 1; fi
    if ls /tmp | grep -q 'lkp_'; then return 1; fi
    return 0
}

# итог одного теста: имя, ожидаемая строка в логе
verdict() {
    name="$1"; expected="$2"

    if grep -q "$expected" "$logs/$name.log" && ./check_log.sh "$logs/$name.log" && clean; then
        echo "OK   $name"
    else
        echo "FAIL $name"
        cleanup
    fi
    tail -n 1 "$logs/$name.log"
}

for file in *.txt; do
    name="${file%.txt}"
    [ "$name" = "unlimited" ] && continue
    $runcmd "$file" "$logs/$name.log" > /dev/null

    # набор timeout должен завершиться по лимиту времени, остальные – прохождением всех судов
    if [ "$name" = "timeout" ]; then expected="Время моделирования вышло"; else expected="Все .* судов прошли"; fi
    verdict "$name" "$expected"
done

# режим без ограничения по времени: SIGUSR1 печатает состояние, SIGINT и SIGTERM прерывают
for sig in INT TERM; do
    name="signal_$sig"
    $runcmd unlimited.txt "$logs/$name.log" > /dev/null &
    job=$!
    sleep 1
    if [ "$multi" = 1 ]; then target=$(pgrep -x dispatcher); else target=$job; fi
    kill -USR1 $target
    sleep 1; kill -$sig $target
    wait $job
    verdict "$name" "Прервано пользователем"
    grep -q "состояние по SIGUSR1" "$logs/$name.log" || echo "FAIL $name: нет состояния по SIGUSR1"
done

if [ "$multi" = 1 ]; then
    # сигнал только одному судну: останавливается вся модель
    name="signal_ship"
    $runcmd unlimited.txt "$logs/$name.log" > /dev/null &
    job=$!
    sleep 2; kill -TERM "$(pgrep -x ship | head -n 1)"
    wait $job
    verdict "$name" "Прервано пользователем"

    # запуск в обратном порядке: сначала суда, потом диспетчер
    name="reverse"
    REVERSE=1 $runcmd basic.txt "$logs/$name.log" > /dev/null
    verdict "$name" "Все .* судов прошли"

    # kill -9 одного судна: остальные участники должны остановиться с сообщением об аварии
    name="kill9_ship"
    $runcmd unlimited.txt "$logs/$name.log" > /dev/null &
    job=$!
    sleep 2; kill -9 "$(pgrep -x ship | head -n 1)"
    wait $job
    verdict "$name" "Аварийное завершение"

    # kill -9 всех участников: остатки удаляет cleanup.sh
    name="kill9_all"
    $runcmd unlimited.txt "$logs/$name.log" > /dev/null &
    job=$!
    sleep 2
    pkill -9 -x dispatcher; pkill -9 -x chamber; pkill -9 -x gate; pkill -9 -x ship
    wait $job 2> /dev/null
    cleanup
    if clean && ! pgrep -x 'dispatcher|chamber|gate|ship' > /dev/null; then echo "OK   $name (остатки удалены)"; else echo "FAIL $name"; fi
else
    # kill -9 супервизора: на Linux участники завершаются сами, остатки удаляет cleanup.sh
    name="kill9"
    $runcmd unlimited.txt "$logs/$name.log" > /dev/null &
    job=$!
    sleep 1; kill -9 $job
    wait $job 2> /dev/null
    sleep 1
    if [ "$(uname)" = "Linux" ] && pgrep -x locks > /dev/null; then echo "FAIL $name: остались процессы"; fi
    cleanup
    if clean; then echo "OK   $name (остатки удалены)"; else echo "FAIL $name"; fi
fi
