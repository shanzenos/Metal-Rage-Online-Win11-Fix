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
  4. DllCharacteristics = 0x0000
  5. Disable y0da's two TLS callbacks (see README)

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
    if bytes(data[0x19235:0x19239]) != enc_bytes:
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

    # 5. Disable y0da's TLS callbacks (they return with a cdecl 'ret' instead
    # of 'ret 0xC'; Win11's WoW64 ntdll does not restore esp, so every thread
    # create/exit jumps to the Reason argument -> access violation -> WER).
    # TLS directory (file 0x190F9) AddressOfCallBacks at 0x19105 points at
    # the callback array {0x1095112D, 0x10951159, 0} (file 0x19119).
    # Point it at the array's terminating 0 (0x10951121) so none is called.
    # Callback 2 (file 0x19159, 0x5A bytes) is ADD/XOR-encoded and callback 1
    # used to decode it at PROCESS_ATTACH, so store it pre-decoded.
    cb_head = bytes.fromhex('9c807c240c01')  # pushfd; cmp byte [esp+0xC],1
    cb_tail = bytes.fromhex('909d90c3')      # nop; popfd; nop; ret
    cb_ptr = bytes(data[0x19105:0x19109])
    cb_arr = bytes(data[0x19119:0x19125])
    cb2 = bytes(data[0x19159:0x19159 + 0x5A])
    cb2_dec = bytes((((b + 0xE3) & 0xFF) ^ 0x2C) for b in cb2)
    if cb_arr != bytes.fromhex('2d11951059119510' '00000000'):
        print("ERROR: TLS callback array is not the expected one; not touching the file")
        return False
    if cb_ptr == bytes.fromhex('19119510') and cb2_dec[:6] == cb_head and cb2_dec[-4:] == cb_tail:
        data[0x19105:0x19109] = bytes.fromhex('21119510')
        data[0x19159:0x19159 + 0x5A] = cb2_dec
        changes.append("TLS callbacks: AddressOfCallBacks 0x10951119 -> 0x10951121, callback 2 pre-decoded")
    elif cb_ptr == bytes.fromhex('21119510') and cb2[:6] == cb_head and cb2[-4:] == cb_tail:
        pass  # TLS callbacks already disabled
    else:
        print("ERROR: TLS callback bytes are neither original nor patched; not touching the file")
        return False

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
