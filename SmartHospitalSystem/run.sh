#!/usr/bin/env bash
# ---------------------------------------------------------------------------
# VoltIQ Care · Smart Hospital Management System
# One-command launcher: compiles the C++17 DSA engine, starts it, then starts
# the dashboard which reverse-proxies /api/* to the engine.
# ---------------------------------------------------------------------------
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PORT="${PORT:-3000}"
ENGINE_PORT="${ENGINE_PORT:-8001}"
CXX="${CXX:-g++}"
CXXFLAGS="${CXXFLAGS:--std=c++17 -O2 -pthread -Wall -Wextra}"

mkdir -p "$ROOT/bin"

echo "▸ Compiling the C++17 DSA engine…"
$CXX $CXXFLAGS -o "$ROOT/bin/hospital" "$ROOT"/cpp/*.cpp
echo "  ✓ bin/hospital built"

cleanup() {
  [[ -n "${ENGINE_PID:-}" ]] && kill "$ENGINE_PID" 2>/dev/null || true
}
trap cleanup EXIT INT TERM

echo "▸ Starting the C++ engine on port $ENGINE_PORT…"
"$ROOT/bin/hospital" serve "$ENGINE_PORT" &
ENGINE_PID=$!

# wait for the engine to accept connections
for _ in $(seq 1 60); do
  if (exec 3<>"/dev/tcp/127.0.0.1/$ENGINE_PORT") 2>/dev/null; then
    exec 3>&- 3<&-
    break
  fi
  sleep 0.25
done
echo "  ✓ engine ready"

if [[ ! -d "$ROOT/web/node_modules" ]]; then
  echo "▸ Dashboard has zero npm dependencies — starting directly."
fi

echo "▸ Starting the dashboard on port $PORT…"
cd "$ROOT/web"
PORT="$PORT" ENGINE_PORT="$ENGINE_PORT" node server.js
