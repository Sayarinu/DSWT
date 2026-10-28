"""Exercise ROM build orchestration without game inputs, downloads or Docker."""
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest


class BuildRomTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix='rom build tests ')
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.scripts = self.root / 'scripts'
        self.scripts.mkdir()
        shutil.copy2(Path(__file__).resolve().parents[1] / 'build-rom.sh', self.scripts)
        self.workspace = self.root / 'game workspace'
        (self.workspace / '.git').mkdir(parents=True)
        self.tls = self.root / 'TLS source'
        self.tls.mkdir()
        self.log = self.root / 'calls.jsonl'
        self.originals = {self.root / 'original.nds': b'original ROM',
                          self.root / 'original.sav': b'original save',
                          self.workspace / 'local.txt': b'local workspace contents'}
        for path, content in self.originals.items():
            path.write_bytes(content)
        self.profile = self.root / 'connection profile.json'
        self.profile.write_text('{}')
        binary = self.root / 'fake tools'
        binary.mkdir()
        self.bash = '/bin/bash' if sys.platform == 'darwin' else shutil.which('bash')
        self.env = dict(os.environ, PATH=str(binary) + os.pathsep + os.environ['PATH'],
                        TEST_WORKSPACE=str(self.workspace), TEST_TLS=str(self.tls),
                        TEST_LOG=str(self.log), TEST_BASH=self.bash,
                        ROM_REVISION='1', WONDER_TRADE_PROFILE=str(self.profile),
                        DOCKER_DEFAULT_PLATFORM='linux/arm64')
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
        self.tool(self.scripts / 'prepare-game.py', '''
record('prepare', sys.argv[1:])
if os.environ.get('TEST_PREPARE_FAIL'):
    raise SystemExit('Preserve local workspace edits')
print(os.environ['TEST_WORKSPACE'])
''')
        self.tool(self.scripts / 'prepare-tls.py', '''
record('tls', sys.argv[1:])
print(os.environ['TEST_TLS'])
''')
        self.tool(self.scripts / 'configure-connection.py', '''
record('profile', sys.argv[1:])
assert Path(sys.argv[1]).is_file()
Path(sys.argv[2]).write_text('configured profile')
''')
        self.tool(binary / 'docker', '''
record('docker', sys.argv[1:])
if sys.argv[1] == 'run':
    for i, arg in enumerate(sys.argv):
        if arg == '-e':
            key, value = sys.argv[i + 1].split('=', 1)
            os.environ[key] = value
    command = sys.argv[sys.argv.index('bash') + 2]
    raise SystemExit(subprocess.run([os.environ['TEST_BASH'], '-c', command],
                                   cwd=os.environ['TEST_WORKSPACE']).returncode)
assert sys.argv[1] == 'build'
''')
        self.tool(binary / 'make', '''
record('make', sys.argv[1:])
if sys.argv[1:] == ['configure']:
    Path('build').mkdir(exist_ok=True)
    Path('build/build.ninja').touch()
elif sys.argv[1:] in (['release'], ['release', 'check']):
    assert Path('build/build.ninja').is_file()
    Path('built').touch()
else:
    assert sys.argv[1:] == ['meson', 'skrew', 'build/lsfgen.stamp']
''')
        meson = self.workspace / 'subprojects/meson-1.12.0/meson.py'
        meson.parent.mkdir(parents=True)
        self.tool(meson, '''
record('meson', sys.argv[1:])
if sys.argv[1] == 'setup':
    Path('build').mkdir()
    Path('build/build.ninja').touch()
else:
    assert sys.argv[1] == 'configure' and Path('build/build.ninja').is_file()
''')

    def tool(self, path, source):
        path.write_text('#!' + sys.executable + '''
import json, os, subprocess, sys
from pathlib import Path
def record(tool, args):
    with open(os.environ['TEST_LOG'], 'a') as log:
        log.write(json.dumps([tool, args]) + '\\n')
''' + source)
        path.chmod(0o700)

    def run_build(self, *args, env=None):
        self.log.unlink(missing_ok=True)
        result = subprocess.run([self.bash, str(self.scripts / 'build-rom.sh'), *args],
                                env=env or self.env, capture_output=True, text=True)
        for path, content in self.originals.items():
            self.assertEqual(path.read_bytes(), content)
        self.calls = [json.loads(line) for line in self.log.read_text().splitlines()] if self.log.exists() else []
        return result

    def calls_for(self, tool):
        return [args for name, args in self.calls if name == tool]

    def assert_docker(self, mode, revision):
        build, run = self.calls_for('docker')
        self.assertEqual(build, ['build', '--platform', 'linux/amd64', '-f',
                                str(self.root / 'tools/rom-builder/Dockerfile'), '-t',
                                'platinumwt-rom-builder', str(self.root / 'tools/rom-builder')])
        self.assertEqual(run[:3], ['run', '--platform', 'linux/amd64'])
        self.assertIn('ROM_REVISION=' + revision, run)
        self.assertIn('PLATINUMWT_BUILD_MODE=' + mode, run)
        self.assertEqual(run[run.index('--user') + 1], f'{os.getuid()}:{os.getgid()}')
        mounts = [run[i + 1] for i, arg in enumerate(run) if arg == '-v']
        self.assertEqual(mounts, [str(self.workspace) + ':/workspace'] +
                         ([str(self.tls) + ':/tls:ro'] if mode == 'modified' else []))
        self.assertEqual(run[run.index('-w') + 1], '/workspace')
        self.assertEqual(run[-4:-1], ['platinumwt-rom-builder', 'bash', '-c'])

    def test_modified_fresh_and_incremental_builds_both_revisions(self):
        for revision in ('0', '1'):
            shutil.rmtree(self.workspace / 'build', ignore_errors=True)
            self.env['ROM_REVISION'] = revision
            for fresh in (True, False):
                with self.subTest(revision=revision, fresh=fresh):
                    result = self.run_build()
                    self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                    self.assertTrue((self.workspace / 'built').is_file())
                    self.assertEqual(self.calls_for('prepare'), [[]])
                    self.assertEqual(self.calls_for('tls'), [[]])
                    self.assertEqual(self.calls_for('profile'), [[str(self.profile),
                                     str(self.workspace / '.git/platinumwt-connection.h')]])
                    self.assertEqual(len(self.calls_for('python')), 4)
                    meson = self.calls_for('meson')
                    self.assertEqual([call[0] for call in meson], ['setup', 'configure'] if fresh else ['configure'])
                    for call in meson:
                        self.assertIn('-Drevision=' + revision, call)
                        self.assertIn('-Dwonder_trade_tls_dir=/tls', call)
                        self.assertIn('-Dwonder_trade_profile_header=/workspace/.git/platinumwt-connection.h', call)
                    if fresh:
                        self.assertIn('--native-file=meson/native.ini', meson[0])
                        self.assertIn('--cross-file=meson/cross_unix.ini', meson[0])
                    self.assertEqual(self.calls_for('make'),
                                     ([['meson', 'skrew', 'build/lsfgen.stamp']] if fresh else []) + [['release']])
                    self.assert_docker('modified', revision)

    def test_pristine_fresh_and_incremental_builds_without_profile(self):
        del self.env['WONDER_TRADE_PROFILE']
        for revision in ('0', '1'):
            shutil.rmtree(self.workspace / 'build', ignore_errors=True)
            self.env['ROM_REVISION'] = revision
            for fresh in (True, False):
                with self.subTest(revision=revision, fresh=fresh):
                    result = self.run_build('--pristine')
                    self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                    self.assertTrue((self.workspace / 'built').is_file())
                    self.assertEqual(self.calls_for('prepare'), [['--pristine']])
                    self.assertFalse(self.calls_for('tls') or self.calls_for('profile'))
                    self.assertEqual(len(self.calls_for('python')), 2)
                    self.assertEqual(self.calls_for('meson'), [['configure', 'build', '-Drevision=' + revision]])
                    self.assertEqual(self.calls_for('make'), [['configure'], ['release', 'check']])
                    self.assert_docker('pristine', revision)

    def test_old_or_missing_python_fails_before_preparation(self):
        for env in (dict(self.env, TEST_OLD_PYTHON='1'),
                    dict(self.env, PYTHON=str(self.root / 'missing python'))):
            with self.subTest(python=env['PYTHON']):
                result = self.run_build(env=env)
                self.assertEqual(result.returncode, 2, result.stdout + result.stderr)
                self.assertIn('Python 3.12', result.stderr)
                self.assertIn('PYTHON=', result.stderr)
                self.assertFalse(self.calls_for('prepare') or self.calls_for('docker'))
                self.assertFalse((self.workspace / '.git/platinumwt-connection.h').exists())

    def test_preparation_failure_preserves_inputs_and_stops_build(self):
        result = self.run_build(env=dict(self.env, TEST_PREPARE_FAIL='1'))
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('Preserve local workspace edits', result.stderr)
        self.assertFalse(self.calls_for('tls') or self.calls_for('profile') or self.calls_for('docker'))

    def test_invalid_arguments_revision_or_missing_profile_stop_before_preparation(self):
        cases = [(('--unknown',), self.env), (('--pristine', 'extra'), self.env),
                 ((), dict(self.env, ROM_REVISION='2')), ((), dict(self.env, WONDER_TRADE_PROFILE=''))]
        for args, env in cases:
            with self.subTest(args=args, revision=env['ROM_REVISION'], profile=env['WONDER_TRADE_PROFILE']):
                result = self.run_build(*args, env=env)
                self.assertEqual(result.returncode, 2, result.stdout + result.stderr)
                self.assertFalse(self.calls_for('prepare') or self.calls_for('docker'))


if __name__ == '__main__':
    unittest.main()
