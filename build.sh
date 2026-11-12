#!/bin/sh
set -eu
cd "$(dirname "$0")"
mkdir -p build
if [ "$(uname -s)" = Darwin ]; then
  "${CXX:-/Library/Developer/CommandLineTools/usr/bin/clang++}" \
    -isysroot /Library/Developer/CommandLineTools/SDKs/MacOSX.sdk \
    -Ithird_party -std=c++20 -O2 -g -Wall -Wextra -Wpedantic \
    minivectordb/main.cpp -o build/minivectordb
else
  "${CXX:-g++}" -Ithird_party -std=c++20 -O2 -g -Wall -Wextra -Wpedantic \
    minivectordb/main.cpp -o build/minivectordb
fi
