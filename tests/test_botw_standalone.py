"""The Breath of the Wild app that runs on a Mac with no repo, no Python and no Xcode (docs/mods.md): the launcher's keyboard
style choice (runtime/gamepad_layout.h), what app/launcher.m reads only under GAME_BOTW (so Tears of the Kingdom's build is the
same), and build_app.py --mac-gpu (a pipeline archive for another Mac's GPU, left out whole when metal-tt made none)."""
import json
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
TOTK = ROOT / 'games/totk'
sys.path.insert(0, str(TOTK / 'ios/tools'))
import build_app          # noqa: E402
import device_pipelines   # noqa: E402

CHOICE = r'''
#include <stdio.h>
#include "gamepad_layout.h"
int main(void) {
    int auto_ = GAMEPAD_TYPE_AUTO, xbox = GAMEPAD_TYPE_XBOX;
    printf("%d%d%d%d ", gamepad_icon_keyboard_picked("keyboard", ""), gamepad_icon_keyboard_picked(NULL, " keyboard\n"),
           gamepad_icon_keyboard_picked("xbox", "auto"), gamepad_icon_keyboard_picked(NULL, NULL));   /* env, file, neither, nothing */
    printf("%d%d%d%d ", gamepad_icon_keyboard_auto(auto_, 0, -1),   /* auto, no pad now, none seen: keyboard */
           gamepad_icon_keyboard_auto(auto_, 1, -1), gamepad_icon_keyboard_auto(auto_, 0, GAMEPAD_TYPE_PLAYSTATION),   /* a pad now / one remembered: its own */
           gamepad_icon_keyboard_auto(xbox, 0, -1));   /* a picked controller type stays */
    printf("%s %d %d\n", gamepad_type_name(GAMEPAD_ICON_KEYBOARD), gamepad_type_parse("keyboard"), GAMEPAD_TYPE_COUNT);
    return 0;
}
'''
TOTK_VIEW = r'''
#include <stdio.h>
#include "gamepad_layout.h"
int main(void) { printf("%s\n", gamepad_type_name(100)); return 0; }   /* no GAME_BOTW: the keyboard is not a style */
'''


def stripped(text, botw):
    """text with the GAME_BOTW parts of its #ifdef / #ifndef GAME_BOTW (#else) lines kept (botw) or dropped, as unifdef does."""
    out, stack = [], []   # (is a GAME_BOTW condition, whether its current branch is kept)
    for line in text.splitlines():
        word = line.strip()
        if word.startswith(('#ifdef GAME_BOTW', '#ifndef GAME_BOTW')) and 'GAME_BOTW1' not in word:
            stack.append((True, botw == word.startswith('#ifdef')))
        elif word.startswith(('#if', '#ifdef', '#ifndef')):
            stack.append((False, True))
        elif word.startswith('#else') and stack and stack[-1][0]:
            stack[-1] = (True, not stack[-1][1])
        elif word.startswith('#endif') and stack:
            stack.pop()
        elif all(kept for _, kept in stack):
            out.append(line)
    return '\n'.join(out)


class KeyboardStyleChoice(unittest.TestCase):
    def compile_run(self, source, *flags):
        cc = shutil.which('cc')
        if not cc:
            self.skipTest('needs cc')
        with tempfile.TemporaryDirectory() as tmp:
            Path(tmp, 'p.c').write_text(source)
            build = subprocess.run([cc, '-std=c11', '-Wall', '-Wextra', '-Werror', *flags, f'-I{TOTK}/runtime', f'{tmp}/p.c', '-o', f'{tmp}/p'],
                                   capture_output=True, text=True)
            self.assertEqual(build.returncode, 0, build.stderr)
            return subprocess.run([f'{tmp}/p'], capture_output=True, text=True).stdout.strip()

    def test_the_three_ways_to_the_keyboard_and_the_controllers_that_keep_their_icons(self):
        self.assertEqual(self.compile_run(CHOICE, '-DGAME_BOTW=1'), '1100 1000 keyboard -1 4')

    def test_tears_of_the_kingdom_has_no_keyboard_style(self):
        self.assertEqual(self.compile_run(TOTK_VIEW), 'auto')

    def test_launcher_decides_in_the_order_the_plan_says(self):
        text = (TOTK / 'app/launcher.m').read_text()
        picked = text.index('gamepad_icon_keyboard_picked(getenv("TOTK_BUTTON_ICONS_STYLE")')
        self.assertLess(picked, text.index('[NSRunLoop.currentRunLoop runUntilDate', picked))   # the env choice comes before any controller check
        self.assertLess(text.index('controller_connected()', picked), text.index('gamepad_icon_keyboard_auto(type, pad_now, detected)'))
        self.assertIn('--bindings', text)
        self.assertIn('[keyed appendData:[NSData dataWithContentsOfFile:bindings]', text)   # other keys, another set
        self.assertIn('mouse ? @"\\nmouse=on\\n" : @"\\nmouse=off\\n"', text)                      # mouse mode on or off, another set
        self.assertIn('mouse ? @"--mouse" : @"--no-mouse"', text)


