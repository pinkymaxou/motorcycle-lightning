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

# The config codec needs the protobuf bindings, generated the same way the
# firmware build generates them (the nanopb plugin reads <proto>.options from
# its working directory).
PROTO_DIR="$PWD/$ROOT/components/protocol/proto"
PB_DIR="$PWD/build/proto"
mkdir -p "$PB_DIR"
( cd "$PROTO_DIR" && protoc "--plugin=protoc-gen-nanopb=$(command -v protoc-gen-nanopb)" \
    "--nanopb_out=$PB_DIR" -I . ws_protocol.proto )

g++ $CXXFLAGS -include stubs/host_compat.h -Istubs \
    -I$ROOT/components/fx/include \
    -I$ROOT/components/config_store/include \
    -I$ROOT/components/nanopb -I"$PB_DIR" \
    test_config.cpp \
    $ROOT/components/config_store/config_rules.cpp \
    $ROOT/components/config_store/config_proto.cpp \
    $ROOT/components/fx/factory_effects.cpp $ROOT/components/fx/effect_eval.cpp \
    "$PB_DIR/ws_protocol.pb.c" \
    $ROOT/components/nanopb/pb_common.c $ROOT/components/nanopb/pb_encode.c \
    $ROOT/components/nanopb/pb_decode.c \
    -o build/test_config

./build/test_blinker
./build/test_eval
./build/test_arbiter
./build/test_config
