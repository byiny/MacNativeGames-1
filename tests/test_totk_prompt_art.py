"""totk_prompt_art.py (make_button_icons.py --our-art): the fit maths and the plans for the glyphs and the button guide masks,
without game data (the geometry on the real glyphs is checked by games/totk/scripts/dev/prompt_sheet.py)."""
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'games/totk/scripts'))
import totk_prompt_art as tpa       # noqa: E402  (needs Python 3.14: compression.zstd, and fontTools)
import prompt_layout as pl      # noqa: E402

OWNER = 'north = a\nsouth = x\nwest = y\neast = b\n'      # owner-map/controls.txt, without its comments
FACES = (0xE040, 0xE041, 0xE042, 0xE043)


class FitMaths(unittest.TestCase):
    def test_fit_centres_the_ink_on_the_box_with_its_shape_kept(self):
        self.assertEqual(tpa.fit((0, 0, 100, 50), (0, 0, 200, 200)), (2, 0, 0, 2, 0, 50))      # the width limits: 2x, centred up
        k, _, _, _, dx, dy = tpa.fit((10, 20, 60, 120), (0, 0, 100, 100))      # the height limits: 1x
        self.assertEqual((k, dx, dy), (1, 15, -20))

    def test_disc_is_the_square_of_the_mean_side_on_the_centre(self):
        self.assertEqual(tpa.disc((0, 0, 100, 60)), (10, -10, 90, 70))


class Plans(unittest.TestCase):
    def glyphs(self, text='', face='position', style='xbox'):
        return tpa.glyph_plan(pl.prompt_controls(text, face), style)

    def test_default_xbox_plan(self):
        plan, notes = self.glyphs()
        self.assertEqual({cp: plan[cp] for cp in FACES}, {0xE040: ('b', 'disc'), 0xE041: ('a', 'disc'), 0xE042: ('y', 'disc'), 0xE043: ('x', 'disc')})
        self.assertEqual([plan[cp] for cp in (0xE044, 0xE045, 0xE046, 0xE047)], [('l1', 'key'), ('r1', 'key'), ('l2', 'key'), ('r2', 'key')])
        self.assertEqual((plan[0xE04B], plan[0xE04C]), (('menu', 'disc'), ('view', 'disc')))
        self.assertTrue(all(plan[cp] == (cp, 'disc') for cp in tpa.DPAD) and len(tpa.DPAD) == 7)
        self.assertEqual(len(plan), 4 + 4 + 2 + 7)
        for cp in (0xE056, 0xE057, 0xE052, 0xE05E):      # the stick glyphs stay the game's
            self.assertNotIn(cp, plan)
        self.assertEqual(notes, [])

    def test_owner_file_by_position_and_by_label(self):
        by_position, _ = self.glyphs(OWNER, 'position')
        self.assertEqual([by_position[cp][0] for cp in FACES], ['y', 'b', 'a', 'x'])
        by_label, _ = self.glyphs(OWNER, 'label')
        self.assertEqual([by_label[cp][0] for cp in FACES], ['x', 'a', 'b', 'y'])
        playstation, _ = self.glyphs(OWNER, 'position', 'playstation')
        self.assertEqual([playstation[cp][0] for cp in FACES], ['y', 'b', 'a', 'x'])      # the drawers are named by Xbox position

    def test_swapped_stick_press_uses_the_other_own_glyph(self):
        plan, _ = self.glyphs('r3 = lstick\n')
        self.assertEqual(plan[0xE056], (('own', 0xE057), 'stick'))
        self.assertNotIn(0xE057, plan)      # r3 drives nothing now

    def test_a_button_nothing_drives_keeps_its_switch_glyph_with_a_note(self):
        plan, notes = self.glyphs('l3 = none\n')
        self.assertNotIn(0xE056, plan)
        self.assertEqual(len(notes), 1)
        plan, notes = self.glyphs('l1 = a\n')
        self.assertNotIn(0xE040, plan)      # A is driven by l1, which has no face picture
        self.assertNotIn(0xE044, plan)      # nothing drives L
        self.assertEqual(len(notes), 2)

    def test_hud_plan(self):
        default, _ = tpa.hud_plan(pl.prompt_controls('', 'position'), 'xbox')
        self.assertEqual(default, {'Nt_KeyTexA_00': ('b', 'disc'), 'Nt_KeyTexB_00': ('a', 'disc'), 'Nt_KeyTexX_00': ('y', 'disc'),
                                   'Nt_KeyTexY_00': ('x', 'disc'), 'Nt_KeyTexL_00': ('l1', 'key'), 'Nt_KeyTexZL_00': ('l2', 'key')})
        owner, _ = tpa.hud_plan(pl.prompt_controls(OWNER, 'position'), 'playstation')
        self.assertEqual([owner[f'Nt_KeyTex{s}_00'][0] for s in 'ABXY'], ['y', 'b', 'a', 'x'])
        self.assertNotIn('Nt_KeyTexLSt_00', owner)
        stick, _ = tpa.hud_plan(pl.prompt_controls('l3 = l\n', 'position'), 'xbox')
        self.assertNotIn('Nt_KeyTexL_00', stick)      # L shows the game's own stick glyph: no mask for it


class Launcher(unittest.TestCase):
    def test_launcher_asks_for_our_art_xbox_and_no_live_prompts(self):
        text = (ROOT / 'games/totk/app/launcher.m').read_text()
        for needle in ('--our-art', 'TOTK_PROMPT_ICONS', 'GAMEPAD_TYPE_XBOX;'):
            self.assertIn(needle, text)

    def test_our_art_is_refused_for_the_keyboard_styles(self):
        for style in ('keyboard', 'azerty'):
            with tempfile.TemporaryDirectory() as tmp:
                run = subprocess.run([sys.executable, '-B', str(ROOT / 'games/totk/scripts/make_button_icons.py'), tmp + '/icons',
                                      '--romfs', tmp, '--style', style, '--our-art'], capture_output=True, text=True)
            self.assertEqual(run.returncode, 2)
            self.assertIn('--our-art applies to the xbox and playstation styles', run.stderr)


if __name__ == '__main__':
    unittest.main()
