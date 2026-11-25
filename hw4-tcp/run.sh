# Запуск всей модели на этой машине: ./run.sh <ip> <порт> [<файл_параметров>|-] [<лог>]
# ip – адрес, на котором слушает сервер и к которому подключаются клиенты (0.0.0.0 – все интерфейсы).
# В терминале – монитор: журнал всей системы (он же пишется в <лог без .log>_monitor.log).
# Сервер пишет свой журнал только в лог, каждый клиент – свой журнал в папку <лог без .log>_clients/.
# Суда запускаются по одному через случайные интервалы (seed из параметров), подключение судна = прибытие.
# Ctrl+C получает только сервер: он останавливает модель, клиенты завершаются по его команде.
# INFRA_DELAY=секунды – подключить камеры и ворота позже судов (проверка очереди до начала шлюзования).
if [ $# -lt 2 ]; then
    echo "Использование: $0 <ip> <порт> [<файл_параметров>|-] [<лог>]"
    exit 1
fi

dir=$(cd "$(dirname "$0")" && pwd)
ip=$1
port=$2
params=${3:--}
log=${4:-locks.log}
bin="$dir/bin"
clients_dir="${log%.log}_clients"
monitor_log="${log%.log}_monitor.log"

# к 0.0.0.0 подключиться нельзя: это «все интерфейсы» для сервера, клиенты идут через loopback
server_host=$ip
[ "$ip" = "0.0.0.0" ] && server_host=127.0.0.1

make -s -C "$dir" || exit 1

# из файла параметров нужны: камеры (1), суда (3), интервал прибытия (4), seed (10), тик (12)
# «-» – параметры по умолчанию; cat - ждал бы ввода с клавиатуры, поэтому читаем только настоящий файл
if [ "$params" != "-" ] && [ -f "$params" ]; then
    set -- $(cat "$params")
else
    set --
fi
chambers=${1:-3}
ships=${3:-10}
interval=${4:-3}
seed=${10:-1}
tick=${12:-500}

trap '' INT
mkdir -p "$clients_dir"
rm -f "$clients_dir"/*.log 2> /dev/null

"$bin/server" "$ip" "$port" "$params" "$log" > /dev/null &
server=$!

# монитор подключается первым, чтобы показать всю историю с начала
"$bin/monitor" "$server_host" "$port" "$monitor_log" &
waited=0
until grep -q "Монитор подключён" "$monitor_log" 2> /dev/null || [ $waited -ge 100 ]; do
    sleep 0.02
    waited=$((waited + 1))
done

launch_infrastructure() {
    chamber_id=0
    while [ $chamber_id -lt "$chambers" ]; do
        "$bin/chamber" "$server_host" "$port" $chamber_id > "$clients_dir/chamber_$chamber_id.log" 2>&1 &
        "$bin/gate" "$server_host" "$port" $chamber_id 0 > "$clients_dir/gate_${chamber_id}_upper.log" 2>&1 &
        "$bin/gate" "$server_host" "$port" $chamber_id 1 > "$clients_dir/gate_${chamber_id}_lower.log" 2>&1 &
        chamber_id=$((chamber_id + 1))
    done
}

# 0 судов в параметрах – суда приходят, пока модель не остановится
launch_ships() {
    count=$ships
    [ "$count" -eq 0 ] && count=1000000

    awk -v seed="$seed" -v count="$count" -v max="$interval" \
        'BEGIN { srand(seed); for (i = 0; i < count; i++) print int(rand() * (max + 1)) }' |
    {
        ship_number=0
        while read -r delay_ticks; do
            sleep "$(awk -v ticks="$delay_ticks" -v tick="$tick" 'BEGIN { print ticks * tick / 1000 }')"
            kill -0 "$server" 2> /dev/null || break
            "$bin/ship" "$server_host" "$port" --seed $((seed * 1000 + ship_number)) \
                > "$clients_dir/ship_launch_$ship_number.log" 2>&1 &
            ship_number=$((ship_number + 1))
        done
    }
}

launch_ships &
launcher=$!
sleep "${INFRA_DELAY:-0}"
launch_infrastructure

wait "$server"
status=$?
kill "$launcher" 2> /dev/null
wait
exit $status
