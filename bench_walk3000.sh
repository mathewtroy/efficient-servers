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

  TIME_OUTPUT=$({ /usr/bin/time -f "%e" nc -N localhost 1234 < ../walk3000.pbf > out.bin; } 2>&1)
  STATUS=$?

  if [ "$STATUS" -ne 0 ]; then
    echo "nc failed on run $i"
    echo "$TIME_OUTPUT"
    kill $SERVER_PID 2>/dev/null || true
    wait $SERVER_PID 2>/dev/null || true
    exit 1
  fi

  TIME_VALUE=$(echo "$TIME_OUTPUT" | tail -n 1)

  echo "run $i: $TIME_VALUE s"

  SUM=$(echo "$SUM + $TIME_VALUE" | bc)

  kill $SERVER_PID 2>/dev/null || true
  wait $SERVER_PID 2>/dev/null || true
done

AVG=$(echo "scale=3; $SUM / 5" | bc)
echo "----------------------"
echo "avg: $AVG s"