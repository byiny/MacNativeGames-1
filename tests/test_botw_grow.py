"""Growing textures, font sheets and archives for the sharp prompt icons (games/botw/scripts/botw_formats.py): a BNTX with its
texture data block and relocation table, a BFFNT inside a SARC, a Yaz0 stream that changes length, the resource size table.
Made-up data only."""
import struct
import sys
import unittest
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tests'))
sys.path.insert(0, str(ROOT / 'games/botw/scripts'))
sys.path.insert(0, str(ROOT / 'games/totk/scripts'))
import botw_formats as bf       # noqa: E402  (make_romfs_patches needs Python 3.14: compression.zstd)
from botw_fixtures import yaz0_encode, sample, sarc, full_bntx, font, noise, rg8_read      # noqa: E402


class BntxResize(unittest.TestCase):
    def setUp(self):
        self.small = {n: noise(48, 48, i) for i, n in enumerate(('a', 'b', 'c'))}
        self.bntx = full_bntx({n: (bf.R8_UNORM, 48, 48, *bf.r8_pack(rows)) for n, rows in self.small.items()})

    def grow(self, name='b', size=96, seed=9):
        rows = noise(size, size, seed)
        image, bh = bf.r8_pack(rows)
        return rows, bf.apply_edits(self.bntx, bf.bntx_resize(self.bntx, {name: (size, size, image, bh)}))

    def test_the_fixture_reads_back(self):
        textures = bf.bntx_textures(self.bntx)
        for name, rows in self.small.items():
            self.assertEqual(bf.r8_read(self.bntx, textures[name]), rows)

    def test_a_grown_texture_reads_back_and_the_others_are_unchanged(self):
        rows, out = self.grow()
        textures = bf.bntx_textures(out)
        self.assertEqual((textures['b'].width, textures['b'].height, textures['b'].block_height), (96, 96, 16))
        self.assertEqual(bf.r8_read(out, textures['b']), rows)
        for name in ('a', 'c'):
            self.assertEqual(bf.r8_read(out, textures[name]), self.small[name])

    def test_sections_pointers_and_alignment_follow(self):
        _, out = self.grow()
        rlt, size = struct.unpack_from('<II', out, 0x18)
        old_rlt = struct.unpack_from('<I', self.bntx, 0x18)[0]
        grow = rlt - old_rlt
        self.assertEqual(size, len(out)); self.assertEqual(grow % bf.GROW_ALIGN, 0); self.assertEqual(rlt % 0x1000, 0)
        self.assertEqual(out[rlt:rlt + 4], b'_RLT'); self.assertEqual(struct.unpack_from('<I', out, rlt + 4)[0], rlt)
        old = self.bntx[old_rlt:]       # the table keeps everything but its own place and the data section's size
        self.assertEqual((out[rlt + 8:rlt + 0x34], out[rlt + 0x38:]), (old[8:0x34], old[0x38:]))
        brtd = struct.unpack_from('<Q', out, 0x30)[0]
        self.assertEqual(brtd + struct.unpack_from('<Q', out, brtd + 8)[0], rlt)
        position, length = struct.unpack_from('<II', out, rlt + 0x10 + 24 + 8)
        self.assertEqual((position, position + length), (brtd, rlt))
        texture = bf.bntx_textures(out)['b']
        self.assertEqual(texture.offset % 0x200, 0)
        self.assertTrue(old_rlt <= texture.offset and texture.offset + texture.image_size <= rlt)
        self.assertEqual(texture.image_size, bf.surface_size(96, 96, 1, 16))

    def test_two_textures_grow_together(self):
        a, b = noise(96, 96, 1), noise(128, 64, 2)
        out = bf.apply_edits(self.bntx, bf.bntx_resize(self.bntx, {'a': (96, 96, *bf.r8_pack(a)), 'c': (128, 64, *bf.r8_pack(b))}))
        textures = bf.bntx_textures(out)
        self.assertEqual(bf.r8_read(out, textures['a']), a); self.assertEqual(bf.r8_read(out, textures['c']), b)
        self.assertEqual(bf.r8_read(out, textures['b']), self.small['b'])

    def test_rg8_and_bc4_pack_read_back(self):
        grey, alpha = noise(100, 60, 3), noise(100, 60, 4)
        image, bh = bf.rg8_pack(grey, alpha)
        bntx = full_bntx({'p': (bf.R8G8_UNORM, 100, 60, image, bh)})
        self.assertEqual(rg8_read(bntx, bf.bntx_textures(bntx)['p']), (grey, alpha))
        flat = [bytearray([y // 4 * 16]) * 64 for y in range(64)]       # flat 4x4 blocks: BC4 holds them exactly
        image, bh = bf.bc4_pack(flat)
        bntx = full_bntx({'s': (bf.BC4_UNORM, 64, 64, image, bh)})
        self.assertEqual(bf.bc4_read(bntx, bf.bntx_textures(bntx)['s']), flat)

    def test_a_bntx_whose_data_does_not_end_at_the_table_is_refused(self):    # negative control
        broken = bytearray(self.bntx)
        brtd = struct.unpack_from('<Q', broken, 0x30)[0]
        struct.pack_into('<Q', broken, brtd + 8, 16)
        with self.assertRaises(ValueError):
            bf.bntx_resize(bytes(broken), {'b': (96, 96, *bf.r8_pack(noise(96, 96, 1)))})


class FontSheet(unittest.TestCase):
    def test_the_sheet_grows_and_the_sections_behind_it_follow(self):
        rows = [bytearray(((x // 4) * 9 + (y // 4) * 5) % 256 for x in range(256)) for y in range(208)]
        data = font(rows)
        old = bf.Bffnt(data)
        big = [bytearray(v for v in r for _ in (0, 1)) for r in rows for _ in (0, 1)]
        new = bf.apply_edits(data, old.grow_sheet(big))
        grown = bf.Bffnt(new)
        self.assertEqual((grown.texture.width, grown.texture.height), (512, 416))
        self.assertEqual(bf.bc4_read(new[grown.sheet_at:], grown.texture), big)
        self.assertEqual(grown.cells, old.cells)
        self.assertEqual((grown.cell, grown.columns, grown.rows), (old.cell, old.columns, old.rows))     # the TGLP grid stays
        grow = len(new) - len(data)
        self.assertEqual(struct.unpack_from('<I', new, 0x0c)[0], len(new))
        tglp = grown.sections[b'TGLP']
        self.assertEqual(struct.unpack_from('<HH', new, tglp + 24), (256, 208))
        sheet_size = struct.unpack_from('<I', new, tglp + 12)[0]
        self.assertEqual(struct.unpack_from('<I', new[grown.sheet_at:], 0x1c)[0], sheet_size)
        finf = grown.sections[b'FINF']
        self.assertEqual(struct.unpack_from('<III', new, finf + 20),
                         (tglp + 8, grown.sections[b'CWDH'] + 8, grown.sections[b'CMAP'] + 8))
        self.assertEqual(new[grown.sections[b'CWDH']:], data[old.sections[b'CWDH']:])
        self.assertEqual(grown.sections[b'CMAP'] - old.sections[b'CMAP'], grow)

    def test_the_font_in_its_archive_grows_and_the_members_after_it_move(self):
        rows = [bytearray(((x // 4) * 9 + (y // 4) * 5) % 256 for x in range(256)) for y in range(208)]
        data = font(rows)
        archive = sarc({'Ancient_00.bffnt': sample(3000, 5), 'External_00.bffnt': data, 'z.bin': sample(5000, 6)})
        big = [bytearray(v for v in r for _ in (0, 1)) for r in rows for _ in (0, 1)]
        edits = bf.sarc_member_edits(archive, 'External_00.bffnt', bf.Bffnt(data).grow_sheet(big))
        out = bf.apply_edits(archive, edits)
        members = bf.sarc_members(out)
        self.assertEqual(out[slice(*members['z.bin'])], sample(5000, 6))
        self.assertEqual(out[slice(*members['Ancient_00.bffnt'])], sample(3000, 5))
        self.assertEqual(bf.Bffnt(out[slice(*members['External_00.bffnt'])]).texture.width, 512)
        stream = yaz0_encode(archive)
        self.assertEqual(bf.yaz0_decode(bf.yaz0_splice(stream, archive, edits)), out)


class GrowingStreams(unittest.TestCase):
    def test_yaz0_splice_with_an_insertion_a_removal_and_a_same_size_edit(self):
        data = sample(60000, 7)
        stream = yaz0_encode(data)
        edits = [(100, b'xyz' * 7), (20000, 0, sample(9000, 8)), (40000, 300, b'q')]
        out = bf.yaz0_splice(stream, data, edits)
        self.assertEqual(bf.yaz0_decode(out), bf.apply_edits(data, edits))
        self.assertEqual(struct.unpack_from('>I', out, 4)[0], len(bf.apply_edits(data, edits)))
        self.assertLess(len(out), len(stream) + 9000 + 4 * 4096 // 8 * 9)    # only the stretches round the edits are literals

    def test_an_insertion_at_the_end(self):
        data = sample(9000, 3)
        stream = yaz0_encode(data)
        self.assertEqual(bf.yaz0_decode(bf.yaz0_splice(stream, data, [(9000, 0, b'tail')])), data + b'tail')

    def test_overlapping_edits_are_refused(self):     # negative control
        with self.assertRaises(ValueError):
            bf.apply_edits(b'abcdef', [(1, 3, b'x'), (2, 1, b'y')])

    def test_the_resource_size_table_rises_by_the_growth(self):
        names = ['Layout/Common.blarc', 'Font/Font_US.bfarc', 'Other.bin']
        entries = sorted((zlib.crc32(n.encode()), 1000 * (i + 1)) for i, n in enumerate(names))
        table = b'RSTB' + struct.pack('<II', len(entries), 0) + b''.join(struct.pack('<II', *e) for e in entries)
        out = bf.apply_edits(table, bf.rstb_edits(table, {'Layout/Common.blarc': 0x4000, 'Font/Font_US.bfarc': 8}))
        sizes = dict(struct.unpack_from('<II', out, 12 + 8 * i) for i in range(len(entries)))
        self.assertEqual(sizes[zlib.crc32(b'Layout/Common.blarc')], 1000 + 0x4000)
        self.assertEqual(sizes[zlib.crc32(b'Font/Font_US.bfarc')], 2008)
        self.assertEqual(sizes[zlib.crc32(b'Other.bin')], 3000)
        with self.assertRaises(ValueError):       # negative control: a name the table does not hold
            bf.rstb_edits(table, {'Missing.bin': 1})


if __name__ == '__main__':
    unittest.main()
