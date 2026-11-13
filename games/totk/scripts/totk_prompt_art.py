"""Our Breath of the Wild controller art (botw_prompts.py: the make_button_icons.py drawings with the Switch system font's
letters) fitted on Tears of the Kingdom's own button glyphs and button guide masks: make_button_icons.py --our-art.

A glyph of scft/External.bfotf (U+E040 to U+E069) is redrawn on the box of the game's own outline: the disc of A B X Y, + -
and the d-pad (a square on the centre of the outline's bounds), the bounds of L R ZL ZR, the body of a stick press. Its
advance stays the game's. The eight button guide masks (Nt_KeyTex*, R8 108x108) are redrawn on the bright box of the game's
mask. A stick press is the game's own outline (the stick pressed), swapped when controls.txt swaps the sticks. The glyphs of
a stick being moved and every glyph that nothing here plans stay as they are. Needs the Switch system font
(extracted/firmware/shared-fonts/0-standard.bfttf), which the app holds.
"""
import io
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'botw' / 'scripts'))
import botw_formats as bf          # noqa: E402
import botw_prompts as bp          # noqa: E402
from fontTools.pens.boundsPen import BoundsPen           # noqa: E402
from fontTools.pens.recordingPen import RecordingPen     # noqa: E402
from fontTools.pens.transformPen import TransformPen     # noqa: E402
from fontTools.ttLib import TTFont                       # noqa: E402

ART = {'xbox': 'xbox', 'playstation': 'playstation', 'playstation-label': 'playstation'}
SYSTEM_FONT = 'shared-fonts/0-standard.bfttf'
FACE = {'a': 0xE040, 'b': 0xE041, 'x': 0xE042, 'y': 0xE043}
SHAPE = {'l': 0xE044, 'r': 0xE045, 'zl': 0xE046, 'zr': 0xE047}
STICK = {'lstick': 0xE056, 'rstick': 0xE057}
MENU = {'plus': 0xE04B, 'minus': 0xE04C}
DPAD = bp.DPAD_GLYPHS
OWN = {'l3': 0xE056, 'r3': 0xE057}      # the game's own stick press glyphs, by the controller button they show
HUD_FACE = {slot: f'Nt_KeyTex{slot.upper()}_00' for slot in 'abxy'}
HUD_SHAPE = {'l': 'Nt_KeyTexL_00', 'zl': 'Nt_KeyTexZL_00'}
HUD_KEEP = ('Nt_KeyTexLSt_00', 'Nt_KeyTexLSt_01')       # the moved stick masks stay the game's


def letters(firmware):
    """The Switch system font's outlines (bp.Letters) from the firmware folder."""
    return bp.Letters(TTFont(io.BytesIO(bp.bfttf_decode((Path(firmware) / SYSTEM_FONT).read_bytes()))))


def glyph_plan(controls, style):
    """({code point: (source, kind)}, notes): source is a controller button (a key of bp.drawers_of) or ('own', code point of
    the game's own glyph); kind is 'disc', 'key' or 'stick' (how the target box is found). The rules of botw_prompts.font_cells."""
    text = bp.style_parts(ART[style])[0]
    plan, notes = {}, []
    for slot, cp in FACE.items():
        pad = controls[slot]
        if pad in FACE:
            plan[cp] = (pad, 'disc')
        else:
            notes.append(f'{slot.upper()}: {pad or "nothing"} has no face glyph; the Switch glyph stays')
    for slot, cp in SHAPE.items():
        pad = controls[slot]
        if pad in OWN:
            plan[cp] = (('own', OWN[pad]), 'key')
        elif text.get(pad):
            plan[cp] = (pad, 'key')
        else:
            notes.append(f'{slot.upper()}: {pad or "nothing"} has no letter here; the Switch glyph stays')
    for slot, cp in STICK.items():
        pad = controls[slot]
        if pad in OWN:
            if OWN[pad] != cp:
                plan[cp] = (('own', OWN[pad]), 'stick')
        elif text.get(pad):
            plan[cp] = (pad, 'stick')
        else:
            notes.append(f'{slot}: {pad or "nothing"} has no picture here; the Switch glyph stays')
    for cp in DPAD:
        plan[cp] = (cp, 'disc')
    for slot, cp in MENU.items():
        pad = controls[slot]
        if pad in ('menu', 'view'):
            plan[cp] = (pad, 'disc')
        else:
            notes.append(f'{slot}: {pad or "nothing"} has no picture here; the Switch glyph stays')
    return plan, notes


