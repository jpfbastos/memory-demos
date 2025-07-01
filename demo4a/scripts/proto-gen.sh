#!/bin/bash
CURRENT_DIR=$(pwd)

mkdir -p "$CURRENT_DIR/genproto"

rm -rf ./genproto/*

for module in $(find $CURRENT_DIR/proto/* -type d); do
    protoc -I "$CURRENT_DIR/proto/" -I ${module} \
           --cpp_out=$CURRENT_DIR/genproto/ \
           --grpc_out=$CURRENT_DIR/genproto/ \
           --plugin=protoc-gen-grpc=`which grpc_cpp_plugin` \
           $module/*.proto
done