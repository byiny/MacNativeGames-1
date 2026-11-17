"""totk_controller_art.py (make_button_icons.py --our-art): the sizes, the face labels, the layout of the drawn pictures, the BC5 and
shadow encoders and redraw_pictures on a minimal BNTX, and the W1g parts (tilted pictures and shadows, R/ZR masks, d-pad icon, popup
letters), without game data (the pictures against the game's own layouts are checked by games/totk/scripts/dev/picture_sheet.py)."""
import struct
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'games/totk/scripts'))
import totk_controller_art as tca       # noqa: E402  (needs Python 3.14: compression.zstd, and fontTools)
import prompt_layout as pl      # noqa: E402

OWNER = 'north = a\nsouth = x\nwest = y\neast = b\n'      # owner-map/controls.txt, without its comments


def build_bntx(facts):
    """A minimal BNTX: name -> (format, width, height, layout = log2 of the block height, image size, selector)."""
    blob, data_at = bytearray(b'BNTX' + bytes(28)), {}
    for name, (fmt, w, h, layout, size, selector) in facts.items():
        i = len(blob); blob += b'BRTI' + bytes(0x100 - 4)
        struct.pack_into('<H', blob, i + 0x16, 1); struct.pack_into('<I', blob, i + 0x1C, fmt)
        struct.pack_into('<IIII', blob, i + 0x24, w, h, 1, 1); struct.pack_into('<I', blob, i + 0x34, layout)
        struct.pack_into('<I', blob, i + 0x50, size); blob[i + 0x58:i + 0x5c] = selector
        n = len(blob); blob += struct.pack('<H', len(name)) + name.encode() + bytes(6); struct.pack_into('<Q', blob, i + 0x60, n)
        p = len(blob); blob += bytes(8); struct.pack_into('<Q', blob, i + 0x70, p); data_at[name] = (p, size)
    for name, (p, size) in data_at.items():
        struct.pack_into('<Q', blob, p, len(blob)); blob += bytes(size)
    return bytes(blob)


def tilted_facts(wide=None):
    """The 18 item-1 BRTIs (and with `wide`, ConGripR_00^s that many texels wide)."""
    facts = {}
    for kind, (picture, shadow) in tca.TILT_PICTURES.items():
        facts[picture] = (tca.ASTC_4X4_SRGB, 580, 340, 3, 303104, tca.RGBA_SELECTOR)
        facts[shadow] = (tca.BC4_UNORM, 314, 360, 4, 81920, bytes([1, 1, 1, 2]))
        for s, (stem, (w, h, bh, size), _, _) in tca.MASKS[kind].items():
            for part in ('00', 'Line'):
                facts[f'{stem}_{part}^s'] = (tca.BC4_UNORM, w, h, bh.bit_length() - 1, size, tca.MASK_SELECTOR)
    if wide:
        name = 'ConGripR_00^s'
        facts[name] = (*facts[name][:1], wide, *facts[name][2:])
    return facts


def ink(rows, level=128):
    pts = [(x, y) for y, row in enumerate(rows) for x, v in enumerate(row) if v >= level]
    return min(p[0] for p in pts), min(p[1] for p in pts), max(p[0] for p in pts) + 1, max(p[1] for p in pts) + 1


def square(pen):
    pen.moveTo((0, 0)); pen.lineTo((0, 100)); pen.lineTo((100, 100)); pen.lineTo((100, 0)); pen.closePath()


def popup_blob(names):
    """A DoCommand_00 stand-in: a 0x14 byte header and one 172 byte txt1 per name, each holding its own letter ('+' for T_Plus_00)."""
    blob = bytearray(0x14)
    struct.pack_into('<H', blob, 6, 0x14); struct.pack_into('<H', blob, 0x10, len(names))
    for name in names:
        pane = bytearray(172)
        pane[0:4] = b'txt1'; struct.pack_into('<I', pane, 4, 172); pane[12:12 + len(name)] = name.encode()
        struct.pack_into('<HH', pane, 84, 4, 4); struct.pack_into('<I', pane, 100, 168)
        pane[168:170] = ('+' if name == 'T_Plus_00' else name[2]).encode('utf-16-le')
        blob += pane
    return bytes(blob)


