# Shared helpers for the polcore research scripts. They read the user's own files from paths in environment
# variables and never write game bytes anywhere:
#   POLCORE_DLL  polcore_native.dll as installed (POL1-packed, SHA-256 73B1864B...)
#   TM_MAPPED    TM.dll's unpacked, mapped image (the spike's tm_mapped.bin, or tmserver's TM_emulated_mapped.dll)
import os
import struct
import sys

import capstone
import pefile

BASE = 0x10000000
COMMON_TABLE_RVA = 0x6FBE8  # what core slot 7 returns
COMMON_TABLE_SIZE = 1200
TM_COMMON_TABLE_GLOBAL = 0x1024DFE8  # where TM.dll keeps that pointer
TM_CORE_GLOBAL = 0x1024DFEC  # TM's second core interface (slot 3)


def need(var):
    path = os.environ.get(var)
    if not path or not os.path.exists(path):
        print("skip: set %s" % var)
        sys.exit(0)
    return path


def lzss(src):
    """The POL1 section's LZSS: control byte MSB first, 1 = literal, 0 = 12-bit back reference of 3..18."""
    out = bytearray()
    i = 0
    while i < len(src):
        control = src[i]
        i += 1
        for _ in range(8):
            if control & 0x80:
                out.append(src[i])
                i += 1
            else:
                distance = (src[i] & 0x0F) << 8 | src[i + 1]
                if distance == 0:
                    return bytes(out)
                for _ in range((src[i] >> 4) + 3):
                    out.append(out[-distance])
                i += 2
            control = (control << 1) & 0xFF
            if i >= len(src):
                return bytes(out)
    return bytes(out)


def polcore_image():
    """Unpacked polcore mapped at its preferred base: (pe, image bytes). Same as running its stub at 0x10000000."""
    pe = pefile.PE(need("POLCORE_DLL"))
    image = bytearray(pe.get_memory_mapped_image())
    text = next(s for s in pe.sections if s.Name.startswith(b".text"))
    pol1 = next(s for s in pe.sections if s.Name.startswith(b"POL1"))
    code = lzss(pe.get_data(pol1.VirtualAddress, pol1.SizeOfRawData))
    image[text.VirtualAddress : text.VirtualAddress + len(code)] = code
    return pe, bytes(image)


def polcore_imports(pe):
    """IAT slot address (at the preferred base) -> 'dll!name'."""
    out = {}
    for d in pe.DIRECTORY_ENTRY_IMPORT:
        dll = d.dll.decode().lower().removesuffix(".dll")
        for imp in d.imports:
            out[imp.address] = "%s!%s" % (dll, imp.name.decode() if imp.name else "#%d" % imp.ordinal)
    return out


def common_table(image):
    return struct.unpack_from("<%dI" % COMMON_TABLE_SIZE, image, COMMON_TABLE_RVA)


def tm_image():
    return open(need("TM_MAPPED"), "rb").read()


def disassembler():
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    md.detail = True
    return md


def u32(image, va):
    return struct.unpack_from("<I", image, va - BASE)[0]


def cstring(image, va, limit=80):
    off = va - BASE
    if not 0 <= off < len(image):
        return None
    end = image.find(b"\0", off, off + limit)
    raw = image[off : end if end >= 0 else off + limit]
    if len(raw) < 3 or any(c < 0x20 or c > 0x7E for c in raw):
        return None
    return raw.decode("ascii")


if __name__ == "__main__":
    # Two literals, a 4-byte copy from 2 back, then the zero-distance end marker.
    assert lzss(bytes([0xC0]) + b"ab" + bytes([0x10, 0x02, 0x00, 0x00])) == b"ababab"
    print("ok")
