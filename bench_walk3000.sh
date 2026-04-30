#!/usr/bin/env bash
set -u

SUM=0

if [ ! -x ./build/efficient_servers ]; then
  echo "Missing ./build/efficient_servers"
  echo "Build the project first:"
  echo "  cmake -S . -B build -DCMAKE_BUILD_TYPE=Release"
  echo "  cmake --build build -j"
  exit 1
fi

# ── Portable timed-nc helper ──────────────────────────────────────────────────
# Returns elapsed seconds (3 decimal places) to stdout; exits with nc's status.
timed_nc() {
  local infile="$1" outfile="$2" port="$3"
  if [[ "$(uname)" == "Linux" ]] && /usr/bin/time -f "%e" true 2>/dev/null; then
    # Linux with GNU time
    { /usr/bin/time -f "%e" nc -N localhost "$port" < "$infile" > "$outfile"; } 2>&1 | tail -n 1
  else
    # macOS / fallback: measure with Python (ms precision)
    python3 - "$infile" "$outfile" "$port" <<'PYEOF'
import sys, socket, time, os

infile, outfile, port = sys.argv[1], sys.argv[2], int(sys.argv[3])
data = open(infile, 'rb').read()

sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
sock.connect(('localhost', port))

t0 = time.perf_counter()
sock.sendall(data)
sock.shutdown(socket.SHUT_WR)   # signal EOF to server

out = bytearray()
while True:
    chunk = sock.recv(65536)
    if not chunk:
        break
    out.extend(chunk)
sock.close()
elapsed = time.perf_counter() - t0

open(outfile, 'wb').write(bytes(out))
print(f'{elapsed:.3f}')
PYEOF
  fi
}
# ─────────────────────────────────────────────────────────────────────────────

for i in 1 2 3 4 5; do
  pkill -f efficient_servers || true

  ./build/efficient_servers 1234 >/dev/null 2>&1 &
  SERVER_PID=$!

  # wait until the server is actually listening
  READY=0
  for _ in $(seq 1 50); do
    if nc -z localhost 1234 >/dev/null 2>&1; then
      READY=1
      break
    fi
    sleep 0.1
  done

  if [ "$READY" -ne 1 ]; then
    echo "Server did not start listening on port 1234"
    kill $SERVER_PID 2>/dev/null || true
    wait $SERVER_PID 2>/dev/null || true
    exit 1
  fi

  TIME_VALUE=$(timed_nc ../walk3000.pbf out.bin 1234)
  STATUS=$?

  if [ "$STATUS" -ne 0 ]; then
    echo "nc/client failed on run $i"
    kill $SERVER_PID 2>/dev/null || true
    wait $SERVER_PID 2>/dev/null || true
    exit 1
  fi

  echo "run $i: $TIME_VALUE s"

  SUM=$(echo "$SUM + $TIME_VALUE" | bc)

  kill $SERVER_PID 2>/dev/null || true
  wait $SERVER_PID 2>/dev/null || true
done

AVG=$(echo "scale=3; $SUM / 5" | bc)
echo "----------------------"
echo "avg: $AVG s"
