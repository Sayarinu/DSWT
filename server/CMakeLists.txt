cmake_minimum_required(VERSION 3.22)
project(platinumwt_server LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Protobuf REQUIRED)
find_package(gRPC CONFIG REQUIRED)
find_package(Boost REQUIRED COMPONENTS system)
find_package(Threads REQUIRED)
find_package(PostgreSQL 16 REQUIRED)
find_package(OpenSSL 3 REQUIRED COMPONENTS Crypto SSL)

set(PROTO_DIR "${CMAKE_CURRENT_SOURCE_DIR}/proto")
foreach(schema IN ITEMS connectivity trade)
    add_custom_command(
        OUTPUT ${schema}.pb.cc ${schema}.pb.h ${schema}.grpc.pb.cc ${schema}.grpc.pb.h
        COMMAND protobuf::protoc
        ARGS --proto_path=${PROTO_DIR} --proto_path=${Protobuf_INCLUDE_DIRS}
             --cpp_out=${CMAKE_CURRENT_BINARY_DIR} --grpc_out=${CMAKE_CURRENT_BINARY_DIR}
             --plugin=protoc-gen-grpc=$<TARGET_FILE:gRPC::grpc_cpp_plugin>
             ${PROTO_DIR}/${schema}.proto
        DEPENDS proto/${schema}.proto
        VERBATIM)
endforeach()

add_library(platinumwt_protocol STATIC
    connectivity.pb.cc connectivity.grpc.pb.cc trade.pb.cc trade.grpc.pb.cc)
target_include_directories(platinumwt_protocol PUBLIC "${CMAKE_CURRENT_BINARY_DIR}")
target_link_libraries(platinumwt_protocol PUBLIC gRPC::grpc++ protobuf::libprotobuf)

add_library(platinumwt_encoding STATIC src/trade_encoding.cpp)
target_include_directories(platinumwt_encoding PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(platinumwt_encoding PRIVATE platinumwt_protocol OpenSSL::Crypto)
target_compile_options(platinumwt_encoding PRIVATE -Wall -Wextra -Wpedantic)

add_library(platinumwt_server STATIC
    src/config.cpp
    src/connectivity_service.cpp
    src/http_request_handler.cpp
    src/http_services.cpp
    src/trade_gateway.cpp
    src/http_session.cpp
    src/http_listener.cpp
    src/server.cpp)
target_include_directories(platinumwt_server PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(platinumwt_server PUBLIC platinumwt_protocol Boost::system Threads::Threads)
target_link_libraries(platinumwt_server PRIVATE platinumwt_encoding OpenSSL::SSL)
target_compile_options(platinumwt_server PRIVATE -Wall -Wextra -Wpedantic)

add_executable(platinumwt-server src/main.cpp)
target_link_libraries(platinumwt-server PRIVATE platinumwt_server)
target_compile_options(platinumwt-server PRIVATE -Wall -Wextra -Wpedantic)

add_library(platinumwt_ledger STATIC src/ledger_store.cpp src/identity_store.cpp src/postgres.cpp)
target_include_directories(platinumwt_ledger PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(platinumwt_ledger PRIVATE PostgreSQL::PostgreSQL OpenSSL::Crypto)
target_compile_options(platinumwt_ledger PRIVATE -Wall -Wextra -Wpedantic)

add_executable(platinumwt-ledger src/ledger_main.cpp)
target_link_libraries(platinumwt-ledger PRIVATE platinumwt_ledger)
target_compile_options(platinumwt-ledger PRIVATE -Wall -Wextra -Wpedantic)

add_library(platinumwt_legality STATIC src/legality_client.cpp)
target_include_directories(platinumwt_legality PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(platinumwt_legality PRIVATE platinumwt_protocol OpenSSL::Crypto)
target_compile_options(platinumwt_legality PRIVATE -Wall -Wextra -Wpedantic)

add_library(platinumwt_trading STATIC src/trade_service.cpp src/identity_service.cpp src/recovery_gate.cpp src/matcher_service.cpp)
target_include_directories(platinumwt_trading PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(platinumwt_trading PUBLIC platinumwt_protocol PRIVATE platinumwt_ledger platinumwt_legality platinumwt_encoding PostgreSQL::PostgreSQL OpenSSL::Crypto)
target_compile_options(platinumwt_trading PRIVATE -Wall -Wextra -Wpedantic)

add_executable(platinumwt-backend src/backend_main.cpp)
target_link_libraries(platinumwt-backend PRIVATE platinumwt_trading platinumwt_server)
target_compile_options(platinumwt-backend PRIVATE -Wall -Wextra -Wpedantic)

include(CTest)
if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
