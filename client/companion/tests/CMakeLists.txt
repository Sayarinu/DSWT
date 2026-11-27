cmake_minimum_required(VERSION 3.22)
project(dswt_companion_tests C)

# Keep the sanitizer-backed host suite within the CI runtime budget.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Host test build type" FORCE)
endif()

# These test executables use assert() for their checks, including release builds.
add_compile_options(-UNDEBUG)
set(CMAKE_C_STANDARD 99)
# The companion's engine and journal may abandon an unsent escrow (LUNE-902),
# as client/companion/Makefile builds them.
add_compile_definitions(WT_ABANDON_UNSENT)
set(CLIENT "${CMAKE_CURRENT_SOURCE_DIR}/../../core")

enable_testing()
add_executable(core_smoke_test core_smoke_test.c "${CLIENT}/src/wonder_trade/pokemon_codec.c")
target_include_directories(core_smoke_test PRIVATE "${CLIENT}/include")
target_compile_definitions(core_smoke_test PRIVATE WT_GAME_FAMILY=WT_FAMILY_GEN4)
target_compile_options(core_smoke_test PRIVATE -Wall -Wextra -Werror -fsanitize=address,undefined)
target_link_options(core_smoke_test PRIVATE -fsanitize=address,undefined)
add_test(NAME core_smoke_test COMMAND core_smoke_test)

# The portable part of the DSi platform layer (src/platform).
set(COMPANION "${CMAKE_CURRENT_SOURCE_DIR}/..")

# LUNE-881: the client journal, built once per family with renamed symbols,
# behind the companion's runtime family selection (src/core/journal_family.h).
foreach(journal_family GEN4 GEN5)
    if(journal_family STREQUAL "GEN4")
        set(journal_define WT_FAMILY_GEN4)
    else()
        set(journal_define WT_FAMILY_BW)
    endif()
    add_library(companion_journal_${journal_family} OBJECT "${CLIENT}/src/wonder_trade/journal.c")
    target_include_directories(companion_journal_${journal_family} PRIVATE "${CLIENT}/include" "${COMPANION}/src")
    target_compile_definitions(companion_journal_${journal_family} PRIVATE WT_GAME_FAMILY=${journal_define})
    string(REPLACE "GEN" "Gen" journal_prefix "CpJournal${journal_family}")
    target_compile_definitions(companion_journal_${journal_family} PRIVATE CP_JOURNAL_PREFIX=${journal_prefix})
    target_compile_options(companion_journal_${journal_family} PRIVATE -include
        "${COMPANION}/src/core/journal_rename.h" -Wall -Wextra -Werror -fsanitize=address,undefined)
endforeach()
add_library(companion_core_journal STATIC "${COMPANION}/src/core/journal_family.c"
    $<TARGET_OBJECTS:companion_journal_GEN4> $<TARGET_OBJECTS:companion_journal_GEN5>)
target_include_directories(companion_core_journal PUBLIC "${CLIENT}/include" "${COMPANION}/src")
target_compile_options(companion_core_journal PRIVATE -Wall -Wextra -Werror -pedantic -fsanitize=address,undefined)
target_link_options(companion_core_journal PUBLIC -fsanitize=address,undefined)
add_executable(core_family_test core_family_test.c)
target_link_libraries(core_family_test PRIVATE companion_core_journal)
target_compile_options(core_family_test PRIVATE -Wall -Wextra -Werror -pedantic -fsanitize=address,undefined)
add_test(NAME core_family_test COMMAND core_family_test)
# Every other core source the companion builds preprocesses the same for each family.
add_test(NAME core_family_independence_test
    COMMAND "${CMAKE_COMMAND}" -DCC=${CMAKE_C_COMPILER} -DCLIENT=${CLIENT} -DOUT=${CMAKE_CURRENT_BINARY_DIR}
        -P "${CMAKE_CURRENT_SOURCE_DIR}/core_family_independence.cmake")
add_executable(journal_io_test journal_io_test.c "${COMPANION}/src/platform/journal_io.c"
    "${CLIENT}/src/wonder_trade/wire.c")
