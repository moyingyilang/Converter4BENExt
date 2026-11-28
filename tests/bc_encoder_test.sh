#!/data/data/com.termux/files/usr/bin/bash
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
SRC=$(cd "$HERE/.." && pwd)
CXX=${CXX:-clang++}
OUT=${TMPDIR:-/data/data/com.termux/files/usr/tmp}/c4ben-bc-test
$CXX -O2 -std=c++17 -I"$SRC/src/third_party" -I"$SRC/src" "$HERE/bc_encoder_test.cpp" -o "$OUT"
"$OUT"
