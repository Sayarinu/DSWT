"""Exercise the companion build orchestration without downloads or Docker."""
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]


class BuildCompanionTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix='companion build tests ')
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.scripts = self.root / 'scripts'
        self.scripts.mkdir()
        for group in ('build', 'prepare', 'port'):
            (self.scripts / group).mkdir()
        # The real profile tool and the tracked placeholder profile.
        for name in ('build-companion.sh', 'configure-connection.py'):
            shutil.copy2(ROOT / 'scripts/build' / name, self.scripts / 'build')
        (self.root / 'client/companion').mkdir(parents=True)
        shutil.copy2(ROOT / 'client/companion/connection-profile.json', self.root / 'client/companion')
        self.tls = self.root / 'TLS source'
        self.tls.mkdir()
        self.log = self.root / 'calls.jsonl'
        binary = self.root / 'fake tools'
        binary.mkdir()
        self.bash = '/bin/bash' if sys.platform == 'darwin' else shutil.which('bash')
        self.env = dict(os.environ, PATH=str(binary) + os.pathsep + os.environ['PATH'],
                        TEST_TLS=str(self.tls), TEST_LOG=str(self.log),
                        COMPANION_BUILD_JOBS='3')
        self.env.pop('CONNECTION_PROFILE', None)
        # The interpreter's path contains spaces; it must remain a single argument.
        self.env['PYTHON'] = str(binary / 'selected python')
        self.tool(binary / 'selected python', '''
record('python', sys.argv[1:])
if os.environ.get('TEST_OLD_PYTHON'):
    sys.version_info = (3, 11, 9)
    exec(sys.argv[2])
else:
    os.execv(sys.executable, [sys.executable, *sys.argv[1:]])
''')
        self.tool(self.scripts / 'prepare/prepare-tls.py', '''
record('tls', sys.argv[1:])
print(os.environ['TEST_TLS'])
''')
        self.tool(self.scripts / 'build/companion-text.py', '''
record('text', sys.argv[1:])
''')
        self.tool(binary / 'docker', '''
record('docker', sys.argv[1:])
if sys.argv[1] == 'run':
    out = next(Path(sys.argv[i + 1].removesuffix(':/out')) for i, arg in enumerate(sys.argv)
               if arg == '-v' and sys.argv[i + 1].endswith(':/out'))
    assert (out / 'dswt-connection.h').is_file()
    rom = next(arg for arg in sys.argv if arg.startswith('ROM=')).removeprefix('ROM=')
    (out / rom.removeprefix('/out/')).write_bytes(b'companion ROM')
''')

    def tool(self, path, source):
        path.write_text('#!' + sys.executable + '''
import json, os, sys
from pathlib import Path
def record(tool, args):
    with open(os.environ['TEST_LOG'], 'a') as log:
        log.write(json.dumps([tool, args]) + '\\n')
''' + source)
        path.chmod(0o700)

    def run_build(self, *args, env=None):
        self.log.unlink(missing_ok=True)
        result = subprocess.run([self.bash, str(self.scripts / 'build/build-companion.sh'), *args],
                                env=env or self.env, capture_output=True, text=True)
        self.calls = [json.loads(line) for line in self.log.read_text().splitlines()] if self.log.exists() else []
        return result

    def calls_for(self, tool):
        return [args for name, args in self.calls if name == tool]

    @property
    def output(self):
        return self.root / 'game/companion'

    def assert_built(self, result, hostname):
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(result.stdout.splitlines()[-1], str(self.output / 'dswt-companion.nds'))
        self.assertEqual((self.output / 'dswt-companion.nds').read_bytes(), b'companion ROM')
        self.assertIn(f'#define WONDER_TRADE_HOSTNAME "{hostname}"', (self.output / 'dswt-connection.h').read_text())
        self.assertFalse((self.output / 'dswt-connection.h.new').exists())
        self.assertEqual(self.calls_for('tls'), [[]])
        self.assertEqual(self.calls_for('text'), [['--output', str(self.output / 'build/text')]])
        builder = self.root / 'tools/builders/companion-builder'
        build, run = self.calls_for('docker')
        self.assertEqual(build, ['build', '-f', str(builder / 'Dockerfile'), '-t',
                                 'dswt-companion-builder', str(builder)])
        self.assertEqual(run[run.index('--user') + 1], f'{os.getuid()}:{os.getgid()}')
        mounts = [run[i + 1] for i, arg in enumerate(run) if arg == '-v']
        self.assertEqual(mounts, [f'{self.root}/client:/dswt/client:ro',
                                  f'{self.tls}:/tls:ro', f'{self.output}:/out'])
        self.assertEqual(run[run.index('-w') + 1], '/dswt/client/companion')
        make = run[run.index('dswt-companion-builder'):]
        self.assertEqual(make, ['dswt-companion-builder', 'make', '-j3', 'BUILDDIR=/out/build',
                                'ROM=/out/dswt-companion.nds', 'TLS_DIR=/tls',
                                'CONNECTION_HEADER=/out/dswt-connection.h'])

    def test_default_build_uses_the_placeholder_profile(self):
        self.assert_built(self.run_build(), 'companion.invalid')

    def test_connection_profile_configures_the_server(self):
        profile = self.root / 'my profile.json'
        profile.write_text(json.dumps({
            'hostname': 'trade.example', 'ipv4': '198.51.100.7', 'port': 8443,
            'current_spki_sha256': 'ab' * 32, 'next_spki_sha256': 'cd' * 32}))
        result = self.run_build(env=dict(self.env, CONNECTION_PROFILE=str(profile)))
        self.assert_built(result, 'trade.example')
        self.assertIn('#define WONDER_TRADE_AUTHORITY "trade.example:8443"',
                      (self.output / 'dswt-connection.h').read_text())

    def test_an_unchanged_profile_keeps_the_header_for_incremental_builds(self):
        self.assert_built(self.run_build(), 'companion.invalid')
        header = self.output / 'dswt-connection.h'
        os.utime(header, ns=(1, 1))
        self.assert_built(self.run_build(), 'companion.invalid')
        self.assertEqual(header.stat().st_mtime_ns, 1)

    def test_an_invalid_profile_stops_before_docker(self):
        profile = self.root / 'bad.json'
        profile.write_text(json.dumps({'hostname': 'trade.example'}))
        result = self.run_build(env=dict(self.env, CONNECTION_PROFILE=str(profile)))
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('Cannot configure connection', result.stderr)
        self.assertFalse(self.calls_for('docker'))
        self.assertFalse((self.output / 'dswt-connection.h').exists())

    def test_rejects_arguments_and_bad_job_counts_without_side_effects(self):
        for args, jobs in ((('--pristine',), '3'), ((), '0'), ((), '04'), ((), 'two')):
            with self.subTest(args=args, jobs=jobs):
                result = self.run_build(*args, env=dict(self.env, COMPANION_BUILD_JOBS=jobs))
                self.assertEqual(result.returncode, 2, result.stdout + result.stderr)
                self.assertEqual(self.calls, [])
                self.assertFalse(self.output.exists())

    def test_requires_python_3_12(self):
        result = self.run_build(env=dict(self.env, TEST_OLD_PYTHON='1'))
        self.assertEqual(result.returncode, 2, result.stdout + result.stderr)
        self.assertIn('Python 3.12 or newer is required', result.stderr)
        self.assertEqual([name for name, _ in self.calls], ['python'])


class CompanionBuilderTests(unittest.TestCase):
    def test_builder_image_is_pinned_by_digest(self):
        dockerfile = (ROOT / 'tools/builders/companion-builder/Dockerfile').read_text()
        self.assertRegex(dockerfile, re.compile(
            r'^FROM skylyrac/blocksds:slim-v\d+\.\d+\.\d+@sha256:[0-9a-f]{64}$', re.M))

    def test_placeholder_profile_names_no_real_server(self):
        profile = json.loads((ROOT / 'client/companion/connection-profile.json').read_text())
        self.assertTrue(profile['hostname'].endswith('.invalid'))
        self.assertTrue(profile['ipv4'].startswith('192.0.2.'))  # TEST-NET-1


if __name__ == '__main__':
    unittest.main()
