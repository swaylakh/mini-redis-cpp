# Mini Redis

A Redis server implemented from scratch in C++23 — no Redis libraries, no networking framework.
Real `redis-cli` connects to it and issues commands over the standard RESP wire protocol.

## What it does

- **RESP protocol** — parses client commands as RESP arrays of bulk strings; replies with simple
  strings, bulk strings, null bulk strings, and arrays
- **Concurrent clients** — one detached `std::thread` per accepted connection
- **Key expiration** — `SET key value PX <ms>` with absolute deadlines, expired keys reclaimed
  lazily on access
- **RDB snapshot loading** — parses a real Redis `.rdb` file byte by byte on startup and restores
  the keyspace, including per-key expiry timestamps

### Commands

| Command | Behaviour |
|---|---|
| `PING` | `+PONG` |
| `ECHO <msg>` | echoes the argument back as a bulk string |
| `SET <key> <value> [PX <ms>]` | stores a value, optionally with a millisecond TTL |
| `GET <key>` | returns the value, or a null bulk string if missing or expired |
| `KEYS *` | returns all live keys, sweeping expired ones as it walks |
| `CONFIG GET dir\|dbfilename` | returns the requested config parameter |

## Running it

Requires `cmake` and a C++23 compiler on a POSIX system (Linux, macOS, or WSL — the networking
code uses POSIX sockets directly). No external libraries.

```sh
./run.sh --dir /tmp/redis-data --dbfilename dump.rdb
```

Listens on `6379`. Then, from another shell:

```sh
redis-cli SET foo bar PX 5000
redis-cli GET foo
redis-cli KEYS '*'
```

If the `.rdb` file at `<dir>/<dbfilename>` exists it is loaded into memory before the server accepts
connections; if not, the server starts with an empty keyspace.

## How it works

```
src/
  Server.cpp    socket setup, accept loop, RESP parsing, command dispatch, expiry checks
  rdb.cpp       RDB file parser — length encodings, integer-encoded strings, expiry opcodes
  rdb.hpp       Entry { value, expires_at }
```

**Storage** is an `unordered_map<string, Entry>` where `Entry::expires_at` is a
`system_clock::time_point`. Keys without a TTL use `time_point::max()` as a sentinel, so every read
is one unconditional comparison rather than an optional check.

**Expiration is lazy.** `GET` erases an entry it finds expired before reporting a miss, and `KEYS *`
sweeps the map as it builds its reply. There is no background expiry cycle.

**RDB parsing** (`rdb.cpp`) skips the 9-byte header, then dispatches on opcode bytes using `peek()`
so unconsumed bytes stay in the stream: `0xFA` aux fields, `0xFE` db selector, `0xFB` resizedb,
`0xFD`/`0xFC` expiry in seconds/milliseconds, `0x00` string key-value pairs, `0xFF` EOF. Lengths use
Redis's prefix encoding — the top two bits of the first byte select a 6-bit, 14-bit, or 32-bit
length, or mark a special string encoding, which is how integer-encoded strings (`int8`/`int16`/`int32`)
are decoded. Expiry opcodes precede the key they apply to, so the pending deadline is held across
loop iterations and cleared only once it has been attached to a key.

## Current limitations

Scoped deliberately — this implements the string-keyspace portion of the challenge:

- **RDB is read-only.** No `SAVE`/`BGSAVE`, and the trailing CRC64 checksum is not verified.
- **String values only.** Lists, hashes, sets, and sorted sets are not implemented, so their RDB
  value types stop the parser. LZF-compressed strings are not decompressed.
- **The keyspace map is not mutex-guarded**, so concurrent writes from multiple client threads race.
  The fix is a `shared_mutex` (or sharded locks); the structural alternative is Redis's own design —
  a single-threaded `epoll` event loop, which removes the shared-state problem entirely.
- **Bulk strings are read line-wise** rather than by their declared byte length, so values
  containing `\r\n` are not handled binary-safely.
- No `DEL`, `TTL`, `EXPIRE`, transactions, replication, pub/sub, or RESP3.
