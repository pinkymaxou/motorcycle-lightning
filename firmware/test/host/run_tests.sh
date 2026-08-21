#!/usr/bin/env bash
# Host-side unit tests for the pure firmware cores (no ESP-IDF needed).
set -euo pipefail
cd "$(dirname "$0")"
mkdir -p build

CXXFLAGS="-std=gnu++17 -Wall -Wextra -Werror -O2"
ROOT=../..

g++ $CXXFLAGS -I$ROOT/components/input_conditioner/include \
    test_blinker.cpp $ROOT/components/input_conditioner/blinker.cpp \
    -o build/test_blinker

g++ $CXXFLAGS -I$ROOT/components/fx/include \
    test_eval.cpp $ROOT/components/fx/effect_eval.cpp \
    -o build/test_eval

g++ $CXXFLAGS -I$ROOT/components/fx/include \
    -I$ROOT/components/input_conditioner/include \
    -I$ROOT/components/config_store/include \
    -I$ROOT/components/event_arbiter/include \
    test_arbiter.cpp $ROOT/components/event_arbiter/event_arbiter.cpp \
    $ROOT/components/fx/effect_eval.cpp $ROOT/components/fx/factory_effects.cpp \
    -o build/test_arbiter

./build/test_blinker
./build/test_eval
./build/test_arbiter
