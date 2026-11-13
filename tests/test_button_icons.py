"""make_button_icons.py: the prompt styles follow the player's controls.txt and face buttons (letters are stubbed: no game data)."""
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SCRIPTS = ROOT/'games/totk/scripts'
sys.path.insert(0, str(SCRIPTS))
import make_button_icons as mbi         # noqa: E402  (needs Python 3.14: compression.zstd, and fontTools)
import prompt_layout as pl      # noqa: E402

OWNER = 'north = a\nsouth = x\nwest = y\neast = b\n'


class Stub:
    """Stands for the letter outlines: remembers the text it was asked to cut out."""
    def __init__(self):
        self.texts = []

    def hole(self, pen, text, cx, cy, cap, max_width):
        self.texts.append(text)


def letter(make, controls, codepoint):
    """The text the style cuts out of the glyph at `codepoint`, or None when the style leaves that glyph alone."""
    g = Stub()
    icons = make(g, controls)
    if codepoint not in icons:
        return None
    from fontTools.pens.recordingPen import RecordingPen
    icons[codepoint][1](RecordingPen())
    return g.texts[-1] if g.texts else '<symbol>'


class StylesFollowControls(unittest.TestCase):
    A, B, X, Y, L, ZR, PLUS = 0xE040, 0xE041, 0xE042, 0xE043, 0xE044, 0xE047, 0xE04B

    def test_default_xbox_is_the_switch_layout_by_position(self):
        xbox = mbi.STYLES['xbox']
        self.assertEqual([letter(xbox, None, c) for c in (self.A, self.B, self.X, self.Y)], ['B', 'A', 'Y', 'X'])
        self.assertEqual((letter(xbox, None, self.L), letter(xbox, None, self.ZR)), ('LB', 'RT'))

    def test_xbox_follows_the_owner_layout_by_position(self):
        controls = pl.prompt_controls(OWNER, 'position')
        xbox = mbi.STYLES['xbox']
        self.assertEqual([letter(xbox, controls, c) for c in (self.A, self.B, self.X, self.Y)], ['Y', 'B', 'A', 'X'])

    def test_xbox_follows_the_owner_layout_by_label(self):   # north is the Xbox X button when the pad maps by label
        controls = pl.prompt_controls(OWNER, 'label')
        xbox = mbi.STYLES['xbox']
        self.assertEqual([letter(xbox, controls, c) for c in (self.A, self.B, self.X, self.Y)], ['X', 'A', 'B', 'Y'])

    def test_shoulders_follow_the_file(self):
        controls = pl.prompt_controls('l1 = zl\nl2 = l\n', 'position')
        xbox = mbi.STYLES['xbox']
        self.assertEqual((letter(xbox, controls, 0xE046), letter(xbox, controls, self.L)), ('LB', 'LT'))

    def test_a_button_nothing_drives_keeps_its_switch_icon(self):
        controls = pl.prompt_controls('menu = none\n', 'position')
        self.assertIsNone(letter(mbi.STYLES['xbox'], controls, self.PLUS))
        self.assertIsNotNone(letter(mbi.STYLES['xbox'], None, self.PLUS))

    def test_playstation_label_is_playstation_with_label_face_buttons(self):
        g = Stub()
        label = mbi.STYLES['playstation-label'](g, None)
        same = mbi.STYLES['playstation'](g, pl.prompt_controls('', 'label'))
        drawers, _ = mbi.playstation_drawers(g)
        self.assertEqual(label.keys(), same.keys())
        self.assertEqual(mbi.slot_glyphs(drawers, pl.prompt_controls('', 'label'))['a'][0], drawers['a'][0])
        self.assertIs(mbi.slot_glyphs(drawers, pl.prompt_controls('', 'label'))['a'], drawers['a'])    # A is Cross
        self.assertIs(mbi.slot_glyphs(drawers, pl.prompt_controls('', 'position'))['a'], drawers['b'])  # A is Circle

    def test_the_stick_presses_show_the_stick_buttons(self):
        xbox, playstation = mbi.STYLES['xbox'], mbi.STYLES['playstation']
        self.assertEqual((letter(xbox, None, 0xE056), letter(xbox, None, 0xE057)), ('LS', 'RS'))
        self.assertIsNotNone(letter(playstation, None, 0xE056))
        self.assertIsNone(letter(mbi.STYLES['xbox'], pl.prompt_controls('l3 = none\n', 'position'), 0xE056))   # nothing drives it: unchanged



