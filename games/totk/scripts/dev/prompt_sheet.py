#!/usr/bin/env python3
"""Check a button icon set made with make_button_icons.py --our-art against the game's own glyphs and masks, and draw a sheet.

usage: prompt_sheet.py --romfs R --mod SET --style xbox|playstation|playstation-label [--face-buttons position|label]
                       [--controls controls.txt] --out sheet.png

Reads the original and the set's External.bfotf (Font/Font.Nin_NX_NVN.bfarc.zs) and the eight Nt_KeyTex masks, plans as the tool
does, and prints ok or FAIL for each item (exit 1 on any FAIL): every glyph keeps its advance; a planned glyph's ink stays in
its target box (2 units; a stick press 4, the raster's step), meets both ends of one axis and is centred; an unplanned glyph
keeps outline and metrics; a planned mask's bright box stays in the game's box (1 px), meets both ends of one axis and is centred;
the LSt and unplanned masks keep their pixels. The PNG shows the game's glyph (grey), ours (white), the target box (red), x 0
and the advance (blue), the baseline (green). Never prints a font key. Game data stays in the folder you name.
"""
import argparse
import struct
import sys
import zlib
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent))
import compression.zstd as zstd                                                    # noqa: E402
import make_button_icons as mbi                                                    # noqa: E402
import totk_prompt_art as tpa                                                      # noqa: E402
import botw_formats as bf                                                          # noqa: E402
import botw_prompts as bp                                                          # noqa: E402
from make_mod_manifest import read_index                                           # noqa: E402
from prompt_layout import describe, prompt_controls                                # noqa: E402
from fontTools.pens.boundsPen import BoundsPen                                     # noqa: E402
from fontTools.pens.recordingPen import RecordingPen                               # noqa: E402
from fontTools.ttLib import TTFont                                                 # noqa: E402
import io                                                                          # noqa: E402

ARCHIVE = mbi.ARCHIVE
GLYPHS = [0xE040, 0xE041, 0xE042, 0xE043, 0xE044, 0xE045, 0xE046, 0xE047, 0xE04B, 0xE04C, 0xE056, 0xE057, 0xE049,
          0xE060, 0xE061, 0xE062, 0xE063, 0xE064, 0xE065, 0xE052, 0xE05E]
MASKS = [*tpa.HUD_FACE.values(), *tpa.HUD_SHAPE.values(), *tpa.HUD_KEEP]
SLOT_OF = {cp: slot for table in (tpa.FACE, tpa.SHAPE, tpa.STICK, tpa.MENU) for slot, cp in table.items()}
GROUND, GREY = 24, 110
failures = 0


def report(ok, text):
    global failures
    failures += not ok
    print(('ok   ' if ok else 'FAIL ') + text)


def decode(raw, dictionaries):
    return zstd.decompress(raw, zstd_dict=dictionaries[zstd.get_frame_info(raw).dictionary_id])


def font_of(archive):
    members = mbi.sarc_members(archive)
    return TTFont(io.BytesIO(mbi.bfotf_decode(archive[slice(*members[mbi.FONT])])[0]))   # the key it derives is dropped


def textures_of(archive):
    members = mbi.sarc_members(archive)
    bntx = archive[slice(*members[mbi.BNTX])]
    return bntx, {t.name.split('^')[0]: t for t in bf.bntx_textures(bntx).values()}


def outline(glyphset, name):
    rec = RecordingPen(); glyphset[name].draw(rec); return rec.value


def bounds_of(glyphset, name):
    pen = BoundsPen(glyphset); glyphset[name].draw(pen); return pen.bounds


def placed(ink, box, tol):
    """(inside the box, meets both ends of one axis, centred), all within tol."""
    inside = ink[0] >= box[0] - tol and ink[1] >= box[1] - tol and ink[2] <= box[2] + tol and ink[3] <= box[3] + tol
    meets = (abs(ink[0] - box[0]) <= tol and abs(ink[2] - box[2]) <= tol) or (abs(ink[1] - box[1]) <= tol and abs(ink[3] - box[3]) <= tol)
    centred = abs((ink[0] + ink[2]) / 2 - (box[0] + box[2]) / 2) <= tol and abs((ink[1] + ink[3]) / 2 - (box[1] + box[3]) / 2) <= tol
    return inside, meets, centred


def label_of(cp, line):
    if cp in tpa.DPAD:
        return f'd-pad {cp:04X}'
    if cp in SLOT_OF:
        return line.replace(' <-', f' {cp:04X} <-', 1)
    return f'L stick {cp:04X} kept'


