#!/usr/bin/env python3
"""
Metal Rage Online - Windows 10/11 PE Patcher
==============================================
Fixes y0da Protector compatibility issues that prevent the game from
launching on modern Windows.

Patches applied:
  1. SizeOfImage alignment (0x6C0CF → 0x6D000)
  2. Force NRV decryption path (NOP the skip-jmp)
  3. NRV decompressor source address (0x400000 → ImageBase)

Usage: python patch_metalrage.py [path/to/MetalRage.exe]
"""
import struct, shutil, os, sys

def find_exe(arg=None):
    candidates = [arg] if arg else []
    candidates += [
        os.path.join('data', 'System', 'MetalRage.exe'),
        'MetalRage.exe',
        os.path.join(os.path.dirname(__file__), 'data', 'System', 'MetalRage.exe'),
    ]
    for c in candidates:
        if c and os.path.isfile(c):
            return os.path.abspath(c)
    return None

def patch(filepath):
    with open(filepath, 'rb') as f:
        data = bytearray(f.read())

    pe_off = struct.unpack_from('<I', data, 0x3C)[0]
    if data[pe_off:pe_off+4] != b'PE\x00\x00':
        print("ERROR: Not a valid PE file"); return False

    opt = pe_off + 24
    if struct.unpack_from('<H', data, opt)[0] != 0x010B:
        print("ERROR: Not PE32"); return False

    image_base = struct.unpack_from('<I', data, opt + 28)[0]
    sect_align = struct.unpack_from('<I', data, opt + 32)[0]
    size_image = struct.unpack_from('<I', data, opt + 56)[0]
    changes = []

    # 1. Fix SizeOfImage alignment
    if sect_align > 0 and size_image % sect_align != 0:
        new_si = ((size_image + sect_align - 1) // sect_align) * sect_align
        struct.pack_into('<I', data, opt + 56, new_si)
        changes.append(f"SizeOfImage: 0x{size_image:X} -> 0x{new_si:X}")

    # 2. NOP the decrypt-skip jmp (force decryption path)
    if data[0x19213] == 0xEB and data[0x19214] == 0x1E:
        data[0x19213] = 0x90
        data[0x19214] = 0x90
        changes.append("Entry decrypt: EB 1E -> 90 90 (force decryption)")

    # 3. Patch NRV source address to match actual ImageBase
    # Encrypted bootstrap at 0x19233: byte 4-5 encode the LE address
    # XOR key = 0x21. Original: 0x400000 -> encrypted 61 21
    ib_bytes = struct.pack('<I', image_base)  # LE bytes of ImageBase
    enc_bytes = bytes(b ^ 0x21 for b in ib_bytes)
    # Bytes 2-5 of encrypted block (offset +2 to +5 from 0x19233)
    data[0x19235] = enc_bytes[0]  # low byte
    data[0x19236] = enc_bytes[1]
    data[0x19237] = enc_bytes[2]
    data[0x19238] = enc_bytes[3]  # high byte
    changes.append(f"NRV source: 0x400000 -> 0x{image_base:08X}")

    # 4. Ensure DllCharacteristics is 0x0000 (no DEP - y0da needs stack exec)
    dll_chars = struct.unpack_from('<H', data, opt + 70)[0]
    if dll_chars != 0x0000:
        struct.pack_into('<H', data, opt + 70, 0x0000)
        changes.append(f"DllCharacteristics: 0x{dll_chars:04X} -> 0x0000")

    if not changes:
        print("No changes needed (already patched?)"); return True

    # Backup
    backup = filepath + '.original'
    if not os.path.exists(backup):
        shutil.copy2(filepath, backup)
        print(f"Backup: {os.path.basename(backup)}")

    with open(filepath, 'wb') as f:
        f.write(data)

    print("Patches applied:")
    for c in changes:
        print(f"  {c}")
    return True

if __name__ == '__main__':
    exe = find_exe(sys.argv[1] if len(sys.argv) > 1 else None)
    if not exe:
        print("Cannot find MetalRage.exe"); sys.exit(1)
    print(f"Patching: {exe}")
    patch(exe)
