#!/bin/bash
# RDB round-trip: SET -> SAVE -> restart -> keys survived, plus CRC64 detection.
. "$(dirname "$0")/lib.sh"
build

run_in_netns << 'TEST'
. /tmp/miniredis-lib.sh
DBDIR=/tmp/rdb_test
rm -rf $DBDIR; mkdir -p $DBDIR

echo "=== Phase 1: SET keys and SAVE ==="
start_server --dir $DBDIR --dbfilename dump.rdb

# Three SETs then a SAVE, all on one connection — expect 4x +OK.
exec 3<>/dev/tcp/127.0.0.1/6379
printf '*3\r\n$3\r\nSET\r\n$4\r\nname\r\n$6\r\nSwayam\r\n' >&3; sleep 0.05
printf '*3\r\n$3\r\nSET\r\n$5\r\nhello\r\n$5\r\nworld\r\n' >&3; sleep 0.05
printf '*3\r\n$3\r\nSET\r\n$6\r\nnumber\r\n$5\r\n12345\r\n' >&3; sleep 0.05
printf '*1\r\n$4\r\nSAVE\r\n' >&3; sleep 0.1
resp=""; while IFS= read -r -t 1 line <&3; do resp+="$line "; done
exec 3<&-

count=$(echo "$resp" | grep -o "+OK" | wc -l)
if [ "$count" -eq 4 ]; then
	echo "PASS: SET x3 + SAVE all returned +OK"; PASS=$((PASS+1))
else
	echo "FAIL: expected 4x +OK, got $count in [$resp]"; FAIL=$((FAIL+1))
fi

if [ -f "$DBDIR/dump.rdb" ]; then
	echo "PASS: dump.rdb created ($(wc -c < $DBDIR/dump.rdb) bytes)"; PASS=$((PASS+1))
else
	echo "FAIL: dump.rdb not found"; FAIL=$((FAIL+1))
fi
stop_servers

echo "=== Phase 2: Restart and verify keys survived ==="
start_server --dir $DBDIR --dbfilename dump.rdb
assert_contains "$(send_cmd '*2\r\n$3\r\nGET\r\n$4\r\nname\r\n')"   "Swayam" "GET name survived restart"
assert_contains "$(send_cmd '*2\r\n$3\r\nGET\r\n$5\r\nhello\r\n')"  "world"  "GET hello survived restart"
assert_contains "$(send_cmd '*2\r\n$3\r\nGET\r\n$6\r\nnumber\r\n')" "12345"  "GET number survived restart"
assert_contains "$(send_cmd '*2\r\n$4\r\nKEYS\r\n$1\r\n*\r\n')"     '\*3'    "KEYS * returned 3 keys"
stop_servers

echo "=== Phase 3: expiry survives the round-trip ==="
start_server --dir $DBDIR --dbfilename dump.rdb
# 60s TTL is long enough that it must still be live after the restart.
exec 3<>/dev/tcp/127.0.0.1/6379
printf '*5\r\n$3\r\nSET\r\n$7\r\ntempkey\r\n$8\r\ntempval!\r\n$2\r\nPX\r\n$5\r\n60000\r\n' >&3; sleep 0.05
printf '*1\r\n$4\r\nSAVE\r\n' >&3; sleep 0.1
while IFS= read -r -t 1 line <&3; do :; done
exec 3<&-
stop_servers

start_server --dir $DBDIR --dbfilename dump.rdb
assert_contains "$(send_cmd '*2\r\n$3\r\nGET\r\n$7\r\ntempkey\r\n')" \
	"tempval!" "key with PX expiry survived round-trip"
stop_servers

echo "=== Phase 4: corrupted RDB trips the CRC64 check ==="
# Flip one byte mid-file so the stored footer no longer matches.
python3 -c "
d = open('$DBDIR/dump.rdb','rb').read()
mid = len(d)//2
d = d[:mid] + bytes([d[mid]^0xFF]) + d[mid+1:]
open('$DBDIR/dump.rdb','wb').write(d)
"
STDERR=$("$BIN" --dir $DBDIR --dbfilename dump.rdb 2>&1 &
	sleep 0.5; kill %1 2>/dev/null; wait 2>/dev/null)
assert_contains "$STDERR" "CRC64 mismatch" "CRC64 mismatch detected on corrupted file"

rm -rf $DBDIR
report
TEST