# ---- the picture ----
class Canvas:
    def __init__(self, width, height):
        self.w, self.h, self.px = width, height, bytearray(bytes([GROUND]) * (width * height * 3))

    def blend(self, x, y, colour, alpha):
        if 0 <= x < self.w and 0 <= y < self.h and alpha > 0:
            i = (y * self.w + x) * 3
            for c in range(3):
                self.px[i + c] = round(self.px[i + c] * (1 - alpha) + colour[c] * alpha)

    def cover(self, rows, x0, y0, colour, alpha, scale=1):
        for y, row in enumerate(rows):
            for x, v in enumerate(row):
                if v:
                    for dy in range(scale):
                        for dx in range(scale):
                            self.blend(x0 + x * scale + dx, y0 + y * scale + dy, colour, alpha * v / 255)

    def rect(self, x0, y0, x1, y1, colour):
        for x in range(round(x0), round(x1) + 1):
            self.blend(x, round(y0), colour, 1); self.blend(x, round(y1), colour, 1)
        for y in range(round(y0), round(y1) + 1):
            self.blend(round(x0), y, colour, 1); self.blend(round(x1), y, colour, 1)

    def save(self, path):
        raw = b''.join(b'\0' + bytes(self.px[y * self.w * 3:(y + 1) * self.w * 3]) for y in range(self.h))
        def chunk(tag, data):
            return struct.pack('>I', len(data)) + tag + data + struct.pack('>I', zlib.crc32(tag + data))
        Path(path).write_bytes(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', self.w, self.h, 8, 2, 0, 0, 0))
                               + chunk(b'IDAT', zlib.compress(raw)) + chunk(b'IEND', b''))


def glyph_cell(canvas, x0, y0, old_set, new_set, old_name, new_name, box, advance):
    def raster(glyphs, name):
        contours = [[((x + 120) * 0.2, (900 - y) * 0.2) for x, y in c] for c in bf.flatten(outline(glyphs, name))]
        return bf.coverage_rect(contours, 240, 240, 4)
    canvas.cover(raster(old_set, old_name), x0, y0, (GREY,) * 3, 1)
    canvas.cover(raster(new_set, new_name), x0, y0, (255, 255, 255), 0.6)
    if box:
        canvas.rect(x0 + (box[0] + 120) * 0.2, y0 + (900 - box[3]) * 0.2, x0 + (box[2] + 120) * 0.2, y0 + (900 - box[1]) * 0.2, (255, 0, 0))
    for x in (0, advance):
        canvas.rect(x0 + (x + 120) * 0.2, y0, x0 + (x + 120) * 0.2, y0 + 239, (60, 60, 255))
    canvas.rect(x0, y0 + 180, x0 + 239, y0 + 180, (0, 200, 0))


def mask_cell(canvas, x0, y0, old, new, box):
    canvas.cover(old, x0, y0, (GREY,) * 3, 1, 2)
    canvas.cover(new, x0, y0, (255, 255, 255), 0.6, 2)
    if box:
        canvas.rect(x0 + box[0] * 2, y0 + box[1] * 2, x0 + box[2] * 2, y0 + box[3] * 2, (255, 0, 0))


