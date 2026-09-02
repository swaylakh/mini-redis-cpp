#!/bin/bash
# End-to-end master -> replica: role, RDB state transfer, live propagation.
. "$(dirname "$0")/lib.sh"
build

run_in_netns << 'TEST'
. /tmp/miniredis-lib.sh

start_server --port 6379
# Seed a key BEFORE the replica connects, so it can only arrive via the RDB snapshot.
send_cmd '*3\r\n$3\r\nSET\r\n$7\r\npre_key\r\n$9\r\npre_value\r\n' >/dev/null

start_server --port 6380 --replicaof 127.0.0.1 6379
sleep 0.3   # allow the handshake to finish

echo "=== Test 1: INFO replication on replica ==="
assert_contains "$(send_cmd '*2\r\n$4\r\nINFO\r\n$11\r\nreplication\r\n' 6380)" \
	"role:slave" "replica reports role:slave"

echo "=== Test 2: Pre-existing key transferred via RDB ==="
assert_contains "$(send_cmd '*2\r\n$3\r\nGET\r\n$7\r\npre_key\r\n' 6380)" \
	"pre_value" "pre_key transferred via RDB snapshot"

echo "=== Test 3: Live SET on master propagates ==="
send_cmd '*3\r\n$3\r\nSET\r\n$4\r\nname\r\n$6\r\nSwayam\r\n' >/dev/null
sleep 0.3   # give propagation time to land
assert_contains "$(send_cmd '*2\r\n$3\r\nGET\r\n$4\r\nname\r\n' 6380)" \
	"Swayam" "SET on master -> GET on replica"

echo "=== Test 4: Second SET propagates too ==="
send_cmd '*3\r\n$3\r\nSET\r\n$5\r\ncount\r\n$2\r\n42\r\n' >/dev/null
sleep 0.3
assert_contains "$(send_cmd '*2\r\n$3\r\nGET\r\n$5\r\ncount\r\n' 6380)" \
	"42" "SET count=42 propagated"

echo "=== Test 5: replica refuses writes ==="
assert_contains "$(send_cmd '*3\r\n$3\r\nSET\r\n$3\r\nfoo\r\n$3\r\nbar\r\n' 6380)" \
	"READONLY" "replica rejects writes"

stop_servers
report
TEST
