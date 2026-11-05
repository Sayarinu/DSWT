find_package(GTest REQUIRED)
include(GoogleTest)

add_executable(platinumwt-unit-tests
    unit/admission_test.cpp
    unit/config_test.cpp
    unit/connectivity_service_test.cpp
    unit/http_request_handler_test.cpp
    unit/wal_archive_barrier_test.cpp)
target_sources(platinumwt-unit-tests PRIVATE unit/telemetry_test.cpp)
target_include_directories(platinumwt-unit-tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../src")
target_link_libraries(platinumwt-unit-tests PRIVATE platinumwt_server platinumwt_admission platinumwt_telemetry GTest::gtest_main)
target_compile_options(platinumwt-unit-tests PRIVATE -Wall -Wextra -Wpedantic)
gtest_discover_tests(platinumwt-unit-tests TEST_PREFIX "unit." PROPERTIES LABELS "unit")

add_executable(platinumwt-legality-tests unit/legality_client_test.cpp)
target_link_libraries(platinumwt-legality-tests PRIVATE platinumwt_legality platinumwt_protocol GTest::gtest_main)
target_compile_options(platinumwt-legality-tests PRIVATE -Wall -Wextra -Wpedantic)
gtest_discover_tests(platinumwt-legality-tests TEST_PREFIX "unit." PROPERTIES LABELS "unit" TIMEOUT 10)

find_package(Python3 REQUIRED COMPONENTS Interpreter)
foreach(module IN ITEMS test_http_gateway test_backend test_malformed_requests)
    add_test(NAME integration.${module}
        COMMAND Python3::Interpreter "${CMAKE_CURRENT_SOURCE_DIR}/integration/run_tests.py"
            $<TARGET_FILE:platinumwt-server> ${module})
    set_tests_properties(integration.${module} PROPERTIES TIMEOUT 30 LABELS "integration")
endforeach()

add_executable(platinumwt-identity-tests integration/identity_test.cpp)
target_include_directories(platinumwt-identity-tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../src")
target_link_libraries(platinumwt-identity-tests PRIVATE platinumwt_ledger PostgreSQL::PostgreSQL GTest::gtest_main)
target_compile_options(platinumwt-identity-tests PRIVATE -Wall -Wextra -Wpedantic)

add_test(NAME integration.ledger
    COMMAND Python3::Interpreter "${CMAKE_CURRENT_SOURCE_DIR}/integration/test_ledger.py"
        $<TARGET_FILE:platinumwt-ledger> "${CMAKE_CURRENT_SOURCE_DIR}/../migrations"
        $<TARGET_FILE:platinumwt-identity-tests>
        $<TARGET_FILE:platinumwt-trade-tests>
        $<TARGET_FILE:platinumwt-matcher-tests>
        $<TARGET_FILE:platinumwt-server>
        $<TARGET_FILE:platinumwt-backend>
        $<TARGET_FILE:platinumwt-gateway-fixture>)
set_tests_properties(integration.ledger PROPERTIES TIMEOUT 180 LABELS "integration")

add_executable(platinumwt-barrier-probe integration/barrier_probe.cpp)
target_link_libraries(platinumwt-barrier-probe PRIVATE platinumwt_server PostgreSQL::PostgreSQL)
target_compile_options(platinumwt-barrier-probe PRIVATE -Wall -Wextra -Wpedantic)
add_test(NAME integration.wal_archive_barrier
    COMMAND Python3::Interpreter "${CMAKE_CURRENT_SOURCE_DIR}/integration/test_wal_archive_barrier.py"
        $<TARGET_FILE:platinumwt-barrier-probe> "${CMAKE_CURRENT_SOURCE_DIR}/../wal-archive.sh")
set_tests_properties(integration.wal_archive_barrier PROPERTIES TIMEOUT 240 LABELS "integration")

add_test(NAME integration.matcher_process
    COMMAND Python3::Interpreter "${CMAKE_CURRENT_SOURCE_DIR}/integration/test_matcher_process.py"
        $<TARGET_FILE:platinumwt-matcher>)
set_tests_properties(integration.matcher_process PROPERTIES TIMEOUT 30 LABELS "integration")

add_executable(platinumwt-trade-tests integration/trade_test.cpp)
target_include_directories(platinumwt-trade-tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../src")
target_link_libraries(platinumwt-trade-tests PRIVATE platinumwt_trading platinumwt_ledger PostgreSQL::PostgreSQL GTest::gtest)
target_compile_options(platinumwt-trade-tests PRIVATE -Wall -Wextra -Wpedantic)

add_executable(platinumwt-matcher-tests integration/matcher_test.cpp)
target_include_directories(platinumwt-matcher-tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../src")
target_link_libraries(platinumwt-matcher-tests PRIVATE platinumwt_trading platinumwt_ledger PostgreSQL::PostgreSQL GTest::gtest)
target_compile_options(platinumwt-matcher-tests PRIVATE -Wall -Wextra -Wpedantic)

add_executable(platinumwt-gateway-fixture integration/gateway_fixture.cpp)
target_include_directories(platinumwt-gateway-fixture PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../src")
target_link_libraries(platinumwt-gateway-fixture PRIVATE platinumwt_trading Boost::system Threads::Threads)
target_compile_options(platinumwt-gateway-fixture PRIVATE -Wall -Wextra -Wpedantic)

# A strict-legality backend/matcher for disposable ROM acceptance only. The final
# service image does not copy this executable or expose its local test barrier.
add_executable(platinumwt-device-fixture integration/device_fixture.cpp)
target_include_directories(platinumwt-device-fixture PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../src")
target_link_libraries(platinumwt-device-fixture PRIVATE platinumwt_trading platinumwt_server PostgreSQL::PostgreSQL)
target_compile_options(platinumwt-device-fixture PRIVATE -Wall -Wextra -Wpedantic)
add_test(NAME integration.device_fixture_refuses_unconfigured_start
    COMMAND ${CMAKE_COMMAND} -E env --unset=PLATINUMWT_DISPOSABLE_DEVICE_TEST
        $<TARGET_FILE:platinumwt-device-fixture>)
set_tests_properties(integration.device_fixture_refuses_unconfigured_start
    PROPERTIES WILL_FAIL TRUE TIMEOUT 10 LABELS "integration")

add_executable(platinumwt-device-matcher-tests integration/device_matcher_test.cpp)
target_link_libraries(platinumwt-device-matcher-tests PRIVATE platinumwt_trading platinumwt_protocol OpenSSL::Crypto GTest::gtest_main)
gtest_discover_tests(platinumwt-device-matcher-tests TEST_PREFIX "integration." PROPERTIES TIMEOUT 15 LABELS "integration")
add_test(NAME integration.device_fixture_connection_guards
    COMMAND Python3::Interpreter "${CMAKE_CURRENT_SOURCE_DIR}/integration/test_device_fixture.py"
        $<TARGET_FILE:platinumwt-device-fixture>)
set_tests_properties(integration.device_fixture_connection_guards PROPERTIES TIMEOUT 15 LABELS "integration")
