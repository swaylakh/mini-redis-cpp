# Mini Redis

A Redis server implemented from scratch in C++23 — no Redis libraries, no networking framework.
Real `redis-cli` connects to it and issues commands over the standard RESP wire protocol.

## What it does

- **RESP protocol** — parses both RESP arrays and inline commands; replies with simple strings,
  bulk strings, null bulk strings, and arrays
- **Single-threaded `epoll` event loop** — handles thousands of concurrent connections without
  threads or locks, matching 80-100% of real Redis throughput on benchmarks
- **Key expiration** — `SET key value PX <ms>` with absolute deadlines, expired keys reclaimed
  lazily on access
- **RDB snapshots** — loads and saves `.rdb` files with CRC64-ECMA-182 checksums, verified on
  startup
- **Master-replica replication** — full PSYNC handshake, RDB state transfer over the wire, and
  live command propagation to connected replicas

### Commands

| Command | Behaviour |
|---|---|
| `PING` | `+PONG` |
| `ECHO <msg>` | echoes the argument back as a bulk string |
| `SET <key> <value> [PX <ms>]` | stores a value, optionally with a millisecond TTL |
| `GET <key>` | returns the value, or a null bulk string if missing or expired |
| `KEYS *` | returns all live keys, sweeping expired ones as it walks |
| `CONFIG GET dir\|dbfilename` | returns the requested config parameter |
| `DEL <key> [key ...]` | deletes one or more keys, returns count deleted |
| `EXISTS <key>` | returns 1 if key exists, 0 otherwise |
| `EXPIRE <key> <seconds>` | sets a TTL on an existing key |
| `TTL <key>` | remaining lifetime in seconds (-1 = no expiry, -2 = missing) |
| `PTTL <key>` | remaining lifetime in milliseconds |
| `SAVE` | writes an RDB snapshot to disk |
| `INFO replication` | reports role, replication ID, and offset |

## Running it

Requires `cmake` and a C++23 compiler on a POSIX system (Linux, macOS, or WSL — the networking
code uses POSIX sockets directly). No external libraries.

```sh
# Start as master
./run.sh --port 6379 --dir /tmp/redis-data --dbfilename dump.rdb

# Start a replica in another terminal
./run.sh --port 6380 --replicaof 127.0.0.1 6379
```

Then, from another shell:

```sh
redis-cli -p 6379 SET foo bar PX 5000
redis-cli -p 6380 GET foo              # reads from replica
redis-cli -p 6379 KEYS '*'
```

If the `.rdb` file at `<dir>/<dbfilename>` exists it is loaded into memory before the server accepts
connections; if not, the server starts with an empty keyspace.

## How it works

```
src/
  server.cpp        epoll event loop, CLI parsing, socket setup
  command.cpp/hpp    RESP parsing, command dispatch, write propagation
  replication.cpp/hpp  PSYNC handshake, RDB transfer to/from master
  rdb.cpp/hpp        RDB snapshot read/write, CRC64 checksum
  server.hpp         shared types (Client, Entry) and extern globals
```

**Storage** is an `unordered_map<string, Entry>` where `Entry::expires_at` is a
`system_clock::time_point`. Keys without a TTL use `time_point::max()` as a sentinel, so every read
is one unconditional comparison rather than an optional check.

**Networking** uses a single-threaded `epoll` event loop with non-blocking sockets. Each client
gets a per-fd read buffer for incremental RESP parsing. Responses are batched into a single
`send()` call per event, supporting pipelined commands efficiently.

**Expiration is lazy.** `GET` erases an entry it finds expired before reporting a miss, and `KEYS *`
sweeps the map as it builds its reply. There is no background expiry cycle.

**RDB** (`rdb.cpp`) handles both reading and writing. Lengths use Redis's variable-length prefix
encoding — the top two bits of the first byte select a 6-bit, 14-bit, or 32-bit length, or mark
a special string encoding for small integers. The CRC64-ECMA-182 checksum is computed over the
full file and verified on load.

**Replication** follows the Redis PSYNC protocol. A replica connects to the master, completes the
PING → REPLCONF → PSYNC handshake, receives an RDB snapshot (built in memory via `build_rdb()`),
and then applies propagated write commands as they arrive through the epoll loop. Replicas reject
direct writes with `-READONLY`.

## Benchmark results

Measured with `redis-benchmark` on WSL (same machine, loopback):

| Test (50 clients, 50k requests) | Mini Redis | Real Redis | Ratio |
|---|---|---|---|
| PING_INLINE | 109,649 | 124,688 | 88% |
| SET | 105,485 | 135,501 | 78% |
| GET | 106,838 | 137,741 | 78% |

## Current limitations

Scoped deliberately — this implements the string-keyspace subset:

- **String values only.** Lists, hashes, sets, and sorted sets are not implemented, so their RDB
  value types stop the parser. LZF-compressed strings are not decompressed.
- **No partial resync.** Replica reconnection always triggers a full RDB transfer (no replication
  backlog).
- No `WAIT`, transactions, pub/sub, streams, or RESP3.
