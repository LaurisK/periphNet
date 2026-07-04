#!/usr/bin/env python3
"""
DFU image tool for PeriphNet firmware.

Commands:
  sign     — patch IMAGE_SIZE + IMAGE_HMAC into a plaintext .bin
             (for direct J-Link flashing during development)
  package  — produce the encrypted .pnfw FWU blob:
             [manifest:64][nonce:12][AES-128-GCM ciphertext][tag:16][crc32:4]
             The manifest is authenticated as GCM AAD; the trailing CRC32
             lets the device check transfer integrity without the key.
  full     — combined BL+APP Intel HEX for factory flashing

Keys come from --key/--hmac-key, DFU_AES_KEY/DFU_HMAC_KEY env vars, or
fall back to the committed development keys (matching bootloader/secrets.c).

Adapted from Zhaga project (lusety-lamp-hw/ZhagaFW).
"""

import struct
import logging
import os
import zlib
import hmac
import hashlib
import secrets
import argparse

logging.basicConfig(level=logging.INFO, format='[%(levelname)s] %(message)s')

# --- Constants (must match dfu_types.h / bl_app_contract.h) ---

DFU_GCM_NONCE_SIZE = 12
DFU_GCM_TAG_SIZE = 16
DFU_HMAC_SIZE = 32

# FWU blob manifest (sFwuManifest)
FWU_BLOB_MAGIC = 0x57464E50  # "PNFW" little-endian
FWU_BLOB_FORMAT = 1
FWU_MANIFEST_SIZE = 64
FWU_BLOB_OVERHEAD = FWU_MANIFEST_SIZE + DFU_GCM_NONCE_SIZE + \
                    DFU_GCM_TAG_SIZE + 4  # 96

# Firmware binary metadata offsets (relative to image base = 0)
FW_OFFSET_APP_HEADER = 0x200
FW_OFFSET_FW_VERSION = 0x204
FW_OFFSET_IMAGE_SIZE = 0x224
FW_OFFSET_IMAGE_HMAC = 0x228
FW_VER_AREA_SIZE = 32

# Addresses for full-image hex generation
DEFAULT_BOOTLOADER_ADDRESS = 0x08000000
DEFAULT_APP_ADDRESS = 0x08008000

# Valid MSP address range for nonce safety check
MSP_RANGE_START = 0x20000000
MSP_RANGE_END = 0x20030000  # STM32F407 has 192KB RAM

# Development keys — match bootloader/secrets.c (GLB_blKey / GLB_hmacKey).
# Publicly known placeholders; production keys come from env/CLI.
DEV_AES_KEY = bytes([
    0x2b, 0x7e, 0x15, 0x16, 0x28, 0xae, 0xd2, 0xa6,
    0xab, 0xf7, 0x15, 0x88, 0x09, 0xcf, 0x4f, 0x3c
])
DEV_HMAC_KEY = b"PNHMAC-DEV-KEY-0123456789abcdefg"
assert len(DEV_HMAC_KEY) == 32


def generate_safe_nonce():
    """Generate a 12-byte nonce that cannot be mistaken for a valid MSP address."""
    while True:
        nonce = secrets.token_bytes(DFU_GCM_NONCE_SIZE)
        first_word = struct.unpack('<I', nonce[:4])[0]
        if first_word < MSP_RANGE_START or first_word >= MSP_RANGE_END:
            return nonce
        logging.info("Nonce collides with valid MSP range, regenerating...")


def calculate_hmac_sha256(data, key):
    """Calculate HMAC-SHA256 of data using key."""
    return hmac.new(key, data, hashlib.sha256).digest()


def load_firmware(input_path):
    """Load firmware from .bin file."""
    if not os.path.exists(input_path):
        raise FileNotFoundError(f"Input file not found: {input_path}")
    logging.info(f"Loading binary file: {input_path}")
    with open(input_path, 'rb') as f:
        return f.read()


