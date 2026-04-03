#!/usr/bin/env bash
set -euo pipefail

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

  sleep 1

  TIME=$({ /usr/bin/time -f "%e" nc -q 1 localhost 1234 < ../walk3000.pbf > out.bin; } 2>&1)

  echo "run $i: $TIME s"

  SUM=$(echo "$SUM + $TIME" | bc)

  kill $SERVER_PID 2>/dev/null || true
  wait $SERVER_PID 2>/dev/null || true
done

AVG=$(echo "scale=3; $SUM / 5" | bc)
echo "----------------------"
echo "avg: $AVG s"