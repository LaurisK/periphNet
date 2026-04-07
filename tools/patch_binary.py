#!/usr/bin/env python3
"""Patch application.bin with correct image_size in the sAppInfo header.

Develop-branch sAppInfo layout at offset 0x200:
  0x00: magic           (uint32)
  0x04: fw_version      (32 bytes - sFwVerArea)
  0x24: image_size      (uint32)  <-- patched
  0x28: image_hmac      (32 bytes)
  0x48: features        (uint32)
  0x4C: min_bl_version  (uint32)
  0x50: reserved        (8 x uint32)
"""

import struct
import sys

APP_INFO_MAGIC = 0x41505049
HEADER_OFFSET = 0x200
SIZE_FIELD_OFFSET = 0x24  # relative to header start

def main():
    if len(sys.argv) < 2:
        print(f"Usage: {sys.argv[0]} <application.bin>")
        sys.exit(1)

    path = sys.argv[1]
    with open(path, 'rb') as f:
        data = bytearray(f.read())

    magic = struct.unpack_from('<I', data, HEADER_OFFSET)[0]
    if magic != APP_INFO_MAGIC:
        print(f"ERROR: Bad magic at 0x{HEADER_OFFSET:X}: 0x{magic:08X}")
        sys.exit(1)

    image_size = len(data)
    size_offset = HEADER_OFFSET + SIZE_FIELD_OFFSET
    struct.pack_into('<I', data, size_offset, image_size)

    with open(path, 'wb') as f:
        f.write(data)

    # Print info
    dt = data[HEADER_OFFSET + 0x04]
    tgt = data[HEADER_OFFSET + 0x05]
    major = struct.unpack_from('<H', data, HEADER_OFFSET + 0x06)[0]
    minor = data[HEADER_OFFSET + 0x08]
    patch = data[HEADER_OFFSET + 0x09]
    features = struct.unpack_from('<I', data, HEADER_OFFSET + 0x48)[0]

    print(f"Patched {path}:")
    print(f"  image_size = {image_size} (0x{image_size:X})")
    print(f"  version = {chr(dt)}{chr(tgt)}{major}.{minor}.{patch}")
    print(f"  features = 0x{features:X}")

if __name__ == '__main__':
    main()
