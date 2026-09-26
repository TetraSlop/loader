"""Read-only PE checks for the compiled loader; does not execute the program."""
from pathlib import Path
import struct
import sys
import re

path = Path(sys.argv[1] if len(sys.argv) > 1 else 'build/RelWithDebInfo/tmloader.exe')
data = path.read_bytes()
pe = struct.unpack_from('<I', data, 0x3c)[0]
assert data[:2] == b'MZ' and data[pe:pe + 4] == b'PE\0\0'
assert struct.unpack_from('<H', data, pe + 4)[0] == 0x14c, 'Expected x86'
optional = pe + 24
assert struct.unpack_from('<H', data, optional)[0] == 0x10b, 'Expected PE32'
assert struct.unpack_from('<H', data, optional + 68)[0] == 2, 'Expected GUI subsystem'
flags = struct.unpack_from('<H', data, optional + 70)[0]
assert not flags & (0x40 | 0x100), 'Legacy DEP/ASLR compatibility flags changed'
assert re.search(rb"requestedExecutionLevel\s+level=['\"]asInvoker['\"]\s+uiAccess=['\"]false['\"]", data), 'asInvoker manifest missing'
assert b'requireAdministrator' not in data
print('PASS x86 PE32, GUI subsystem, asInvoker manifest and legacy compatibility flags')
