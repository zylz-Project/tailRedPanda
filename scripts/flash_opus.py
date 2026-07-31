#!/usr/bin/env python3
"""
Flash Opus Audio Files to W25Q512 SPI Flash via WiFi HTTP API.

Usage:
    python3 flash_opus.py <opus_dir> [--host HOST]

Example:
    python3 flash_opus.py /path/to/opus_compressed/
    python3 flash_opus.py /path/to/opus_compressed/ --host 192.168.4.1

Requirements:
    pip install requests
"""

import argparse
import os
import struct
import sys
import time

try:
    import requests
except ImportError:
    print("ERROR: requests library not found. Install with: pip install requests")
    sys.exit(1)

# ============================================================================
# Constants (match flash_audio.h)
# ============================================================================
FLASH_AUDIO_MAX_FILES = 32
FLASH_AUDIO_FILENAME_MAX = 64
FLASH_AUDIO_ENTRY_SIZE = 80  # 64 + 4*4
FLASH_AUDIO_TOC_MAGIC = 0x41444E50  # "PNDA"
FLASH_AUDIO_TOC_VERSION = 1
FLASH_AUDIO_TOC_SECTOR = 0
FLASH_AUDIO_DATA_START = 0x001000
W25Q512_SECTOR_SIZE = 4096


def build_toc(files):
    """Build TOC binary from file metadata list."""
    n = len(files)
    toc = bytearray()
    toc.extend(struct.pack("<III", FLASH_AUDIO_TOC_MAGIC, FLASH_AUDIO_TOC_VERSION, n))

    for f in files:
        entry = bytearray(FLASH_AUDIO_ENTRY_SIZE)
        # Filename (64 bytes, null-padded)
        name_bytes = f["name"].encode("utf-8")[: FLASH_AUDIO_FILENAME_MAX - 1]
        entry[0 : len(name_bytes)] = name_bytes
        # Offset, Size, Sample Rate
        struct.pack_into("<III", entry, 64, f["offset"], f["size"], f["sample_rate"])
        toc.extend(entry)

    # Pad to sector size
    if len(toc) < W25Q512_SECTOR_SIZE:
        toc.extend(b"\xFF" * (W25Q512_SECTOR_SIZE - len(toc)))
    return bytes(toc)


def upload_file(host, filename, data, sample_rate=48000):
    """Upload a single Opus file to ESP32 via HTTP API."""
    import io

    url = f"http://{host}/api/flash/upload"

    # Build multipart form data
    boundary = "----FlashOpusUploadBoundary"
    body = io.BytesIO()
    body.write(f"--{boundary}\r\n".encode())
    body.write(
        f'Content-Disposition: form-data; name="file"; filename="{filename}"\r\n'.encode()
    )
    body.write(b"Content-Type: application/octet-stream\r\n\r\n")
    body.write(data)
    body.write(f"\r\n--{boundary}--\r\n".encode())

    headers = {"Content-Type": f"multipart/form-data; boundary={boundary}"}
    body.seek(0)

    print(f"  Uploading: {filename} ({len(data)} bytes)...", end=" ", flush=True)
    try:
        resp = requests.post(url, data=body, headers=headers, timeout=60)
        if resp.status_code == 200 and resp.text.strip() == "OK":
            print("OK")
            return True
        else:
            print(f"FAILED ({resp.status_code}: {resp.text[:100]})")
            return False
    except Exception as e:
        print(f"FAILED ({e})")
        return False


def erase_all(host):
    """Erase all audio files on the ESP32 SPI Flash."""
    url = f"http://{host}/api/flash/erase"
    print("Erasing all audio files...", end=" ", flush=True)
    try:
        resp = requests.post(url, timeout=10)
        if resp.status_code == 200:
            print("OK")
            return True
        else:
            print(f"FAILED ({resp.status_code})")
            return False
    except Exception as e:
        print(f"FAILED ({e})")
        return False


