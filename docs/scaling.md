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
widening it does not get to 100k.

## 100k: source addresses

**What raised it.** `load_gen` gained `--source`, a list of addresses to
connect from, taken in turn. Every address is a port range of its own, so two
of them give 129022 connections. On loopback no setup is needed: Linux answers
to all of 127.0.0.0/8. Each socket is bound to its address with
`IP_BIND_ADDRESS_NO_PORT` set, which leaves the port for `connect()` to pick
against the whole four-tuple, the same choice an unbound socket gets.

```bash
scripts/docker.sh --ulimit nofile=262144:262144 \
    --sysctl net.ipv4.ip_local_port_range="1024 65535" -- \
    scripts/bench.sh epoll --connections 100000 --duration 2 \
    --source 127.0.0.1-127.0.0.2
```

```
connect:   100000 requested, 100000 established, 0 failed in 79.034 s
           time to connect: p50 2203 us, p99 597582 us, max 689024 us
```

**What was still slow.** Every connection was made, but 79 seconds is the
same cost the failed runs paid. Picking a port starts somewhere in the range
and walks until it finds a free one, so the walk lengthens as the range
fills, and by the end each address was 78% full. The time to connect shows it
as a tail: p99 at 600 ms against a p50 of 2 ms. Four addresses, 39% full at
the end, took a seventieth of the time:

```bash
scripts/docker.sh --ulimit nofile=262144:262144 \
    --sysctl net.ipv4.ip_local_port_range="1024 65535" -- \
    scripts/bench.sh epoll --connections 100000 --duration 2 \
    --source 127.0.0.1-127.0.0.4
```

| Measure                    | Value       |
| -------------------------- | ----------- |
| Established                | 100000      |
| Connect phase              | 1.1 s       |
| Time to connect, p99       | 3.0 ms      |
| Request latency, p50 / p99 | 45 / 185 us |
| Server peak resident       | 7600 kB     |

The server's resident memory is up 3.5 MB on 10k, about 40 bytes for each
connection, as before.

## 1M: the ceiling on open files, and memory

```bash
scripts/docker.sh --ulimit nofile=1048576:1048576 \
    --sysctl net.ipv4.ip_local_port_range="1024 65535" -- \
    scripts/bench.sh epoll --connections 1000000 --duration 2 \
    --source 127.0.0.1-127.0.0.32
```

| Measure                    | Value        |
| -------------------------- | ------------ |
| Established                | 1000000      |
| Connect phase              | 18.8 s       |
| Time to connect, p50 / p99 | 2.3 / 6.5 ms |
| Request latency, p50 / p99 | 22 / 136 us  |
| Server peak resident       | 73824 kB     |

**What was in the way.** Only the open-file limit, again, and this time at
the top of its range. `fs.nr_open` is the most descriptors the kernel lets
any process have, 1048576 here, and a million sockets plus a handful of
other descriptors fits under it. One more is refused, and Docker passes the
refusal on:

```
docker run --ulimit nofile=1100000:1100000 ...
error setting rlimit type 7: operation not permitted
```

`fs.nr_open` is not one of the sysctls a container can set, so past this
point the host has to be changed, or the connections split between
processes.

Thirty-two source addresses, each 48% full at the end, kept the connect
phase to 19 seconds, 53k connections a second. `ss -s` counted the result
from inside the container, where both ends of every connection live:

```
TCP:   2000016 (estab 2000000, closed 15, orphaned 0, timewait 0)
```

**Memory.** The server's 74 MB is about 70 bytes for each connection. The
kernel's memory for the sockets is the larger cost and is not in that number.
Holding the million connections for 30 seconds with `--hold 30`, and reading
`free -m` in the same container before and during:

| `free -m`  | Before | During |
| ---------- | ------ | ------ |
| used       | 2973   | 9691   |
| buff/cache | 1378   | 5052   |
| free       | 11809  | 1591   |

Two million sockets, one at each end, took 10.2 GB, about 5 kB each.
`/proc/net/sockstat` reported TCP `mem` at 2717 pages both times: that
figure, which `net.ipv4.tcp_mem` limits, counts data queued in socket
buffers, and an idle connection queues none. The 5 kB is the socket
structures themselves. This machine has 16 GB, so memory is the next limit
after `fs.nr_open`, and it would come before two million.
