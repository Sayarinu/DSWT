add_executable(companion_write_fault_test write_fault_test.c
    "${COMPANION_SAVE}/save_image.c" "${COMPANION_SAVE}/gen5_save.c" "${COMPANION_SAVE}/sha256.c" "${COMPANION_SAVE}/posix_file.c"
    "${COMPANION_SAVE}/journal_save.c" "${COMPANION_SAVE}/journal_host_io.c" "${COMPANION_SAVE}/musical_area.c"
    "${COMPANION_SRC}/platform/journal_io.c" "${COMPANION_SRC}/adapter/platinum.c"
    "${COMPANION_SRC}/adapter/platinum_receive.c"
    "${COMPANION_SRC}/session/visit.c"
    "${CLIENT}/src/wonder_trade/pokemon_codec.c"
    "${CLIENT}/src/wonder_trade/wire.c" "${CLIENT}/src/wonder_trade/engine.c"
    "${CLIENT}/src/wonder_trade/session.c")
target_include_directories(companion_write_fault_test PRIVATE "${COMPANION_SRC}" "${CLIENT}/include")
target_compile_definitions(companion_write_fault_test PRIVATE WT_GAME_FAMILY=WT_FAMILY_GEN4
    CP_FAULT_BINARY_DIR="${CMAKE_CURRENT_BINARY_DIR}")
target_compile_options(companion_write_fault_test PRIVATE -O2 -Wall -Wextra -Werror -pedantic -fsanitize=address,undefined)
target_link_options(companion_write_fault_test PRIVATE -fsanitize=address,undefined)
target_link_libraries(companion_write_fault_test PRIVATE companion_core_journal)
# All recovery assertions are mandatory; only exact, still-failing ticketed
# exceptions are accepted. The full sweep remains an opt-in diagnostic.
add_test(NAME companion_write_fault_test COMMAND companion_write_fault_test)
set_tests_properties(companion_write_fault_test PROPERTIES TIMEOUT 120
    WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")

# LUNE-880: the same matrix over Diamond/Pearl and HeartGold/SoulSilver saves.
foreach(fault_game 1 2)
    set(fault_target companion_write_fault_test_game${fault_game})
    add_executable(${fault_target} write_fault_test.c
        "${COMPANION_SAVE}/save_image.c" "${COMPANION_SAVE}/gen5_save.c" "${COMPANION_SAVE}/sha256.c" "${COMPANION_SAVE}/posix_file.c"
        "${COMPANION_SAVE}/journal_save.c" "${COMPANION_SAVE}/journal_host_io.c" "${COMPANION_SAVE}/musical_area.c"
        "${COMPANION_SRC}/platform/journal_io.c" "${COMPANION_SRC}/adapter/platinum.c"
        "${COMPANION_SRC}/adapter/platinum_receive.c"
        "${COMPANION_SRC}/session/visit.c"
        "${CLIENT}/src/wonder_trade/pokemon_codec.c"
        "${CLIENT}/src/wonder_trade/wire.c" "${CLIENT}/src/wonder_trade/engine.c"
        "${CLIENT}/src/wonder_trade/session.c")
    target_include_directories(${fault_target} PRIVATE "${COMPANION_SRC}" "${CLIENT}/include")
    target_compile_definitions(${fault_target} PRIVATE WT_GAME_FAMILY=WT_FAMILY_GEN4 FX_GAME=${fault_game}
        CP_FAULT_BINARY_DIR="${CMAKE_CURRENT_BINARY_DIR}/game${fault_game}")
    target_compile_options(${fault_target} PRIVATE -O2 -Wall -Wextra -Werror -pedantic -fsanitize=address,undefined)
    target_link_options(${fault_target} PRIVATE -fsanitize=address,undefined)
    target_link_libraries(${fault_target} PRIVATE companion_core_journal)
    file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/game${fault_game}")
    add_test(NAME ${fault_target} COMMAND ${fault_target})
    set_tests_properties(${fault_target} PROPERTIES TIMEOUT 300
        WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/game${fault_game}")
endforeach()