def get_status(host):
    """Get current flash audio status from ESP32."""
    url = f"http://{host}/api/flash/status"
    try:
        resp = requests.get(url, timeout=5)
        return resp.json()
    except Exception as e:
        print(f"Failed to get status: {e}")
        return None


def main():
    parser = argparse.ArgumentParser(
        description="Flash Opus audio files to W25Q512 SPI Flash via WiFi"
    )
    parser.add_argument(
        "opus_dir", nargs="?", default=None,
        help="Directory containing .opus files to upload (default: ../opus_audio/)"
    )
    parser.add_argument(
        "--host", default="192.168.4.1", help="ESP32 IP address (default: 192.168.4.1)"
    )
    parser.add_argument(
        "--erase", action="store_true", help="Erase all existing files before uploading"
    )
    parser.add_argument(
        "--verify", action="store_true", help="Verify after upload"
    )
    args = parser.parse_args()

    opus_dir = args.opus_dir
    if opus_dir is None:
        # Default to the project's opus_audio/ directory
        script_dir = os.path.dirname(os.path.abspath(__file__))
        opus_dir = os.path.join(script_dir, "..", "opus_audio")
    if not os.path.isdir(opus_dir):
        print(f"ERROR: Directory not found: {opus_dir}")
        sys.exit(1)

    # Find all .opus files
    opus_files = sorted(
        [f for f in os.listdir(opus_dir) if f.endswith(".opus")]
    )
    if not opus_files:
        print(f"ERROR: No .opus files found in {opus_dir}")
        sys.exit(1)

    print(f"Found {len(opus_files)} Opus files:")
    total_size = 0
    file_info = []
    for fn in opus_files:
        fpath = os.path.join(opus_dir, fn)
        fsize = os.path.getsize(fpath)
        total_size += fsize
        print(f"  {fn}: {fsize:,} bytes ({fsize/1024:.1f} KB)")
        file_info.append({"path": fpath, "name": fn, "size": fsize})

    print(f"\nTotal: {total_size:,} bytes ({total_size/1024:.1f} KB / {total_size/1024/1024:.1f} MB)")
    print(f"Available: 64 MB on W25Q512")

    if total_size > 60 * 1024 * 1024:
        print("WARNING: Total size exceeds available space (accounting for alignment)")
        if input("Continue? [y/N] ").strip().lower() != "y":
            sys.exit(1)

    # Check connection
    print(f"\nConnecting to ESP32 at {args.host}...")
    status = get_status(args.host)
    if status is None:
        print(f"ERROR: Cannot connect to ESP32 at http://{args.host}")
        print("Make sure the ESP32 is powered on and connected to WiFi.")
        print(f"  SSID: (check config.h)")
        print(f"  URL:  http://{args.host}")
        sys.exit(1)
    print(f"Connected. Currently {status.get('count', 0)} files on flash.")

    # Erase if requested
    if args.erase:
        if not erase_all(args.host):
            print("ERROR: Erase failed")
            sys.exit(1)
        time.sleep(1)

    # Upload files one by one using the HTTP API
    print("\nUploading files...")
    success_count = 0
    fail_count = 0
    for fi in file_info:
        with open(fi["path"], "rb") as f:
            data = f.read()
        if upload_file(args.host, fi["name"], data):
            success_count += 1
        else:
            fail_count += 1
        time.sleep(0.5)  # Small delay between uploads

    print(f"\nUpload complete: {success_count} succeeded, {fail_count} failed")

    if args.verify:
        print("\nVerifying...")
        time.sleep(2)
        status = get_status(args.host)
        if status:
            count = status.get("count", 0)
            total = status.get("total_size", 0)
            print(f"Flash has {count} files, {total:,} bytes total")
            if count == len(opus_files):
                print("Verification PASSED")
            else:
                print(f"Verification FAILED: expected {len(opus_files)} files, got {count}")

    print("\nDone! Use http://{}/flash to manage files via web UI.".format(args.host))


if __name__ == "__main__":
    main()
