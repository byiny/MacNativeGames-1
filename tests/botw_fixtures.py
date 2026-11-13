"""Builders and readers that the Breath of the Wild prompt tests share: made-up Yaz0, SARC, BNTX and BFFNT data (no game data) and
the test-only pixel helpers that botw_formats.py does not need."""
import random
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT/'games/botw/scripts'))
sys.path.insert(0, str(ROOT/'games/totk/scripts'))
import botw_formats as bf       # noqa: E402  (make_romfs_patches needs Python 3.14: compression.zstd)


def yaz0_encode(data):
    """A plain greedy Yaz0 encoder (back references up to 4096 bytes), enough to test the splice against references."""
    out, i, n, seen = bytearray(b'Yaz0' + struct.pack('>I', len(data)) + bytes(8)), 0, len(data), {}
    while i < n:
        code, body = 0, bytearray()
        for bit in range(8):
            if i >= n:
                break
            best, dist = 0, 0
            for j in reversed(seen.get(data[i:i + 3], [])[-16:]):
                if i - j > 4096:
                    break
                length = 3
                while i + length < n and length < 0x111 and data[j + length] == data[i + length]:
                    length += 1
                if length > best:
                    best, dist = length, i - j
            step = 1
            if best >= 3:
                d = dist - 1
                if best < 18:
                    body += bytes([(best - 2) << 4 | d >> 8, d & 255])
                else:
                    body += bytes([d >> 8, d & 255, best - 18])
                step = best
            else:
                code |= 0x80 >> bit; body.append(data[i])
            for k in range(i, i + step):
                seen.setdefault(data[k:k + 3], []).append(k)
            i += step
        out.append(code); out += body
    return bytes(out)


def sample(size, seed=1):
    rng = random.Random(seed)
    words = [bytes(rng.randrange(256) for _ in range(rng.randrange(3, 40))) for _ in range(30)]
    out = bytearray()
    while len(out) < size:
        out += rng.choice(words) if rng.random() < 0.8 else bytes([rng.randrange(256)])
    return bytes(out[:size])


