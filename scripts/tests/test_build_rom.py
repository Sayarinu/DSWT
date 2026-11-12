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
        self.env.pop('ROM_BUILD_JOBS', None)
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
args, build = sys.argv[1:], Path('build')
if args[0].startswith('GAME_VERSION='):
    # pokeheartgold keeps one build directory per version.
    build = build / (args.pop(0).removeprefix('GAME_VERSION=').lower() + '.us')
if args == ['configure'] or args == ['setup_rom']:
    build.mkdir(parents=True, exist_ok=True)
    (build / 'build.ninja').touch()
    if args == ['configure'] and not Path('build/revision').exists():
        Path('build/revision').write_text('1')
elif args[0] == 'release':
    if os.environ.get('TEST_RELEASE_FAIL'):
        raise SystemExit(9)
    assert (build / 'build.ninja').is_file()
    command = args[1].removeprefix('NINJA=')
    # GNU Make exports command-line variables to Ninja and its children.
    os.environ['NINJA'] = command
    raise SystemExit(subprocess.run(shlex.split(command)).returncode)
elif args[0] == 'check':
    assert args[1:] == ['NINJA=ninja']
    assert Path('built').is_file()
else:
    assert args == ['meson', 'skrew', f'{build}/lsfgen.stamp']
''')
        self.tool(binary / 'ninja', '''
record('ninja', sys.argv[1:])
assert sys.argv[1:] == ['-j' + os.environ['ROM_BUILD_JOBS']]
if Path('build/regenerate').exists():
    # Meson interprets NINJA as one executable name during regeneration.
    assert shutil.which(os.environ.get('NINJA', 'ninja')), 'Could not detect Ninja'
    record('regenerate', [Path('build/revision').read_text()])
    Path('build/regenerate').unlink()
Path('built').touch()
''')
        meson = self.workspace / 'subprojects/meson-1.12.0/meson.py'
        meson.parent.mkdir(parents=True)
        self.tool(meson, '''
record('meson', sys.argv[1:])
build = Path(sys.argv[2])
if sys.argv[1] == 'setup':
    build.mkdir(parents=True)
    (build / 'build.ninja').touch()
else:
    assert sys.argv[1] == 'configure' and (build / 'build.ninja').is_file()
if build != Path('build'):
    raise SystemExit(0)
revision = next(arg.split('=', 1)[1] for arg in sys.argv if arg.startswith('-Drevision='))
previous = Path('build/revision')
if previous.exists() and previous.read_text() != revision:
    Path('build/regenerate').touch()
previous.write_text(revision)
''')

    def tool(self, path, source):
        path.write_text('#!' + sys.executable + '''