def hud_plan(controls, style):
    """({texture (no ^d): (controller button, kind)}, notes) for the button guide masks."""
    text = bp.style_parts(ART[style])[0]
    plan, notes = {}, []
    for slot, name in HUD_FACE.items():
        pad = controls[slot]
        if pad in FACE:
            plan[name] = (pad, 'disc')
        else:
            notes.append(f'{slot.upper()}: {pad or "nothing"} has no face picture; the Switch mask stays')
    for slot, name in HUD_SHAPE.items():
        pad = controls[slot]
        if pad not in OWN and text.get(pad):
            plan[name] = (pad, 'key')
        else:
            notes.append(f'{slot.upper()}: {pad or "nothing"} has no letter here; the Switch mask stays')
    return plan, notes


def disc(b):
    """The square of side (w + h) / 2 on the centre of bounds `b` (x0, y0, x1, y1)."""
    cx, cy, half = (b[0] + b[2]) / 2, (b[1] + b[3]) / 2, ((b[2] - b[0]) + (b[3] - b[1])) / 4
    return cx - half, cy - half, cx + half, cy + half


def fit(ink, box):
    """The matrix (k, 0, 0, k, dx, dy) that makes `ink` as large as fits in `box` with its shape kept, on the box's centre."""
    k = min((box[2] - box[0]) / (ink[2] - ink[0]), (box[3] - box[1]) / (ink[3] - ink[1]))
    return k, 0, 0, k, (box[0] + box[2]) / 2 - k * (ink[0] + ink[2]) / 2, (box[1] + box[3]) / 2 - k * (ink[1] + ink[3]) / 2


def glyph_box(glyphset, name, kind):
    """The box of a glyph's ink in font units: 'key' its bounds, 'disc' the square of disc(), 'stick' the body of a stick
    press (botw_prompts.stick_body on a raster of 4 units a pixel)."""
    pen = BoundsPen(glyphset); glyphset[name].draw(pen)
    x0, y0, x1, y1 = pen.bounds
    if kind == 'key':
        return pen.bounds
    if kind == 'disc':
        return disc(pen.bounds)
    rec = RecordingPen(); glyphset[name].draw(rec)
    contours = [[((x - x0) / 4 + 1, (y1 - y) / 4 + 1) for x, y in c] for c in bf.flatten(rec.value)]
    rows = bf.coverage_rect(contours, int((x1 - x0) / 4) + 2, int((y1 - y0) / 4) + 2, 4)
    a, t, c, e = bp.stick_body(rows)
    return x0 + (a - 1) * 4, y1 - (e - 1) * 4, x0 + (c - 1) * 4, y1 - (t - 1) * 4


def font_icons(font, controls, style, letters):
    """({code point: (the game's advance, draw)}, notes) for the planned glyphs of `font` (the External.bfotf TTFont). Call it
    before any charstring of the font is replaced: a glyph taken from the game's own is recorded here."""
    plan, notes = glyph_plan(controls, style)
    drawers = bp.drawers_of(ART[style], letters)
    cmap, glyphs, icons = font.getBestCmap(), font.getGlyphSet(), {}
    for cp, (source, kind) in plan.items():
        target = glyph_box(glyphs, cmap[cp], kind)
        rec = RecordingPen()
        if isinstance(source, tuple):
            glyphs[cmap[source[1]]].draw(rec)
            ink = glyph_box(glyphs, cmap[source[1]], kind)
        else:
            drawers[source][1](rec)
            bounds = BoundsPen(None); rec.replay(bounds)
            ink = bounds.bounds
        matrix = fit(ink, target)
        icons[cp] = (font['hmtx'][cmap[cp]][0], lambda pen, rec=rec, matrix=matrix: rec.replay(TransformPen(pen, matrix)))
    return icons, notes


def hud_textures(bntx, controls, style, letters):
    """(the BNTX bytes, same length, with the planned masks redrawn, notes). Each is fitted on the bright box of the game's own
    mask (the disc box of A B X Y). The LSt masks and a mask nothing plans keep their pixels."""
    found = {t.name.split('^')[0]: t for t in bf.bntx_textures(bntx).values()}
    wanted = [*HUD_FACE.values(), *HUD_SHAPE.values(), *HUD_KEEP]
    missing = [name for name in wanted if name not in found]
    if missing:
        raise ValueError('not in the layout archive: ' + ', '.join(missing))
    plan, notes = hud_plan(controls, style)
    drawers = bp.drawers_of(ART[style], letters)
    out = bytearray(bntx)
    for name, (pad, kind) in plan.items():
        t = found[name]
        bf.check_r8(t)
        if t.width != t.height:
            raise ValueError(f'{name}: {t.width}x{t.height} pixels, not square')
        old = bf.r8_read(bntx, t)
        box = bp.disc_box(old) if kind == 'disc' else bp.bright_box(old)
        rows = bp.render(drawers[pad], t.width, box)
        image = bf.r8_image(bntx, t, rows)
        out[t.offset:t.offset + len(image)] = image
    return bytes(out), notes
