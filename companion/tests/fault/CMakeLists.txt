add_executable(companion_write_fault_test write_fault_test.c
    "${COMPANION_SAVE}/save_image.c" "${COMPANION_SAVE}/sha256.c" "${COMPANION_SAVE}/posix_file.c"
    "${COMPANION_SAVE}/journal_save.c" "${COMPANION_SAVE}/journal_host_io.c"
    "${COMPANION_SRC}/platform/journal_io.c" "${COMPANION_SRC}/adapter/platinum.c"
    "${COMPANION_SRC}/session/visit.c"
    "${CLIENT}/src/wonder_trade/pokemon_codec.c" "${CLIENT}/src/wonder_trade/journal.c"
    "${CLIENT}/src/wonder_trade/wire.c" "${CLIENT}/src/wonder_trade/engine.c"
    "${CLIENT}/src/wonder_trade/session.c")
target_include_directories(companion_write_fault_test PRIVATE "${COMPANION_SRC}" "${CLIENT}/include")
target_compile_definitions(companion_write_fault_test PRIVATE WT_GAME_FAMILY=WT_FAMILY_GEN4
    CP_FAULT_BINARY_DIR="${CMAKE_CURRENT_BINARY_DIR}")
target_compile_options(companion_write_fault_test PRIVATE -O1 -Wall -Wextra -Werror -pedantic -fsanitize=address,undefined)
target_link_options(companion_write_fault_test PRIVATE -fsanitize=address,undefined)
# Strict recovery is the intended end state (CP_FAULT_REQUIRE_RESUME=1), but
# must NOT be registered as a passing safety check before LUNE-896/LUNE-897.
# CI exercises the sampled matrix and reports each failed recovery plus distinct
# state counts. Replace this coverage registration with the strict binary once
# both fixes land; do not treat a coverage-only exit 0 as proof of safety.
add_test(NAME companion_write_fault_coverage
    COMMAND "${CMAKE_COMMAND}" -E env CP_FAULT_COVERAGE_ONLY=1 "$<TARGET_FILE:companion_write_fault_test>")
set_tests_properties(companion_write_fault_coverage PROPERTIES TIMEOUT 120
    WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
