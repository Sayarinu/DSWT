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
tls_sources = []
foreach path : ['library', 'tf-psa-crypto/core', 'tf-psa-crypto/extras',
                'tf-psa-crypto/platform', 'tf-psa-crypto/utilities',
                'tf-psa-crypto/drivers/builtin/src']
    source_paths = run_command(find_program('python3'), '-c',
        'import pathlib,sys; print(chr(10).join(str(p) for p in sorted(pathlib.Path(sys.argv[1]).glob("*.c"))))',
        tls_root / path, check: true).stdout().strip().split('\n')
    foreach source : source_paths
        tls_sources += files(source)
    endforeach
endforeach
wt_tls = static_library('wonder_trade_tls', tls_sources,
    include_directories: wt_tls_includes, c_args: wt_tls_args + ['-thumb'], native: false)
