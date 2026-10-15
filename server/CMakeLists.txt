cmake_minimum_required(VERSION 3.22)
project(platinumwt_server LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Protobuf REQUIRED)
find_package(gRPC CONFIG REQUIRED)
find_package(Boost REQUIRED COMPONENTS system)
find_package(Threads REQUIRED)

set(PROTO_DIR "${CMAKE_CURRENT_SOURCE_DIR}/proto")
add_custom_command(
    OUTPUT connectivity.pb.cc connectivity.pb.h connectivity.grpc.pb.cc connectivity.grpc.pb.h
    COMMAND protobuf::protoc
    ARGS --proto_path=${PROTO_DIR} --proto_path=${Protobuf_INCLUDE_DIRS}
         --cpp_out=${CMAKE_CURRENT_BINARY_DIR} --grpc_out=${CMAKE_CURRENT_BINARY_DIR}
         --plugin=protoc-gen-grpc=$<TARGET_FILE:gRPC::grpc_cpp_plugin>
         ${PROTO_DIR}/connectivity.proto
    DEPENDS proto/connectivity.proto
    VERBATIM)

add_library(platinumwt_protocol STATIC connectivity.pb.cc connectivity.grpc.pb.cc)
target_include_directories(platinumwt_protocol PUBLIC "${CMAKE_CURRENT_BINARY_DIR}")
target_link_libraries(platinumwt_protocol PUBLIC gRPC::grpc++ protobuf::libprotobuf)

add_library(platinumwt_server STATIC
    src/config.cpp
    src/connectivity_service.cpp
    src/http_request_handler.cpp
    src/http_session.cpp
    src/http_listener.cpp
    src/server.cpp)
target_include_directories(platinumwt_server PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(platinumwt_server PUBLIC platinumwt_protocol Boost::system Threads::Threads)
target_compile_options(platinumwt_server PRIVATE -Wall -Wextra -Wpedantic)

add_executable(platinumwt-server src/main.cpp)
target_link_libraries(platinumwt-server PRIVATE platinumwt_server)
target_compile_options(platinumwt-server PRIVATE -Wall -Wextra -Wpedantic)

include(CTest)
if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
