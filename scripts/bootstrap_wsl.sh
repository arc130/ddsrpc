#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
wsl_path_root="$(wslpath -a "$ROOT" 2>/dev/null || printf '%s' "$ROOT")"
# wslpath -a 默认按 Windows 路径转换；在 WSL 内 $ROOT 已是 /mnt/... 时会被二次加前缀，
# 此处回退到原路径，保证脚本在 WSL 内直接执行也正确。
[[ -d "$wsl_path_root" ]] || wsl_path_root="$ROOT"

cmake -S "$wsl_path_root/third_party/Transport" \
  -B /tmp/ddsrpc-transport-spike \
  -DASYNCTASK_DIR="$wsl_path_root/third_party/AsyncTask" \
  -DTRANSPORT_BUILD_TESTS=OFF \
  -DTRANSPORT_BUILD_PERF=OFF \
  -DCMAKE_BUILD_TYPE=Debug
cmake --build /tmp/ddsrpc-transport-spike -j"$(nproc)"
printf 'Transport build spike OK\n'
