#!/usr/bin/env bash
# Start one echo server, run the load generator against it, and print the
# limits the run happened under, the load generator's report, and the
# server's peak memory.
#
#   scripts/bench.sh <model> [load_gen options...]
#
# Expects the `bench` preset to be built. Meant to run inside the container:
#
#   scripts/docker.sh --ulimit nofile=65536:65536 -- \
#       scripts/bench.sh epoll --connections 10000
set -euo pipefail

model=${1:?usage: bench.sh <threads|select|poll|epoll|epoll-et|io_uring> [load_gen options...]}
shift

build=${C1M_BUILD_DIR:-build-bench}
port=${C1M_PORT:-9000}

echo "kernel:    $(uname -r)"
echo "nofile:    $(ulimit -n)"
echo "ports:     $(tr '\t' '-' </proc/sys/net/ipv4/ip_local_port_range)"
echo "somaxconn: $(cat /proc/sys/net/core/somaxconn)"

"$build/app/echo_server" "$model" --port "$port" &
server=$!
trap 'kill "$server" 2>/dev/null; wait "$server" 2>/dev/null || true' EXIT

until ss --tcp --listening --numeric --no-header "sport = :$port" | grep -q .; do
  if ! kill -0 "$server" 2>/dev/null; then
    echo "bench.sh: the server exited before it was listening" >&2
    exit 1
  fi
  sleep 0.05
done

status=0
"$build/benchmark/load_gen" --port "$port" "$@" || status=$?

# VmHWM is the most physical memory the process has held at any one time. It
# does not include what the kernel holds for the sockets themselves.
if [[ -r /proc/$server/status ]]; then
  awk '/^VmHWM:/ { printf "server:    peak resident memory %d kB\n", $2 }' \
    "/proc/$server/status"
else
  echo "server:    exited during the run"
fi
exit "$status"
