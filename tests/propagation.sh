#!/bin/bash
# Verify the master pushes write commands down an established replica link,
# by impersonating a replica and reading the raw propagated bytes.
. "$(dirname "$0")/lib.sh"
build

run_in_netns << 'TEST'
. /tmp/miniredis-lib.sh
start_server

# fd 4 plays the replica: full handshake, then left open to receive the stream.
exec 4<>/dev/tcp/127.0.0.1/6379
printf '*1\r\n$4\r\nPING\r\n' >&4
sleep 0.1; read -r -t 1 line <&4
printf '*3\r\n$8\r\nREPLCONF\r\n$14\r\nlistening-port\r\n$4\r\n6380\r\n' >&4
sleep 0.1; read -r -t 1 line <&4
printf '*3\r\n$8\r\nREPLCONF\r\n$4\r\ncapa\r\n$6\r\npsync2\r\n' >&4
sleep 0.1; read -r -t 1 line <&4
printf '*3\r\n$5\r\nPSYNC\r\n$1\r\n?\r\n$2\r\n-1\r\n' >&4
sleep 0.3

read -r -t 1 line <&4                      # +FULLRESYNC
read -r -t 1 rdb_prefix <&4                # $<len>
rdb_len=$(echo "$rdb_prefix" | tr -d '$\r\n')
dd bs=1 count="$rdb_len" <&4 >/dev/null 2>&1   # drain the snapshot
echo "Replica handshake complete ($rdb_len byte RDB), listening for propagation..."

# A separate ordinary client issues the write.
send_cmd '*3\r\n$3\r\nSET\r\n$5\r\nhello\r\n$5\r\nworld\r\n' >/dev/null
sleep 0.2

# Whatever arrives on fd 4 now is the propagated command stream.
replica_data=""
while IFS= read -r -t 1 line <&4; do replica_data+="$line "; done
echo "Replica received: $replica_data"

assert_contains "$replica_data" "SET"   "replica received SET command"
assert_contains "$replica_data" "hello" "replica received key 'hello'"
assert_contains "$replica_data" "world" "replica received value 'world'"

exec 4<&-
stop_servers
report
TEST
