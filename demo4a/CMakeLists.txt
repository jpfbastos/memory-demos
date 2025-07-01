cmake_minimum_required(VERSION 3.28)
project(demo4a LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED TRUE)

# Find dependencies
find_package(Protobuf REQUIRED)
find_package(gRPC REQUIRED)
find_package(Threads REQUIRED)

# Locate CUDA runtime (cudart) from Conda
find_library(CUDART_LIB cudart HINTS $ENV{CONDA_PREFIX}/lib)
include_directories($ENV{CONDA_PREFIX}/include)
link_directories($ENV{CONDA_PREFIX}/lib)

# Generated protobuf/GRPC sources
set(GENPROTO_LIB genproto_lib)
set(GENPROTO_DIR ${PROJECT_SOURCE_DIR}/genproto)

file(GLOB_RECURSE PROTO_SRC ${GENPROTO_DIR}/*.pb.cc)
file(GLOB_RECURSE PROTO_HDR ${GENPROTO_DIR}/*.pb.h)

add_library(${GENPROTO_LIB} ${PROTO_SRC} ${PROTO_HDR})
target_include_directories(${GENPROTO_LIB} PUBLIC ${GENPROTO_DIR})
target_link_libraries(${GENPROTO_LIB} PUBLIC protobuf::libprotobuf gRPC::grpc++)

# Main executable
add_executable(server src/server.cpp)
add_executable(client src/client.cpp)

# Link libraries; wrap static libs and runtime into a group to resolve inter-dependencies
# This ensures symbols like __cxa_call_terminate from libstdc++ are pulled in
target_link_libraries(server
  PRIVATE
    ${GENPROTO_LIB}
    gRPC::grpc++
    protobuf::libprotobuf
    gcc_s               # GCC support library
    ${CUDART_LIB}         # CUDA runtime from Conda
)

target_link_libraries(client
  PRIVATE
    ${GENPROTO_LIB}
    gRPC::grpc++
    protobuf::libprotobuf
    gcc_s               # GCC support library
    ${CUDART_LIB}         # CUDA runtime from Conda
)