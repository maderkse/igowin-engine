#!/bin/bash
#
# igowin.sh - start or stop the igowin web game on the phone
#
#   ./igowin.sh            start engine + web server, keep the phone awake, open the browser
#   ./igowin.sh start -n   same, without opening the browser
#   ./igowin.sh stop       stop both and release the wake lock
#   ./igowin.sh quit       stop, and also send Chrome away to the home screen
#   ./igowin.sh status     show whether they are running
#
# Runs inside the Ubuntu proot under Termux; Termux's own commands
# (wake lock, open url) are called through their full path.

DIR=$(cd "$(dirname "$0")" && pwd)
TERMUX_BIN=/data/data/com.termux/files/usr/bin
LOGS="$DIR/logs"
URL=http://127.0.0.1:8080
ENGINE_PORT=9999

engine_pids() { pgrep -f "^./build/igowin-gtp . --listen $ENGINE_PORT"; }
web_pids() { pgrep -f "^python3 webgtp.py"; }

termux() {
	# run a Termux helper if it exists (not available when run outside Termux)
	[ -x "$TERMUX_BIN/$1" ] && "$TERMUX_BIN/$@"
}

build_engine() {
	echo "Building the engine (first run only)..."
	mkdir -p "$DIR/build"
	for f in "$DIR"/*.c; do
		case "$(basename "$f")" in g2pat-old.c|g2check.c|sample.c) continue;; esac
		gcc -O2 -fsigned-char -w -c -o "$DIR/build/$(basename "${f%.c}").o" "$f" || return 1
	done
	gcc -O2 -o "$DIR/build/igowin-gtp" "$DIR"/build/*.o -lm
}

wait_for() {
	# wait_for <seconds> <command...>: retry until the command succeeds
	local n=$1; shift
	for _ in $(seq "$n"); do
		"$@" >/dev/null 2>&1 && return 0
		sleep 1
	done
	return 1
}

engine_up() { (exec 3<>/dev/tcp/127.0.0.1/$ENGINE_PORT) 2>/dev/null; }
web_up() { curl -s "$URL/api/status" | grep -q '"connected": true'; }

stop() {
	local pids
	pids="$(web_pids) $(engine_pids)"
	if [ -n "${pids// /}" ]; then
		kill $pids 2>/dev/null
		echo "Stopped igowin."
	fi
}

start() {
	cd "$DIR" || exit 1
	[ -x build/igowin-gtp ] || build_engine || { echo "Build failed."; exit 1; }
	stop
	sleep 1
	mkdir -p "$LOGS"

	termux termux-wake-lock && echo "Wake lock on: Termux keeps running in the background."

	nohup ./build/igowin-gtp . --listen $ENGINE_PORT > "$LOGS/engine.log" 2>&1 &
	if ! wait_for 15 engine_up; then
		echo "The engine did not start, see $LOGS/engine.log"
		exit 1
	fi

	nohup python3 webgtp.py > "$LOGS/web.log" 2>&1 &
	if ! wait_for 15 web_up; then
		echo "The web server did not start, see $LOGS/web.log"
		exit 1
	fi

	echo "igowin is running: $URL"
	[ "$1" = "-n" ] || termux termux-open-url "$URL"
}

case "${1:-start}" in
	start)  start "$2" ;;
	stop)   stop; termux termux-wake-unlock && echo "Wake lock off." ;;
	quit)
		stop; termux termux-wake-unlock
		# a web page may not close its own tab, so bring up the home screen instead
		termux am start -a android.intent.action.MAIN -c android.intent.category.HOME > /dev/null
		;;
	status)
		if engine_up; then echo "engine: running"; else echo "engine: stopped"; fi
		if web_up; then echo "web:    running ($URL)"; else echo "web:    stopped"; fi
		;;
	*)      echo "usage: $0 [start [-n] | stop | quit | status]"; exit 1 ;;
esac