def main():
    parser = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    parser.add_argument('--romfs', type=Path, required=True)
    parser.add_argument('--mod', type=Path, required=True)
    parser.add_argument('--style', choices=sorted(tpa.ART), required=True)
    parser.add_argument('--face-buttons', choices=('position', 'label'))
    parser.add_argument('--controls', type=Path)
    parser.add_argument('--out', type=Path, required=True)
    args = parser.parse_args()
    face = args.face_buttons or ('label' if args.style == 'playstation-label' else 'position')
    text = args.controls.read_text() if args.controls else ''
    controls = prompt_controls(text, face)
    brand = 'playstation' if args.style.startswith('playstation') else 'xbox'

    index = read_index(args.romfs / 'runtime-index.bin')
    with (args.romfs / 'romfs.bin').open('rb') as image:
        def read(path):
            offset, size = index[path]; image.seek(offset); return image.read(size)
        pack = zstd.decompress(read('Pack/ZsDic.pack.zs'))
        dictionaries = {}
        for lo, hi in mbi.sarc_members(pack).values():
            d = zstd.ZstdDict(pack[lo:hi]); dictionaries[d.dict_id] = d
        layout = next(p for p in index if p.startswith(mbi.LAYOUT_PREFIX) and p.endswith('.blarc.zs'))
        old_font, old_hud = font_of(decode(read(ARCHIVE), dictionaries)), textures_of(decode(read(layout), dictionaries))
    new_font = font_of(decode((args.mod / ARCHIVE).read_bytes(), dictionaries))
    new_hud = textures_of(decode((args.mod / layout).read_bytes(), dictionaries))

    cmap, old_set, new_set = old_font.getBestCmap(), old_font.getGlyphSet(), new_font.getGlyphSet()
    plan, _ = tpa.glyph_plan(controls, args.style)
    lines = dict(zip(controls, describe(text, face, brand)))      # slot -> `Switch A <- Xbox Y`

    for cp in sorted(c for c in cmap if 0xE040 <= c <= 0xE069):
        name = cmap[cp]
        report(old_font['hmtx'][name][0] == new_font['hmtx'][name][0], f'advance {cp:04X} {old_font["hmtx"][name][0]}')
    for cp in sorted(cmap):
        if cp in plan:
            continue
        name = cmap[cp]
        same = outline(old_set, name) == outline(new_set, name) and old_font['hmtx'][name] == new_font['hmtx'][name]
        report(same, f'unplanned {cp:04X} keeps outline and metrics')
    for cp, (source, kind) in sorted(plan.items()):
        name = cmap[cp]
        box = tpa.glyph_box(old_set, name, kind)
        ink = tpa.glyph_box(new_set, name, kind) if kind == 'stick' else bounds_of(new_set, name)
        flags = placed(ink, box, 4 if kind == 'stick' else 2)
        report(all(flags), f'glyph {cp:04X} {kind} ink {tuple(round(v, 1) for v in ink)} box {tuple(round(v, 1) for v in box)}'
               f' (inside, meets, centred = {flags})')

    old_bntx, old_t = old_hud
    new_bntx, new_t = new_hud
    hud, _ = tpa.hud_plan(controls, args.style)
    boxes = {}
    for name in MASKS:
        old, new = bf.r8_read(old_bntx, old_t[name]), bf.r8_read(new_bntx, new_t[name])
        if name in hud:
            box = (bp.disc_box if hud[name][1] == 'disc' else bp.bright_box)(old)
            boxes[name] = box
            ink = bp.bright_box(new)
            flags = placed(ink, box, 1)
            report(all(flags), f'mask {name} ink {ink} box {tuple(round(v, 1) for v in box)} (inside, meets, centred = {flags})')
        else:
            report(old == new, f'mask {name} keeps its pixels')

    cells = [('glyph', cp) for cp in GLYPHS] + [('mask', name) for name in MASKS]
    gap, label, cell = 8, 28, 240
    rows_n = (len(cells) + 5) // 6
    canvas = Canvas(6 * cell + 5 * gap, rows_n * (cell + label + gap))
    letters = bp.Letters(TTFont(io.BytesIO(bp.bfttf_decode(next(args.romfs.parent.parent.glob('firmware/shared-fonts/0-standard.bfttf')).read_bytes()))))
    for i, (kind, item) in enumerate(cells):
        x0, y0 = (i % 6) * (cell + gap), (i // 6) * (cell + label + gap)
        if kind == 'glyph':
            cp = item
            box = tpa.glyph_box(old_set, cmap[cp], plan[cp][1]) if cp in plan else None
            glyph_cell(canvas, x0, y0, old_set, new_set, cmap[cp], cmap[cp], box, old_font['hmtx'][cmap[cp]][0])
            words = label_of(cp, lines.get(SLOT_OF.get(cp), ''))
        else:
            old, new = bf.r8_read(old_bntx, old_t[item]), bf.r8_read(new_bntx, new_t[item])
            mask_cell(canvas, x0 + 12, y0 + 12, old, new, boxes.get(item))
            slot = next((s for s, n in {**tpa.HUD_FACE, **tpa.HUD_SHAPE}.items() if n == item), None)
            words = f'mask {item.removeprefix("Nt_KeyTex")} ' + (lines[slot].removeprefix('Switch ') if item in hud else 'kept')
        words = ''.join(ch if 32 <= ord(ch) < 127 else '?' for ch in words)
        contours = letters.text(words, 12, x0 + cell / 2, y0 + cell + 14, cell - 4)
        canvas.cover(bf.coverage_rect([[(x - x0, y - y0 - cell) for x, y in c] for c in contours], cell, label, 4), x0, y0 + cell, (255, 255, 255), 1)
    args.out.parent.mkdir(parents=True, exist_ok=True)
    canvas.save(args.out)
    print(f'{args.out}: {len(cells)} cells, {failures} FAIL')
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
