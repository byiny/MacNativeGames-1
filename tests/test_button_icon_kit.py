"""The button icon tools build_app.py puts into the app (button_icons_kit; app/launcher.m runs them at start, docs/mods.md):
the scripts in the repo layout, kit.txt with the tool and an id that changes with the scripts and the RomFS, and the tool
starting from the kit alone."""
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest import mock
import build_app

REPO = Path(__file__).resolve().parents[1]


def work_tree(root, index=b'index'):
    """A work folder with the repo's scripts and a stand-in RomFS index (work/totk, work/botw as prepare_work makes them)."""
    tree = root / 'work/totk'
    (tree / 'runtime').mkdir(parents=True)
    (tree / 'scripts').symlink_to(REPO / 'games/totk/scripts')
    (tree / 'runtime/controller_map.h').symlink_to(REPO / 'games/totk/runtime/controller_map.h')
    (root / 'work/botw').mkdir()
    (root / 'work/botw/scripts').symlink_to(REPO / 'games/botw/scripts')
    (tree / 'extracted/botw190/romfs').mkdir(parents=True)
    (tree / 'extracted/botw190/romfs/runtime-index.bin').write_bytes(index)
    return tree


def kit_info(res):
    return dict(line.split('=', 1) for line in (res / 'button-icons/kit.txt').read_text().splitlines())


