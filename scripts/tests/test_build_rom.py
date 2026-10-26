"""Exercise fresh/incremental ROM configuration without game inputs or Docker."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


class BuildRomTests(unittest.TestCase):
    def test_profile_is_available_at_first_setup_and_incremental_build(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            scripts = root / 'scripts'
            scripts.mkdir()
            shutil.copy2(Path(__file__).resolve().parents[1] / 'build-rom.sh', scripts)
            workspace = root / 'workspace'
            workspace.mkdir()
            for name in ('prepare-game.py', 'prepare-tls.py'):
                (scripts / name).write_text('print(' + repr(str(workspace)) + ')\n')
            (scripts / 'configure-connection.py').write_text('')
            binary = root / 'bin'
            binary.mkdir()
            docker = binary / 'docker'
            docker.write_text('''#!/usr/bin/env python3
import os, subprocess, sys
if sys.argv[1] == 'run':
    command = sys.argv[sys.argv.index('bash')+2]
    for i, arg in enumerate(sys.argv):
        if arg == '-e':
            key, value = sys.argv[i+1].split('=', 1)
            os.environ[key] = value
    raise SystemExit(subprocess.run(['bash', '-c', command], cwd=os.environ['TEST_WORKSPACE']).returncode)
''')
            make = binary / 'make'
            make.write_text('''#!/usr/bin/env python3
from pathlib import Path
import sys
if sys.argv[1:] == ['configure']:
    raise SystemExit('missing TLS options at first setup')
if sys.argv[1:] == ['release']:
    assert Path('build/build.ninja').exists()
    Path('built').touch()
''')
            meson = workspace / 'subprojects/meson-1.12.0/meson.py'
            meson.parent.mkdir(parents=True)
            meson.write_text('''import sys
from pathlib import Path
assert '-Dwonder_trade_tls_dir=/tls' in sys.argv
assert '-Dwonder_trade_profile_header=/workspace/.git/platinumwt-connection.h' in sys.argv
assert '-Drevision=1' in sys.argv
if sys.argv[1] == 'setup':
    assert '--native-file=meson/native.ini' in sys.argv
    assert '--cross-file=meson/cross_unix.ini' in sys.argv
    Path('build').mkdir()
    Path('build/build.ninja').touch()
else:
    assert sys.argv[1] == 'configure' and Path('build/build.ninja').exists()
''')
            for path in (docker, make):
                path.chmod(0o700)
            env = dict(os.environ, PATH=str(binary)+os.pathsep+os.environ['PATH'],
                       TEST_WORKSPACE=str(workspace), ROM_REVISION='1',
                       WONDER_TRADE_PROFILE=str(root/'profile.json'))
            for _ in range(2):
                result = subprocess.run(['bash', str(scripts/'build-rom.sh')], env=env,
                                        capture_output=True, text=True)
                self.assertEqual(result.returncode, 0, result.stdout+result.stderr)
                self.assertTrue((workspace/'built').exists())
                (workspace/'built').unlink()


if __name__ == '__main__':
    unittest.main()