def pad_to_alignment(data, alignment=8):
    """Pad data to specified alignment with 0xFF bytes."""
    remainder = len(data) % alignment
    if remainder != 0:
        padding = alignment - remainder
        data += b'\xFF' * padding
        logging.info(f"Padded firmware by {padding} bytes to {alignment}-byte alignment")
    return data


def patch_firmware(firmware, hmac_key):
    """
    Patch IMAGE_SIZE and IMAGE_HMAC into the firmware binary.

    1. Write firmware length at FW_OFFSET_IMAGE_SIZE
    2. Zero HMAC field at FW_OFFSET_IMAGE_HMAC
    3. Compute HMAC-SHA256 over entire binary (with HMAC zeroed)
    4. Write HMAC at FW_OFFSET_IMAGE_HMAC
    """
    fw = bytearray(firmware)
    fw_size = len(fw)

    min_size = FW_OFFSET_IMAGE_HMAC + DFU_HMAC_SIZE
    if fw_size < min_size:
        raise ValueError(f"Firmware too small ({fw_size} bytes), need at least {min_size}")

    # Verify APP_INFO magic at offset 0x200
    magic = struct.unpack_from('<I', fw, FW_OFFSET_APP_HEADER)[0]
    if magic != 0x41505049:  # "APPI"
        raise ValueError(f"Invalid APP_INFO magic at 0x{FW_OFFSET_APP_HEADER:X}: "
                         f"0x{magic:08X} (expected 0x41505049)")

    # 1. Patch IMAGE_SIZE
    struct.pack_into('<I', fw, FW_OFFSET_IMAGE_SIZE, fw_size)
    logging.info(f"Patched IMAGE_SIZE={fw_size} at offset 0x{FW_OFFSET_IMAGE_SIZE:02X}")

    # 2. Zero HMAC field
    fw[FW_OFFSET_IMAGE_HMAC:FW_OFFSET_IMAGE_HMAC + DFU_HMAC_SIZE] = b'\x00' * DFU_HMAC_SIZE

    # 3. Compute HMAC over entire binary (with HMAC field zeroed)
    hmac_value = calculate_hmac_sha256(bytes(fw), hmac_key)
    logging.info(f"Computed HMAC: {hmac_value.hex()}")

    # 4. Write HMAC
    fw[FW_OFFSET_IMAGE_HMAC:FW_OFFSET_IMAGE_HMAC + DFU_HMAC_SIZE] = hmac_value
    logging.info(f"Patched IMAGE_HMAC at offset 0x{FW_OFFSET_IMAGE_HMAC:02X}")

    return bytes(fw)


def sign_firmware(input_path, output_path, hmac_key):
    """
    Sign firmware: patch IMAGE_SIZE and IMAGE_HMAC (no encryption).
    Output is a plain signed .bin for direct J-Link flashing.
    """
    firmware = load_firmware(input_path)
    logging.info(f"Raw firmware size: {len(firmware)} bytes")

    firmware = pad_to_alignment(firmware, 8)
    firmware = patch_firmware(firmware, hmac_key)

    out_path = output_path or input_path  # in-place by default
    with open(out_path, 'wb') as f:
        f.write(firmware)

    logging.info(f"Written signed firmware to: {out_path}")
    return {
        'output': out_path,
        'firmware_size': len(firmware),
    }


def build_manifest(firmware):
    """Build the 64-byte cleartext sFwuManifest for a signed firmware."""
    fw_version = firmware[FW_OFFSET_FW_VERSION:
                          FW_OFFSET_FW_VERSION + FW_VER_AREA_SIZE]
    image_size = len(firmware)
    blob_size = image_size + FWU_BLOB_OVERHEAD

    manifest = struct.pack('<II', FWU_BLOB_MAGIC, FWU_BLOB_FORMAT)
    manifest += fw_version
    manifest += struct.pack('<II', image_size, blob_size)
    manifest += b'\x00' * 16  # reserved
    assert len(manifest) == FWU_MANIFEST_SIZE
    return manifest


