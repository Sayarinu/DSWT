# LUNE-881: the companion links one build of the client core for every game
# family, with journal.c built per family (src/core/journal_family.h). Every
# other core source it compiles must preprocess to the same text whatever the
# family, or the one build would serve some game wrongly. The ROM build runs
# the same check with the ARM compiler and Mbed TLS (companion/Makefile,
# family-check); here a third-party header the host lacks (Mbed TLS) becomes
# an empty one, which cannot depend on the family either.
# Run with -DCC=<compiler> -DCLIENT=<client dir> -DOUT=<scratch dir>.
set(sources engine.c wire.c http.c tls.c seed.c recovery.c entropy.c pokemon_codec.c boot.c session.c)
set(stub "${OUT}/family-stub")
file(REMOVE_RECURSE "${stub}")
file(MAKE_DIRECTORY "${stub}")
set(include "-I${CLIENT}/include" "-I${stub}")
foreach(source ${sources})
    unset(first)
    foreach(family WT_FAMILY_GEN4 WT_FAMILY_BW WT_FAMILY_B2W2)
        foreach(attempt RANGE 64)
            execute_process(COMMAND "${CC}" -E -P ${include} -DWT_GAME_FAMILY=${family}
                    "-DMBEDTLS_CONFIG_FILE=\"wonder_trade/tls_config.h\""
                    "-DTF_PSA_CRYPTO_CONFIG_FILE=\"wonder_trade/crypto_config.h\""
                    "${CLIENT}/src/wonder_trade/${source}"
                OUTPUT_VARIABLE text ERROR_VARIABLE error RESULT_VARIABLE result)
            if(result EQUAL 0)
                break()
            endif()
            # Clang: 'x.h' file not found; GCC: x.h: No such file or directory.
            if(error MATCHES "'((mbedtls|psa)/[A-Za-z0-9_/]+\\.h)' file not found")
                file(WRITE "${stub}/${CMAKE_MATCH_1}" "")
            elseif(error MATCHES "[ <]((mbedtls|psa)/[A-Za-z0-9_/]+\\.h): No such file")
                file(WRITE "${stub}/${CMAKE_MATCH_1}" "")
            else()
                message(FATAL_ERROR "${source} (${family}): ${error}")
            endif()
        endforeach()
        if(NOT result EQUAL 0)
            message(FATAL_ERROR "${source} (${family}): ${error}")
        endif()
        # Compared by name: preprocessed C is full of ';', CMake's list separator.
        if(NOT DEFINED first)
            set(first "${text}")
        elseif(NOT text STREQUAL first)
            message(FATAL_ERROR "${source} depends on WT_GAME_FAMILY: build it per family in the companion")
        endif()
    endforeach()
    message(STATUS "${source}: family-independent")
endforeach()
