find_package(GTest REQUIRED)
include(GoogleTest)

add_executable(platinumwt-unit-tests
    unit/config_test.cpp
    unit/connectivity_service_test.cpp
    unit/http_request_handler_test.cpp)
target_link_libraries(platinumwt-unit-tests PRIVATE platinumwt_server GTest::gtest_main)
target_compile_options(platinumwt-unit-tests PRIVATE -Wall -Wextra -Wpedantic)
gtest_discover_tests(platinumwt-unit-tests TEST_PREFIX "unit." PROPERTIES LABELS "unit")

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
        $<TARGET_FILE:platinumwt-identity-tests>)
set_tests_properties(integration.ledger PROPERTIES TIMEOUT 180 LABELS "integration")
