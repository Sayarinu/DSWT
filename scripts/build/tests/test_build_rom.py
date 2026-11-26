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
    def setUp(self, native=False):
        temporary = tempfile.TemporaryDirectory(prefix='rom-native-' if native else 'rom build tests ')
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.scripts = self.root / 'scripts'
        self.scripts.mkdir()
        for group in ('build', 'prepare', 'port'):
            (self.scripts / group).mkdir()
        shutil.copy2(Path(__file__).resolve().parents[1] / 'build-rom.sh', self.scripts / 'build')
        native_script = Path(__file__).resolve().parents[1] / 'build-rom-macos.sh'
        if native_script.exists():
            shutil.copy2(native_script, self.scripts / 'build')
        self.workspace = self.root / ('workspace' if native else 'game workspace')
        (self.workspace / '.git').mkdir(parents=True)
        self.tls = self.root / ('tls' if native else 'TLS source')
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
        self.env.pop('ROM_BUILD_HOST', None)
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
        self.tool(self.scripts / 'prepare/prepare-game.py', '''
record('prepare', sys.argv[1:])
if os.environ.get('TEST_PREPARE_FAIL'):
    raise SystemExit('Preserve local workspace edits')
print(os.environ['TEST_WORKSPACE'])
''')
        self.tool(self.scripts / 'prepare/prepare-europe.py', '''
record('europe', sys.argv[1:])
assert Path(sys.argv[1]).is_file() and (Path(sys.argv[2]) / '.git').is_dir()
print(Path(sys.argv[2]) / '.git/dswt-europe')
''')
        self.tool(self.scripts / 'prepare/prepare-tls.py', '''
record('tls', sys.argv[1:])
print(os.environ['TEST_TLS'])
''')
        self.tool(self.scripts / 'build/configure-connection.py', '''
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
    for i, arg in enumerate(sys.argv):
        if arg == '-v' and sys.argv[i + 1].endswith(':/dswt:ro'):
            # The repository mount: its scripts run from the host copy.
            host = sys.argv[i + 1].removesuffix(':/dswt:ro')
            command = re.sub(r'(?<![\w/])/dswt/(\S+)', lambda m: shlex.quote(host + '/' + m[1]), command)
    raise SystemExit(subprocess.run([os.environ['TEST_BASH'], '-c', command],
                                   cwd=os.environ['TEST_WORKSPACE']).returncode)
assert sys.argv[1] == 'build'
''')
        for wine in ('wineserver', 'wineboot'):
            self.tool(binary / wine, f'''
record({wine!r}, sys.argv[1:])
assert Path(os.environ['WINEPREFIX']).is_dir()
''')
        self.tool(binary / 'make', '''
record('make', sys.argv[1:])
args, build = sys.argv[1:], Path('build')
if args[0].startswith('-j'):
    # pokediamond's Make build.
    raise SystemExit(0)
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
assert sys.argv[1] == '-j' + os.environ['ROM_BUILD_JOBS']
if Path('build/regenerate').exists():
    # Meson interprets NINJA as one executable name during regeneration.
    assert shutil.which(os.environ.get('NINJA', 'ninja')), 'Could not detect Ninja'
    record('regenerate', [Path('build/revision').read_text()])
    Path('build/regenerate').unlink()
Path('built').touch()
''')
        self.tool(self.workspace / 'configure.py', '''
record('configure', sys.argv[1:])
Path('build.ninja').touch()
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
import json, os, re, shlex, shutil, subprocess, sys
from pathlib import Path
def record(tool, args):
    with open(os.environ['TEST_LOG'], 'a') as log:
        log.write(json.dumps([tool, args]) + '\\n')
''' + source)
        path.chmod(0o700)

    def run_build(self, *args, env=None):
        self.log.unlink(missing_ok=True)
        result = subprocess.run([self.bash, str(self.scripts / 'build/build-rom.sh'), *args],
                                env=env or self.env, capture_output=True, text=True)
        for path, content in self.originals.items():
            self.assertEqual(path.read_bytes(), content)
        self.calls = [json.loads(line) for line in self.log.read_text().splitlines()] if self.log.exists() else []
        return result

    def calls_for(self, tool):
        return [args for name, args in self.calls if name == tool]

    def native_environment(self, game='diamond'):
        self.setUp(native=True)
        binary = self.root / 'fake tools'
        self.tool(binary / 'uname', "print(os.environ.get('TEST_OS', 'Darwin'))\n")
        self.tool(binary / 'arch', "raise SystemExit(int(os.environ.get('TEST_ARCH_FAIL', '0')))\n")
        self.tool(binary / 'brew', "print(os.environ['TEST_BREW'])\n")
        self.env.update(TEST_BREW=str(self.root / 'brew'), ROM_BUILD_HOST='macos',
                        ROM_GAME=game, ROM_BUILD_JOBS='4')
        (self.root / 'game').mkdir()
        self.env.pop('ROM_REVISION')
        self.tool(binary / 'tar', "print(os.environ.get('TEST_TAR', 'tar (GNU tar) 1.35'))\n")
        self.tool(binary / 'make', """
if sys.argv[1:] == ['--version']:
    print(os.environ.get('TEST_MAKE_VERSION', 'GNU Make 4.4.1'))
else:
    record('make', sys.argv[1:])
    raise SystemExit(int(os.environ.get('TEST_MAKE_FAIL', '0')))
""")
        for name in ('gsed', 'sha1sum', 'gcc', 'g++', 'pkg-config', 'arm-none-eabi-objcopy',
                     'arm-none-eabi-ar', 'arm-none-eabi-objdump', 'clang', 'llvm-objcopy',
                     'llvm-ar', 'cargo', 'wine', 'patch'):
            self.tool(binary / name, 'pass\n')
        self.tool(binary / 'cargo', "raise SystemExit(int(os.environ.get('TEST_CARGO_FAIL', '0')))\n")
        assets = self.root / 'assets'
        (assets / 'mwccarm').mkdir(parents=True)
        (assets / 'mwccarm/license.dat').touch()
        (assets / 'sdk-bin').mkdir()
        (assets / 'ARM7-TS.lcf.template').touch()
        (assets / 'ARM9-TS.lcf.template').touch()
        for cpu in ('arm7', 'arm9'):
            (self.workspace / cpu).mkdir()
        self.tool(self.scripts / 'prepare/prepare-rom-macos.py',
                  f"record('mac-tools', sys.argv[1:])\n"
                  f"if os.environ.get('TEST_ASSETS_FAIL'): raise SystemExit(7)\nprint({str(assets)!r})\n")
        return assets

    def test_native_dp_builds_both_versions_and_modes_without_docker_shim(self):
        for game in ('diamond', 'pearl'):
            for mode in ('modified', 'pristine'):
                with self.subTest(game=game, mode=mode):
                    assets = self.native_environment(game)
                    args = ('--pristine',) if mode == 'pristine' else ()
                    result = self.run_build(*args)
                    self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                    extra = ['COMPARE=1'] if mode == 'pristine' else [
                        'COMPARE=0', f'WT_TLS_DIR={self.tls}', f'WT_MSL_DIR={assets}/msl',
                        f'WT_PROFILE_HEADER={self.workspace}/.git/dswt-connection.h']
                    self.assertEqual(self.calls_for('make'), [['tools'], [
                        '-j4', f'GAME_VERSION={game.upper()}', *extra]])
                    self.assertEqual(self.calls_for('prepare'), [
                        ['--game', 'dp', *args]])
                    self.assertEqual(self.calls_for('wineserver'), [['-p'], ['-k'], ['-w']])
                    self.assertFalse(self.calls_for('docker'))
                    self.assertTrue((self.workspace / 'tools/mwccarm/license.dat').exists())

    def test_native_bw2_builds_both_versions_and_modes(self):
        for game, version in (('black2', 'b2_us'), ('white2', 'w2_us')):
            for mode in ('modified', 'pristine'):
                with self.subTest(game=game, mode=mode):
                    assets = self.native_environment(game)
                    dump = self.root / 'own dump.nds'
                    dump.write_bytes(b'owned input')
                    self.env['DSWT_BW2_ROM'] = str(dump)
                    args = ('--pristine',) if mode == 'pristine' else ()
                    result = self.run_build(*args)
                    self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                    options = [version]
                    if mode == 'modified':
                        options += ['--dsd', f'{assets}/dsd', '--wonder-trade-tls', str(self.tls),
                                    '--wonder-trade-profile', str(self.workspace / '.git/dswt-connection.h'),
                                    '--wonder-trade-msl', f'{assets}/msl']
                    self.assertEqual(self.calls_for('configure'), [options])
                    target = [] if mode == 'pristine' else [f'build/poke{game}_us.nds']
                    self.assertEqual(self.calls_for('ninja'), [['-j4', *target]])
                    self.assertEqual((self.workspace / f'orig/baserom_{version}.nds').read_bytes(), b'owned input')
                    self.assertEqual(dump.read_bytes(), b'owned input')
                    self.assertFalse(self.calls_for('docker') or self.calls_for('make'))

    def test_native_prerequisites_fail_before_workspace_preparation(self):
        for change, message in (({'TEST_OS': 'Linux'}, 'macOS'),
                                ({'TEST_TAR': 'bsdtar 3.8'}, 'GNU tar'),
                                ({'TEST_MAKE_VERSION': 'GNU Make 3.81'}, 'GNU Make 4'),
                                ({'ROM_GAME': 'black2', 'TEST_CARGO_FAIL': '1'}, 'rustup toolchain install 1.100'),
                                ({'TEST_ARCH_FAIL': '1'}, 'Rosetta'),
                                ({'ROM_GAME': 'platinum'}, 'diamond, pearl, black2 or white2')):
            with self.subTest(change=change):
                self.native_environment()
                dump = self.root / 'dump.nds'
                dump.touch()
                self.env['DSWT_BW2_ROM'] = str(dump)
                result = self.run_build(env=dict(self.env, **change))
                self.assertEqual(result.returncode, 2, result.stdout + result.stderr)
                self.assertIn(message, result.stderr)
                self.assertFalse(self.calls_for('prepare') or self.calls_for('mac-tools'))

    def test_native_refuses_spaces_and_unknown_host_mode(self):
        for host, message in (('macos', 'whitespace'), ('native-ish', 'ROM_BUILD_HOST')):
            result = self.run_build(env=dict(self.env, ROM_GAME='diamond', ROM_REVISION='5', ROM_BUILD_HOST=host))
            self.assertEqual(result.returncode, 2, result.stdout + result.stderr)
            self.assertIn(message, result.stderr)
            self.assertFalse(self.calls_for('prepare'))

    def test_native_missing_tools_explain_how_to_install_them(self):
        for tool, instruction in (('docker', 'Docker Desktop'), ('llvm-objcopy', 'brew install'),
                                  ('wine', 'Wine Stable')):
            with self.subTest(tool=tool):
                self.native_environment()
                binary = self.root / 'fake tools'
                # Limit PATH so installed host tools cannot hide the missing prerequisite.
                for name in ('bash', 'dirname'):
                    (binary / name).symlink_to(shutil.which(name))
                (binary / tool).unlink()
                self.env['PATH'] = str(binary)
                result = self.run_build()
                self.assertEqual(result.returncode, 2, result.stdout + result.stderr)
                self.assertIn(f'Missing {tool}', result.stderr)
                self.assertIn(instruction, result.stderr)
                self.assertFalse(self.calls_for('prepare'))

    def test_native_refuses_workspace_and_tls_paths_with_spaces(self):
        for variable in ('TEST_WORKSPACE', 'TEST_TLS'):
            with self.subTest(variable=variable):
                self.native_environment()
                path = self.root / 'path with spaces'
                (path / '.git').mkdir(parents=True)
                self.env[variable] = str(path)
                result = self.run_build()
                self.assertEqual(result.returncode, 2, result.stdout + result.stderr)
                self.assertIn('workspace and TLS paths must not contain whitespace', result.stderr)
                self.assertFalse(self.calls_for('mac-tools') or self.calls_for('make'))

    def test_native_make_failure_is_propagated_and_stops_wine(self):
        self.native_environment()
        result = self.run_build(env=dict(self.env, TEST_MAKE_FAIL='9'))
        self.assertEqual(result.returncode, 9, result.stdout + result.stderr)
        self.assertEqual(self.calls_for('wineserver'), [['-p'], ['-k'], ['-w']])
        self.assertEqual(self.calls_for('make'), [['tools']])

    def test_native_tool_preparation_failure_stops_the_build(self):
        self.native_environment()
        result = self.run_build(env=dict(self.env, TEST_ASSETS_FAIL='1'))
        self.assertEqual(result.returncode, 7, result.stdout + result.stderr)
        self.assertFalse(self.calls_for('make') or self.calls_for('wineserver'))

    def assert_docker(self, mode, revision, jobs='1', image='dswt-rom-builder', builder='tools/builders/rom-builder', extra=()):
        build, run = self.calls_for('docker')
        self.assertEqual(build, ['build', '--platform', 'linux/amd64', '-f',
                                str(self.root / builder / 'Dockerfile'), '-t',
                                image, str(self.root / builder)])
        self.assertEqual(run[:3], ['run', '--platform', 'linux/amd64'])
        self.assertIn('ROM_REVISION=' + revision, run)
        self.assertIn('ROM_BUILD_JOBS=' + jobs, run)
        self.assertIn('DSWT_BUILD_MODE=' + mode, run)
        self.assertEqual(run[run.index('--user') + 1], f'{os.getuid()}:{os.getgid()}')
        mounts = [run[i + 1] for i, arg in enumerate(run) if arg == '-v']
        self.assertEqual(mounts, [str(self.workspace) + ':/workspace'] +
                         ([str(self.tls) + ':/tls:ro'] if mode == 'modified' else []) + list(extra))
        self.assertEqual(run[run.index('-w') + 1], '/workspace')
        self.assertEqual(run[-4:-1], [image, 'bash', '-c'])

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
                    self.assertFalse(self.calls_for('europe'))
                    self.assertFalse(self.calls_for('tls') or self.calls_for('profile'))
                    self.assertEqual(len(self.calls_for('python')), 2)
                    self.assertEqual(self.calls_for('meson'), [['configure', 'build', '-Drevision=' + revision]])
                    self.assertEqual(self.calls_for('make'),
                                     [['configure'], ['release', 'NINJA=env -u NINJA ninja -j1'], ['check', 'NINJA=ninja']])
                    self.assert_docker('pristine', revision)

    def test_european_builds_stage_the_verified_dump_after_preparation(self):
        dump = self.root / 'european dump.nds'
        dump.write_bytes(b'european ROM')
        for mode in ('modified', 'pristine'):
            with self.subTest(mode=mode):
                args = ('--pristine',) if mode == 'pristine' else ()
                result = self.run_build(*args, env=dict(self.env, ROM_REVISION='10', DSWT_EUROPE_ROM=str(dump)))
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertEqual(self.calls_for('europe'), [[str(dump), str(self.workspace)]])
                self.assertEqual([name for name, _ in self.calls][1:5], ['python', 'prepare', 'python', 'europe'])
                self.assert_docker(mode, '10')
        self.assertEqual(dump.read_bytes(), b'european ROM')

    def test_european_build_requires_a_local_dump(self):
        for dump in ('', str(self.root / 'missing.nds')):
            with self.subTest(dump=dump):
                result = self.run_build('--pristine', env=dict(self.env, ROM_REVISION='10', DSWT_EUROPE_ROM=dump))
                self.assertEqual(result.returncode, 2, result.stdout + result.stderr)
                self.assertIn('DSWT_EUROPE_ROM', result.stderr)
                self.assertEqual(self.calls, [])

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

    def test_diamond_and_pearl_build_from_one_dp_workspace(self):
        # The image's toolchain is already installed in this workspace.
        license = self.workspace / 'tools/mwccarm/license.dat'
        license.parent.mkdir(parents=True)
        license.touch()
        for game in ('diamond', 'pearl'):
            env = dict(self.env, ROM_GAME=game, ROM_BUILD_JOBS='4')
            del env['ROM_REVISION']
            with self.subTest(game=game):
                result = self.run_build('--pristine', env=env)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertEqual(self.calls_for('prepare'), [['--game', 'dp', '--pristine']])
                self.assertEqual(self.calls_for('wineserver'), [['-p']])
                self.assertEqual(self.calls_for('wineboot'), [['-i']])
                self.assertEqual(self.calls_for('make'), [['-j4', f'GAME_VERSION={game.upper()}', 'COMPARE=1']])
                self.assertFalse(self.calls_for('meson') or self.calls_for('ninja'))
                self.assert_docker('pristine', '5', '4', 'dswt-rom-builder-dp', 'tools/builders/rom-builder-dp')
                self.assertIn('ROM_GAME=' + game, self.calls_for('docker')[1])
            with self.subTest(game=game, mode='modified'):
                result = self.run_build(env=env)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertEqual(self.calls_for('prepare'), [['--game', 'dp']])
                self.assertEqual(self.calls_for('make'), [[
                    '-j4', f'GAME_VERSION={game.upper()}', 'COMPARE=0', 'WT_TLS_DIR=/tls',
                    'WT_MSL_DIR=/opt/pokediamond/msl', 'WT_PROFILE_HEADER=/workspace/.git/dswt-connection.h']])
                self.assertFalse(self.calls_for('meson') or self.calls_for('ninja'))
                self.assert_docker('modified', '5', '4', 'dswt-rom-builder-dp', 'tools/builders/rom-builder-dp')

    def test_black2_and_white2_build_from_the_players_dump(self):
        # The image's tools are already installed in this workspace.
        marker = self.workspace / 'tools/mwccarm/dswt-rom-builder-bw2'
        marker.parent.mkdir(parents=True)
        marker.touch()
        for game, version in (('black2', 'b2_us'), ('white2', 'w2_us')):
            dump = self.root / f'{game} dump.nds'
            dump.write_bytes(game.encode() + b' ROM')
            env = dict(self.env, ROM_GAME=game, ROM_BUILD_JOBS='4', DSWT_BW2_ROM=str(dump))
            del env['ROM_REVISION'], env['WONDER_TRADE_PROFILE']
            baserom = self.workspace / f'orig/baserom_{version}.nds'
            for fresh in (True, False):
                with self.subTest(game=game, fresh=fresh):
                    result = self.run_build('--pristine', env=env)
                    self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                    self.assertEqual(self.calls_for('prepare'), [['--game', 'bw2', '--pristine']])
                    self.assertEqual(baserom.read_bytes(), game.encode() + b' ROM')
                    self.assertEqual(self.calls_for('configure'), [['--no-download', version]])
                    self.assertEqual(self.calls_for('ninja'), [['-j4']])
                    self.assertFalse(self.calls_for('make') or self.calls_for('meson'))
                    self.assertFalse(self.calls_for('tls') or self.calls_for('profile'))
                    self.assert_docker('pristine', '0', '4', 'dswt-rom-builder-bw2', 'tools/builders/rom-builder-bw2')
                    self.assertIn('ROM_GAME=' + game, self.calls_for('docker')[1])
            self.assertEqual(dump.read_bytes(), game.encode() + b' ROM')

    def test_black2_and_white2_modified_builds_link_wonder_trade(self):
        marker = self.workspace / 'tools/mwccarm/dswt-rom-builder-bw2'
        marker.parent.mkdir(parents=True, exist_ok=True)
        marker.touch()
        for game, version, rom in (('black2', 'b2_us', 'pokeblack2_us.nds'), ('white2', 'w2_us', 'pokewhite2_us.nds')):
            dump = self.root / f'{game} dump.nds'
            dump.write_bytes(game.encode())
            env = dict(self.env, ROM_GAME=game, DSWT_BW2_ROM=str(dump))
            del env['ROM_REVISION']
            with self.subTest(game=game):
                result = self.run_build(env=env)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertEqual(self.calls_for('prepare'), [['--game', 'bw2']])
                self.assertEqual(self.calls_for('tls'), [[]])
                self.assertEqual(self.calls_for('configure'), [[
                    '--no-download', version, '--wonder-trade-tls', '/tls',
                    '--wonder-trade-profile', '/workspace/.git/dswt-connection.h',
                    '--wonder-trade-msl', '/opt/pokebw2/msl']])
                self.assertEqual(self.calls_for('ninja'), [['-j1', 'build/' + rom]])
                self.assert_docker('modified', '0', '1', 'dswt-rom-builder-bw2', 'tools/builders/rom-builder-bw2')

    def test_black_and_white_build_from_the_players_dump(self):
        (self.workspace / 'tools/mwccarm/mwccarm/dsi/1.1p1').mkdir(parents=True)
        for tool in ('ndsrom', 'delink', 'romlink'):
            self.tool(self.workspace / f'tools/{tool}.py', f"record({tool!r}, sys.argv[1:])\n")
        units = self.workspace / 'layout/units_arm9.json'
        units.parent.mkdir(parents=True, exist_ok=True)
        units.write_text('tracked\n')
        self.tool(self.workspace / 'tools/disasm.py',
                  "record('disasm', sys.argv[1:])\nopen('layout/units_arm9.json', 'w').write('recounted')\n")
        for game, extract in (('black', 'extract'), ('white', 'extract_white')):
            dump = self.root / f'{game} dump.nds'
            dump.write_bytes(game.encode())
            env = dict(self.env, ROM_GAME=game, DSWT_BW_ROM=str(dump))
            del env['ROM_REVISION'], env['WONDER_TRADE_PROFILE']
            with self.subTest(game=game):
                result = self.run_build('--pristine', env=env)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertEqual(self.calls_for('prepare'), [['--game', 'bw', '--pristine']])
                self.assertEqual((self.workspace / f'{game}.nds').read_bytes(), game.encode())
                self.assertEqual(self.calls_for('ndsrom'), [['extract', f'{game}.nds', '-o', extract]])
                self.assertEqual(self.calls_for('delink'), [['-e', extract, '-l', f'build/{game}/layout']])
                self.assertEqual(self.calls_for('romlink'), [['--target', game, '--relink']])
                self.assertEqual(self.calls_for('disasm'), [['arm9', '--split'], ['overlay', '--all']])
                self.assertEqual(units.read_text(), 'tracked\n')
                self.assertEqual(self.calls_for('wineserver'), [['-p']])
                self.assert_docker('pristine', '0', '1', 'dswt-rom-builder-bw', 'tools/builders/rom-builder-bw')
            with self.subTest(game=game, mode='modified'):
                report = self.workspace / ('build/rom/report.json' if game == 'black' else 'build/rom/report_white.json')
                report.parent.mkdir(parents=True, exist_ok=True)
                report.write_text('{"identical": true}')
                self.tool(self.scripts / 'port/port-bw.py', "record('port', sys.argv[1:])\n")
                result = self.run_build(env=dict(env, WONDER_TRADE_PROFILE=str(self.profile)))
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertEqual(self.calls_for('prepare'), [['--game', 'bw']])
                # The finished pristine link is reused; the port adds Wonder Trade to the dump.
                self.assertEqual(self.calls_for('romlink'), [])
                self.assertEqual(self.calls_for('port'), [[game, f'{game}.nds', f'build/dswt/{game}.nds',
                                                           '--workspace', '/workspace', '--profile',
                                                           '/workspace/.git/dswt-connection.h', '--tls', '/tls',
                                                           '--msl', '/opt/aletheia/msl', '--work',
                                                           f'build/dswt/port-{game}']])
                self.assert_docker('modified', '0', '1', 'dswt-rom-builder-bw', 'tools/builders/rom-builder-bw',
                                   [f'{self.root}:/dswt:ro'])
                report.unlink()

    def test_black2_and_white2_need_a_dump_and_a_profile(self):
        dump = self.root / 'dump.nds'
        dump.write_bytes(b'ROM')
        base = dict(self.env, DSWT_BW2_ROM=str(dump))
        del base['ROM_REVISION']
        cases = [((), dict(base, WONDER_TRADE_PROFILE=''), 'WONDER_TRADE_PROFILE'),
                 (('--pristine',), dict(base, ROM_REVISION='1'), 'ROM_REVISION'),
                 (('--pristine',), dict(base, DSWT_BW2_ROM=''), 'DSWT_BW2_ROM'),
                 (('--pristine',), dict(base, DSWT_BW2_ROM=str(self.root / 'missing.nds')), 'DSWT_BW2_ROM')]
        for args, env, message in cases:
            for game in ('black2', 'white2'):
                with self.subTest(game=game, message=message, rom=env['DSWT_BW2_ROM']):
                    result = self.run_build(*args, env=dict(env, ROM_GAME=game))
                    self.assertEqual(result.returncode, 2, result.stdout + result.stderr)
                    self.assertIn(message, result.stderr)
                    self.assertEqual(self.calls, [])
                    self.assertFalse((self.workspace / 'orig').exists())

    def test_unknown_game_or_hgss_revision_stops_before_preparation(self):
        for game, revision in (('diamond', '0'), ('pearl', '4'), ('Diamond', '5'), ('HeartGold', '0'),
                               ('heartgold', '1'), ('soulsilver', '01')):
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
