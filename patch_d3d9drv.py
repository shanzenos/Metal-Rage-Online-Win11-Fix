#!/usr/bin/env python3
"""
Metal Rage Online - D3D9Drv.dll patcher
=======================================
Win11's WMI hangs during DxDiag enumeration. This makes the DxDiag helper
function return TRUE immediately without calling CoCreateInstance.

Patch at file offset 0xF040 (8 bytes):
  Original: 83 EC 10 56 6A 00 8B F1  (sub esp,0x10; push esi; push 0; mov esi,ecx)
  Patched:  B0 01 C2 04 00 90 90 90  (mov al,1; ret 4; nop; nop; nop)

Usage: python patch_d3d9drv.py [path/to/D3D9Drv.dll]
"""
import os, shutil, sys

OFFSET = 0xF040
ORIGINAL = bytes.fromhex('83EC10566A008BF1')
PATCHED = bytes.fromhex('B001C204009090' '90')


def find_dll(arg=None):
    candidates = [arg] if arg else []
    candidates += [
        os.path.join('data', 'System', 'D3D9Drv.dll'),
        'D3D9Drv.dll',
        os.path.join(os.path.dirname(os.path.abspath(__file__)), 'data', 'System', 'D3D9Drv.dll'),
    ]
    for c in candidates:
        if c and os.path.isfile(c):
            return os.path.abspath(c)
    return None


def patch(filepath):
    with open(filepath, 'rb') as f:
        data = bytearray(f.read())

    current = bytes(data[OFFSET:OFFSET + 8])
    if current == PATCHED:
        print("No changes needed (already patched?)")
        return True
    if current != ORIGINAL:
        print("ERROR: unexpected bytes at 0x%X (%s); not touching the file" % (OFFSET, current.hex(' ')))
        return False

    backup = filepath + '.original'
    if not os.path.exists(backup):
        shutil.copy2(filepath, backup)
        print("Backup: " + os.path.basename(backup))

    data[OFFSET:OFFSET + 8] = PATCHED
    with open(filepath, 'wb') as f:
        f.write(data)
    print("Patched: 83 EC 10 56 6A 00 8B F1 -> B0 01 C2 04 00 90 90 90")
    return True


if __name__ == '__main__':
    dll = find_dll(sys.argv[1] if len(sys.argv) > 1 else None)
    if not dll:
        print("Cannot find D3D9Drv.dll")
        sys.exit(1)
    print("Patching: " + dll)
    sys.exit(0 if patch(dll) else 1)
