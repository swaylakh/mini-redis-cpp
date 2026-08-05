#!/bin/sh
#
# Build and run the server. Arguments are passed straight through, e.g.
#
#   ./run.sh --dir /tmp/redis-data --dbfilename dump.rdb

set -e # Exit early if any command fails

cd "$(dirname "$0")"

cmake -B build -S .
cmake --build build

exec ./build/server "$@"
