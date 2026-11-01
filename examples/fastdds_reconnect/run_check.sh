#!/usr/bin/env bash
# Real Fast DDS cross-process reconnect check.
#
# Flow:
#   1. server1 (name=before, count=1) + client -> first ready + property snapshot
#   2. kill server1 -> client sees link down (wait for an observable non-Up state)
#   3. server2 (name=after, count=2) -> client re-subscribes, refreshes properties,
#      becomes ready again, receives a new SIGNAL, and completes an RPC
#
# DDS may take one participant lease (~20s) to report the old participant as gone;
# this is why the link-down phase has a 70s budget.
set -u
script_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
repo_root=$(cd "$script_dir/../.." && pwd)
cd "$repo_root"

build_dir=${BUILD_DIR:-build-fastdds}
bin=${BIN:-"$repo_root/$build_dir/bin/fastdds_reconnect"}
domain=${DOMAIN:-44}
logs=$(mktemp -d)
server_log="$logs/server.log"
client_log="$logs/client.log"
server_pid=
client_pid=

cleanup() {
  [ -n "$client_pid" ] && kill "$client_pid" 2>/dev/null || true
  [ -n "$server_pid" ] && kill "$server_pid" 2>/dev/null || true
  [ -n "$client_pid" ] && wait "$client_pid" 2>/dev/null || true
  [ -n "$server_pid" ] && wait "$server_pid" 2>/dev/null || true
  rm -rf "$logs"
}
trap 'rc=$?; cleanup; exit "$rc"' EXIT

if [ ! -x "$bin" ]; then
  echo "FAIL: missing executable $bin"
  echo "Build it first: cmake -S . -B $build_dir -DDDSRPC_BUILD_EXAMPLES=ON && cmake --build $build_dir -j"
  exit 1
fi

start_server() {
  "$bin" --role server --domain "$domain" --name "$1" --count "$2" >>"$server_log" 2>&1 &
  server_pid=$!
}

wait_log() {
  local file=$1 pattern=$2 ticks=$3
  for _ in $(seq 1 "$ticks"); do
    if grep -q "$pattern" "$file" 2>/dev/null; then
      return 0
    fi
    if [ -n "$client_pid" ] && [ "$file" = "$client_log" ] &&
       ! kill -0 "$client_pid" 2>/dev/null; then
      return 1
    fi
    sleep 0.2
  done
  return 1
}

start_server before 1
if ! wait_log "$server_log" RECONNECT_SERVER_READY 100; then
  echo "FAIL: first server did not become ready"
  cat "$server_log"
  exit 1
fi

"$bin" --role client --domain "$domain" >"$client_log" 2>&1 &
client_pid=$!
if ! wait_log "$client_log" "RECONNECT_CLIENT_READY_1 name=before count=1" 150; then
  echo "FAIL: client did not observe the first server"
  cat "$client_log"
  exit 1
fi

kill "$server_pid" 2>/dev/null || true
wait "$server_pid" 2>/dev/null || true
server_pid=

if ! wait_log "$client_log" RECONNECT_CLIENT_DOWN 350; then
  echo "FAIL: client did not observe link loss"
  cat "$client_log"
  exit 1
fi

start_server after 2
if ! wait_log "$server_log" "RECONNECT_SERVER_READY name=after count=2" 100; then
  echo "FAIL: second server did not become ready"
  cat "$server_log"
  exit 1
fi

for _ in $(seq 1 200); do
  ! kill -0 "$client_pid" 2>/dev/null && break
  sleep 0.2
done
if kill -0 "$client_pid" 2>/dev/null; then
  echo "FAIL: client did not finish after reconnect"
  cat "$client_log"
  exit 1
fi

wait "$client_pid"
client_rc=$?
client_pid=

cat "$client_log"
echo "CLIENT_RC=$client_rc"
echo "---SERVER---"
cat "$server_log"
exit "$client_rc"
