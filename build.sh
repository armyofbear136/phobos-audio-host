#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")"

rm -rf build
mkdir build
cd build

cmake -DCMAKE_BUILD_TYPE=Release ..
cmake --build . -j

echo
echo "Build complete: build/PhobosHost_artefacts/Release/PhobosHost"
