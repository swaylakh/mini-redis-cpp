#!/bin/bash
# DEL / EXISTS / TTL / PTTL / EXPIRE behaviour.
. "$(dirname "$0")/lib.sh"
build

run_in_netns << 'TEST'
. /tmp/miniredis-lib.sh
start_server --port 6379

echo "=== DEL ==="
# Seed two keys, then delete both in one call.
send_cmd '*3\r\n$3\r\nSET\r\n$1\r\na\r\n$1\r\n1\r\n' >/dev/null
send_cmd '*3\r\n$3\r\nSET\r\n$1\r\nb\r\n$1\r\n2\r\n' >/dev/null
assert_contains "$(send_cmd '*3\r\n$3\r\nDEL\r\n$1\r\na\r\n$1\r\nb\r\n')" ":2" "DEL 2 keys"
assert_contains "$(send_cmd '*2\r\n$3\r\nDEL\r\n$5\r\nghost\r\n')" ":0" "DEL nonexistent"

echo "=== EXISTS ==="
send_cmd '*3\r\n$3\r\nSET\r\n$4\r\nname\r\n$6\r\nSwayam\r\n' >/dev/null
assert_contains "$(send_cmd '*2\r\n$6\r\nEXISTS\r\n$4\r\nname\r\n')" ":1" "EXISTS returns 1"
assert_contains "$(send_cmd '*2\r\n$6\r\nEXISTS\r\n$5\r\nghost\r\n')" ":0" "EXISTS returns 0"

echo "=== TTL / PTTL ==="
# 30s TTL, so TTL should read ~29-30 and PTTL ~29000-30000.
send_cmd '*5\r\n$3\r\nSET\r\n$4\r\ntemp\r\n$2\r\nhi\r\n$2\r\nPX\r\n$5\r\n30000\r\n' >/dev/null
assert_matches "$(send_cmd '*2\r\n$3\r\nTTL\r\n$4\r\ntemp\r\n')" ":[0-9]" "TTL returns seconds"
assert_matches "$(send_cmd '*2\r\n$4\r\nPTTL\r\n$4\r\ntemp\r\n')" ":[0-9]{4,5}" "PTTL returns ms"
# -1 means the key exists but has no expiry; -2 means no such key.
assert_contains "$(send_cmd '*2\r\n$3\r\nTTL\r\n$4\r\nname\r\n')" ":-1" "TTL -1 for no expiry"
assert_contains "$(send_cmd '*2\r\n$3\r\nTTL\r\n$5\r\nghost\r\n')" ":-2" "TTL -2 for missing key"

echo "=== EXPIRE ==="
assert_contains "$(send_cmd '*3\r\n$6\r\nEXPIRE\r\n$4\r\nname\r\n$2\r\n60\r\n')" ":1" "EXPIRE set on existing key"
assert_matches "$(send_cmd '*2\r\n$3\r\nTTL\r\n$4\r\nname\r\n')" ":5[0-9]|:60" "TTL reflects EXPIRE"
assert_contains "$(send_cmd '*3\r\n$6\r\nEXPIRE\r\n$5\r\nghost\r\n$2\r\n60\r\n')" ":0" "EXPIRE 0 for missing key"

stop_servers
report
TEST