TEXTURES = ['Nt_KeyBtnSubBaseAL_00^s', 'Nt_KeyBtnSubBaseBL_00^s']
PLACE_AT = {'right': (39.0, 0.0), 'bottom': (0.0, -39.0), 'top': (0.0, 39.0), 'left': (-39.0, 0.0)}
LETTER_AT = {'right': (39.0, 1.0), 'bottom': (2.0, -39.0), 'top': (1.0, 39.0), 'left': (-38.0, 0.0)}
SWITCH_AT = {'A': 'right', 'B': 'bottom', 'X': 'top', 'Y': 'left'}
BACKING = {'right': (0, 1.0, 0.0), 'bottom': (1, 1.0, 0.0), 'top': (1, -1.0, 180.0), 'left': (0, 1.0, 180.0)}
PARTS = {'A': ['T_B_00', 'T_X_00', 'T_Y_00'], 'B': ['T_A_01', 'T_X_01', 'T_Y_01'], 'X': ['T_A_02', 'T_B_02', 'T_Y_02'],
         'Y': ['T_A_03', 'T_B_03', 'T_X_03']}


def doc_blob():
    """A DoCommand_00 stand-in with 1.4.3's shape: txl1 with the two backing textures, one mat1 entry per backing, and the
    parts N_<A|B|X|Y>_00 (backing, three letters, lit button with name picture, window and text, rush copy of the name; A has
    a hold copy N_StopA_00 too) in the Switch's places, Y mirrored. Returns (blob, {pane name: offset})."""
    sections, panes, mats = [], {}, []

    def pane(magic, name, xy=(0.0, 0.0), turn=0.0, scale=1.0, origin=0, text=None, material=0):
        b = bytearray(172); b[0:4] = magic; struct.pack_into('<I', b, 4, 172); b[9] = origin; b[12:12 + len(name)] = name.encode()
        struct.pack_into('<2f', b, 0x2c, *xy); struct.pack_into('<f', b, 0x40, turn); struct.pack_into('<2f', b, 0x44, scale, 1.0)
        if magic == b'txt1':
            b[0x5c] = origin; struct.pack_into('<HH', b, 0x54, 4, 4); struct.pack_into('<I', b, 0x64, 168)
            b[168:170] = (text or 'x').encode('utf-16-le')
        if magic == b'pic1':
            struct.pack_into('<H', b, 0x64, material)
        sections.append((name, bytes(b)))

    def nest(magic):
        sections.append((None, magic + struct.pack('<I', 8)))

    def backing(name, place):
        texture, scale, turn = BACKING[place]; mats.append(texture); pane(b'pic1', name, turn=turn, scale=scale, material=len(mats) - 1)

    def button(name, s):
        place = SWITCH_AT[s]; left = place == 'left'
        pane(b'pan1', name, PLACE_AT[place]); nest(b'pas1')
        pane(b'pic1', f'P_Capture{s}_0{len(name) % 7}', (-90.0 if left else 90.0, 6.0))
        pane(b'wnd1', f'W_Base{s}_{name[-2:]}', (50.0 if left else -50.0, 0.0), scale=-1.0 if left else 1.0)
        pane(b'txt1', f'T_Guide{s}_{name[-2:]}', (-34.0 if left else 32.0, 0.0), origin=2 if left else 1, text='G')
        nest(b'pae1')
    pane(b'pan1', 'RootPane'); nest(b'pas1')
    for s, letters in PARTS.items():
        place = SWITCH_AT[s]
        pane(b'pan1', f'N_{s}_00'); nest(b'pas1')
        pane(b'pan1', f'N_Capture{s}_00', (-117.0, 330.0)); nest(b'pas1')
        pane(b'txt1', f'T_CaptureGuide{s}_00', (56.0 if place == 'left' else -58.0, 0.0), origin=2 if place == 'left' else 1, text='G')
        nest(b'pae1')
        pane(b'pan1', f'N_Loop{s}_00'); nest(b'pas1')
        backing(f'P_Base{s}_00', place)
        for name in letters:
            pane(b'txt1', name, LETTER_AT[SWITCH_AT[name[2]]], text=name[2])
        button(f'N_Btn{s}_00', s)
        nest(b'pae1')
        if s == 'A':
            pane(b'pan1', 'N_StopA_00'); nest(b'pas1')
            backing('P_BaseA_01', 'right')
            for name in ('T_B_01', 'T_X_02', 'T_Y_03'):
                pane(b'txt1', name, LETTER_AT[SWITCH_AT[name[2]]], text=name[2])
            button('N_BtnA_01', 'A')
            nest(b'pae1')
            pane(b'pan1', 'N_HoldA_00'); nest(b'pas1'); pane(b'pan1', 'N_LoopA_03', (100.0, 0.0)); nest(b'pas1')
            pane(b'pic1', 'P_CaptureA_05', (29.0, 6.0)); nest(b'pae1'); nest(b'pae1')
        x, y = PLACE_AT[place]
        pane(b'pan1', f'N_Rush{s}_00'); nest(b'pas1'); pane(b'pan1', f'N_Loop{s}_01'); nest(b'pas1')
        pane(b'pic1', f'P_Capture{s}_01', (x + (-90.0 if place == 'left' else 90.0), y + 6.0)); nest(b'pae1'); nest(b'pae1')
        nest(b'pae1')
    nest(b'pae1')
    names = b''.join(t.encode() + b'\0' for t in TEXTURES)
    txl = bytearray(b'txl1' + bytes(4) + struct.pack('<HH', 2, 0) + struct.pack('<2I', 8, 8 + len(TEXTURES[0]) + 1) + names)
    txl += bytes(-len(txl) % 4); struct.pack_into('<I', txl, 4, len(txl))
    mat = bytearray(b'mat1' + bytes(4) + struct.pack('<HH', len(mats), 0)); head = len(mat) + 4 * len(mats)
    mat += b''.join(struct.pack('<I', head + 0x30 * i) for i in range(len(mats)))
    for i, texture in enumerate(mats):
        entry = bytearray(0x30); entry[0:5] = b'mat%02d' % i; entry[0x1c] = 0x15; struct.pack_into('<H', entry, 0x2c, texture)
        mat += entry
    struct.pack_into('<I', mat, 4, len(mat))
    blob = bytearray(0x14); struct.pack_into('<H', blob, 6, 0x14); struct.pack_into('<H', blob, 0x10, 2 + len(sections))
    blob += txl + mat
    for name, body in sections:
        if name:
            panes[name] = len(blob)
        blob += body
    return bytes(blob), panes


