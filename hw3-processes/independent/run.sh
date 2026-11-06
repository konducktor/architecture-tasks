#!/bin/sh
# Запуск всех участников как независимых процессов: ./run.sh [файл_параметров|-] [лог]
# Переменная REVERSE=1 запускает участников в обратном порядке (сначала суда, потом диспетчер).
dir=$(cd "$(dirname "$0")" && pwd)
params=${1:--}
log=${2:-locks.log}
bin="$dir/bin"

make -s -C "$dir" || exit 1
"$bin/dispatcher" -c > /dev/null # остатки прошлого запуска

# число камер и судов берётся из тех же параметров (по умолчанию 3 и 10)
set -- $(cat "$params" 2> /dev/null)
chambers=${1:-3}
ships=${3:-10}

trap '' INT # Ctrl+C получают участники, скрипт дожидается их завершения
pids=""

launch_dispatcher() {
    "$bin/dispatcher" "$params" "$log" &
    pids="$pids $!"
}

launch_chambers() {
    c=0
    while [ $c -lt "$chambers" ]; do
        "$bin/chamber" "$params" "$log" $c &
        pids="$pids $!"
        "$bin/gate" "$params" "$log" $c 0 &
        pids="$pids $!"
        "$bin/gate" "$params" "$log" $c 1 &
        pids="$pids $!"
        c=$((c + 1))
    done
}

launch_ships() {
    i=0
    while [ $i -lt "$ships" ]; do
        "$bin/ship" "$params" "$log" $i &
        pids="$pids $!"
        i=$((i + 1))
    done
}

if [ -n "$REVERSE" ]; then
    launch_ships; launch_chambers; launch_dispatcher
else
    launch_dispatcher; launch_chambers; launch_ships
fi

status=0
for pid in $pids; do wait "$pid" || status=1; done
exit $status
