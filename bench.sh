#!/bin/bash
# Derive the source dir from this script's location - no absolute paths.
SRC="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/src"

g++ -std=c++23 -O2 -pthread $SRC/server.cpp $SRC/command.cpp $SRC/replication.cpp $SRC/rdb.cpp -o /tmp/miniredis 2>&1
if [ $? -ne 0 ]; then echo "BUILD FAILED"; exit 1; fi
echo "BUILD OK"
echo ""

echo "========================================="
echo "  MINI REDIS — 10 clients, 10k requests"
echo "========================================="
unshare -rn bash << 'MINI'
ip link set lo up
/tmp/miniredis >/dev/null 2>&1 &
sleep 0.5
redis-benchmark -p 6379 -c 10 -n 10000 -t ping,set,get --csv 2>/dev/null
kill %1 2>/dev/null; wait 2>/dev/null
MINI

echo ""
echo "========================================="
echo "  REAL REDIS — 10 clients, 10k requests"
echo "========================================="
redis-benchmark -p 6379 -c 10 -n 10000 -t ping,set,get --csv 2>/dev/null

echo ""
echo "========================================="
echo "  MINI REDIS — 50 clients, 50k requests"
echo "========================================="
unshare -rn bash << 'MINI2'
ip link set lo up
/tmp/miniredis >/dev/null 2>&1 &
sleep 0.5
redis-benchmark -p 6379 -c 50 -n 50000 -t ping,set,get --csv 2>/dev/null
kill %1 2>/dev/null; wait 2>/dev/null
MINI2

echo ""
echo "========================================="
echo "  REAL REDIS — 50 clients, 50k requests"
echo "========================================="
redis-benchmark -p 6379 -c 50 -n 50000 -t ping,set,get --csv 2>/dev/null
