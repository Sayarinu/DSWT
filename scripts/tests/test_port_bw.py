"""The Black/White port's cartridge writer and checks, on synthetic inputs."""
from pathlib import Path
import struct
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from retail_port import bw, nds  # noqa: E402


def cartridge(arm9_size, ntr_end, twl_start, twl_size, capacity=0):
    """A DSi-enhanced cartridge: ARM9 at 0x4000, one file after it, a DSi area at twl_start."""
    rom = bytearray(b"\xff" * (0x20000 << capacity))
    rom[:0x4000] = bytes(0x4000)
    rom[0x12], rom[0x14] = 2, capacity
    struct.pack_into("<II", rom, 0x20, 0x4000, 0x02004000)
    struct.pack_into("<II", rom, 0x28, 0x02004000, arm9_size)
    rom[0x4000:0x4000 + arm9_size] = bytes(range(256)) * (arm9_size // 256)
    after = nds.align(0x4000 + arm9_size, nds.FILE_ALIGN)
    fat = after
    struct.pack_into("<II", rom, 0x48, fat, 8)
    struct.pack_into("<II", rom, fat, after + 0x200, after + 0x210)
    rom[after + 0x200:after + 0x210] = b"file contents!!!"
    struct.pack_into("<I", rom, 0x80, ntr_end)
    struct.pack_into("<H", rom, 0x92, twl_start // bw.TWL_ALIGN)
    struct.pack_into("<I", rom, 0x1C0, twl_start + 0x100)
    struct.pack_into("<I", rom, 0x210, twl_start + twl_size)
    rom[twl_start:twl_start + twl_size] = b"\x5a" * twl_size
    return bytes(rom)


class RelayoutTests(unittest.TestCase):
    def test_the_dsi_area_stays_when_the_ds_area_still_fits(self):
        rom = cartridge(0x1000, 0x6000, 0x80000, 0x400, capacity=3)
        out = bw.relayout(rom, rom[0x4000:0x5000] + bytes(0x800))
        self.assertEqual(nds.u32(out, 0x2C), 0x1800)
        self.assertEqual(nds.u16(out, 0x92) * bw.TWL_ALIGN, 0x80000)
        self.assertEqual(out[0x80000:0x80400], b"\x5a" * 0x400)
        start, end = nds.file_span(out, 0)
        self.assertEqual(out[start:end], b"file contents!!!")
        self.assertEqual(nds.u16(out, nds.HEADER_CRC), nds.crc16(out[:nds.HEADER_CRC]))

    def test_the_dsi_area_moves_past_a_grown_ds_area(self):
        rom = cartridge(0x1000, 0x6000, 0x80000, 0x400, capacity=4)
        out = bw.relayout(rom, rom[0x4000:0x5000] + bytes(0x80000))
        self.assertEqual(nds.u16(out, 0x92) * bw.TWL_ALIGN, 0x100000)
        self.assertEqual(nds.u32(out, 0x1C0), 0x100100)
        self.assertEqual(nds.u32(out, 0x210), 0x100400)
        self.assertEqual(out[0x100000:0x100400], b"\x5a" * 0x400)
        start, end = nds.file_span(out, 0)
        self.assertEqual(out[start:end], b"file contents!!!")

    def test_a_cartridge_too_small_is_refused(self):
        rom = cartridge(0x1000, 0x6000, 0x80000, 0x400, capacity=2)
        with self.assertRaises(bw.PortError):
            bw.relayout(rom, rom[0x4000:0x5000] + bytes(0x80000))


class VeneerTests(unittest.TestCase):
    def test_arm_veneer_loads_the_target(self):
        self.assertEqual(bw.arm_veneer(0x02213D41), struct.pack("<II", 0xE51FF004, 0x02213D41))

    def test_only_position_independent_instructions_move(self):
        self.assertTrue(bw.relocatable_arm(0xE92D4038))   # stmdb sp!, {r3-r5, lr}
        self.assertTrue(bw.relocatable_arm(0xE1A05000))   # mov r5, r0
        self.assertFalse(bw.relocatable_arm(0xEB000010))  # bl
        self.assertFalse(bw.relocatable_arm(0xE59F0010))  # ldr r0, [pc, #0x10]
        self.assertFalse(bw.relocatable_arm(0xE1A0F00E))  # mov pc, lr
        self.assertFalse(bw.relocatable_arm(0x0A000000))  # conditional branch


if __name__ == "__main__":
    unittest.main()
