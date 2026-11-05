find_package(GTest REQUIRED)
include(GoogleTest)

add_executable(dswt-unit-tests
    unit/admission_test.cpp
    unit/config_test.cpp
    unit/connectivity_service_test.cpp
    unit/http_request_handler_test.cpp
    unit/wal_archive_barrier_test.cpp)
target_sources(dswt-unit-tests PRIVATE unit/telemetry_test.cpp)
target_include_directories(dswt-unit-tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../src")
target_link_libraries(dswt-unit-tests PRIVATE dswt_server dswt_admission dswt_telemetry GTest::gtest_main)
target_compile_options(dswt-unit-tests PRIVATE -Wall -Wextra -Wpedantic)
gtest_discover_tests(dswt-unit-tests TEST_PREFIX "unit." PROPERTIES LABELS "unit")

add_executable(dswt-legality-tests unit/legality_client_test.cpp)
target_link_libraries(dswt-legality-tests PRIVATE dswt_legality dswt_protocol GTest::gtest_main)
target_compile_options(dswt-legality-tests PRIVATE -Wall -Wextra -Wpedantic)
gtest_discover_tests(dswt-legality-tests TEST_PREFIX "unit." PROPERTIES LABELS "unit" TIMEOUT 10)

find_package(Python3 REQUIRED COMPONENTS Interpreter)
foreach(module IN ITEMS test_http_gateway test_backend test_malformed_requests)
    add_test(NAME integration.${module}
        COMMAND Python3::Interpreter "${CMAKE_CURRENT_SOURCE_DIR}/integration/run_tests.py"
            $<TARGET_FILE:dswt-server> ${module})
    set_tests_properties(integration.${module} PROPERTIES TIMEOUT 30 LABELS "integration")
endforeach()

add_executable(dswt-identity-tests integration/identity_test.cpp)
target_include_directories(dswt-identity-tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../src")
target_link_libraries(dswt-identity-tests PRIVATE dswt_ledger PostgreSQL::PostgreSQL GTest::gtest_main)
target_compile_options(dswt-identity-tests PRIVATE -Wall -Wextra -Wpedantic)

add_test(NAME integration.ledger
    COMMAND Python3::Interpreter "${CMAKE_CURRENT_SOURCE_DIR}/integration/test_ledger.py"
        $<TARGET_FILE:dswt-ledger> "${CMAKE_CURRENT_SOURCE_DIR}/../migrations"
        $<TARGET_FILE:dswt-identity-tests>
        $<TARGET_FILE:dswt-trade-tests>
        $<TARGET_FILE:dswt-matcher-tests>
        $<TARGET_FILE:dswt-server>
        $<TARGET_FILE:dswt-backend>
        $<TARGET_FILE:dswt-gateway-fixture>)
set_tests_properties(integration.ledger PROPERTIES TIMEOUT 180 LABELS "integration")

add_executable(dswt-barrier-probe integration/barrier_probe.cpp)
target_link_libraries(dswt-barrier-probe PRIVATE dswt_server PostgreSQL::PostgreSQL)
target_compile_options(dswt-barrier-probe PRIVATE -Wall -Wextra -Wpedantic)
add_test(NAME integration.wal_archive_barrier
    COMMAND Python3::Interpreter "${CMAKE_CURRENT_SOURCE_DIR}/integration/test_wal_archive_barrier.py"
        $<TARGET_FILE:dswt-barrier-probe> "${CMAKE_CURRENT_SOURCE_DIR}/../wal-archive.sh")
set_tests_properties(integration.wal_archive_barrier PROPERTIES TIMEOUT 240 LABELS "integration")

add_test(NAME integration.matcher_process
    COMMAND Python3::Interpreter "${CMAKE_CURRENT_SOURCE_DIR}/integration/test_matcher_process.py"
        $<TARGET_FILE:dswt-matcher>)
set_tests_properties(integration.matcher_process PROPERTIES TIMEOUT 30 LABELS "integration")

add_executable(dswt-trade-tests integration/trade_test.cpp)
target_include_directories(dswt-trade-tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../src")
target_link_libraries(dswt-trade-tests PRIVATE dswt_trading dswt_ledger PostgreSQL::PostgreSQL GTest::gtest)
target_compile_options(dswt-trade-tests PRIVATE -Wall -Wextra -Wpedantic)

add_executable(dswt-matcher-tests integration/matcher_test.cpp)
target_include_directories(dswt-matcher-tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../src")
target_link_libraries(dswt-matcher-tests PRIVATE dswt_trading dswt_ledger PostgreSQL::PostgreSQL GTest::gtest)
target_compile_options(dswt-matcher-tests PRIVATE -Wall -Wextra -Wpedantic)

add_executable(dswt-gateway-fixture integration/gateway_fixture.cpp)
target_include_directories(dswt-gateway-fixture PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../src")
target_link_libraries(dswt-gateway-fixture PRIVATE dswt_trading Boost::system Threads::Threads)
target_compile_options(dswt-gateway-fixture PRIVATE -Wall -Wextra -Wpedantic)

# A strict-legality backend/matcher for disposable ROM acceptance only. The final
# service image does not copy this executable or expose its local test barrier.
add_executable(dswt-device-fixture integration/device_fixture.cpp)
target_include_directories(dswt-device-fixture PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../src")
target_link_libraries(dswt-device-fixture PRIVATE dswt_trading dswt_server PostgreSQL::PostgreSQL)
target_compile_options(dswt-device-fixture PRIVATE -Wall -Wextra -Wpedantic)
add_test(NAME integration.device_fixture_refuses_unconfigured_start
    COMMAND ${CMAKE_COMMAND} -E env --unset=DSWT_DISPOSABLE_DEVICE_TEST
        $<TARGET_FILE:dswt-device-fixture>)
set_tests_properties(integration.device_fixture_refuses_unconfigured_start
    PROPERTIES WILL_FAIL TRUE TIMEOUT 10 LABELS "integration")

add_executable(dswt-device-matcher-tests integration/device_matcher_test.cpp)
target_link_libraries(dswt-device-matcher-tests PRIVATE dswt_trading dswt_protocol OpenSSL::Crypto GTest::gtest_main)
gtest_discover_tests(dswt-device-matcher-tests TEST_PREFIX "integration." PROPERTIES TIMEOUT 15 LABELS "integration")
add_test(NAME integration.device_fixture_connection_guards
    COMMAND Python3::Interpreter "${CMAKE_CURRENT_SOURCE_DIR}/integration/test_device_fixture.py"
        $<TARGET_FILE:dswt-device-fixture>)
set_tests_properties(integration.device_fixture_connection_guards PROPERTIES TIMEOUT 15 LABELS "integration")