class LauncherBotwOnly(unittest.TestCase):
    text = (TOTK / 'app/launcher.m').read_text()

    def test_what_the_standalone_app_adds_is_in_the_botw_view_only(self):
        botw, other = stripped(self.text, True), stripped(self.text, False)
        for needle in ('kit_path(', 'pythonpath', 'TOTK_BUTTON_ICONS_STYLE', '--bindings', 'code mods:', '-code.pchtxt', 'TOTK_PCHTXT',
                       'Contents/Resources/ui-pack', 'MNG_UI_PACK_DIR', 'hasPrefix:@"MNG_"'):
            with self.subTest(needle=needle):
                self.assertIn(needle, botw)
                self.assertNotIn(needle, other)

    def test_python_and_ui_pack_and_code_mods_follow_the_environment(self):
        botw = stripped(self.text, True)
        self.assertIn('value.isAbsolutePath ? value : [NSBundle.mainBundle.bundlePath stringByAppendingPathComponent:value]', botw)
        self.assertIn('environment[@"PYTHONPATH"] = kit_path(site)', botw)
        self.assertIn('!getenv("TOTK_PCHTXT") && [NSFileManager.defaultManager fileExistsAtPath:code]', botw)
        self.assertIn('!getenv("MNG_UI_PACK_DIR") && [NSFileManager.defaultManager fileExistsAtPath:pack]', botw)

    def test_settings_txt_takes_mng_names_too(self):
        botw = stripped(self.text, True)
        self.assertIn('!([name hasPrefix:@"TOTK_"] || [name hasPrefix:@"MNG_"] || [name hasPrefix:@"BOTW_"])', botw)
        self.assertIn('expected TOTK_NAME=value, MNG_NAME=value or BOTW_NAME=value', botw)