class ButtonIconKit(unittest.TestCase):
    def test_kit_layout_and_id(self):
        with tempfile.TemporaryDirectory() as tmp:
            tmp = Path(tmp)
            tree = work_tree(tmp / 'a')
            build_app.button_icons_kit(tree, tmp / 'res', 'botw190', 'botw')
            kit = tmp / 'res/button-icons'
            for rel in ('games/botw/scripts/botw_prompts.py', 'games/totk/scripts/prompt_layout.py', 'games/totk/runtime/controller_map.h'):
                self.assertTrue((kit / rel).is_file(), rel)
            info = kit_info(tmp / 'res')
            self.assertEqual(info['tool'], 'games/botw/scripts/botw_prompts.py')
            self.assertEqual(info['python'], str(tree.resolve() / '.venv/bin/python'))
            # another RomFS, or a build overlay, gives another id: a set made for the earlier app is not reused
            other = work_tree(tmp / 'b', index=b'other')
            build_app.button_icons_kit(other, tmp / 'res2', 'botw190', 'botw')
            self.assertNotEqual(kit_info(tmp / 'res2')['id'], info['id'])
            (other / 'extracted/botw190/romfs-patches').mkdir()
            (other / 'extracted/botw190/romfs-patches/manifest.tsv').write_text('# none\n')
            (other / 'extracted/botw190/romfs/runtime-index.bin').write_bytes(b'index')
            build_app.button_icons_kit(other, tmp / 'res3', 'botw190', 'botw')
            self.assertNotEqual(kit_info(tmp / 'res3')['id'], info['id'])

    def test_totk_kit_names_its_tool(self):
        with tempfile.TemporaryDirectory() as tmp:
            tmp = Path(tmp)
            tree = work_tree(tmp)
            build_app.button_icons_kit(tree, tmp / 'res', 'botw190', 'totk')
            self.assertEqual(kit_info(tmp / 'res')['tool'], 'games/totk/scripts/make_button_icons.py')
            botw = tmp / 'res/button-icons/games/botw/scripts'      # the art of make_button_icons.py --our-art, nothing else of the game
            for name in ('botw_controller_art', 'botw_formats', 'botw_prompts'):
                self.assertTrue((botw / f'{name}.py').is_file(), name)
            self.assertFalse((botw / 'extract_exefs.py').exists())
            self.assertTrue((tmp / 'res/button-icons/games/totk/scripts/totk_controller_art.py').is_file())     # the controller pictures

    def test_bundled_python_is_named_relative_to_the_app(self):
        with tempfile.TemporaryDirectory() as tmp:
            tmp = Path(tmp)
            tree, res = work_tree(tmp), tmp / 'app/Contents/Resources/totk'
            self.assertNotIn('pythonpath', kit_info(self.make(tree, tmp / 'plain', False)))   # the default: the work folder's Python, absolute
            with mock.patch.object(build_app, 'bundle_python_runtime') as bundled:
                build_app.button_icons_kit(tree, res, 'botw190', 'botw', bundle_python=True, logs=tmp / 'logs')
            info = kit_info(res)
            self.assertEqual(info['python'], 'Contents/Resources/python/bin/python3.14')
            self.assertEqual(info['pythonpath'], 'Contents/Resources/totk/button-icons/site')
            bundled.assert_called_once_with(tree, tmp / 'app/Contents/Resources/python', res / 'button-icons/site', tmp / 'logs')
            self.assertEqual(res.parents[2] / info['python'], tmp / 'app/Contents/Resources/python/bin/python3.14')   # where the runtime goes
            command = tmp / 'app/Contents/Resources/Install Mods.command'
            self.assertTrue(command.stat().st_mode & 0o100)
            self.assertIn('mods/botw190', command.read_text())
            self.assertIn(info['python'], command.read_text())
            self.assertEqual(subprocess.run(['bash', '-n', command]).returncode, 0)

    def make(self, tree, res, bundle):
        build_app.button_icons_kit(tree, res, 'botw190', 'botw', bundle_python=bundle)
        return res

    def test_the_mod_manifest_tool_and_the_key_caps_start_from_the_kit(self):
        with tempfile.TemporaryDirectory() as tmp:
            tmp = Path(tmp)
            kit = self.make(work_tree(tmp), tmp / 'res', False) / 'button-icons'
            run = subprocess.run([sys.executable, '-B', kit / 'games/totk/scripts/make_mod_manifest.py', '--help'], capture_output=True, text=True)
            self.assertEqual(run.returncode, 0, run.stderr)
            self.assertIn('--app', run.stdout)
            keys = subprocess.run([sys.executable, '-B', '-c', f'import sys; sys.path.insert(0, {str(kit)!r}); '
                                   'from tools.builder.keyboard_layout import default_layout; print(default_layout().valid)'],
                                  capture_output=True, text=True, cwd=tmp)       # botw_prompts.py --style keyboard needs it
            self.assertEqual(keys.stdout.strip(), 'True', keys.stderr)
            # the tool itself, with only the kit on its path (no repo: the kit folder is moved away from the work folder first)
            moved = tmp / 'moved-kit'
            shutil.copytree(kit, moved)
            shutil.rmtree(tmp / 'work')
            run = subprocess.run([sys.executable, '-B', '-I', moved / 'games/botw/scripts/botw_prompts.py', '--style', 'keyboard', '--help'],
                                 capture_output=True, text=True, cwd=tmp)
            if run.returncode and 'fontTools' in run.stderr:
                self.skipTest('needs fontTools (requirements.txt)')
            self.assertEqual(run.returncode, 0, run.stderr)
            self.assertIn('--bindings', run.stdout)
            labels = subprocess.run([sys.executable, '-B', '-I', '-c', f'import sys; sys.path[:0] = [{str(moved / "games/botw/scripts")!r}]; '
                                     'import botw_prompts as b; labels, notes = b.keyboard_controls(); print(len(labels) > 10, notes)'],
                                    capture_output=True, text=True, cwd=tmp)   # tools/builder/keyboard_layout.py is found in the kit
            if labels.returncode and 'fontTools' in labels.stderr:
                self.skipTest('needs fontTools (requirements.txt)')
            self.assertEqual(labels.stdout.strip(), 'True []', labels.stderr)

    def test_tools_start_from_the_kit(self):
        with tempfile.TemporaryDirectory() as tmp:
            tmp = Path(tmp)
            build_app.button_icons_kit(work_tree(tmp), tmp / 'res', 'botw190', 'botw')
            for tool in ('games/botw/scripts/botw_prompts.py', 'games/totk/scripts/make_button_icons.py'):
                run = subprocess.run([sys.executable, '-E', '-s', tmp / 'res/button-icons' / tool, '--help'], capture_output=True, text=True)
                if run.returncode and 'fontTools' in run.stderr:
                    self.skipTest('needs fontTools (requirements.txt)')
                self.assertEqual(run.returncode, 0, run.stderr)
                self.assertIn('--face-buttons', run.stdout)


if __name__ == '__main__':
    unittest.main()