def text_at(msbt, index):
    """Where text `index` of a msbt_blob sits (read with its section sizes, not with the code under test)."""
    lbl = 0x20 + 16; txt = (lbl + struct.unpack_from('<I', msbt, 0x24)[0] + 15) & ~15
    txt += 16
    return txt + struct.unpack_from('<I', msbt, txt + 4 + 4 * index)[0]


def msbt_blob(names, magic=b'MsgStdBn', long_text=None):
    """A DoCommand_00.msbt stand-in: LBL1 with one slot per name (name, u32 index) and TXT2 with one text each, every letter label
    holding its own letter + U+0000 (`long_text` names a label that holds a longer text), T_Plus_00 '+'."""
    texts = [(('+' if n == 'T_Plus_00' else n[2]) + ('ab' if n == long_text else '') + '\0').encode('utf-16-le') for n in names]
    lbl = struct.pack('<I', len(names)) + b''.join(struct.pack('<II', 1, 4 + 8 * len(names) + sum(5 + len(m) for m in names[:i])) for i in range(len(names)))
    lbl += b''.join(bytes([len(n)]) + n.encode() + struct.pack('<I', i) for i, n in enumerate(names))
    offsets, at = [], 4 + 4 * len(texts)
    for t in texts:
        offsets.append(at); at += len(t)
    txt = struct.pack('<I', len(texts)) + struct.pack(f'<{len(texts)}I', *offsets) + b''.join(texts)

    def section(magic, data):
        out = magic + struct.pack('<I', len(data)) + bytes(8) + data
        return out + b'\xab' * (-len(out) % 16)
    body = section(b'LBL1', lbl) + section(b'TXT2', txt)
    head = bytearray(0x20); head[:8] = magic; head[8:10] = b'\xff\xfe'; struct.pack_into('<H', head, 0x0e, 2)
    struct.pack_into('<I', head, 0x12, 0x20 + len(body))
    return bytes(head) + body


LETTER_PANES = ['T_B_00', 'T_X_00', 'T_Y_00', 'T_B_01', 'T_X_02', 'T_Y_03', 'T_A_01', 'T_X_01', 'T_Y_01', 'T_A_02', 'T_B_02', 'T_Y_02',
                'T_A_03', 'T_B_03', 'T_X_03']
RINGS = {   # the ability guide's rings in the tilted frames, picture pixels (the distinct points of the game's table)
    'console': [(44.7, 122.2), (44.7, 123.3), (49.2, 150.0), (68.4, 162.2), (531.9, 171.1), (531.9, 176.6), (536.5, 101.1), (554.6, 114.4),
                (554.6, 115.6), (557.4, 111.9), (534.2, 127.8), (535.3, 130.0)],
    'grip': [(176.9, 126.7), (178.1, 160.0), (202.9, 176.7), (399.7, 187.8), (403.6, 183.9), (400.8, 96.7), (400.8, 97.8), (425.7, 114.4),
             (431.3, 114.4), (400.8, 131.1), (401.9, 131.1)],
    'pro': [(172.4, 156.7), (225.6, 181.1), (244.8, 197.8), (346.5, 208.9), (352.0, 207.9), (404.2, 120.0), (404.2, 121.1), (438.1, 145.6),
            (443.7, 146.7), (404.2, 172.2), (405.3, 172.2)],
}


