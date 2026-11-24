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

# The portable part of the DSi platform layer (src/platform).
set(COMPANION "${CMAKE_CURRENT_SOURCE_DIR}/..")
add_executable(journal_io_test journal_io_test.c "${COMPANION}/src/platform/journal_io.c"
    "${CLIENT}/src/wonder_trade/journal.c" "${CLIENT}/src/wonder_trade/wire.c")
add_executable(entropy_pool_test entropy_pool_test.c "${COMPANION}/src/platform/entropy_pool.c")
foreach(test journal_io_test entropy_pool_test)
    target_include_directories(${test} PRIVATE "${COMPANION}/src" "${CLIENT}/include")
    target_compile_definitions(${test} PRIVATE WT_GAME_FAMILY=WT_FAMILY_GEN4)
    target_compile_options(${test} PRIVATE -Wall -Wextra -Werror -fsanitize=address,undefined)
    target_link_options(${test} PRIVATE -fsanitize=address,undefined)
    add_test(NAME ${test} COMMAND ${test})
endforeach()
