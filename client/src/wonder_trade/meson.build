tls_root = get_option('wonder_trade_tls_dir')
tls_include_paths = [
    'include', 'library', 'tf-psa-crypto/include', 'tf-psa-crypto/core',
    'tf-psa-crypto/dispatch', 'tf-psa-crypto/extras', 'tf-psa-crypto/platform',
    'tf-psa-crypto/utilities', 'tf-psa-crypto/drivers/builtin/include',
    'tf-psa-crypto/drivers/builtin/src', 'tf-psa-crypto/drivers/p256-m',
    'tf-psa-crypto/drivers/pqcp/include', 'tf-psa-crypto/drivers/everest/include',
]
wt_tls_includes = [public_includes]
foreach path : tls_include_paths
    wt_tls_includes += include_directories(tls_root / path)
endforeach
wt_tls_args = [
    '-DMBEDTLS_CONFIG_FILE="wonder_trade/tls_config.h"',
    '-DTF_PSA_CRYPTO_CONFIG_FILE="wonder_trade/crypto_config.h"',
]
# A game whose static binary must stay small names the few Mbed TLS sources its
# resident code needs; they become wt_tls_resident and leave wt_tls.
resident_names = get_variable('wt_tls_resident_sources', [])
tls_sources = []
tls_resident_sources = []
foreach path : ['library', 'tf-psa-crypto/core', 'tf-psa-crypto/extras',
                'tf-psa-crypto/platform', 'tf-psa-crypto/utilities',
                'tf-psa-crypto/drivers/builtin/src']
    source_paths = run_command(find_program('python3'), '-c',
        'import pathlib,sys; print(chr(10).join(str(p) for p in sorted(pathlib.Path(sys.argv[1]).glob("*.c"))))',
        tls_root / path, check: true).stdout().strip().split('\n')
    foreach source : source_paths
        resident = false
        foreach name : resident_names
            resident = resident or source.endswith('/' + name)
        endforeach
        if resident
            tls_resident_sources += files(source)
        else
            tls_sources += files(source)
        endif
    endforeach
endforeach
wt_tls = static_library('wonder_trade_tls', tls_sources,
    include_directories: wt_tls_includes, c_args: wt_tls_args + ['-thumb'],
    native: false, pic: false)
if resident_names.length() > 0
    assert(tls_resident_sources.length() == resident_names.length(), 'Missing resident Mbed TLS source')
    wt_tls_resident = static_library('wonder_trade_tls_resident', tls_resident_sources,
        include_directories: wt_tls_includes, c_args: wt_tls_args + ['-thumb'],
        native: false, pic: false)
endif

# Platinum's enrolled game variant (wiki Protocol, "Game variants"): European English is
# revision 10; US builds keep sending legacy code 1.
if meson.project_name() == 'pokeplatinum'
    wt_tls_args += '-DWT_GAME_VARIANT=' + (get_option('revision') == '10' ? '0x020C020Au' : '1u')
endif
