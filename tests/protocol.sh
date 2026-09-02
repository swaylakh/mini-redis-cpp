#!/bin/bash
# Protocol robustness: malformed RESP must not take the server down.
# A single client sending garbage should never affect other connections.
. "$(dirname "$0")/lib.sh"
build

run_in_netns << 'TEST'
. /tmp/miniredis-lib.sh
start_server

# Send raw bytes, then check the server is still serving other clients.
# $1 = bytes to send, $2 = label
survives(){
	exec 3<>/dev/tcp/127.0.0.1/6379 2>/dev/null
	printf "$1" >&3 2>/dev/null
	sleep 0.2
	exec 3<&- 2>/dev/null
	# A fresh connection must still get a PONG.
	if echo "$(send_cmd '*1\r\n$4\r\nPING\r\n')" | grep -q "PONG"; then
		echo "PASS: survives $2"; PASS=$((PASS+1))
	else
		echo "FAIL: server died on $2"; FAIL=$((FAIL+1))
	fi
}

echo "=== malformed RESP ==="
survives '*abc\r\n'                          "non-numeric array count"
survives '*3\r\n$xyz\r\n'                    "non-numeric bulk length"
survives '*-1\r\n'                           "negative array count"
survives '*1\r\n$-5\r\nhello\r\n'            "negative bulk length"
survives '*99999999999999999999\r\n'         "array count overflowing int"
survives '*1\r\n$99999999999999999999\r\n'   "bulk length overflowing int"

echo "=== malformed command arguments ==="
survives '*5\r\n$3\r\nSET\r\n$1\r\nk\r\n$1\r\nv\r\n$2\r\nPX\r\n$3\r\nabc\r\n' "SET PX with non-numeric TTL"
survives '*3\r\n$6\r\nEXPIRE\r\n$1\r\nk\r\n$3\r\nabc\r\n'                     "EXPIRE with non-numeric seconds"

echo "=== inline commands still work ==="
# Inline form is part of RESP and is what redis-benchmark uses for PING.
assert_contains "$(send_cmd 'PING\r\n')" "PONG" "inline PING replies PONG"
assert_contains "$(send_cmd 'ECHO hi\r\n')" "hi" "inline ECHO echoes argument"

stop_servers
report
TEST
