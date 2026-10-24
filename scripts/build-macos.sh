#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
cmake -S . -B build-v3 -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-v3 --parallel
ctest --test-dir build-v3 --output-on-failure
