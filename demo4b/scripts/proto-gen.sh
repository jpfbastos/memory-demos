#!/bin/bash
CURRENT_DIR=$(pwd)

PROTOC="$CONDA_PREFIX/bin/protoc"
GRPC_PLUGIN="$CONDA_PREFIX/bin/grpc_cpp_plugin"

mkdir -p "$CURRENT_DIR/genproto"
rm -rf "$CURRENT_DIR/genproto/*"

for module in $(find $CURRENT_DIR/proto/* -type d); do
    "$PROTOC" -I "$CURRENT_DIR/proto/" -I "$module" \
        --cpp_out="$CURRENT_DIR/genproto/" \
        --grpc_out="$CURRENT_DIR/genproto/" \
        --plugin=protoc-gen-grpc="$GRPC_PLUGIN" \
        "$module"/*.proto
done