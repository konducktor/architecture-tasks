#!/bin/sh
# Все наборы данных и проверки отключений: ./run_tests.sh
# Логи сервера и клиентов сохраняются в tests/logs
cd "$(dirname "$0")"
run=../run.sh
bin=../bin
port=${PORT:-5077}
logs=logs
mkdir -p "$logs"
make -s -C .. || exit 1

no_leftovers() {
    sleep 0.5
    ! pgrep -f "$bin/(server|chamber|gate|ship)" > /dev/null
}

# итог одного теста: имя, ожидаемая строка в логе сервера
verdict() {
    name="$1"; expected="$2"

    if grep -Eq "$expected" "$logs/$name.log" && ./check_log.sh "$logs/$name.log" && no_leftovers; then
        echo "OK   $name"
    else
        echo "FAIL $name"
        pkill -f "$bin/(server|chamber|gate|ship)"
    fi
    tail -n 1 "$logs/$name.log"
}

start_in_background() {
    $run "$1" "$logs/$2.log" "$port" > /dev/null &
    job=$!
    sleep "$3"
}

for file in basic.txt by_direction.txt large.txt one_chamber.txt small_max_wait.txt timeout.txt; do
    name="${file%.txt}"
    $run "$file" "$logs/$name.log" "$port" > /dev/null

    if [ "$name" = "timeout" ]; then expected="Время моделирования вышло"; else expected="Все .* судов прошли"; fi
    verdict "$name" "$expected"
done

# камеры и ворота подключаются позже судов: суда ждут в очереди
name="late_infrastructure"
INFRA_DELAY=1 $run basic.txt "$logs/$name.log" "$port" > /dev/null
verdict "$name" "Все .* судов прошли"

# режим без ограничений: SIGUSR1 печатает состояние, SIGINT и SIGTERM останавливают
for sig in INT TERM; do
    name="signal_$sig"
    start_in_background unlimited.txt "$name" 2
    kill -USR1 "$(pgrep -f "$bin/server")"
    sleep 1; kill -$sig "$(pgrep -f "$bin/server")"
    wait $job
    verdict "$name" "Прервано пользователем"
    grep -q "состояние по SIGUSR1" "$logs/$name.log" || echo "FAIL $name: нет состояния по SIGUSR1"
done

# одно судно убито: оно уходит из системы, остальные проходят
name="kill9_ship"
start_in_background disconnect.txt "$name" 1
kill -9 "$(pgrep -f "$bin/ship" | head -n 1)"
wait $job
verdict "$name" "Все суда обработаны"

# убиты все суда, что сейчас в системе: те, что в камерах, выводятся без связи
name="kill9_ships"
start_in_background disconnect.txt "$name" 1
pkill -9 -f "$bin/ship"
wait $job
verdict "$name" "Все (суда обработаны|.* судов прошли)"
grep -Eq "без связи|покинуло систему" "$logs/$name.log" || echo "FAIL $name: нет судов, потерявших связь"

# лишнее судно и повторная камера получают отказ, модель продолжает работу
name="rejects"
start_in_background disconnect.txt "$name" 1.5
extra_ship=$($bin/ship 127.0.0.1 "$port" 2>&1)
extra_chamber=$($bin/chamber 127.0.0.1 "$port" 0 2>&1)
wait $job
verdict "$name" "Все .* судов прошли"
echo "$extra_ship" | grep -q "все суда модели уже сформированы" || echo "FAIL $name: лишнее судно не отклонено"
echo "$extra_chamber" | grep -q "уже подключён" || echo "FAIL $name: повторная камера не отклонена"

# камера убита: шлюз не может работать, модель останавливается с выводом очередей
name="kill9_chamber"
start_in_background unlimited.txt "$name" 2
kill -9 "$(pgrep -f "$bin/chamber" | head -n 1)"
wait $job
verdict "$name" "Аварийная остановка"
