# Scaling log

Each step here asks `load_gen` to hold more connections open against the
`epoll` server, over loopback, inside one container. The entry for a step
records the limit that broke, how the break showed, and what raised it. Every
number comes from a single run, so it gives the shape of things, not a
measurement to quote.

Every run is `scripts/bench.sh epoll --connections N --duration 2` with the
`docker run` flags shown, so each step can be repeated from its own entry.
All of them ran on kernel `6.18.54.1-microsoft-standard-WSL2`.

## Baseline: 500 connections

```bash
scripts/docker.sh scripts/bench.sh epoll --connections 500 --duration 2
```

| Measure                    | Value   |
| -------------------------- | ------- |
| Established                | 500     |
| Time to connect, p99       | 3.0 ms  |
| Request latency, p50 / p99 | 30 / 113 us |
| Server peak resident       | 3612 kB |

## 10k: open files

At Docker's default limits the run breaks at once.

```bash
scripts/docker.sh scripts/bench.sh epoll --connections 10000 --duration 2
```

```
nofile:    1024
echo_server: accept: Too many open files
connect:   10000 requested, 1018 established, 8982 failed in 0.020 s
           8980 x Too many open files
           2 x Connection reset by peer
server:    exited during the run
```

**What broke.** A process starts with 1024 open file descriptors allowed,
and the server and the load generator share that limit here because they run
in the same container. Each side spent a few on itself and reached its limit
after 1018 sockets. The server's `accept()` failed with `EMFILE`, which it
treats as a limit it cannot work around, so it exited; the two resets are
clients it closed on the way out. The load generator's `socket()` calls failed
the same way.

**What raised it.** The per-process limit, passed to the container:

```bash
scripts/docker.sh --ulimit nofile=65536:65536 -- \
    scripts/bench.sh epoll --connections 10000 --duration 2
```

| Measure                    | Value   |
| -------------------------- | ------- |
| Established                | 10000   |
| Time to connect, p99       | 2.6 ms  |
| Request latency, p50 / p99 | 34 / 174 us |
| Server peak resident       | 4056 kB |

The server's resident memory grew by about 440 kB for 9500 more connections,
under 50 bytes each. The kernel's own memory for the sockets is not in that
number.

## 100k: ephemeral ports

```bash
scripts/docker.sh --ulimit nofile=262144:262144 -- \
    scripts/bench.sh epoll --connections 100000 --duration 2
```

```
ports:     32768-60999
connect:   100000 requested, 28232 established, 71768 failed in 75.868 s
           71768 x Cannot assign requested address
```

**What broke.** Every connection is a distinct (source address, source port,
destination address, destination port) tuple. The load generator uses one
source address and one destination, so it has one port per connection, and
`net.ipv4.ip_local_port_range` offers 28232 of them: the number established.
After that, `connect()` fails with `EADDRNOTAVAIL`.

The connect phase also took 76 seconds, where 10k took a tenth of one. The
kernel finds a free port by searching the range, and the search gets longer as
the range fills; a search that finds nothing is the longest of all, and there
were 71768 of those.

**What raised it.** The range is a per-namespace sysctl, so the container can
widen it:

```bash
scripts/docker.sh --ulimit nofile=262144:262144 \
    --sysctl net.ipv4.ip_local_port_range="1024 65535" -- \
    scripts/bench.sh epoll --connections 100000 --duration 2
```

```
ports:     1024-65535
connect:   100000 requested, 64511 established, 35489 failed in 159.591 s
           35489 x Cannot assign requested address
           time to connect: p50 3498 us, p99 405128 us, max 112499918 us
server:    peak resident memory 7576 kB
```

That is 64512 ports in the range, less port 9000, which the server itself is
listening on. The widened range is the most one source address can give, so
widening it does not get to 100k. The next step needs more source addresses,
and the load generator cannot bind to them yet.