class TexturePackTable(unittest.TestCase):
    """With the HD texture pack on, the icons grow the pack's resource size table (--texture-pack), so one table holds the
    pack's raised textures and the fonts' growth; its manifest line names the pack's file as the original (runtime/romfs.c
    then loads it in place of the pack's), and a pack without a table changes nothing."""
    TABLE = 'System/Resource/ResourceSizeTable.Product.143.Nin_NX_NVN.rsizetable.zs'
    FONT, TEXTURE = 'Font/Font.Nin_NX_NVN.bfarc.zs', 'Model/Big.Nin_NX_NVN.bntx.zs'

    def table(self, font, texture):
        import struct, zlib
        entries = sorted((zlib.crc32(p.removesuffix('.zs').encode()), s) for p, s in ((self.FONT, font), (self.TEXTURE, texture)))
        return b'RESTBL' + struct.pack('<IIII', 1, 160, 2, 0) + b''.join(struct.pack('<II', c, s) for c, s in entries)

    def test_the_pack_table_is_grown_and_named_as_the_original(self):
        import hashlib, struct, zlib
        with tempfile.TemporaryDirectory() as tmp:
            pack = Path(tmp)
            data = mbi.zstd.compress(self.table(1000, 9_000_000))
            (pack/self.TABLE).parent.mkdir(parents=True)
            (pack/self.TABLE).write_bytes(data)
            sha = hashlib.sha256(data).hexdigest()
            (pack/'manifest.tsv').write_text(f'# path\tsize\tsha256\toriginal_sha256\n{self.TABLE}\t{len(data)}\t{sha}\t{"0" * 64}\n')
            raw, named = mbi.pack_restbl(pack, self.TABLE)
            self.assertEqual((raw, named), (data, sha))
            grown = mbi.grow_restbl(mbi.zstd.decompress(raw), self.table(1000, 4000), {self.FONT: 352})
            sizes = {struct.unpack_from('<I', grown, 22 + i * 8)[0]: struct.unpack_from('<I', grown, 26 + i * 8)[0] for i in range(2)}
            self.assertEqual(sizes[zlib.crc32(self.FONT.removesuffix('.zs').encode())], 1352)
            self.assertEqual(sizes[zlib.crc32(self.TEXTURE.removesuffix('.zs').encode())], 9_000_000)   # the pack's raise stays

    def test_a_pack_without_a_table_changes_nothing(self):
        with tempfile.TemporaryDirectory() as tmp:
            (Path(tmp)/'manifest.tsv').write_text(f'# path\tsize\tsha256\toriginal_sha256\n{self.TEXTURE}\t1\t{"a" * 64}\t{"b" * 64}\n')
            self.assertIsNone(mbi.pack_restbl(Path(tmp), self.TABLE))

    def test_the_launcher_keys_the_set_on_the_pack_and_passes_it(self):
        launcher = (ROOT/'games/totk/app/launcher.m').read_text()
        self.assertIn('[arguments addObjectsFromArray:@[@"--texture-pack", pack]]', launcher)
        self.assertIn('if ([line hasPrefix:@"System/Resource/ResourceSizeTable."]) { [keyed appendData:', launcher)

class Command(unittest.TestCase):
    def run_tool(self, *args):
        return subprocess.run([sys.executable, str(SCRIPTS/'make_button_icons.py'), 'mod', '--romfs', 'nowhere', *args],
                              capture_output=True, text=True)

    def test_controls_needs_the_face_mode(self):
        with tempfile.NamedTemporaryFile('w', suffix='.txt') as f:
            f.write(OWNER); f.flush()
            result = self.run_tool('--style', 'xbox', '--controls', f.name)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn('--controls needs --face-buttons', result.stderr)

    def test_a_bad_controls_file_names_its_line(self):
        with tempfile.NamedTemporaryFile('w', suffix='.txt') as f:
            f.write('east = a\nsout = b\n'); f.flush()
            result = self.run_tool('--style', 'xbox', '--controls', f.name, '--face-buttons', 'position')
            self.assertNotEqual(result.returncode, 0)
            self.assertIn('line 2: unknown controller button', result.stderr)

    def test_keyboard_keys_do_not_follow_a_controller(self):
        result = self.run_tool('--style', 'keyboard', '--face-buttons', 'label')
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('apply to the xbox and playstation styles', result.stderr)


if __name__ == '__main__':
    unittest.main()
