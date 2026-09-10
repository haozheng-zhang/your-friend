#!/usr/bin/env bash
set -euo pipefail

project_dir="$(cd "$(dirname "$0")/.." && pwd)"
object_dir="$project_dir/build"
output_dir="$project_dir/dist"

mkdir -p "$object_dir" "$output_dir"

x86_64-w64-mingw32-windres \
  -I "$project_dir/src" \
  "$project_dir/src/fly.rc" \
  -O coff \
  -o "$object_dir/fly-resources.o"

x86_64-w64-mingw32-g++ \
  -std=c++17 -O2 -s -mwindows -municode -DWIN32_LEAN_AND_MEAN \
  -static -static-libgcc -static-libstdc++ \
  "$project_dir/src/main.cpp" \
  "$object_dir/fly-resources.o" \
  -lgdiplus -lole32 -luuid -lgdi32 -luser32 \
  -o "$output_dir/DesktopFly_5min.exe"

echo "Built $output_dir/DesktopFly_5min.exe"
