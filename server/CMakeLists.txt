cmake_minimum_required(VERSION 3.22)
project(dswt_server LANGUAGES CXX)

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

add_library(dswt_protocol STATIC
    connectivity.pb.cc connectivity.grpc.pb.cc trade.pb.cc trade.grpc.pb.cc)
target_include_directories(dswt_protocol PUBLIC "${CMAKE_CURRENT_BINARY_DIR}")
target_link_libraries(dswt_protocol PUBLIC gRPC::grpc++ protobuf::libprotobuf)

add_library(dswt_encoding STATIC src/trade_encoding.cpp)
target_include_directories(dswt_encoding PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(dswt_encoding PRIVATE dswt_protocol OpenSSL::Crypto)
target_compile_options(dswt_encoding PRIVATE -Wall -Wextra -Wpedantic)

add_library(dswt_telemetry STATIC src/telemetry.cpp)
target_include_directories(dswt_telemetry PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(dswt_telemetry PUBLIC gRPC::grpc++ Threads::Threads)
target_compile_options(dswt_telemetry PRIVATE -Wall -Wextra -Wpedantic)

add_library(dswt_server STATIC
    src/config.cpp
    src/connectivity_service.cpp
    src/http_request_handler.cpp
    src/http_services.cpp
    src/trade_gateway.cpp
    src/http_session.cpp
    src/http_listener.cpp
    src/server.cpp)
target_sources(dswt_server PRIVATE src/bounded_query.cpp src/metrics_server.cpp src/wal_archive_barrier.cpp)
target_include_directories(dswt_server PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(dswt_server PUBLIC dswt_protocol Boost::system Threads::Threads)
target_link_libraries(dswt_server PRIVATE dswt_encoding dswt_admission dswt_telemetry OpenSSL::SSL PostgreSQL::PostgreSQL)
target_compile_options(dswt_server PRIVATE -Wall -Wextra -Wpedantic)

add_executable(dswt-server src/main.cpp)
target_link_libraries(dswt-server PRIVATE dswt_server)
target_compile_options(dswt-server PRIVATE -Wall -Wextra -Wpedantic)

add_library(dswt_ledger STATIC src/ledger_store.cpp src/identity_store.cpp src/postgres.cpp)
target_include_directories(dswt_ledger PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(dswt_ledger PRIVATE PostgreSQL::PostgreSQL OpenSSL::Crypto)
target_compile_options(dswt_ledger PRIVATE -Wall -Wextra -Wpedantic)

add_executable(dswt-ledger src/ledger_main.cpp)
target_link_libraries(dswt-ledger PRIVATE dswt_ledger)
target_compile_options(dswt-ledger PRIVATE -Wall -Wextra -Wpedantic)

add_library(dswt_legality STATIC src/legality_client.cpp)
target_include_directories(dswt_legality PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(dswt_legality PRIVATE dswt_protocol dswt_telemetry OpenSSL::Crypto)
target_compile_options(dswt_legality PRIVATE -Wall -Wextra -Wpedantic)

add_library(dswt_admission STATIC src/admission.cpp)
target_include_directories(dswt_admission PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(dswt_admission PUBLIC Boost::system PRIVATE OpenSSL::Crypto)
target_compile_options(dswt_admission PRIVATE -Wall -Wextra -Wpedantic)

add_library(dswt_trading STATIC src/trade_service.cpp src/identity_service.cpp src/recovery_gate.cpp src/matcher_service.cpp src/match_worker.cpp)
target_include_directories(dswt_trading PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(dswt_trading PUBLIC dswt_protocol PRIVATE dswt_ledger dswt_legality dswt_encoding dswt_admission dswt_telemetry PostgreSQL::PostgreSQL OpenSSL::Crypto)
target_compile_options(dswt_trading PRIVATE -Wall -Wextra -Wpedantic)

add_executable(dswt-backend src/backend_main.cpp)
target_link_libraries(dswt-backend PRIVATE dswt_trading dswt_server)
target_compile_options(dswt-backend PRIVATE -Wall -Wextra -Wpedantic)

add_executable(dswt-matcher src/matcher_main.cpp)
target_link_libraries(dswt-matcher PRIVATE dswt_trading dswt_server)
target_compile_options(dswt-matcher PRIVATE -Wall -Wextra -Wpedantic)

include(CTest)
if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
