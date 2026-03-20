#!/usr/bin/env python3
"""
DFU image tool for PeriphNet firmware.

Patches IMAGE_SIZE and IMAGE_HMAC into firmware binary, optionally encrypts
with AES-128-GCM for OTA, or generates combined BL+APP hex for factory flash.

Adapted from Zhaga project (lusety-lamp-hw/ZhagaFW).
"""

import struct
import logging
import os
import hmac
import hashlib
import secrets
import argparse

logging.basicConfig(level=logging.INFO, format='[%(levelname)s] %(message)s')

# --- Constants (must match dfu_types.h / bl_app_contract.h) ---

DFU_GCM_NONCE_SIZE = 12
DFU_GCM_TAG_SIZE = 16
DFU_HMAC_SIZE = 32

# Firmware binary metadata offsets (relative to image base = 0)
FW_OFFSET_APP_HEADER = 0x200
FW_OFFSET_IMAGE_SIZE = 0x224
FW_OFFSET_IMAGE_HMAC = 0x228

# Addresses for full-image hex generation
DEFAULT_BOOTLOADER_ADDRESS = 0x08000000
DEFAULT_APP_ADDRESS = 0x08008000

# Valid MSP address range for nonce safety check
MSP_RANGE_START = 0x20000000
MSP_RANGE_END = 0x20030000  # STM32F407 has 192KB RAM

# Default AES key (matches GLB_blKey in bootloader/secrets.c)
AES_KEY = bytes([
    0x2b, 0x7e, 0x15, 0x16, 0x28, 0xae, 0xd2, 0xa6,
    0xab, 0xf7, 0x15, 0x88, 0x09, 0xcf, 0x4f, 0x3c
])


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


def patch_firmware(firmware, aes_key):
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
    hmac_value = calculate_hmac_sha256(bytes(fw), aes_key)
    logging.info(f"Computed HMAC: {hmac_value.hex()}")

    # 4. Write HMAC
    fw[FW_OFFSET_IMAGE_HMAC:FW_OFFSET_IMAGE_HMAC + DFU_HMAC_SIZE] = hmac_value
    logging.info(f"Patched IMAGE_HMAC at offset 0x{FW_OFFSET_IMAGE_HMAC:02X}")

    return bytes(fw)


def sign_firmware(input_path, output_path, key=None):
    """
    Sign firmware: patch IMAGE_SIZE and IMAGE_HMAC (no encryption).
    Output is a plain signed .bin ready for direct upload.
    """
    aes_key = key if key is not None else AES_KEY

    firmware = load_firmware(input_path)
    logging.info(f"Raw firmware size: {len(firmware)} bytes")

    firmware = pad_to_alignment(firmware, 8)
    firmware = patch_firmware(firmware, aes_key)

    out_path = output_path or input_path  # in-place by default
    with open(out_path, 'wb') as f:
        f.write(firmware)

    logging.info(f"Written signed firmware to: {out_path}")
    return {
        'output': out_path,
        'firmware_size': len(firmware),
    }


def generate_dfu_blob(input_path, output_path, key=None):
    """
    Generate encrypted DFU blob: [nonce:12][ciphertext:N][tag:16]

    Steps:
    1. Load firmware binary
    2. Pad to 8-byte alignment
    3. Patch IMAGE_SIZE and IMAGE_HMAC
    4. Generate safe nonce
    5. Encrypt with AES-128-GCM
    6. Output: [nonce][ciphertext][tag]
    """
    try:
        from cryptography.hazmat.primitives.ciphers.aead import AESGCM
    except ImportError:
        raise RuntimeError("Encryption requires 'cryptography' package: "
                           "pip install cryptography")

    aes_key = key if key is not None else AES_KEY

    firmware = load_firmware(input_path)
    logging.info(f"Raw firmware size: {len(firmware)} bytes")

    firmware = pad_to_alignment(firmware, 8)
    logging.info(f"Aligned firmware size: {len(firmware)} bytes")

    firmware = patch_firmware(firmware, aes_key)

    nonce = generate_safe_nonce()
    logging.info(f"Nonce: {nonce.hex()}")

    logging.info("Encrypting firmware with AES-128-GCM...")
    aesgcm = AESGCM(aes_key)
    ciphertext_with_tag = aesgcm.encrypt(nonce, firmware, None)

    ciphertext = ciphertext_with_tag[:-DFU_GCM_TAG_SIZE]
    tag = ciphertext_with_tag[-DFU_GCM_TAG_SIZE:]
    logging.info(f"Ciphertext size: {len(ciphertext)} bytes")
    logging.info(f"Tag: {tag.hex()}")

    blob = nonce + ciphertext + tag
    with open(output_path, 'wb') as f:
        f.write(blob)

    logging.info(f"Written blob to: {output_path}")
    logging.info(f"Blob size: {len(blob)} bytes "
                 f"(nonce:{DFU_GCM_NONCE_SIZE} + ct:{len(ciphertext)} + tag:{DFU_GCM_TAG_SIZE})")

    return {
        'output': output_path,
        'firmware_size': len(firmware),
        'blob_size': len(blob),
        'nonce': nonce.hex(),
        'tag': tag.hex(),
    }