add_executable(entropy_pool_test entropy_pool_test.c "${COMPANION}/src/platform/entropy_pool.c")
add_executable(wifi_status_test wifi_status_test.c "${COMPANION}/src/platform/wifi_status.c")
foreach(test journal_io_test entropy_pool_test wifi_status_test)
    target_include_directories(${test} PRIVATE "${COMPANION}/src" "${CLIENT}/include")
    target_compile_definitions(${test} PRIVATE WT_GAME_FAMILY=WT_FAMILY_GEN4)
    target_compile_options(${test} PRIVATE -Wall -Wextra -Werror -fsanitize=address,undefined)
    target_link_options(${test} PRIVATE -fsanitize=address,undefined)
    add_test(NAME ${test} COMMAND ${test})
endforeach()
target_link_libraries(journal_io_test PRIVATE companion_core_journal)

# LUNE-866: portable Platinum save image and host file adapter.
set(COMPANION_SAVE "${CMAKE_CURRENT_SOURCE_DIR}/../src/save")
add_library(companion_save STATIC
    "${COMPANION_SAVE}/save_image.c"
    "${COMPANION_SAVE}/gen5_save.c"
    "${COMPANION_SAVE}/sha256.c"
    "${COMPANION_SAVE}/posix_file.c")
target_include_directories(companion_save PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/../src" "${CLIENT}/include")
target_compile_options(companion_save PRIVATE -Wall -Wextra -Werror -pedantic -fsanitize=address,undefined)
target_link_options(companion_save PUBLIC -fsanitize=address,undefined)

add_executable(save_image_test save/save_image_test.c "${CLIENT}/src/wonder_trade/pokemon_codec.c")
target_include_directories(save_image_test PRIVATE "${CLIENT}/include")
target_compile_definitions(save_image_test PRIVATE WT_GAME_FAMILY=WT_FAMILY_GEN4)
target_link_libraries(save_image_test PRIVATE companion_save)
add_executable(posix_file_test save/posix_file_test.c)
target_link_libraries(posix_file_test PRIVATE companion_save)
add_executable(save_sha256_probe save/sha256_probe.c)
target_link_libraries(save_sha256_probe PRIVATE companion_save)
foreach(save_target save_image_test posix_file_test save_sha256_probe)
    target_compile_options(${save_target} PRIVATE -Wall -Wextra -Werror -pedantic -fsanitize=address,undefined)
endforeach()
add_test(NAME save_image_test COMMAND save_image_test)
# LUNE-880: the Diamond/Pearl and HeartGold/SoulSilver layouts.
add_executable(save_layout_test save/save_layout_test.c)
target_link_libraries(save_layout_test PRIVATE companion_save)
target_compile_options(save_layout_test PRIVATE -O2 -Wall -Wextra -Werror -pedantic -fsanitize=address,undefined)
add_test(NAME save_layout_test COMMAND save_layout_test)
# LUNE-881: the Black/White (0) and Black 2/White 2 (1) layouts.
foreach(gen5_game 0 1)
    add_executable(gen5_save_test_game${gen5_game} save/gen5_save_test.c)
    target_compile_definitions(gen5_save_test_game${gen5_game} PRIVATE FX_GAME=${gen5_game})
    target_link_libraries(gen5_save_test_game${gen5_game} PRIVATE companion_save)
    target_compile_options(gen5_save_test_game${gen5_game} PRIVATE -O2 -Wall -Wextra -Werror -pedantic
        -fsanitize=address,undefined)
    add_test(NAME gen5_save_test_game${gen5_game} COMMAND gen5_save_test_game${gen5_game})
endforeach()
add_test(NAME posix_file_test COMMAND posix_file_test)
find_package(Python3 3.12 REQUIRED COMPONENTS Interpreter)
add_test(NAME save_image_integration_test
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/save/save_image_integration_test.py"
        "$<TARGET_FILE:save_image_test>" "$<TARGET_FILE:save_sha256_probe>")

# --- ROM reader (LUNE-867): header, NitroFS, NARC and game data -------------
set(COMPANION_SRC "${CMAKE_CURRENT_SOURCE_DIR}/../src")
add_executable(game_data_test game_data_test.c posix_reader.c
    "${COMPANION_SRC}/game/rom.c" "${COMPANION_SRC}/game/nitrofs.c" "${COMPANION_SRC}/game/gamedata.c"
    "${COMPANION_SRC}/game/text.c" "${COMPANION_SRC}/game/pairing.c")
target_include_directories(game_data_test PRIVATE "${COMPANION_SRC}" "${CLIENT}/include")
target_compile_options(game_data_test PRIVATE -Wall -Wextra -Werror -pedantic -fsanitize=address,undefined)
target_link_options(game_data_test PRIVATE -fsanitize=address,undefined)
add_test(NAME game_data_test COMMAND game_data_test WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
# LUNE-881: Generation V data against owned dumps given as arguments.
add_executable(gen5_rom_check gen5_rom_check.c posix_reader.c
    "${COMPANION_SRC}/game/rom.c" "${COMPANION_SRC}/game/nitrofs.c" "${COMPANION_SRC}/game/gamedata.c"
    "${COMPANION_SRC}/game/text.c")
target_include_directories(gen5_rom_check PRIVATE "${COMPANION_SRC}" "${CLIENT}/include")
target_compile_options(gen5_rom_check PRIVATE -Wall -Wextra -Werror -pedantic -fsanitize=address,undefined)
target_link_options(gen5_rom_check PRIVATE -fsanitize=address,undefined)
add_test(NAME gen5_rom_check COMMAND gen5_rom_check)

# LUNE-870: save journal detection/provisioning and backup lifecycle.
add_library(companion_journal STATIC
    "${COMPANION_SAVE}/journal_save.c"
    "${COMPANION_SAVE}/musical_area.c"
    "${COMPANION_SAVE}/journal_host_io.c"
    "${COMPANION_SAVE}/backup_save.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/../src/platform/journal_io.c"
   )
target_include_directories(companion_journal PUBLIC "${CLIENT}/include")
target_compile_definitions(companion_journal PRIVATE WT_GAME_FAMILY=WT_FAMILY_GEN4)
target_compile_options(companion_journal PRIVATE -Wall -Wextra -Werror -pedantic -fsanitize=address,undefined)
target_link_libraries(companion_journal PUBLIC companion_save companion_core_journal)
# LUNE-901: a first enrollment write cut in any SD sector order.
add_executable(torn_enrollment_test journal/torn_enrollment_test.c "${CLIENT}/src/wonder_trade/wire.c")
target_link_libraries(torn_enrollment_test PRIVATE companion_journal)
target_compile_options(torn_enrollment_test PRIVATE -Wall -Wextra -Werror -pedantic -fsanitize=address,undefined)
add_test(NAME torn_enrollment_test COMMAND torn_enrollment_test)
foreach(cp_journal_test journal_save_test backup_save_test gen5_journal_test)
    add_executable(${cp_journal_test} "journal/${cp_journal_test}.c")
    target_link_libraries(${cp_journal_test} PRIVATE companion_journal)
    target_compile_options(${cp_journal_test} PRIVATE -Wall -Wextra -Werror -pedantic -fsanitize=address,undefined)
    add_test(NAME ${cp_journal_test} COMMAND ${cp_journal_test})
endforeach()
add_test(NAME cp_journal_integration_test
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/journal/journal_integration_test.py"
        "$<TARGET_FILE:save_image_test>" "$<TARGET_FILE:journal_save_test>")

# --- Screens (LUNE-872): state machine and renderer -------------------------
# With the generated text tables (scripts/build/companion-text.py; the pinned font
# downloads once into game/companion/font).
set(COMPANION_TEXT_DIR "${CMAKE_CURRENT_BINARY_DIR}/text")
set(REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../..")
add_custom_command(
    OUTPUT "${COMPANION_TEXT_DIR}/companion_text.c" "${COMPANION_TEXT_DIR}/companion_text.h"
    COMMAND "${Python3_EXECUTABLE}" "${REPO_ROOT}/scripts/build/companion-text.py" --output "${COMPANION_TEXT_DIR}"
    DEPENDS "${REPO_ROOT}/scripts/build/companion-text.py" "${REPO_ROOT}/client/games/res/text/wonder_trade.json"
        "${REPO_ROOT}/client/companion/res/text/companion.json"
    VERBATIM)
add_library(companion_ui STATIC "${COMPANION_TEXT_DIR}/companion_text.c"
    "${COMPANION_SRC}/ui/ui.c" "${COMPANION_SRC}/ui/canvas.c" "${COMPANION_SRC}/ui/render.c"
    "${COMPANION_SRC}/ui/demo.c" "${COMPANION_SRC}/ui/keyboard.c" "${COMPANION_SRC}/platform/wifi_status.c"
    "${COMPANION_SRC}/game/text.c" "${COMPANION_SRC}/game/gamedata.c"
    "${COMPANION_SRC}/game/nitrofs.c" "${COMPANION_SRC}/game/rom.c")
target_include_directories(companion_ui PUBLIC "${COMPANION_SRC}" "${CLIENT}/include" "${COMPANION_TEXT_DIR}")
target_compile_definitions(companion_ui PUBLIC WT_GAME_FAMILY=WT_FAMILY_GEN4)
set_target_properties(companion_ui PROPERTIES C_STANDARD 11)
target_compile_options(companion_ui PUBLIC -Wall -Wextra -Werror -fsanitize=address,undefined)
target_link_options(companion_ui PUBLIC -fsanitize=address,undefined)
foreach(test ui_state_test ui_render_test ui_layout_test)
    add_executable(${test} ui/${test}.c)
    set_target_properties(${test} PROPERTIES C_STANDARD 11)
    target_link_libraries(${test} PRIVATE companion_ui)
    add_test(NAME ${test} COMMAND ${test})
endforeach()

# --- Platinum offer adapter and visit driver (LUNE-871) ---------------------
add_library(companion_adapter STATIC "${COMPANION_SRC}/game/portrait.c" "${COMPANION_SRC}/adapter/platinum.c" "${COMPANION_SRC}/adapter/holdable.c"
    "${COMPANION_SRC}/adapter/gen5.c" "${COMPANION_SRC}/adapter/gen5_receive.c"
    "${COMPANION_SRC}/adapter/platinum_receive.c" "${COMPANION_SRC}/adapter/rom_data.c"
    "${COMPANION_SRC}/game/rom.c" "${COMPANION_SRC}/game/nitrofs.c" "${COMPANION_SRC}/game/gamedata.c"
    "${COMPANION_SRC}/game/text.c" "${COMPANION_SRC}/game/pairing.c"
    "${CLIENT}/src/wonder_trade/pokemon_codec.c")
target_include_directories(companion_adapter PUBLIC "${COMPANION_SRC}" "${CLIENT}/include")
target_compile_definitions(companion_adapter PUBLIC WT_GAME_FAMILY=WT_FAMILY_GEN4)
target_link_libraries(companion_adapter PUBLIC companion_save companion_journal)
# Exercise both read-only ROM transports against the same game archives.
add_executable(portrait_test ui/portrait_test.c posix_reader.c)
target_link_libraries(portrait_test PRIVATE companion_adapter companion_ui)
add_test(NAME portrait_test COMMAND portrait_test)

add_executable(card_rom_test card/card_rom_test.c card/sim_card.c posix_reader.c)
target_link_libraries(card_rom_test PRIVATE companion_adapter)
add_executable(card_slot_test card/card_slot_test.c "${COMPANION_SRC}/platform/nds/card_slot.c")
target_include_directories(card_slot_test PRIVATE card/stub)
target_link_libraries(card_slot_test PRIVATE companion_save)
foreach(card_test card_rom_test card_slot_test)
    target_sources(${card_test} PRIVATE "${COMPANION_SRC}/platform/nds/card_key.c" "${COMPANION_SRC}/platform/nds/card_rom.c")
    target_include_directories(${card_test} PRIVATE "${COMPANION_SRC}")
    target_compile_options(${card_test} PRIVATE -Wall -Wextra -Werror -pedantic -fsanitize=address,undefined)
    target_link_options(${card_test} PRIVATE -fsanitize=address,undefined)
    add_test(NAME ${card_test} COMMAND ${card_test})
endforeach()
set_tests_properties(card_slot_test PROPERTIES TIMEOUT 5)

add_executable(platinum_adapter_test adapter/platinum_test.c)
add_executable(platinum_receive_test adapter/platinum_receive_test.c "${COMPANION_SRC}/session/visit.c"
    "${CLIENT}/src/wonder_trade/engine.c"
    "${CLIENT}/src/wonder_trade/wire.c" "${CLIENT}/src/wonder_trade/session.c")
target_include_directories(platinum_receive_test PRIVATE "${CLIENT}/tests")
add_executable(visit_test session/visit_test.c "${COMPANION_SRC}/session/visit.c"
    "${CLIENT}/src/wonder_trade/engine.c"
    "${CLIENT}/src/wonder_trade/wire.c" "${CLIENT}/src/wonder_trade/session.c")
target_include_directories(visit_test PRIVATE "${CLIENT}/tests")
foreach(adapter_target companion_adapter platinum_adapter_test platinum_receive_test visit_test)
    target_compile_options(${adapter_target} PRIVATE -Wall -Wextra -Werror -pedantic -fsanitize=address,undefined)
endforeach()
foreach(adapter_test platinum_adapter_test platinum_receive_test visit_test)
    target_link_libraries(${adapter_test} PRIVATE companion_adapter companion_core_journal)
    add_test(NAME ${adapter_test} COMMAND ${adapter_test})
endforeach()

# LUNE-880: the same adapter on Diamond/Pearl (1) and HeartGold/SoulSilver (2) saves.
foreach(adapter_game 1 2)
    add_executable(gen4_adapter_test_game${adapter_game} adapter/gen4_adapter_test.c)
    target_compile_definitions(gen4_adapter_test_game${adapter_game} PRIVATE FX_GAME=${adapter_game})
    target_compile_options(gen4_adapter_test_game${adapter_game} PRIVATE -Wall -Wextra -Werror -pedantic
        -fsanitize=address,undefined)
    target_link_libraries(gen4_adapter_test_game${adapter_game} PRIVATE companion_adapter)
    add_test(NAME gen4_adapter_test_game${adapter_game} COMMAND gen4_adapter_test_game${adapter_game})
endforeach()

# LUNE-881: the Generation V adapter on Black/White (0) and Black 2/White 2 (1) saves.
foreach(adapter_game 0 1)
    add_executable(gen5_adapter_test_game${adapter_game} adapter/gen5_adapter_test.c)
    target_compile_definitions(gen5_adapter_test_game${adapter_game} PRIVATE FX5_GAME=${adapter_game})
    target_compile_options(gen5_adapter_test_game${adapter_game} PRIVATE -Wall -Wextra -Werror -pedantic
        -fsanitize=address,undefined)
    target_link_libraries(gen5_adapter_test_game${adapter_game} PRIVATE companion_adapter)
    add_test(NAME gen5_adapter_test_game${adapter_game} COMMAND gen5_adapter_test_game${adapter_game})
endforeach()

foreach(adapter_game 0 1)
    add_executable(gen5_stock_test_game${adapter_game} adapter/gen5_stock_test.c)
    target_compile_definitions(gen5_stock_test_game${adapter_game} PRIVATE FX5_GAME=${adapter_game})
    target_compile_options(gen5_stock_test_game${adapter_game} PRIVATE -O1 -Wall -Wextra -Werror -pedantic
        -fsanitize=address,undefined)
    target_link_libraries(gen5_stock_test_game${adapter_game} PRIVATE companion_adapter)
    add_test(NAME gen5_stock_test_game${adapter_game} COMMAND gen5_stock_test_game${adapter_game})
endforeach()

# LUNE-899: a stock-game save between a cut companion step and its resume.
# The sweeps build the adapter themselves at -O1 (still with the sanitizers):
# every case hashes and checks whole saves, and must stay within CI's budget.
set(STOCK_SOURCES "${COMPANION_SRC}/adapter/platinum.c" "${COMPANION_SRC}/adapter/platinum_receive.c"
    "${COMPANION_SRC}/adapter/gen5.c" "${COMPANION_SRC}/adapter/gen5_receive.c"
    "${COMPANION_SAVE}/journal_save.c" "${COMPANION_SAVE}/musical_area.c" "${COMPANION_SRC}/platform/journal_io.c"
    "${COMPANION_SAVE}/save_image.c" "${COMPANION_SAVE}/gen5_save.c" "${COMPANION_SAVE}/sha256.c"
    "${CLIENT}/src/wonder_trade/pokemon_codec.c")
add_executable(stock_save_test adapter/stock_save_test.c ${STOCK_SOURCES})
add_executable(stock_resume_test session/stock_resume_test.c ${STOCK_SOURCES} "${COMPANION_SRC}/session/visit.c"
    "${CLIENT}/src/wonder_trade/engine.c"
    "${CLIENT}/src/wonder_trade/wire.c" "${CLIENT}/src/wonder_trade/session.c")
foreach(stock_test stock_save_test stock_resume_test)
    target_include_directories(${stock_test} PRIVATE "${COMPANION_SRC}" "${CLIENT}/include" "${CLIENT}/tests")
    target_compile_definitions(${stock_test} PRIVATE WT_GAME_FAMILY=WT_FAMILY_GEN4)
    target_compile_options(${stock_test} PRIVATE -O1 -Wall -Wextra -Werror -pedantic -fsanitize=address,undefined)
    target_link_options(${stock_test} PRIVATE -fsanitize=address,undefined)
    target_link_libraries(${stock_test} PRIVATE companion_core_journal)
    add_test(NAME ${stock_test} COMMAND ${stock_test})
endforeach()

# --- Session model (LUNE-895): saves, ROMs, adapter and visits together ----
# A synchronous fake worker, files in a temporary folder and the in-memory
# server; boot.c and recovery.c see a stub Mbed TLS header.
add_executable(app_model_test app/model_test.c posix_reader.c "${COMPANION_SRC}/game/twilight.c"
    "${COMPANION_SRC}/app/model.c" "${COMPANION_SRC}/platform/wifi_config.c"
    "${COMPANION_SRC}/session/visit.c"
    "${CLIENT}/src/wonder_trade/engine.c" "${CLIENT}/src/wonder_trade/wire.c"
    "${CLIENT}/src/wonder_trade/session.c" "${CLIENT}/src/wonder_trade/boot.c"
    "${CLIENT}/src/wonder_trade/recovery.c")
target_sources(app_model_test PRIVATE card_save/sim.c "${COMPANION_SRC}/platform/nds/card_store.c"
    "${COMPANION_SRC}/platform/nds/card_save.c")
target_compile_definitions(app_model_test PRIVATE CP_CARD_SAVE_WRITES=1)
target_include_directories(app_model_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/app/stub" "${CLIENT}/tests")
set_target_properties(app_model_test PROPERTIES C_STANDARD 11)
target_compile_options(app_model_test PRIVATE -Wall -Wextra -Werror -fsanitize=address,undefined)
target_link_libraries(app_model_test PRIVATE companion_adapter companion_journal companion_ui)
add_test(NAME app_model_test COMMAND app_model_test)

# LUNE-874: host-only power-cut matrix over save and journal persistence.
add_subdirectory(fault)
add_subdirectory(card_save)

# --- Wi-Fi settings (LUNE-875): wifi.cfg, key rules and the keyboard --------
add_executable(wifi_config_test wifi_config_test.c "${COMPANION_SRC}/platform/wifi_config.c"
    "${COMPANION_SRC}/platform/wifi_status.c")
target_include_directories(wifi_config_test PRIVATE "${COMPANION_SRC}")
target_compile_options(wifi_config_test PRIVATE -Wall -Wextra -Werror -pedantic -fsanitize=address,undefined)
target_link_options(wifi_config_test PRIVATE -fsanitize=address,undefined)
add_test(NAME wifi_config_test COMMAND wifi_config_test)
add_executable(ui_keyboard_test ui/ui_keyboard_test.c)
set_target_properties(ui_keyboard_test PROPERTIES C_STANDARD 11)
target_link_libraries(ui_keyboard_test PRIVATE companion_ui)
add_test(NAME ui_keyboard_test COMMAND ui_keyboard_test)
