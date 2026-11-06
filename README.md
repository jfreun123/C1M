# C1M

Working from the C10k problem up to one million concurrent connections, to learn networking.  Built from [cpp-starter](https://github.com/jfreun123/cpp-starter).

## Goals

- [x] Build the same echo server several ways (thread per connection, `select`, `poll`, `epoll`, `io_uring`) so the models can be compared like for like
- [ ] Add a UDP echo server next to the TCP ones
- [x] Push one machine from 10k to 100k to 1M concurrent connections, recording which limit broke at each step and what fixed it
- [x] Write a load generator that can open and hold that many connections
- [ ] Measure every model:  memory per connection, latency, syscalls per request
- [ ] Reproduce each failure (blocked socket, full accept queue, file descriptor exhaustion) and diagnose it with standard Linux tools (`ss`, `lsof`, `strace`, `/proc`)
- [ ] Be able to answer these from code and measurements in this repo:
  - [ ] What is the modern replacement for `netstat`?
  - [ ] What does it mean for a socket to be blocked, and how would you find what it is blocked on?
  - [ ] TCP vs UDP, and when would you use one or the other?
  - [ ] What steps are involved in creating a new connection?
  - [ ] How does `epoll` work, and how would you use it to build a server?
  - [ ] Design an async web server handling 100k concurrent connections (what kind of sockets, how `epoll` works and why it is useful, what `io_uring` does)

## Reproducibility

Everything builds and runs inside one Docker image, through `scripts/docker.sh`. Docker is the only prerequisite.

- **Pinned:** the base image (Ubuntu 24.04, by digest), and with it the compiler, standard library and tools (clang 18, CMake 3.28). The apt packages are not pinned to exact versions.
- **Not pinned:** the kernel. A container runs on the host's kernel, so `scripts/bench.sh` prints `uname -r` with every run.
- **Explicit:** kernel limits. Containers start with Docker's defaults, which include a limit of 1024 open files. Anything an experiment raises goes on the command line that ran it, before a `--`:

  ```bash
  scripts/docker.sh --ulimit nofile=65536:65536 -- scripts/bench.sh epoll --connections 10000
  ```

  `--sysctl` does the same for the network settings that are per container, such as `net.ipv4.ip_local_port_range` and `net.core.somaxconn`.

Docker's default seccomp profile blocks two things this repo uses. ThreadSanitizer cannot start under it, and `io_uring` is refused outright. Both need `--security-opt seccomp=unconfined`, passed the same way. Without it the `io_uring` tests are skipped, and say so.

## Building and testing

```bash
scripts/docker.sh cmake --preset dev
scripts/docker.sh cmake --build --preset dev
scripts/docker.sh --security-opt seccomp=unconfined -- ctest --preset dev
```

| Preset  | What it is                                             |
| ------- | ------------------------------------------------------ |
| `dev`   | Debug build with clang-tidy and cppcheck               |
| `asan`  | `dev` with AddressSanitizer                            |
| `tsan`  | `dev` with ThreadSanitizer                             |
| `bench` | Optimized build, the one `scripts/bench.sh` runs       |

The `tsan` preset runs the test binary while it builds, so the build needs the seccomp flag as well as the tests:

```bash
scripts/docker.sh cmake --preset tsan
scripts/docker.sh --security-opt seccomp=unconfined -- cmake --build --preset tsan
scripts/docker.sh --security-opt seccomp=unconfined -- ctest --preset tsan
```

The same `cmake` and `ctest` commands work without `scripts/docker.sh` on a Linux machine that has the packages in the `Dockerfile` installed. The build directories are shared between the two, so delete `build*/` when switching.

## Running

```bash
scripts/docker.sh cmake --preset bench
scripts/docker.sh cmake --build --preset bench
scripts/docker.sh --ulimit nofile=65536:65536 -- \
    scripts/bench.sh epoll --connections 10000 --rate 10000
```

`scripts/bench.sh <model> [load_gen options]` starts one server, runs the load generator against it over loopback in the same container, and prints the limits the run happened under, the load generator's report, and the server's peak resident memory. The limit each step up in connections ran into, and what raised it, is recorded in [`docs/scaling.md`](docs/scaling.md).

| Model      | Server                                                    | Source                                                   |
| ---------- | --------------------------------------------------------- | -------------------------------------------------------- |
| `threads`  | Blocking sockets, one thread per connection               | [`blocking_server.hpp`](source/blocking_server.hpp)      |
| `select`   | One thread, non-blocking sockets, `select()`              | [`select_server.hpp`](source/select_server.hpp)          |
| `poll`     | One thread, non-blocking sockets, `poll()`                | [`poll_server.hpp`](source/poll_server.hpp)              |
| `epoll`    | One thread, non-blocking sockets, level-triggered `epoll` | [`epoll_server.hpp`](source/epoll_server.hpp)            |
| `epoll-et` | The same, edge-triggered                                  | [`epoll_server.hpp`](source/epoll_server.hpp)            |
| `io_uring` | One thread, completions from `io_uring`                   | [`uring_server.hpp`](source/uring_server.hpp)            |

The `select`, `poll` and `epoll` servers share [`echo_connection.hpp`](source/echo_connection.hpp), so they differ only in how they learn that a socket is ready. The `io_uring` server has the kernel do the reads and writes, and needs the seccomp flag to run. A server that hits a limit it cannot work around (out of file descriptors, a descriptor too large for `select()`) exits and says which one.

To run the two programs by hand, open a shell in the container with `scripts/docker.sh bash`:

```bash
build-bench/app/echo_server epoll --port 9000 &
build-bench/benchmark/load_gen --port 9000 --connections 1000 --hold 60
```

`load_gen` options:

| Option                  | Default     | Meaning                                              |
| ----------------------- | ----------- | ---------------------------------------------------- |
| `--port`                | required    | Server port                                          |
| `--host`                | `127.0.0.1` | Server IPv4 address                                  |
| `--source`              | none        | Addresses to connect from, taken in turn: IPv4 addresses or `A-B` ranges, comma-separated |
| `--connections`         | 1000        | Connections to open                                  |
| `--connect-concurrency` | 256         | Connects in flight at any one time                   |
| `--connect-timeout`     | 10          | Seconds before a connect counts as failed            |
| `--rate`                | 1000        | Requests per second, spread over all the connections |
| `--duration`            | 5           | Seconds of sending requests                          |
| `--size`                | 64          | Bytes per request                                    |
| `--hold`                | 0           | Seconds to stay connected afterwards                 |

It exits non-zero if a connection failed, a request went unanswered, or the server closed a connection.

## Code formatting

```bash
scripts/docker.sh cmake --build build -t format-check
scripts/docker.sh cmake --build build -t format-fix
```

## Project structure

```
C1M/
├── source/                  # Header-only library: the servers and what they share
├── app/                     # echo_server, the binary that runs one of them
├── benchmark/               # load_gen, the load generator
├── test/                    # One Google Test suite, run against every server
├── scripts/
│   ├── docker.sh            # Run a command in the toolchain image
│   └── bench.sh             # One server, one load generator run, one report
├── docs/                    # The scaling log
├── cmake/                   # CMake modules
├── Dockerfile               # The toolchain image
├── CMakeLists.txt
└── CMakePresets.json        # dev, asan, tsan, bench presets
```