def package_firmware(input_path, output_path, aes_key, hmac_key):
    """
    Generate the encrypted .pnfw FWU blob:

        [manifest:64][nonce:12][ciphertext:N][tag:16][crc32:4]

    - plaintext is the signed firmware (IMAGE_SIZE + HMAC patched)
    - manifest is authenticated as GCM AAD
    - trailing CRC32 (zlib) covers everything before it — keyless
      transfer-integrity check for the device
    """
    try:
        from cryptography.hazmat.primitives.ciphers.aead import AESGCM
    except ImportError:
        raise RuntimeError("Encryption requires 'cryptography' package: "
                           "pip install cryptography")

    firmware = load_firmware(input_path)
    logging.info(f"Raw firmware size: {len(firmware)} bytes")

    firmware = pad_to_alignment(firmware, 8)
    firmware = patch_firmware(firmware, hmac_key)

    manifest = build_manifest(firmware)
    nonce = generate_safe_nonce()
    logging.info(f"Nonce: {nonce.hex()}")

    logging.info("Encrypting firmware with AES-128-GCM (manifest as AAD)...")
    aesgcm = AESGCM(aes_key)
    ciphertext_with_tag = aesgcm.encrypt(nonce, firmware, manifest)

    body = manifest + nonce + ciphertext_with_tag
    crc = zlib.crc32(body) & 0xFFFFFFFF
    blob = body + struct.pack('<I', crc)

    expected_size = len(firmware) + FWU_BLOB_OVERHEAD
    assert len(blob) == expected_size, \
        f"blob size {len(blob)} != expected {expected_size}"

    with open(output_path, 'wb') as f:
        f.write(blob)

    logging.info(f"Written blob to: {output_path}")
    logging.info(f"Blob size: {len(blob)} bytes (image {len(firmware)} + "
                 f"overhead {FWU_BLOB_OVERHEAD}), crc32=0x{crc:08X}")

    return {
        'output': output_path,
        'firmware_size': len(firmware),
        'blob_size': len(blob),
        'crc32': f"0x{crc:08X}",
    }


def generate_full_image(input_path, output_path, bootloader_path,
                        hmac_key, app_address=None):
    """
    Generate combined BL+APP Intel HEX image for factory flashing.
    """
    try:
        from intelhex import IntelHex
    except ImportError:
        raise RuntimeError("Full image generation requires 'intelhex' package: "
                           "pip install intelhex")

    if app_address is None:
        app_address = DEFAULT_APP_ADDRESS

    if not os.path.exists(bootloader_path):
        raise FileNotFoundError(f"Bootloader file not found: {bootloader_path}")

    firmware = load_firmware(input_path)
    logging.info(f"Raw firmware size: {len(firmware)} bytes")

    firmware = pad_to_alignment(firmware, 8)
    firmware = patch_firmware(firmware, hmac_key)

    logging.info(f"Loading bootloader: {bootloader_path}")
    boot_ih = IntelHex()
    boot_ext = os.path.splitext(bootloader_path)[1].lower()
    if boot_ext == '.hex':
        boot_ih.loadhex(bootloader_path)
    elif boot_ext == '.bin':
        with open(bootloader_path, 'rb') as f:
            boot_data = f.read()
        boot_ih.frombytes(boot_data, offset=DEFAULT_BOOTLOADER_ADDRESS)
    else:
        raise ValueError(f"Unsupported bootloader format: {boot_ext}")

    combined = IntelHex()
    combined.merge(boot_ih, overlap='replace')
    combined.frombytes(firmware, offset=app_address)

    out_path = output_path
    if not out_path.lower().endswith('.hex'):
        out_path = os.path.splitext(out_path)[0] + '.hex'

    combined.write_hex_file(out_path)

    addresses = combined.addresses()
    logging.info(f"Written full image to: {out_path}")
    logging.info(f"Address range: 0x{min(addresses):08X} - 0x{max(addresses):08X}")

    return {
        'output': out_path,
        'firmware_size': len(firmware),
        'start_address': f"0x{min(addresses):08X}",
        'end_address': f"0x{max(addresses):08X}",
    }


