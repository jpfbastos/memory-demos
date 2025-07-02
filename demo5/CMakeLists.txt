cmake_minimum_required(VERSION 3.28)
project(demo5 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED TRUE)

# --- Use Conda environment path ---
if(NOT DEFINED CMAKE_PREFIX_PATH)
  set(CMAKE_PREFIX_PATH $ENV{CONDA_PREFIX})
endif()

# --- Find dependencies from Conda ---
find_package(Protobuf REQUIRED)
find_package(gRPC REQUIRED)
find_package(Threads REQUIRED)

# --- CUDA Runtime (optional if you're using CUDA in your code) ---
find_library(CUDART_LIB cudart HINTS $ENV{CONDA_PREFIX}/lib)
include_directories($ENV{CONDA_PREFIX}/include)
link_directories($ENV{CONDA_PREFIX}/lib)

# --- Generated protobuf/grpc sources ---
set(GENPROTO_LIB genproto_lib)
set(GENPROTO_DIR ${PROJECT_SOURCE_DIR}/genproto)

file(GLOB_RECURSE PROTO_SRC ${GENPROTO_DIR}/*.pb.cc)
file(GLOB_RECURSE PROTO_HDR ${GENPROTO_DIR}/*.pb.h)

add_library(${GENPROTO_LIB} ${PROTO_SRC} ${PROTO_HDR})
target_include_directories(${GENPROTO_LIB} PUBLIC ${GENPROTO_DIR})
target_link_libraries(${GENPROTO_LIB}
  PUBLIC
    protobuf::libprotobuf
    gRPC::grpc++
)

# --- Server executable ---
add_executable(server src/server.cpp)
target_link_libraries(server
  PRIVATE
    ${GENPROTO_LIB}
    protobuf::libprotobuf
    gRPC::grpc++
    Threads::Threads
    ${CUDART_LIB}
    uuid
)

# --- Client executable ---
add_executable(client src/client.cpp)
target_link_libraries(client
  PRIVATE
    ${GENPROTO_LIB}
    protobuf::libprotobuf
    gRPC::grpc++
    Threads::Threads
    ${CUDART_LIB}
)

