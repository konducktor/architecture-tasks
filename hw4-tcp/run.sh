# Запуск всей модели на этой машине: ./run.sh [<файл_параметров>|-] [<лог>] [<порт>]
# Сервер пишет полный журнал в терминал и в лог, клиенты – свои события в <лог>.clients.log.
# Суда запускаются по одному через случайные интервалы (seed из параметров), подключение судна = прибытие.
# Ctrl+C получает только сервер: он останавливает модель, клиенты завершаются по его команде.
# INFRA_DELAY=секунды – подключить камеры и ворота позже судов (проверка очереди до начала шлюзования).
dir=$(cd "$(dirname "$0")" && pwd)
params=${1:--}
log=${2:-locks.log}
port=${3:-${PORT:-6767}}
bin="$dir/bin"
clients_log="${log%.log}.clients.log"

make -s -C "$dir" || exit 1

# из файла параметров нужны: камеры (1), суда (3), интервал прибытия (4), seed (10), тик (12)
set -- $(cat "$params" 2> /dev/null)
chambers=${1:-3}
ships=${3:-10}
interval=${4:-3}
seed=${10:-1}
tick=${12:-500}

trap '' INT
: > "$clients_log"

"$bin/server" "$params" "$log" "$port" &
server=$!

launch_infrastructure() {
    chamber_id=0
    while [ $chamber_id -lt "$chambers" ]; do
        "$bin/chamber" 127.0.0.1 "$port" $chamber_id >> "$clients_log" 2>&1 &
        "$bin/gate" 127.0.0.1 "$port" $chamber_id 0 >> "$clients_log" 2>&1 &
        "$bin/gate" 127.0.0.1 "$port" $chamber_id 1 >> "$clients_log" 2>&1 &
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
            "$bin/ship" 127.0.0.1 "$port" --seed $((seed * 1000 + ship_number)) >> "$clients_log" 2>&1 &
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
