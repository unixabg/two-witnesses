# SPDX-FileCopyrightText: 2026 Richard Nelson
# SPDX-License-Identifier: Apache-2.0
"""Run Espressif's compiled esp_radar_motion_dec (ESP32-C3 build) under the unicorn
RISC-V emulator. Reference implementation: slow, but it is Espressif's own code."""
import struct

import numpy as np
from elftools.elf.elffile import ELFFile
from unicorn import Uc, UC_ARCH_RISCV, UC_MODE_RISCV32, UcError
from unicorn.riscv_const import (UC_RISCV_REG_A0, UC_RISCV_REG_A1, UC_RISCV_REG_A2, UC_RISCV_REG_A3,
                                 UC_RISCV_REG_A4, UC_RISCV_REG_A5, UC_RISCV_REG_RA, UC_RISCV_REG_SP,
                                 UC_RISCV_REG_GP)

ARGS = [UC_RISCV_REG_A0, UC_RISCV_REG_A1, UC_RISCV_REG_A2, UC_RISCV_REG_A3, UC_RISCV_REG_A4, UC_RISCV_REG_A5]
STACK_TOP = 0x0080_0000
IO_BASE = 0x0090_0000          # host-owned scratch region for input/output arrays
IO_SIZE = 0x0010_0000
RET_ADDR = 0x00A0_0000         # calls return here; we stop the emulator on reaching it


class MotionDec:
    def __init__(self, elf_path):
        self.uc = Uc(UC_ARCH_RISCV, UC_MODE_RISCV32)
        self.sym = {}
        with open(elf_path, "rb") as fh:
            elf = ELFFile(fh)
            for seg in elf.iter_segments():
                if seg["p_type"] != "PT_LOAD":
                    continue
                lo = seg["p_vaddr"] & ~0xFFF
                hi = (seg["p_vaddr"] + seg["p_memsz"] + 0xFFF) & ~0xFFF
                self.uc.mem_map(lo, hi - lo)
                self.uc.mem_write(seg["p_vaddr"], seg.data())
            for s in elf.get_section_by_name(".symtab").iter_symbols():
                if s.name:
                    self.sym.setdefault(s.name, s["st_value"])
        self.uc.mem_map(STACK_TOP - 0x10_0000, 0x10_0000)
        self.uc.mem_map(IO_BASE, IO_SIZE)
        self.uc.mem_map(RET_ADDR, 0x1000)
        self.gp = self.sym.get("__global_pointer$", 0)

    # --- memory helpers ---
    def _put(self, offset, arr):
        b = np.ascontiguousarray(arr, dtype=np.float32).tobytes()
        self.uc.mem_write(IO_BASE + offset, b)
        return IO_BASE + offset, offset + ((len(b) + 15) & ~15)

    def _u32(self, name):
        return struct.unpack("<I", self.uc.mem_read(self.sym[name], 4))[0]

    def _f32(self, name):
        return struct.unpack("<f", self.uc.mem_read(self.sym[name], 4))[0]

    def _call(self, fn, *args):
        for reg, v in zip(ARGS, args):
            self.uc.reg_write(reg, int(v) & 0xFFFFFFFF)
        self.uc.reg_write(UC_RISCV_REG_SP, STACK_TOP - 16)
        self.uc.reg_write(UC_RISCV_REG_RA, RET_ADDR)
        if self.gp:
            self.uc.reg_write(UC_RISCV_REG_GP, self.gp)
        self.uc.emu_start(self.sym[fn], RET_ADDR)

    # --- the three library functions ---
    def compute(self, data, split=None):
        """data: rows x cols (float32), in ring order. split: rows in the first segment
        (the device passes the ring buffer as two pieces when it wraps)."""
        data = np.asarray(data, dtype=np.float32)
        rows, cols = data.shape
        r0 = rows if split is None else split
        a0, off = self._put(0, data[:r0])
        a1, off = self._put(off, data[r0:] if rows > r0 else np.zeros((1, cols), np.float32))
        out = IO_BASE + off
        self._call("call_compute", cols, r0, a0, rows - r0, a1, out)
        res = np.frombuffer(bytes(self.uc.mem_read(out, 4 * cols)), dtype=np.float32).copy()
        return self._u32("ret_code"), res

    def wander(self, a, b):
        a = np.asarray(a, np.float32)
        pa, off = self._put(0, a)
        pb, off = self._put(off, b)
        self._call("call_wander", pa, pb, len(a))
        return self._f32("ret_float")

    def jitter(self, current, history, buff_num):
        """history: list of window_size vectors (the device's ring of recent results)."""
        cur = np.asarray(current, np.float32)
        pc, off = self._put(0, cur)
        ptrs = []
        for h in history:
            p, off = self._put(off, h)
            ptrs.append(p)
        pp = IO_BASE + off
        self.uc.mem_write(pp, struct.pack(f"<{len(ptrs)}I", *ptrs))
        self._call("call_jitter", pc, pp, len(cur), len(history), buff_num)
        return self._f32("ret_float")

    def counters(self):
        return {k: self._u32(k) for k in ("n_malloc", "n_powf", "n_sqrtf", "n_log", "last_pow_exp_bits")}
