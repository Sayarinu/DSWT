cmake_minimum_required(VERSION 3.22)
project(dswt_companion_tests C)

# These test executables use assert() for their checks, including release builds.
add_compile_options(-UNDEBUG)
set(CMAKE_C_STANDARD 99)
set(CLIENT "${CMAKE_CURRENT_SOURCE_DIR}/../../client")

enable_testing()
add_executable(core_smoke_test core_smoke_test.c "${CLIENT}/src/wonder_trade/pokemon_codec.c")
target_include_directories(core_smoke_test PRIVATE "${CLIENT}/include")
target_compile_definitions(core_smoke_test PRIVATE WT_GAME_FAMILY=WT_FAMILY_GEN4)
target_compile_options(core_smoke_test PRIVATE -Wall -Wextra -Werror -fsanitize=address,undefined)
target_link_options(core_smoke_test PRIVATE -fsanitize=address,undefined)
add_test(NAME core_smoke_test COMMAND core_smoke_test)

# LUNE-866: portable Platinum save image and host file adapter.
set(COMPANION_SAVE "${CMAKE_CURRENT_SOURCE_DIR}/../src/save")
add_library(companion_save STATIC
    "${COMPANION_SAVE}/save_image.c"
    "${COMPANION_SAVE}/sha256.c"
    "${COMPANION_SAVE}/posix_file.c")
target_include_directories(companion_save PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/../src")
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
target_include_directories(game_data_test PRIVATE "${COMPANION_SRC}")
target_compile_options(game_data_test PRIVATE -Wall -Wextra -Werror -pedantic -fsanitize=address,undefined)
target_link_options(game_data_test PRIVATE -fsanitize=address,undefined)
add_test(NAME game_data_test COMMAND game_data_test WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
