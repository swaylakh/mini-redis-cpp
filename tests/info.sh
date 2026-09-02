#!/bin/bash
# INFO replication fields and the REPLCONF handshake acknowledgements.
. "$(dirname "$0")/lib.sh"
build

run_in_netns << 'TEST'
. /tmp/miniredis-lib.sh
start_server

echo "=== INFO replication ==="
resp=$(send_cmd '*2\r\n$4\r\nINFO\r\n$11\r\nreplication\r\n')
assert_contains "$resp" "role:master" "role is master"
assert_matches "$resp" "master_replid:[0-9a-f]{40}" "replid is 40 hex chars"
assert_contains "$resp" "master_repl_offset:0" "offset is 0"

echo "=== REPLCONF handshake ==="
# Both REPLCONF forms are sent on one connection; each should get its own +OK.
exec 3<>/dev/tcp/127.0.0.1/6379
printf '*3\r\n$8\r\nREPLCONF\r\n$14\r\nlistening-port\r\n$4\r\n6380\r\n' >&3
sleep 0.1
printf '*3\r\n$8\r\nREPLCONF\r\n$4\r\ncapa\r\n$6\r\npsync2\r\n' >&3
sleep 0.1
resp=""
while IFS= read -r -t 1 line <&3; do resp+="$line "; done
exec 3<&-

count=$(echo "$resp" | grep -o "+OK" | wc -l)
if [ "$count" -eq 2 ]; then
	echo "PASS: both REPLCONF commands got +OK"; PASS=$((PASS+1))
else
	echo "FAIL: expected 2x +OK, got $count in [$resp]"; FAIL=$((FAIL+1))
fi

stop_servers
report
TEST
