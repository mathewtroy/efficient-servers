#!/usr/bin/env bash
set -euo pipefail

CLIENTS="${1:-100}"
FILE="${2:-../walk3000.pbf}"
PORT="${3:-1234}"

if [ ! -x ./build/efficient_servers ]; then
  echo "Missing ./build/efficient_servers"
  echo "Build the project first:"
  echo "  cmake -S . -B build -DCMAKE_BUILD_TYPE=Release"
  echo "  cmake --build build -j"
  exit 1
fi

if [ ! -f "$FILE" ]; then
  echo "File not found: $FILE"
  exit 1
fi

pkill -f efficient_servers || true

./build/efficient_servers "$PORT" >/dev/null 2>&1 &
SERVER_PID=$!

cleanup() {
  kill "$SERVER_PID" 2>/dev/null || true
  wait "$SERVER_PID" 2>/dev/null || true
}
trap cleanup EXIT

READY=0
for _ in $(seq 1 100); do
  if nc -z localhost "$PORT" >/dev/null 2>&1; then
    READY=1
    break
  fi
  sleep 0.05
done

if [ "$READY" -ne 1 ]; then
  echo "Server did not start listening on port $PORT"
  exit 1
fi

python3 - "$CLIENTS" "$FILE" "$PORT" <<'PY'
import concurrent.futures
import socket
import statistics
import sys
import time
from pathlib import Path

clients = int(sys.argv[1])
path = sys.argv[2]
port = int(sys.argv[3])
payload = Path(path).read_bytes()

def run_one(_):
    t0 = time.perf_counter()
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.connect(("127.0.0.1", port))
    sock.sendall(payload)
    sock.shutdown(socket.SHUT_WR)

    total = 0
    while True:
        chunk = sock.recv(65536)
        if not chunk:
            break
        total += len(chunk)
    sock.close()
    return time.perf_counter() - t0, total

start = time.perf_counter()
with concurrent.futures.ThreadPoolExecutor(max_workers=clients) as executor:
    results = list(executor.map(run_one, range(clients)))
wall = time.perf_counter() - start

times = sorted(t for t, _ in results)
sizes = sorted(set(size for _, size in results))

def percentile(p):
    if not times:
        return 0.0
    index = min(len(times) - 1, int((len(times) - 1) * p / 100))
    return times[index]

print(f"clients: {clients}")
print(f"file: {path}")
print(f"payload_bytes: {len(payload)}")
print(f"wall: {wall:.3f} s")
print(f"throughput: {clients / wall:.1f} clients/s")
print(f"avg: {statistics.mean(times):.3f} s")
print(f"p50: {percentile(50):.3f} s")
print(f"p95: {percentile(95):.3f} s")
print(f"max: {max(times):.3f} s")
print(f"response_sizes: {sizes}")
PY
