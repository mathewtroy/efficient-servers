#!/usr/bin/env bash
set -euo pipefail

FILE="${1:-}"

if [ -z "$FILE" ]; then
  echo "Usage: ./run_test.sh <pbf-file>"
  exit 1
fi

if [ ! -f "$FILE" ]; then
  echo "File not found: $FILE"
  exit 1
fi

if [ ! -x ./build/efficient_servers ]; then
  echo "Missing ./build/efficient_servers"
  echo "Build the project first:"
  echo "  cmake -S . -B build -DCMAKE_BUILD_TYPE=Release"
  echo "  cmake --build build -j"
  exit 1
fi

if [ ! -x ./build/decode_responses ]; then
  echo "Missing ./build/decode_responses"
  echo "Build the project first:"
  echo "  cmake -S . -B build -DCMAKE_BUILD_TYPE=Release"
  echo "  cmake --build build -j"
  exit 1
fi

pkill -f efficient_servers || true

./build/efficient_servers 1234 &
SERVER_PID=$!

sleep 1

/usr/bin/time -f "elapsed: %e s" nc -q 1 localhost 1234 < "$FILE" > out.bin

./build/decode_responses

kill $SERVER_PID 2>/dev/null || true
wait $SERVER_PID 2>/dev/null || true