def labels(text='', face='position', style='xbox'):
    return tca.face_labels(pl.prompt_controls(text, face), style)


class Geometry(unittest.TestCase):
    def test_sizes_of_the_three_pictures(self):
        for kind, (k, rf) in {'console': (1.017, 9.83), 'grip': (1.303, 12.5), 'pro': (1.189, 12.5)}.items():
            got = tca.sizes(tca.ANCHORS[kind])
            self.assertAlmostEqual(got[0], k, delta=0.01, msg=kind)
            self.assertAlmostEqual(got[1], rf, delta=0.01, msg=kind)

    def test_spots_of_the_pro_picture(self):
        spots = tca.spots(tca.ANCHORS['pro'])
        expect = {'x': (403.6, 84.45), 'a': (436.95, 113.9), 'b': (403.6, 143.35), 'y': (370.25, 113.9)}      # X north, A east, B south, Y west
        for slot, (x, y) in expect.items():
            self.assertAlmostEqual(spots[slot][0], x, delta=0.01)
            self.assertAlmostEqual(spots[slot][1], y, delta=0.01)

    def test_face_labels(self):
        self.assertEqual(labels(), {'a': 'B', 'b': 'A', 'x': 'Y', 'y': 'X'})
        self.assertEqual(labels(OWNER), {'a': 'Y', 'b': 'B', 'x': 'A', 'y': 'X'})
        self.assertEqual(labels(style='playstation'), {'a': 'Circle', 'b': 'Cross', 'x': 'Triangle', 'y': 'Square'})
        self.assertEqual(labels(OWNER, style='playstation'), {'a': 'Triangle', 'b': 'Circle', 'x': 'Cross', 'y': 'Square'})
        self.assertEqual(labels(face='label', style='playstation-label'), {'a': 'Cross', 'b': 'Circle', 'x': 'Square', 'y': 'Triangle'})
        self.assertEqual(labels('l1 = a\n')['a'], 'LB')

    def test_every_picture_keeps_its_buttons_in_the_body_and_paints_the_shoulders(self):
        for kind in tca.PICTURES:
            for style in ('xbox', 'playstation'):
                for text in ('', OWNER):
                    (grey, alpha), notes = tca.draw(kind, style, pl.prompt_controls(text, 'position'), None)
                    self.assertEqual((len(grey), len(grey[0])), (340, 580))
                    self.assertEqual(tca.layout_problems(kind, style, alpha), [], (kind, style, bool(text)))
                    self.assertEqual(notes, [])

    def test_a_face_button_without_a_name_is_drawn_plain_with_a_note(self):
        controls = dict(pl.prompt_controls('', 'position'), a=None)
        _, notes = tca.draw('pro', 'xbox', controls, None)
        self.assertEqual(len(notes), 1)
        self.assertIn('Switch A', notes[0])