def sarc(members):
    """A little-endian SARC (the layout make_romfs_patches.sarc_members reads), members 8-byte aligned."""
    names, table, data = bytearray(), bytearray(), bytearray()
    for name, blob in members.items():
        while len(data) % 8:
            data += b'\0'
        table += struct.pack('<4I', 0, 0x01000000 | len(names) // 4, len(data), len(data) + len(blob))
        names += name.encode() + b'\0'
        while len(names) % 4:
            names += b'\0'
        data += blob
    head = b'SFAT' + struct.pack('<HHI', 12, len(members), 0x65) + bytes(table) + b'SFNT' + struct.pack('<HH', 8, 0) + bytes(names)
    start = 0x14 + len(head); start += -start % 8
    out = bytearray(b'SARC' + struct.pack('<H', 0x14) + b'\xff\xfe' + struct.pack('<IIHH', 0, start, 0x100, 0) + head)
    out += b'\0' * (start - len(out)) + data
    struct.pack_into('<I', out, 8, len(out))
    return bytes(out)


def bntx(textures, fmt=bf.R8_UNORM, size=48):
    """A BNTX holding one BRTI per name, each a size x size R8 block-linear texture (block height 8, 4096 bytes), as in the game."""
    return rg8_bntx([(name, fmt, size, size, 3, rows, None) for name, rows in textures.items()])


def bc4_bntx(width, height, block_height, rows):
    """A BNTX with one BC4 block-linear texture holding `rows` (a list of bytearray rows), as inside a BFFNT's glyph sheet."""
    blocks_w, blocks_h = width // 4, height // 4
    gobs = -(-blocks_h // (8 * block_height)) * block_height * (width * 2 // 64)
    image = bytearray(gobs * 512)
    for by in range(blocks_h):
        for bx in range(blocks_w):
            at = bf.gob_address(bx, by, blocks_w, 8, block_height)
            image[at:at + 8] = bf.bc4_encode([rows[by * 4 + i // 4][bx * 4 + i % 4] for i in range(16)])
    head = bytearray(b'BNTX' + bytes(0x1fc)); name = struct.pack('<H', 1) + b'x' + bytes(5)
    entry = bytearray(0xa0); entry[:4] = b'BRTI'
    struct.pack_into('<HHHIIIIIIII', entry, 0x12, 0, 0, 1, 1, bf.BC4_UNORM, 0, width, height, 1, 1, block_height.bit_length() - 1)
    struct.pack_into('<I', entry, 0x50, len(image))
    name_at, brti_at = len(head), len(head) + len(name)
    pointers_at = brti_at + len(entry); data_at = pointers_at + 8
    struct.pack_into('<Q', entry, 0x60, name_at); struct.pack_into('<Q', entry, 0x70, pointers_at)
    return bytes(head + name + entry + struct.pack('<Q', data_at) + image)


def bffnt(rows, columns=5, grid_rows=4, cell=50, block_height=1, codes=(0xE040, 0xE041, 0xE042, 0xE043), cell_height=None):
    """A BFFNT: header, TGLP with the sheet (a BNTX) and a CMAP table starting at 0xE040 (glyph i = code 0xE040 + i). Cells are
    cell x cell_height pixels (default square)."""
    cell_height = cell_height or cell
    sheet = bc4_bntx(256, grid_rows * (cell_height + 1) + 4, block_height, rows)
    tglp_at = 0x14
    sheet_at = 0x1000
    tglp = b'TGLP' + struct.pack('<I', sheet_at + len(sheet) - tglp_at) + struct.pack('<BBBBIHHHHHHI', cell, cell_height, 1, cell, len(sheet), 36, 12, columns, grid_rows, 256, grid_rows * (cell_height + 1) + 4, sheet_at)
    table = struct.pack('<%dH' % len(codes), *range(len(codes)))
    cmap = b'CMAP' + struct.pack('<I', 24 + len(table)) + struct.pack('<IIHHI', codes[0], codes[-1], 1, 0, 0) + table
    out = bytearray(b'FFNT' + b'\xff\xfe' + struct.pack('<H', 0x14) + bytes(4) + bytes(4) + struct.pack('<H', 2) + bytes(2))
    body = bytearray(out) + bytearray(tglp.ljust(sheet_at - len(out), b'\0')) + bytearray(sheet)
    cmap_at = len(body); body += cmap
    # the sections are found by walking sizes: TGLP's size reaches the CMAP
    struct.pack_into('<I', body, tglp_at + 4, cmap_at - tglp_at)
    return bytes(body)


def full_bntx(textures):
    """A BNTX laid out as the game's: header (relocation table offset and file size at 0x18), an NX block pointing at the BRTD,
    one BRTI per texture with its data pointer, the BRTD holding the images (0x200 aligned), and an _RLT with two sections
    (the headers, then the BRTD) at the end, 0x1000 aligned. `textures`: name -> (format, width, height, image bytes, block
    height)."""
    out = bytearray(b'BNTX' + bytes(0x1c)) + b'NX  ' + bytes(0x1c)
    names, brtis = {}, []
    for name in textures:
        names[name] = len(out); raw = name.encode()
        out += struct.pack('<H', len(raw)) + raw + bytes(-(2 + len(raw)) % 8)
    for name, (fmt, w, h, image, bh) in textures.items():
        brti = len(out); entry = bytearray(0xa0); entry[:4] = b'BRTI'
        struct.pack_into('<HHHIIIIIIII', entry, 0x12, 0, 0, 1, 1, fmt, 0, w, h, 1, 1, bh.bit_length() - 1 | 0x10000)
        struct.pack_into('<I', entry, 0x50, len(image)); struct.pack_into('<Q', entry, 0x60, names[name])
        out += entry; brtis.append(brti)
    pointers = len(out); out += bytes(8 * len(textures))
    out += bytes(-(len(out) + 16) % 0x200)
    brtd = len(out); out += b'BRTD' + bytes(12)
    for i, (name, (_, _, _, image, _)) in enumerate(textures.items()):
        out += bytes(-len(out) % 0x200)
        struct.pack_into('<Q', out, pointers + 8 * i, len(out)); struct.pack_into('<Q', out, brtis[i] + 0x70, pointers + 8 * i)
        out += image
    out += bytes(-len(out) % 0x1000)
    rlt = len(out)
    struct.pack_into('<Q', out, brtd + 8, rlt - brtd); struct.pack_into('<Q', out, 0x30, brtd)
    out += b'_RLT' + struct.pack('<II', rlt, 2) + bytes(4)
    out += struct.pack('<QIIII', 0, 0, brtd, 0, 1) + struct.pack('<QIIII', 0, brtd, rlt - brtd, 1, 1)
    out += struct.pack('<IHBB', 0x30, 1, 1, 0) + struct.pack('<IHBB', pointers, len(textures), 1, 0)
    struct.pack_into('<II', out, 0x18, rlt, len(out))
    return bytes(out)


def font(sheet_rows, codes=(0xE040, 0xE041), cell=(50, 50), grid=(5, 4)):
    """A BFFNT: FINF (pointers to the TGLP, CWDH and CMAP data), TGLP with a full BNTX sheet at 0x1000, then CWDH and CMAP. Cells
    of cell (width, height) pixels in a grid of (columns, rows)."""
    image, bh = bf.bc4_pack(sheet_rows)
    sheet = full_bntx({'sheet': (bf.BC4_UNORM, len(sheet_rows[0]), len(sheet_rows), image, bh)})
    out = bytearray(b'FFNT\xff\xfe' + struct.pack('<HIIHH', 0x14, 0x04010000, 0, 4, 0))
    finf = len(out); out += b'FINF' + struct.pack('<I', 0x20) + bytes(0x18)
    tglp = len(out); out += b'TGLP' + bytes(4) + struct.pack('<BBBBIHHHHHHI', *cell, 1, 48, len(sheet), 36, 12, *grid, len(sheet_rows[0]), len(sheet_rows), 0x1000)
    out += bytes(0x1000 - len(out)) + sheet
    cwdh = len(out); widths = bytes(3 * len(codes)) + bytes(-(16 + 3 * len(codes)) % 4)
    out += b'CWDH' + struct.pack('<IHHI', 16 + len(widths), 0, len(codes) - 1, 0) + widths
    cmap = len(out); table = struct.pack(f'<{len(codes)}H', *range(len(codes)))
    out += b'CMAP' + struct.pack('<IIIHHI', 24 + len(table), codes[0], codes[-1], 1, 0, 0) + table
    struct.pack_into('<I', out, tglp + 4, cwdh - tglp); struct.pack_into('<III', out, finf + 20, tglp + 8, cwdh + 8, cmap + 8)
    struct.pack_into('<I', out, 0x0c, len(out))
    return bytes(out)


def rg8_bntx(specs):
    """A BNTX with one R8G8 or R8 block-linear texture for each (name, format, width, height, log2 of the block height, grey rows,
    alpha rows or None for an R8 texture), laid out as the game's are."""
    head = bytearray(b'BNTX' + bytes(0x1fc)); names = bytearray(); brti = bytearray(); pointers = bytearray(); images = []
    base = len(head)
    for name, fmt, width, height, log2, grey, alpha in specs:
        element = 2 if alpha is not None else 1
        raw = name.encode()
        name_at = base + len(names); names += struct.pack('<H', len(raw)) + raw + b'\0' * (-(2 + len(raw)) % 8)
        block = 1 << log2
        gobs_wide = -(-width * element // 64)
        size = -(-(-(-height // 8)) // block) * block * gobs_wide * 512
        entry = bytearray(0xa0)
        entry[:4] = b'BRTI'
        struct.pack_into('<HHHIIIIIIII', entry, 0x12, 0, 0, 1, 1, fmt, 0, width, height, 1, 1, log2)
        struct.pack_into('<I', entry, 0x50, size)
        struct.pack_into('<Q', entry, 0x60, name_at)
        brti += entry; pointers += struct.pack('<Q', 0)
        image = bytearray(size)
        for y in range(height):
            for x in range(width):
                at = bf.gob_address(x, y, width, element, block)
                image[at] = grey[y][x]
                if alpha is not None:
                    image[at + 1] = alpha[y][x]
        images.append(image)
    brti_at = base + len(names); pointers_at = brti_at + len(brti)
    out = bytearray(head + names + brti + pointers)
    for i, image in enumerate(images):
        o = brti_at + i * 0xa0
        struct.pack_into('<Q', out, o + 0x70, pointers_at + i * 8)
        struct.pack_into('<Q', out, pointers_at + i * 8, len(out))
        out += image
    return bytes(out)


def noise(width, height, seed):
    rng = random.Random(seed)
    return [bytes(rng.randrange(256) for _ in range(width)) for _ in range(height)]


def rg8_read(bntx, texture):
    """The pixels of an R8G8 texture as two pictures (grey rows, alpha rows), each a list of bytes, top row first."""
    bf.check_rg8(texture)
    base, w = texture.offset, texture.width
    at = [[base + bf.gob_address(x, y, w, 2, texture.block_height) for x in range(w)] for y in range(texture.height)]
    return [bytes(bntx[a] for a in row) for row in at], [bytes(bntx[a + 1] for a in row) for row in at]


def coverage(contours, size, oversample=8):
    """coverage_rect() over a square size x size picture."""
    return bf.coverage_rect(contours, size, size, oversample)


def enclosed_marks(rows, window=(8, 8, 40, 40), dark=0xE0, largest=400):
    """The dark marks (a letter cut out of a white shape): connected groups of pixels below `dark` that lie inside `window`
    (x0, y0, x1, y1) and have at most `largest` pixels. Returns a list of pixel-coordinate sets; the outline of a shape is
    longer than `largest` and stays out."""
    height, width = len(rows), len(rows[0])
    seen, marks = set(), []
    for y in range(height):
        for x in range(width):
            if rows[y][x] >= dark or (x, y) in seen:
                continue
            group, todo = set(), [(x, y)]
            while todo:
                px, py = todo.pop()
                if (px, py) in group or not (0 <= px < width and 0 <= py < height) or rows[py][px] >= dark:
                    continue
                group.add((px, py))
                todo += [(px + dx, py + dy) for dx in (-1, 0, 1) for dy in (-1, 0, 1) if dx or dy]
            seen |= group
            xs, ys = [p[0] for p in group], [p[1] for p in group]
            if len(group) <= largest and min(xs) >= window[0] and min(ys) >= window[1] and max(xs) < window[2] and max(ys) < window[3]:
                marks.append(group)
    return marks