class MacGpuPipelines(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        root = Path(self.tmp.name)
        self.made, self.work, self.files, self.res = root / 'made', root / 'work', root / 'work/files', root / 'res'
        for folder in (self.made, self.files, self.res / 'build/shader-libs', self.res / 'build/shader-seed', root / 'logs'):
            folder.mkdir(parents=True, exist_ok=True)
        (self.res / 'build/shader-libs/a.metallib').write_bytes(b'lib')
        (self.made / 'aa.metalarchive').write_bytes(b'a'); (self.made / 'aa.keys').write_text('k1\nk2\n')
        self.calls = []
        self.logs = root / 'logs'

    def tearDown(self):
        self.tmp.cleanup()

    def fake_run(self, pipelines, failure=None):
        def run(cmd, log, **kw):
            self.calls.append([str(c) for c in cmd])
            arch = cmd[cmd.index('--arch') + 1]
            if failure:
                raise failure
            if pipelines:   # what device_pipelines.py --out writes, and its report
                (self.files / f'pipelines-{arch}-g0-x.metalarchive').write_bytes(b'gpu')
            (self.files / f'pipelines-{arch}.json').write_text('{}'); (self.files / f'pipelines-{arch}.tsv').write_text('')
            Path(log).write_text(json.dumps({'archs': [{'arch': arch, 'pipelines': pipelines, 'files': 1, 'failed': 0}], 'no script': 0,
                                             'library from source': 2}) + '\n')
        return run

    def run_gpu(self, run, gpus=('applegpu_g14g',)):
        with mock.patch.object(build_app, 'run', run), mock.patch.object(build_app, 'say'):
            return build_app.mac_gpu_pipelines(ROOT / 'games/totk', self.res, self.made, self.work, self.files, 'applegpu_g13s', gpus, self.logs)

    def test_arguments_reach_the_tool_and_the_signed_folders_are_only_read(self):
        self.assertEqual(self.run_gpu(self.fake_run(7)), {'applegpu_g14g': 7})
        cmd = self.calls[0]
        self.assertEqual(cmd[cmd.index('--sdk') + 1], 'macosx')
        self.assertEqual(Path(cmd[cmd.index('--out') + 1]), self.files)
        self.assertEqual(Path(cmd[cmd.index('--seed') + 1]), self.res / 'build/shader-seed')
        self.assertEqual(json.loads((self.work / 'lib-map.json').read_text()).popitem()[1], str((self.res / 'build/shader-libs/a.metallib').resolve()))
        self.assertEqual(sorted(p.name for p in (self.work / 'target-src').iterdir()), ['aa.keys', 'aa.metalarchive'])
        self.assertTrue((self.files / 'pipelines-applegpu_g14g-g0-x.metalarchive').is_file())

    def test_a_gpu_with_no_compiled_pipeline_is_left_out_whole(self):
        (self.files / 'pipelines-applegpu_g13s.json').write_text('{}')   # the host's files stay
        self.assertEqual(self.run_gpu(self.fake_run(0), ('applegpu_g14g', 'applegpu_g13s')), {})
        self.assertEqual(sorted(p.name for p in self.files.iterdir()), ['pipelines-applegpu_g13s.json'])   # no stamp, no file; the host's own GPU is no target
        self.assertEqual(len(self.calls), 1)

    def test_nothing_here_stops_the_build(self):
        self.assertEqual(self.run_gpu(self.fake_run(1, build_app.BuildError('metal-tt: killed'))), {})
        self.assertEqual(list(self.files.iterdir()), [])
        self.assertEqual(build_app.parse_args(['g.nsp', '-k', 'k', '--mac-gpu', 'applegpu_g14g', '--mac-gpu', 'applegpu_g15g', '--bundle-python']).mac_gpu,
                         ['applegpu_g14g', 'applegpu_g15g'])
        self.assertIsNone(build_app.parse_args(['g.nsp', '-k', 'k']).mac_gpu)


class DevicePipelinesOutFolder(unittest.TestCase):
    def test_out_and_lib_map_leave_the_seed_alone_and_keep_other_gpus(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            seed, mac, out, libs = root / 'seed', root / 'mac', root / 'out', root / 'libs'
            for folder in (seed, mac, out, libs):
                folder.mkdir()
            (seed / 'seed-manifest.json').write_text(json.dumps({'chain': 'chain-x', 'files': {}}))
            (seed / 'pipelines-applegpu_old-k0-1.metalarchive').write_bytes(b'in the seed')
            (out / 'pipelines-applegpu_g13s.json').write_text('host'); (out / 'pipelines-applegpu_g14g-old.metalarchive').write_bytes(b'last build')
            (libs / 'a.metallib').write_bytes(b'a')
            (root / 'map.json').write_text(json.dumps({'aa' * 32: str(libs / 'a.metallib')}))
            (mac / 'k1-1.metalarchive').write_bytes(b'k'); (mac / 'k1-1.keys').write_text('k1\n')
            script = {'libraries': {'paths': [{'label': 'AA' * 32, 'path': ''}]}, 'pipelines': {'render_pipelines': [{'vertex_function': 'x'}]}}
            paths = []
            def fake_run(cmd, **kw):
                if 'metal-tt' in cmd:
                    paths.append(json.loads(Path(cmd[cmd.index('-arch') + 2]).read_text())['libraries']['paths'][0]['path'])
                    Path(cmd[cmd.index('-o') + 1]).write_bytes(b'gpu code')
                return subprocess.CompletedProcess(cmd, 0, '', '')
            argv = ['device_pipelines.py', '--sdk', 'macosx', '--arch', 'applegpu_g14g', '--mac', str(mac), '--seed', str(seed), '--lib-map', str(root / 'map.json'),
                    '--out', str(out), '--cache', str(root / 'cache'), '--groups', '0']
            with mock.patch.object(sys, 'argv', argv), mock.patch.object(device_pipelines, 'tool', lambda *a: str(root / 'tt')), \
                 mock.patch.object(device_pipelines, 'script_of', lambda *a: json.loads(json.dumps(script))), \
                 mock.patch.object(device_pipelines.subprocess, 'run', fake_run), mock.patch('builtins.print'):
                (root / 'tt').write_bytes(b'tt')
                device_pipelines.main()
            self.assertEqual(paths, [str(libs / 'a.metallib')])   # the Mac's library, not a seed path
            self.assertEqual(sorted(p.name for p in seed.iterdir()), ['pipelines-applegpu_old-k0-1.metalarchive', 'seed-manifest.json'])
            self.assertEqual(sorted(p.name for p in out.iterdir() if 'old' in p.name), [])   # this GPU's last files go
            self.assertEqual((out / 'pipelines-applegpu_g13s.json').read_text(), 'host')
            self.assertEqual((out / 'pipelines-applegpu_g14g.tsv').read_text(), 'k1\tk1-1\n')
            self.assertEqual(json.loads((out / 'pipelines-applegpu_g14g.json').read_text())['chain'], 'chain-x')


if __name__ == '__main__':
    unittest.main()