class Encoding(unittest.TestCase):
    def test_linear_table(self):
        self.assertEqual((tca.LINEAR[0], tca.LINEAR[255]), (0, 255))
        self.assertTrue(all(a <= b for a, b in zip(tca.LINEAR, tca.LINEAR[1:])))
        self.assertEqual((tca.LINEAR[13], tca.LINEAR[135], tca.LINEAR[193]), (1, 62, 136))

    def test_picture_bc5_stores_linear_grey_and_alpha(self):
        grey = [bytes([193] * 290 + [255] * 290) for _ in range(340)]
        alpha = [bytes([255] * 290 + [0] * 290) for _ in range(340)]
        image = tca.picture_bc5(grey, alpha, bytes(303104), 8)
        self.assertEqual(len(image), 303104)
        for x, y, r, g in ((10, 10, 136, 255), (289, 100, 136, 255), (290, 100, 62, 0), (570, 330, 62, 0)):
            at = tca.bf.gob_address(x // 4, y // 4, 145, 16, 8)
            i = y % 4 * 4 + x % 4
            self.assertEqual((tca.bf.bc4_decode(image[at:at + 8])[i], tca.bf.bc4_decode(image[at + 8:at + 16])[i]), (r, g), (x, y))

    def test_shadow_rows_blur_the_moved_silhouette(self):
        alpha = [bytes([255 if 100 <= x <= 479 and 50 <= y <= 289 else 0 for x in range(580)]) for y in range(340)]
        rows = tca.shadow_rows(alpha)
        self.assertEqual((len(rows), len(rows[0])), (360, 314))
        self.assertEqual(rows[180][150], 255)
        self.assertEqual(rows[180][54], 0)
        self.assertTrue(100 <= rows[180][124] <= 160)

    def bntx(self, grip_height=340):
        facts = {}
        for kind, (picture, shadow) in tca.PICTURES.items():
            facts[picture] = (tca.ASTC_4X4_SRGB, 580, grip_height if kind == 'grip' else 340, 3, 303104, tca.RGBA_SELECTOR)
            facts[shadow] = (tca.BC4_UNORM, 314, 360, 4, 81920, bytes([2, 2, 2, 3]))
        return build_bntx(facts)

    def run_redraw(self, blob):
        out, drawn, notes = tca.redraw_pictures(blob, pl.prompt_controls('', 'position'), 'xbox', None)
        return out, drawn, notes, tca.bf.bntx_textures(blob), tca.bf.bntx_textures(out)

    def test_redraw_pictures_on_a_minimal_bntx(self):
        blob = self.bntx()
        out, drawn, notes, old, new = self.run_redraw(blob)
        self.assertEqual((len(out), notes), (len(blob), []))
        self.assertEqual(drawn, [p for p, _ in tca.PICTURES.values()])
        at = {tca.bf.Texture(out, m).name: m for m in range(len(out)) if out[m:m + 4] == b'BRTI'}
        for picture, shadow in tca.PICTURES.values():
            self.assertEqual(struct.unpack_from('<I', out, at[picture] + 0x1C)[0], tca.BC5_UNORM)
            self.assertEqual(out[at[picture] + 0x58:at[picture] + 0x5c], bytes([2, 2, 2, 3]))
            self.assertEqual(out[at[shadow]:at[shadow] + 0x100], blob[at[shadow]:at[shadow] + 0x100])
            for name in (picture, shadow):
                self.assertTrue(any(out[new[name].offset:new[name].offset + new[name].image_size]), name)

    def test_a_picture_that_differs_from_the_game_stays(self):
        blob = self.bntx(grip_height=341)
        out, drawn, notes, old, new = self.run_redraw(blob)
        self.assertEqual(len(drawn), 2)
        self.assertEqual(len(notes), 1)
        self.assertIn('grip', notes[0])
        for name in tca.PICTURES['grip']:
            self.assertEqual(out[new[name].offset:new[name].offset + new[name].image_size], blob[old[name].offset:old[name].offset + old[name].image_size])


class Tilted(unittest.TestCase):
    def test_masks(self):
        for kind, shoulders in tca.MASKS.items():
            for s, (stem, (w, h, _, _), _, box) in shoulders.items():
                fill, line = tca.mask_rows(kind, s)
                self.assertEqual(([len(fill), len(fill[0])], [len(line), len(line[0])]), ([h, w], [h, w]), (kind, s))
                self.assertEqual(ink(fill), box, (kind, s))
                grown = (box[0] - 5, box[1] - 5, box[2] + 5, box[3] + 5)
                self.assertEqual(ink(line), grown, (kind, s))
                self.assertTrue(0 <= grown[0] and 0 <= grown[1] and grown[2] <= w and grown[3] <= h, (kind, s))
                self.assertFalse(any(f >= 128 and l >= 128 for fr, lr in zip(fill, line) for f, l in zip(fr, lr)), (kind, s))

    def test_dilate(self):
        grid = [[0] * 11 for _ in range(11)]
        grid[5][5] = 255
        self.assertEqual(sum(v == 255 for row in tca.dilate(grid, 2.0) for v in row), 13)
        self.assertEqual(sum(v == 255 for row in tca.dilate(grid, 5.0) for v in row), 81)

    def test_every_tilted_picture_keeps_its_buttons_in_the_body_and_the_band_middles_painted(self):
        for kind in tca.TILT_PICTURES:
            for style in ('xbox', 'playstation'):
                for text in ('', OWNER):
                    (grey, alpha), notes = tca.draw_tilted(kind, style, pl.prompt_controls(text, 'position'), None)
                    self.assertEqual((len(grey), len(grey[0])), (340, 580))
                    self.assertEqual(tca.tilt_problems(kind, style, alpha), [], (kind, style, bool(text)))
                    self.assertEqual(notes, [])

    def test_the_game_rings_lie_inside_the_tilted_buttons(self):
        for kind, rings in RINGS.items():
            by = tca.TILT[kind]['by']
            for x, y in rings:
                inside = min(((x - cx) / r) ** 2 + ((y - cy) / (r * by)) ** 2 for (cx, cy), r in tca.tilt_buttons(kind).values())
                self.assertLessEqual(inside, 1, (kind, x, y))

    def test_tilted_shadow_rows(self):
        alpha = [bytes([255 if 100 <= x <= 479 and 50 <= y <= 339 else 0 for x in range(580)]) for y in range(340)]
        rows = tca.tilted_shadow_rows(alpha, 'pro')
        self.assertEqual((len(rows), len(rows[0])), (360, 314))
        self.assertEqual((rows[180][150], rows[180][40]), (255, 0))
        self.assertTrue(100 <= rows[180][125] <= 160)
        self.assertGreaterEqual(rows[340][150], 250)
        self.assertEqual(rows[359][150], 0)

    def test_bc4_texture(self):
        for (w, h, bh, size) in ((76, 41, 2, 3072), (59, 30, 1, 1024), (64, 64, 2, 2048)):
            image = tca.bc4_texture([bytes([200] * w) for _ in range(h)], w, h, bh, bytes(size))
            self.assertEqual(len(image), size)
            blocks = (w + 3) // 4
            for y in range(h):
                for x in range(w):
                    at = tca.bf.gob_address(x // 4, y // 4, blocks, 8, bh)
                    self.assertEqual(tca.bf.bc4_decode(image[at:at + 8])[y % 4 * 4 + x % 4], 200, (w, h, x, y))

    def test_glow(self):
        rows = [bytes(255 if 12 <= x <= 51 and 12 <= y <= 51 else 0 for x in range(64)) for y in range(64)]
        g = tca.glow(rows)
        self.assertEqual((g[32][32], g[0][0]), (252, 0))
        self.assertTrue(1 <= g[32][8] <= 251)

    def test_popup_plan(self):
        for text, face, style, east, south, north, west in (
                ('', 'position', 'xbox', 'B', 'A', 'Y', 'X'), (OWNER, 'position', 'xbox', 'Y', 'B', 'A', 'X'),
                ('', 'position', 'playstation', '\u25cb', '\u00d7', '\u25b3', '\u25a1'),
                (OWNER, 'position', 'playstation', '\u25b3', '\u25cb', '\u00d7', '\u25a1'),
                ('', 'label', 'playstation-label', '\u00d7', '\u25cb', '\u25a1', '\u25b3')):
            plan, notes = tca.popup_plan(pl.prompt_controls(text, face), style)
            self.assertEqual((plan['a'], plan['b'], plan['x'], plan['y'], notes), (east, south, north, west, []), (style, bool(text)))
        plan, notes = tca.popup_plan(pl.prompt_controls('l1 = a\n', 'position'), 'xbox')
        self.assertEqual(plan['a'], ' ')
        self.assertEqual(len(notes), 1)
        self.assertIn('Switch A', notes[0])

    def test_popup_letters(self):
        controls = pl.prompt_controls('', 'position')
        blob = popup_blob(LETTER_PANES + ['T_Plus_00'])
        out, notes = tca.popup_letters(blob, controls, 'xbox')
        plan, _ = tca.popup_plan(controls, 'xbox')
        self.assertEqual((len(out), notes), (len(blob), []))
        for i, name in enumerate(LETTER_PANES + ['T_Plus_00']):
            at = 0x14 + 172 * i + 168
            want = '+' if name == 'T_Plus_00' else plan[name[2].lower()]
            self.assertEqual(out[at:at + 4], want.encode('utf-16-le') + b'\0\0', name)
        few = popup_blob(LETTER_PANES[:14] + ['T_Plus_00'])
        out, notes = tca.popup_letters(few, controls, 'xbox')
        self.assertEqual(out, few)
        self.assertEqual(len(notes), 1)

    def test_popup_places(self):
        blob, at = doc_blob()
        xy = lambda b, name: struct.unpack_from('<2f', b, at[name] + 0x2c)  # noqa: E731
        def backing(b, name):   # (A or B: the backing's texture, scale x, turn)
            m = b.index(b'mat1'); entry = m + struct.unpack_from('<I', b, m + 12 + 4 * struct.unpack_from('<H', b, at[name] + 0x64)[0])[0]
            return (TEXTURES[struct.unpack_from('<H', b, entry + 0x2c)[0]][16], struct.unpack_from('<f', b, at[name] + 0x44)[0],
                    struct.unpack_from('<f', b, at[name] + 0x40)[0])
        out, notes = tca.popup_places(blob, pl.prompt_controls('', 'position'))   # Xbox by position: already the Xbox places
        self.assertEqual((out, notes), (blob, []))
        # the owner's layout: Switch A by Y (top), B by B (right), X by A (bottom), Y by X (left)
        out, notes = tca.popup_places(blob, pl.prompt_controls(OWNER, 'position'))
        self.assertEqual((len(out), notes), (len(blob), []))
        place = {'A': 'top', 'B': 'right', 'X': 'bottom', 'Y': 'left'}
        for s in 'ABXY':
            self.assertEqual(xy(out, f'N_Btn{s}_00'), PLACE_AT[place[s]], s)
            self.assertEqual(backing(out, f'P_Base{s}_00'), ('A' if place[s] in ('right', 'left') else 'B',
                             *{'right': (1.0, 0.0), 'bottom': (1.0, 0.0), 'top': (-1.0, 180.0), 'left': (1.0, 180.0)}[place[s]]), s)
            for name in PARTS[s]:
                self.assertEqual(xy(out, name), LETTER_AT[place[name[2]]], name)
            x, y = PLACE_AT[place[s]]   # the rush copy keeps its offset from the button
            self.assertEqual(xy(out, f'P_Capture{s}_01'), (x + (-90.0 if s == 'Y' else 90.0), y + 6.0), s)
        self.assertEqual((xy(out, 'N_BtnA_01'), xy(out, 'T_X_02'), backing(out, 'P_BaseA_01')), ((0.0, 39.0), (2.0, -39.0), ('B', -1.0, 180.0)))
        self.assertEqual(xy(out, 'P_CaptureA_05'), (-10.0, 45.0))    # hold copy under N_LoopA_03 at x 100: (90, 6) from the button
        self.assertEqual((xy(out, 'W_BaseA_00'), out[at['T_GuideA_00'] + 9]), ((-50.0, 0.0), 1))   # not mirrored
        # Switch A by X (left): A takes Y's mirror image, and Y (by A, bottom) leaves it
        out, notes = tca.popup_places(blob, pl.prompt_controls('west = a\nnorth = b\neast = x\nsouth = y\n', 'position'))
        self.assertEqual(notes, [])
        self.assertEqual((xy(out, 'N_BtnA_00'), xy(out, 'P_CaptureA_01'), xy(out, 'P_CaptureA_05')), ((-39.0, 0.0), (-129.0, 6.0), (-229.0, 6.0)))
        self.assertEqual((xy(out, 'W_BaseA_00'), struct.unpack_from('<f', out, at['W_BaseA_00'] + 0x44)[0]), ((50.0, 0.0), -1.0))
        for name in ('T_GuideA_00', 'T_CaptureGuideA_00'):
            self.assertEqual((out[at[name] + 9], out[at[name] + 0x5c], xy(out, name)[0] > 0), (2, 2, name == 'T_CaptureGuideA_00'), name)
        self.assertEqual((xy(out, 'N_BtnY_00'), xy(out, 'W_BaseY_00'), out[at['T_GuideY_00'] + 9]), ((0.0, -39.0), (-50.0, 0.0), 1))
        self.assertEqual(backing(out, 'P_BaseA_00'), ('A', 1.0, 180.0))

    def test_popup_places_of_the_wrong_shape_stay(self):
        blob, at = doc_blob()
        controls = pl.prompt_controls(OWNER, 'position')
        moved = bytearray(blob); struct.pack_into('<f', moved, at['T_Y_01'] + 0x2c, 5.0)
        renamed = bytearray(blob); renamed[at['N_X_00'] + 12:at['N_X_00'] + 18] = b'N_Q_00'
        for what, bad in (('a letter off its place', moved), ('a missing part', renamed)):
            out, notes = tca.popup_places(bytes(bad), controls)
            self.assertEqual(out, bytes(bad), what)
            self.assertTrue(len(notes) == 1 and notes[0].endswith('; the Switch places stay'), (what, notes))
        out, notes = tca.popup_places(blob, {'a': 'a', 'b': 'a', 'x': 'x', 'y': 'y'})
        self.assertEqual((out, len(notes)), (blob, 1))

    def test_popup_messages(self):
        controls = pl.prompt_controls('', 'position')
        names = LETTER_PANES + ['T_Plus_00', 'T_Other_00']
        blob = msbt_blob(names)
        for style in ('xbox', 'playstation'):
            out, notes = tca.popup_messages(blob, controls, style)
            plan, _ = tca.popup_plan(controls, style)
            self.assertEqual((len(out), notes), (len(blob), []))
            for name in names:
                at = text_at(blob, names.index(name))
                want = plan[name[2].lower()] if name in LETTER_PANES else '+' if name == 'T_Plus_00' else 'O'
                self.assertEqual(out[at:at + 4], (want + '\0').encode('utf-16-le'), (style, name))
            changed = {i for i in range(len(blob)) if out[i] != blob[i]}   # nothing else moves: not the other labels, not T_Plus, not the headers
            texts = {text_at(blob, names.index(n)) + k for n in LETTER_PANES for k in range(4)}
            self.assertTrue(changed <= texts)
        out, _ = tca.popup_messages(blob, controls, 'playstation')
        at = text_at(out, names.index('T_A_01'))
        self.assertEqual(out[at:at + 4], '\u25cb\0'.encode('utf-16-le'))   # default position map: Switch A is the east button, a circle
        blank, notes = tca.popup_messages(blob, pl.prompt_controls('l1 = a\n', 'position'), 'xbox')
        self.assertEqual(len(notes), 1)
        at = text_at(blank, names.index('T_A_01'))
        self.assertEqual(blank[at:at + 4], ' \0'.encode('utf-16-le'))

    def test_popup_messages_of_the_wrong_shape_stay(self):
        controls = pl.prompt_controls('', 'position')
        good = msbt_blob(LETTER_PANES + ['T_Plus_00'])
        for what, blob in (('not MsgStdBn', msbt_blob(LETTER_PANES, magic=b'MsgPrjBn')),
                           ('big endian', good[:8] + b'\xfe\xff' + good[10:]),
                           ('14 labels', msbt_blob(LETTER_PANES[:14] + ['T_Plus_00'])),
                           ('16 labels', msbt_blob(LETTER_PANES + ['T_A_00'])),
                           ('a long text', msbt_blob(LETTER_PANES + ['T_Plus_00'], long_text='T_X_02')),
                           ('cut short', good[:0x60]), ('no sections', good[:0x20])):
            out, notes = tca.popup_messages(blob, controls, 'xbox')
            self.assertEqual(out, blob, what)
            self.assertEqual(len(notes), 1, what)
            self.assertTrue(notes[0].startswith('action popup messages: ') and notes[0].endswith('; the Switch letters stay'), (what, notes))

    def test_redraw_tilted_on_a_minimal_bntx(self):
        blob = build_bntx(tilted_facts())
        controls = pl.prompt_controls('', 'position')
        out, drawn, notes = tca.redraw_tilted(blob, controls, 'xbox', None)
        new = tca.bf.bntx_textures(out)
        at = {tca.bf.Texture(out, m).name: m for m in range(len(out)) if out[m:m + 4] == b'BRTI'}
        self.assertEqual((len(out), notes, drawn), (len(blob), [], [p for p, _ in tca.TILT_PICTURES.values()]))
        for name, t in new.items():
            image = out[t.offset:t.offset + t.image_size]
            self.assertTrue(any(image), name)
            if name in [p for p, _ in tca.TILT_PICTURES.values()]:
                self.assertEqual(struct.unpack_from('<I', out, at[name] + 0x1C)[0], tca.BC5_UNORM)
                self.assertEqual(out[at[name] + 0x58:at[name] + 0x5c], tca.GREY_ALPHA)
            else:
                self.assertEqual(out[at[name]:at[name] + 0x100], blob[at[name]:at[name] + 0x100], name)
        bad = build_bntx(tilted_facts(wide=93))
        out, drawn, notes = tca.redraw_tilted(bad, controls, 'xbox', None)
        self.assertEqual(len(drawn), 2)
        self.assertEqual(len(notes), 1)
        self.assertIn('grip', notes[0])
        old, new = tca.bf.bntx_textures(bad), tca.bf.bntx_textures(out)
        for name in (*tca.TILT_PICTURES['grip'], *[f'{stem}_{part}^s' for stem, _, _, _ in tca.MASKS['grip'].values() for part in ('00', 'Line')]):
            self.assertEqual(out[new[name].offset:new[name].offset + new[name].image_size], bad[old[name].offset:old[name].offset + old[name].image_size], name)

    def test_dpad_masks(self):
        facts = {name: (tca.BC4_UNORM, 64, 64, 1, 2048, tca.MASK_SELECTOR) for name in tca.DPAD_MASKS}
        blob = build_bntx(facts)
        out, notes = tca.dpad_masks(blob, 'xbox', None, drawer=(1000, square))
        new = tca.bf.bntx_textures(out)
        self.assertEqual((len(out), notes), (len(blob), []))
        for name in tca.DPAD_MASKS:
            self.assertTrue(any(out[new[name].offset:new[name].offset + 2048]), name)
        at = [m for m in range(len(out)) if out[m:m + 4] == b'BRTI']
        self.assertTrue(all(out[m:m + 0x100] == blob[m:m + 0x100] for m in at))
        facts[tca.DPAD_MASKS[1]] = (tca.BC4_UNORM, 64, 32, 1, 2048, tca.MASK_SELECTOR)
        blob = build_bntx(facts)
        out, notes = tca.dpad_masks(blob, 'xbox', None, drawer=(1000, square))
        self.assertEqual((out, len(notes)), (blob, 1))


class Tool(unittest.TestCase):
    def test_the_tool_calls_redraw_pictures(self):
        source = (ROOT / 'games/totk/scripts/make_button_icons.py').read_text()
        for name in ('redraw_pictures', 'redraw_tilted', 'dpad_masks', 'popup_letters', 'popup_places', 'popup_messages'):
            self.assertIn(name, source)


if __name__ == '__main__':
    unittest.main()