def generate_full_image(input_path, output_path, bootloader_path,
                        app_address=None, key=None):
    """
    Generate combined BL+APP Intel HEX image for factory flashing.
    """
    try:
        from intelhex import IntelHex
    except ImportError:
        raise RuntimeError("Full image generation requires 'intelhex' package: "
                           "pip install intelhex")

    aes_key = key if key is not None else AES_KEY
    if app_address is None:
        app_address = DEFAULT_APP_ADDRESS

    if not os.path.exists(bootloader_path):
        raise FileNotFoundError(f"Bootloader file not found: {bootloader_path}")

    firmware = load_firmware(input_path)
    logging.info(f"Raw firmware size: {len(firmware)} bytes")

    firmware = pad_to_alignment(firmware, 8)
    firmware = patch_firmware(firmware, aes_key)

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


if __name__ == "__main__":
    parser = argparse.ArgumentParser(
        description="DFU image tool: sign, encrypt, or generate combined hex images.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
  # Sign firmware (patch IMAGE_SIZE + HMAC, no encryption)
  python dfu_image_tool.py sign -i firmware.bin -o firmware_signed.bin

  # Sign in-place
  python dfu_image_tool.py sign -i firmware.bin

  # Generate encrypted OTA blob
  python dfu_image_tool.py encrypt -i firmware.bin -o firmware_ota.bin

  # Generate combined BL+APP hex for factory flashing
  python dfu_image_tool.py full -i firmware.bin -o full_image.hex -b bootloader.hex

  # With custom AES key (hex string)
  python dfu_image_tool.py sign -i firmware.bin --key 2b7e151628aed2a6abf7158809cf4f3c
        """
    )

    subparsers = parser.add_subparsers(dest="command", required=True)

    # --- sign subcommand ---
    p_sign = subparsers.add_parser("sign", help="Sign firmware (HMAC, no encryption)")
    p_sign.add_argument("-i", "--input", required=True, help="Input .bin file")
    p_sign.add_argument("-o", "--output", help="Output .bin file (default: in-place)")
    p_sign.add_argument("--key", help="AES-128 key as hex string (32 hex chars)")

    # --- encrypt subcommand ---
    p_enc = subparsers.add_parser("encrypt", help="Sign + encrypt with AES-128-GCM")
    p_enc.add_argument("-i", "--input", required=True, help="Input .bin file")
    p_enc.add_argument("-o", "--output", required=True, help="Output encrypted blob")
    p_enc.add_argument("--key", help="AES-128 key as hex string (32 hex chars)")

    # --- full subcommand ---
    p_full = subparsers.add_parser("full", help="Generate combined BL+APP hex")
    p_full.add_argument("-i", "--input", required=True, help="Input firmware .bin")
    p_full.add_argument("-o", "--output", required=True, help="Output .hex file")
    p_full.add_argument("-b", "--bootloader", required=True, help="Bootloader .hex or .bin")
    p_full.add_argument("--app-addr", type=lambda x: int(x, 0),
                        default=DEFAULT_APP_ADDRESS,
                        help=f"App base address (default: 0x{DEFAULT_APP_ADDRESS:08X})")
    p_full.add_argument("--key", help="AES-128 key as hex string (32 hex chars)")

    args = parser.parse_args()

    # Parse AES key
    parsed_key = None
    if args.key:
        try:
            parsed_key = bytes.fromhex(args.key)
            if len(parsed_key) != 16:
                logging.error(f"AES key must be 16 bytes (32 hex chars), got {len(parsed_key)}")
                exit(1)
        except ValueError:
            logging.error(f"Invalid hex string for --key: {args.key}")
            exit(1)

    # Also check DFU_AES_KEY environment variable
    if parsed_key is None:
        env_key = os.environ.get('DFU_AES_KEY')
        if env_key:
            try:
                parsed_key = bytes.fromhex(env_key)
                if len(parsed_key) == 16:
                    logging.info("Using AES key from DFU_AES_KEY environment variable")
                else:
                    logging.warning(f"DFU_AES_KEY has wrong length ({len(parsed_key)}), using default")
                    parsed_key = None
            except ValueError:
                logging.warning("DFU_AES_KEY is not valid hex, using default")

    if args.command == "sign":
        result = sign_firmware(args.input, args.output, key=parsed_key)
    elif args.command == "encrypt":
        result = generate_dfu_blob(args.input, args.output, key=parsed_key)
    elif args.command == "full":
        result = generate_full_image(args.input, args.output, args.bootloader,
                                     app_address=args.app_addr, key=parsed_key)

    if result:
        logging.info("\n=== Summary ===")
        for k, v in result.items():
            logging.info(f"  {k}: {v}")
