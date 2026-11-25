#!/bin/sh
# Все наборы данных, проверки отключений и сетевых адресов: ./run_tests.sh
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
    ! pgrep -f "$bin/(server|chamber|gate|ship|monitor)" > /dev/null
}

# монитор видел то же, что и сервер: инварианты в его журнале и одинаковая итоговая статистика
monitor_agrees() {
    monitor_log="$logs/$1_monitor.log"
    ./check_monitor_log.sh "$monitor_log" &&
        [ "$(tail -n 1 "$monitor_log")" = "$(tail -n 1 "$logs/$1.log")" ]
}

# итог одного теста: имя, ожидаемая строка в логе сервера и в журнале монитора
verdict() {
    name="$1"; expected="$2"

    if grep -Eq "$expected" "$logs/$name.log" && ./check_log.sh "$logs/$name.log" &&
        grep -Eq "$expected" "$logs/${name}_monitor.log" && monitor_agrees "$name" && no_leftovers; then
        echo "OK   $name"
    else
        echo "FAIL $name"
        pkill -f "$bin/(server|chamber|gate|ship|monitor)"
    fi
    tail -n 1 "$logs/$name.log"
}

start_in_background() {
    $run 127.0.0.1 "$port" "$1" "$logs/$2.log" > /dev/null &
    job=$!
    sleep "$3"
}

for file in basic.txt by_direction.txt large.txt one_chamber.txt small_max_wait.txt timeout.txt; do
    name="${file%.txt}"
    $run 127.0.0.1 "$port" "$file" "$logs/$name.log" > /dev/null

    if [ "$name" = "timeout" ]; then expected="Время моделирования вышло"; else expected="Все .* судов прошли"; fi
    verdict "$name" "$expected"
done

# камеры и ворота подключаются позже судов: суда ждут в очереди
name="late_infrastructure"
INFRA_DELAY=1 $run 127.0.0.1 "$port" basic.txt "$logs/$name.log" > /dev/null
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

# сервер слушает все интерфейсы (0.0.0.0), клиенты подключаются через loopback
name="all_interfaces"
$run 0.0.0.0 "$port" basic.txt "$logs/$name.log" > /dev/null
verdict "$name" "Все .* судов прошли"

# сетевой адрес машины вместо loopback: так же клиенты подключаются с других компьютеров
lan_ip=$(hostname -I 2> /dev/null | awk '{ print $1 }')
[ -z "$lan_ip" ] && lan_ip=$(ipconfig getifaddr en0 2> /dev/null)
name="lan_address"
if [ -n "$lan_ip" ]; then
    $run "$lan_ip" "$port" basic.txt "$logs/$name.log" > /dev/null
    verdict "$name" "Все .* судов прошли"
else
    echo "SKIP $name: у машины нет сетевого адреса"
fi

# у каждого клиента свой журнал, и в нём только его собственные события
name="separate_logs"
clients_dir="$logs/basic_clients"
if [ -s "$clients_dir/chamber_0.log" ] && [ -s "$clients_dir/gate_0_upper.log" ] && [ -s "$clients_dir/ship_launch_0.log" ] &&
    ! grep -q "камера" "$clients_dir/gate_0_upper.log"; then
    echo "OK   $name"
else
    echo "FAIL $name"
fi

# неверный адрес и адрес чужой машины: сервер сразу сообщает об ошибке
name="bad_address"
if ! $bin/server 300.1.1.1 "$port" 2> /dev/null && ! $bin/server 203.0.113.7 "$port" 2> /dev/null; then
    echo "OK   $name"
else
    echo "FAIL $name"
fi

# мониторы подключаются посреди работы: получают снимок состояния; отключение монитора не влияет на модель
name="late_monitors"
start_in_background unlimited.txt "$name" 1.5
$bin/monitor 127.0.0.1 "$port" "$logs/${name}_late.log" > /dev/null &
late_monitor=$!
$bin/monitor 127.0.0.1 "$port" "$logs/${name}_killed.log" > /dev/null &
killed_monitor=$!
sleep 1; kill -9 $killed_monitor
sleep 0.5; kill -INT "$(pgrep -f "$bin/server")"
wait $job; wait $late_monitor
verdict "$name" "Прервано пользователем"
if grep -q "состояние системы на момент подключения" "$logs/${name}_late.log" &&
    grep -q "Прервано пользователем" "$logs/${name}_late.log" &&
    [ "$(tail -n 1 "$logs/${name}_late.log")" = "$(tail -n 1 "$logs/$name.log")" ] &&
    grep -q "монитор: соединение разорвано" "$logs/$name.log"; then
    echo "OK   $name: снимок, итог и отключение монитора"
else
    echo "FAIL $name: снимок, итог и отключение монитора"
fi
