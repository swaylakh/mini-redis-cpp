#!/bin/bash
# Wire-level PSYNC: drive the replica handshake by hand and inspect the raw
# FULLRESYNC line and the RDB payload the master streams back.
. "$(dirname "$0")/lib.sh"
build

run_in_netns << 'TEST'
. /tmp/miniredis-lib.sh
start_server

# Put a key in the store so the transferred snapshot is not empty.
send_cmd '*3\r\n$3\r\nSET\r\n$4\r\nname\r\n$6\r\nSwayam\r\n' >/dev/null

# Hold one connection open for the whole handshake — each step is answered
# in order on the same socket.
exec 3<>/dev/tcp/127.0.0.1/6379

printf '*1\r\n$4\r\nPING\r\n' >&3
sleep 0.1; read -r -t 1 line <&3
assert_contains "$line" "PONG" "handshake step 1: PING -> PONG"

printf '*3\r\n$8\r\nREPLCONF\r\n$14\r\nlistening-port\r\n$4\r\n6380\r\n' >&3
sleep 0.1; read -r -t 1 line <&3
assert_contains "$line" "+OK" "handshake step 2: REPLCONF listening-port"

printf '*3\r\n$8\r\nREPLCONF\r\n$4\r\ncapa\r\n$6\r\npsync2\r\n' >&3
sleep 0.1; read -r -t 1 line <&3
assert_contains "$line" "+OK" "handshake step 3: REPLCONF capa"

printf '*3\r\n$5\r\nPSYNC\r\n$1\r\n?\r\n$2\r\n-1\r\n' >&3
sleep 0.3
read -r -t 1 line <&3
assert_matches "$line" "^\+FULLRESYNC [0-9a-f]{40} [0-9]+" "PSYNC -> FULLRESYNC with replid and offset"

# The RDB follows as $<len>\r\n then exactly len raw bytes (no trailing \r\n).
read -r -t 1 rdb_prefix <&3
if echo "$rdb_prefix" | grep -qE '^\$[0-9]+'; then
	echo "PASS: got RDB length prefix"; PASS=$((PASS+1))
	rdb_len=$(echo "$rdb_prefix" | tr -d '$\r\n')
	echo "  RDB size: $rdb_len bytes"
	rdb_data=$(dd bs=1 count="$rdb_len" <&3 2>/dev/null)
	assert_contains "$rdb_data" "REDIS" "RDB starts with REDIS header"
	assert_contains "$rdb_data" "Swayam" "RDB contains key name=Swayam"
else
	echo "FAIL: no RDB prefix — got [$rdb_prefix]"; FAIL=$((FAIL+1))
fi

exec 3<&-
stop_servers
report
TEST
