# Minimal Windows x86 structured exception dispatch for Unicorn: build EXCEPTION_RECORD and CONTEXT on the
# guest stack, call each fs:[0] handler, and resume from the CONTEXT it leaves when it returns
# ExceptionContinueExecution. Enough for protectors that use faults as control flow.
import struct

from unicorn.x86_const import (UC_X86_REG_EAX, UC_X86_REG_EBP, UC_X86_REG_EBX, UC_X86_REG_ECX, UC_X86_REG_EDI,
                               UC_X86_REG_EDX, UC_X86_REG_EFLAGS, UC_X86_REG_EIP, UC_X86_REG_ESI, UC_X86_REG_ESP,
                               UC_X86_REG_DR0, UC_X86_REG_DR1, UC_X86_REG_DR2, UC_X86_REG_DR3, UC_X86_REG_DR6,
                               UC_X86_REG_DR7)

ACCESS_VIOLATION = 0xC0000005
BREAKPOINT = 0x80000003
SINGLE_STEP = 0x80000004
ILLEGAL = 0xC000001D
DIVIDE = 0xC0000094
PRIVILEGED = 0xC0000096

# CONTEXT offsets
REGS = ((0x9C, UC_X86_REG_EDI), (0xA0, UC_X86_REG_ESI), (0xA4, UC_X86_REG_EBX), (0xA8, UC_X86_REG_EDX),
        (0xAC, UC_X86_REG_ECX), (0xB0, UC_X86_REG_EAX), (0xB4, UC_X86_REG_EBP), (0xB8, UC_X86_REG_EIP),
        (0xC0, UC_X86_REG_EFLAGS), (0xC4, UC_X86_REG_ESP))
DEBUG = ((0x04, UC_X86_REG_DR0), (0x08, UC_X86_REG_DR1), (0x0C, UC_X86_REG_DR2), (0x10, UC_X86_REG_DR3),
         (0x14, UC_X86_REG_DR6), (0x18, UC_X86_REG_DR7))


class Dispatcher:
    def __init__(self, uc, teb, return_address):
        self.uc, self.teb, self.back = uc, teb, return_address
        self.frames = []  # (context address, registration record) of handlers in progress
        self.count = 0

    def u32(self, a):
        return struct.unpack('<I', self.uc.mem_read(a, 4))[0]

    def raise_(self, code, address, info=()):
        """Start dispatching; returns the handler address to run, or None when no handler is left."""
        uc = self.uc
        self.count += 1
        esp = uc.reg_read(UC_X86_REG_ESP)
        ctx = (esp - 0x400) & ~0xF
        rec = ctx - 0x60
        uc.mem_write(ctx, bytes(0x2CC))
        uc.mem_write(ctx, struct.pack('<I', 0x1003F))
        for off, reg in REGS:
            uc.mem_write(ctx + off, struct.pack('<I', uc.reg_read(reg)))
        uc.mem_write(ctx + 0xB8, struct.pack('<I', address))
        uc.mem_write(rec, struct.pack('<5I', code, 0, 0, address, len(info)) + b''.join(struct.pack('<I', i) for i in info))
        return self._call(self.u32(self.teb), rec, ctx)

    def _call(self, frame, rec, ctx):
        if frame in (0, 0xFFFFFFFF):
            return None
        uc = self.uc
        handler = self.u32(frame + 4)
        sp = rec - 0x20
        uc.mem_write(sp, struct.pack('<5I', self.back, rec, frame, ctx, 0))
        uc.reg_write(UC_X86_REG_ESP, sp)
        self.frames.append((ctx, frame, rec))
        return handler

    def returned(self):
        """The handler returned to our address; returns the address to resume at."""
        uc = self.uc
        ctx, frame, rec = self.frames.pop()
        disposition = uc.reg_read(UC_X86_REG_EAX)
        if disposition == 1:  # ExceptionContinueSearch
            return self._call(self.u32(frame), rec, ctx)
        for off, reg in REGS:
            uc.reg_write(reg, self.u32(ctx + off))
        for off, reg in DEBUG:
            uc.reg_write(reg, self.u32(ctx + off))
        return self.u32(ctx + 0xB8)
