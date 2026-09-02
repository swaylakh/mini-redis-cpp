#!/bin/bash
#
# Shared helpers for the mini-redis test scripts.
#
# Every test follows the same shape: build the server, then run the test body
# inside a private network namespace so it can bind 6379 even when a real
# redis-server is already listening on the host.
#
# Usage:
#
#   . "$(dirname "$0")/lib.sh"
#   build
#   run_in_netns << 'TEST'
#     . /tmp/miniredis-lib.sh
#     start_server --port 6379
#     assert_contains "$(send_cmd '*1\r\n$4\r\nPING\r\n')" "PONG" "PING replies PONG"
#     report
#   TEST

# Repo root, derived from this file's location — no absolute paths baked in.
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SRC="$ROOT/src"
BIN=/tmp/miniredis

# Compile every translation unit in src/ into $BIN.
build(){
	echo "=== Building ==="
	g++ -std=c++23 -O2 -pthread \
		"$SRC/server.cpp" "$SRC/command.cpp" "$SRC/replication.cpp" "$SRC/rdb.cpp" \
		-o "$BIN" 2>&1 || { echo "BUILD FAILED"; exit 1; }
	echo "BUILD OK"
	echo ""
}

# Run the test body (read from stdin) inside an unprivileged network namespace
# with loopback up. The library is copied to a fixed path so the heredoc body,
# which runs in a fresh shell, can source it.
run_in_netns(){
	cp "${BASH_SOURCE[0]}" /tmp/miniredis-lib.sh
	local body
	body=$(cat)
	unshare -rn bash -c "ip link set lo up; $body"
}

# --- helpers below are used inside the netns body ---

PASS=0
FAIL=0
SERVER_PIDS=()

# Launch the server in the background and give it time to bind.
# Extra arguments are passed straight through (--port, --replicaof, --dir, ...).
start_server(){
	"$BIN" "$@" >/dev/null 2>&1 &
	SERVER_PIDS+=($!)
	sleep 0.3
}

# Kill every server started by start_server.
stop_servers(){
	kill "${SERVER_PIDS[@]}" 2>/dev/null
	wait 2>/dev/null
	SERVER_PIDS=()
}

# Send one RESP command to a port (default 6379) and echo the whole reply.
# Usage: send_cmd '<resp bytes>' [port]
send_cmd(){
	local port=${2:-6379}
	exec 3<>/dev/tcp/127.0.0.1/"$port"
	printf "$1" >&3
	sleep 0.15
	local resp="" line
	while IFS= read -r -t 1 line <&3; do resp+="$line "; done
	exec 3<&-
	echo "$resp"
}

# Assert that $1 contains the substring $2; $3 is the label printed either way.
assert_contains(){
	if echo "$1" | grep -q -- "$2"; then
		echo "PASS: $3"; PASS=$((PASS+1))
	else
		echo "FAIL: $3 — got [$1]"; FAIL=$((FAIL+1))
	fi
}

# Same, but $2 is an extended regex.
assert_matches(){
	if echo "$1" | grep -qE -- "$2"; then
		echo "PASS: $3"; PASS=$((PASS+1))
	else
		echo "FAIL: $3 — got [$1]"; FAIL=$((FAIL+1))
	fi
}

# Print the tally and exit non-zero if anything failed, so CI can gate on it.
report(){
	echo ""
	echo "============================="
	echo "Results: $PASS passed, $FAIL failed"
	echo "============================="
	[ "$FAIL" -eq 0 ]
}