import json, os, shlex, shutil, subprocess, sys
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

    def assert_docker(self, mode, revision, jobs='1'):
        build, run = self.calls_for('docker')
        self.assertEqual(build, ['build', '--platform', 'linux/amd64', '-f',
                                str(self.root / 'tools/rom-builder/Dockerfile'), '-t',
                                'dswt-rom-builder', str(self.root / 'tools/rom-builder')])
        self.assertEqual(run[:3], ['run', '--platform', 'linux/amd64'])
        self.assertIn('ROM_REVISION=' + revision, run)
        self.assertIn('ROM_BUILD_JOBS=' + jobs, run)
        self.assertIn('DSWT_BUILD_MODE=' + mode, run)
        self.assertEqual(run[run.index('--user') + 1], f'{os.getuid()}:{os.getgid()}')
        mounts = [run[i + 1] for i, arg in enumerate(run) if arg == '-v']
        self.assertEqual(mounts, [str(self.workspace) + ':/workspace'] +
                         ([str(self.tls) + ':/tls:ro'] if mode == 'modified' else []))
        self.assertEqual(run[run.index('-w') + 1], '/workspace')
        self.assertEqual(run[-4:-1], ['dswt-rom-builder', 'bash', '-c'])

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
                                     str(self.workspace / '.git/dswt-connection.h')]])
                    self.assertEqual(len(self.calls_for('python')), 4)
                    meson = self.calls_for('meson')
                    self.assertEqual([call[0] for call in meson], ['setup', 'configure'] if fresh else ['configure'])
                    for call in meson:
                        self.assertIn('-Drevision=' + revision, call)
                        self.assertIn('-Dwonder_trade_tls_dir=/tls', call)
                        self.assertIn('-Dwonder_trade_profile_header=/workspace/.git/dswt-connection.h', call)
                    if fresh:
                        self.assertIn('--native-file=meson/native.ini', meson[0])
                        self.assertIn('--cross-file=meson/cross_unix.ini', meson[0])
                    self.assertEqual(self.calls_for('make'),
                                     ([['meson', 'skrew', 'build/lsfgen.stamp']] if fresh else [])
                                     + [['release', 'NINJA=env -u NINJA ninja -j1']])
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
                    self.assertEqual(self.calls_for('make'),
                                     [['configure'], ['release', 'NINJA=env -u NINJA ninja -j1'], ['check', 'NINJA=ninja']])
                    self.assert_docker('pristine', revision)

    def test_heartgold_and_soulsilver_build_from_one_hgss_workspace(self):
        for game in ('heartgold', 'soulsilver'):
            version = game.upper()
            build = f'build/{game}.us'
            env = dict(self.env, ROM_GAME=game)
            del env['ROM_REVISION']
            with self.subTest(game=game, mode='pristine'):
                result = self.run_build('--pristine', env=env)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertEqual(self.calls_for('prepare'), [['--game', 'hgss', '--pristine']])
                self.assertFalse(self.calls_for('meson'))
                self.assertEqual(self.calls_for('make'), [
                    [f'GAME_VERSION={version}', 'setup_rom'],
                    [f'GAME_VERSION={version}', 'release', 'NINJA=env -u NINJA ninja -j1'],
                    [f'GAME_VERSION={version}', 'check', 'NINJA=ninja']])
                self.assert_docker('pristine', '0')
                self.assertIn('ROM_GAME=' + game, self.calls_for('docker')[1])
            # The fixture shares one directory; real pristine builds use their own workspace.
            shutil.rmtree(self.workspace / 'build')
            for fresh in (True, False):
                with self.subTest(game=game, mode='modified', fresh=fresh):
                    result = self.run_build(env=env)
                    self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                    self.assertEqual(self.calls_for('prepare'), [['--game', 'hgss']])
                    meson = self.calls_for('meson')
                    self.assertEqual([call[:2] for call in meson],
                                     ([['setup', build]] if fresh else []) + [['configure', build]])
                    for call in meson:
                        self.assertIn('-Dgame_version=' + version, call)
                        self.assertIn('-Dwonder_trade_tls_dir=/tls', call)
                        self.assertIn('-Dwonder_trade_profile_header=/workspace/.git/dswt-connection.h', call)
                    self.assertEqual(self.calls_for('make'),
                                     ([[f'GAME_VERSION={version}', 'meson', 'skrew', f'{build}/lsfgen.stamp']] if fresh else [])
                                     + [[f'GAME_VERSION={version}', 'release', 'NINJA=env -u NINJA ninja -j1']])
                    self.assert_docker('modified', '0')

    def test_unknown_game_or_hgss_revision_stops_before_preparation(self):
        for game, revision in (('diamond', '0'), ('HeartGold', '0'), ('heartgold', '1'), ('soulsilver', '01')):
            with self.subTest(game=game, revision=revision):
                result = self.run_build(env=dict(self.env, ROM_GAME=game, ROM_REVISION=revision))
                self.assertEqual(result.returncode, 2, result.stdout + result.stderr)
                self.assertIn('ROM_', result.stderr)
                self.assertEqual(self.calls, [])

    def test_positive_job_overrides_apply_to_both_build_modes(self):
        for mode in ('modified', 'pristine'):
            for jobs in ('1', '2', '8'):
                with self.subTest(mode=mode, jobs=jobs):
                    args = ('--pristine',) if mode == 'pristine' else ()
                    result = self.run_build(*args, env=dict(self.env, ROM_BUILD_JOBS=jobs))
                    self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                    self.assertIn(['release', 'NINJA=env -u NINJA ninja -j' + jobs], self.calls_for('make'))
                    if mode == 'pristine':
                        self.assertEqual(self.calls_for('make')[-1], ['check', 'NINJA=ninja'])
                    self.assert_docker(mode, '1', jobs)

    def test_revision_switches_regenerate_with_discoverable_ninja_in_both_modes(self):
        for mode in ('modified', 'pristine'):
            shutil.rmtree(self.workspace / 'build', ignore_errors=True)
            for index, revision in enumerate(('1', '0', '1')):
                with self.subTest(mode=mode, revision=revision, index=index):
                    args = ('--pristine',) if mode == 'pristine' else ()
                    result = self.run_build(*args, env=dict(self.env, ROM_REVISION=revision, ROM_BUILD_JOBS='2'))
                    self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                    self.assertEqual(self.calls_for('ninja'), [['-j2']])
                    self.assertEqual(self.calls_for('regenerate'), [[revision]] if index else [])
                    if mode == 'pristine':
                        self.assertEqual(self.calls_for('make')[-1], ['check', 'NINJA=ninja'])

    def test_invalid_job_override_fails_before_any_preparation(self):
        for mode in ('modified', 'pristine'):
            for jobs in ('', '0', '-1', '+2', '01', '1.5', ' 2', '2 ', '1\n2', '2;touch unexpected', '$(touch unexpected)'):
                with self.subTest(mode=mode, jobs=jobs):
                    args = ('--pristine',) if mode == 'pristine' else ()
                    result = self.run_build(*args, env=dict(self.env, ROM_BUILD_JOBS=jobs))
                    self.assertEqual(result.returncode, 2, result.stdout + result.stderr)
                    self.assertIn('ROM_BUILD_JOBS', result.stderr)
                    self.assertEqual(self.calls, [])
                    self.assertFalse((self.workspace / '.git/dswt-connection.h').exists())

    def test_release_failure_stops_before_checks_without_retry(self):
        for mode in ('modified', 'pristine'):
            with self.subTest(mode=mode):
                args = ('--pristine',) if mode == 'pristine' else ()
                result = self.run_build(*args, env=dict(self.env, TEST_RELEASE_FAIL='1'))
                self.assertEqual(result.returncode, 9, result.stdout + result.stderr)
                calls = self.calls_for('make')
                self.assertEqual(calls.count(['release', 'NINJA=env -u NINJA ninja -j1']), 1)
                self.assertFalse(any('check' in call for call in calls))

    def test_old_or_missing_python_fails_before_preparation(self):
        for env in (dict(self.env, TEST_OLD_PYTHON='1'),
                    dict(self.env, PYTHON=str(self.root / 'missing python'))):
            with self.subTest(python=env['PYTHON']):
                result = self.run_build(env=env)
                self.assertEqual(result.returncode, 2, result.stdout + result.stderr)
                self.assertIn('Python 3.12', result.stderr)
                self.assertIn('PYTHON=', result.stderr)
                self.assertFalse(self.calls_for('prepare') or self.calls_for('docker'))
                self.assertFalse((self.workspace / '.git/dswt-connection.h').exists())

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