def resolve_key(cli_value, env_var, expected_len, dev_default, name):
    """Resolve a key: CLI arg > env var > committed dev default (warned)."""
    for source, value in (("--" + name, cli_value),
                          (env_var, os.environ.get(env_var))):
        if not value:
            continue
        try:
            key = bytes.fromhex(value)
        except ValueError:
            logging.error(f"{source} is not valid hex")
            exit(1)
        if len(key) != expected_len:
            logging.error(f"{source} must be {expected_len} bytes "
                          f"({expected_len * 2} hex chars), got {len(key)}")
            exit(1)
        logging.info(f"Using {name} from {source}")
        return key

    logging.warning(f"Using committed DEVELOPMENT {name} — "
                    f"set {env_var} for production builds")
    return dev_default


if __name__ == "__main__":
    parser = argparse.ArgumentParser(
        description="DFU image tool: sign, package (.pnfw), or generate combined hex.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
  # Sign firmware (patch IMAGE_SIZE + HMAC, no encryption; in-place)
  python dfu_image_tool.py sign -i firmware.bin

  # Generate encrypted FWU blob
  python dfu_image_tool.py package -i firmware.bin -o firmware.pnfw

  # Generate combined BL+APP hex for factory flashing
  python dfu_image_tool.py full -i firmware.bin -o full_image.hex -b bootloader.bin

Keys: --key/--hmac-key (hex) > DFU_AES_KEY/DFU_HMAC_KEY env > dev defaults.
        """
    )

    subparsers = parser.add_subparsers(dest="command", required=True)

    def add_key_args(p, aes=False):
        if aes:
            p.add_argument("--key", help="AES-128 key as hex (32 hex chars)")
        p.add_argument("--hmac-key", help="HMAC-SHA256 key as hex (64 hex chars)")

    # --- sign subcommand ---
    p_sign = subparsers.add_parser("sign", help="Sign firmware (HMAC, no encryption)")
    p_sign.add_argument("-i", "--input", required=True, help="Input .bin file")
    p_sign.add_argument("-o", "--output", help="Output .bin file (default: in-place)")
    add_key_args(p_sign)

    # --- package subcommand ---
    p_pkg = subparsers.add_parser("package",
                                  help="Sign + encrypt into .pnfw FWU blob")
    p_pkg.add_argument("-i", "--input", required=True, help="Input .bin file")
    p_pkg.add_argument("-o", "--output", required=True, help="Output .pnfw blob")
    add_key_args(p_pkg, aes=True)

    # --- full subcommand ---
    p_full = subparsers.add_parser("full", help="Generate combined BL+APP hex")
    p_full.add_argument("-i", "--input", required=True, help="Input firmware .bin")
    p_full.add_argument("-o", "--output", required=True, help="Output .hex file")
    p_full.add_argument("-b", "--bootloader", required=True, help="Bootloader .hex or .bin")
    p_full.add_argument("--app-addr", type=lambda x: int(x, 0),
                        default=DEFAULT_APP_ADDRESS,
                        help=f"App base address (default: 0x{DEFAULT_APP_ADDRESS:08X})")
    add_key_args(p_full)

    args = parser.parse_args()

    hmac_key = resolve_key(args.hmac_key, 'DFU_HMAC_KEY', 32,
                           DEV_HMAC_KEY, "hmac-key")

    if args.command == "sign":
        result = sign_firmware(args.input, args.output, hmac_key)
    elif args.command == "package":
        aes_key = resolve_key(args.key, 'DFU_AES_KEY', 16, DEV_AES_KEY, "key")
        result = package_firmware(args.input, args.output, aes_key, hmac_key)
    elif args.command == "full":
        result = generate_full_image(args.input, args.output, args.bootloader,
                                     hmac_key, app_address=args.app_addr)

    if result:
        logging.info("\n=== Summary ===")
        for k, v in result.items():
            logging.info(f"  {k}: {v}")
