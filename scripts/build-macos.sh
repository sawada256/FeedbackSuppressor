#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
cmake -S . -B build-v21 -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-v21 --parallel
ctest --test-dir build-v21 --output-on-failure
