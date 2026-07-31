#!/usr/bin/env python3
"""
Build a flashable binary blob containing TOC + all Opus files.
Output: opus_data.bin

Usage:
    python3 build_opus_bin.py <opus_dir> <output_bin>

This binary is embedded into firmware via EMBED_FILES.
On first boot, flash_audio_init() copies it to external SPI Flash.
"""

import argparse
import os
import struct
import sys

# Constants (match flash_audio.h)
FLASH_AUDIO_MAX_FILES = 32
FLASH_AUDIO_FILENAME_MAX = 64
FLASH_AUDIO_ENTRY_SIZE = 80
FLASH_AUDIO_TOC_MAGIC = 0x41444E50  # "PNDA"
FLASH_AUDIO_TOC_VERSION = 1
W25Q512_SECTOR_SIZE = 4096
FLASH_AUDIO_DATA_START = 0x001000


def build_toc(files):
    n = len(files)
    buf = bytearray()
    buf.extend(struct.pack("<III", FLASH_AUDIO_TOC_MAGIC,
                           FLASH_AUDIO_TOC_VERSION, n))
    for f in files:
        entry = bytearray(FLASH_AUDIO_ENTRY_SIZE)
        # Strip .opus extension for TOC name (max 63 bytes + null)
        display_name = f["name"]
        if display_name.endswith(".opus"):
            display_name = display_name[:-5]
        name_bytes = display_name.encode("utf-8")[:FLASH_AUDIO_FILENAME_MAX - 1]
        entry[0:len(name_bytes)] = name_bytes
        struct.pack_into("<III", entry, 64,
                         f["offset"], f["size"], f.get("sample_rate", 48000))
        # Estimated duration in ms (Opus ~48kbps mono → bytes/6 ≈ ms)
        duration_ms = f["size"] * 1000 // 6000
        struct.pack_into("<I", entry, 76, duration_ms)
        buf.extend(entry)
    # Pad to sector size
    if len(buf) < W25Q512_SECTOR_SIZE:
        buf.extend(b"\xFF" * (W25Q512_SECTOR_SIZE - len(buf)))
    return bytes(buf)


def get_opus_files(opus_dir):
    """Return ordered list of .opus filenames.
    If file_order.txt exists, use its order (line N = TOC index N).
    Otherwise fall back to alphabetical sorted().
    """
    all_files = {f for f in os.listdir(opus_dir) if f.endswith(".opus")}
    if not all_files:
        return []

    order_path = os.path.join(opus_dir, "file_order.txt")
    if os.path.exists(order_path):
        with open(order_path, "r", encoding="utf-8") as f:
            ordered = []
            for line in f:
                name = line.strip()
                if not name or name.startswith("#"):
                    continue
                if name in all_files:
                    ordered.append(name)
                    all_files.discard(name)
                else:
                    print(f"WARNING: file_order.txt lists '{name}' but file not found, skipping")
            # Append any .opus files not listed in file_order.txt
            if all_files:
                extra = sorted(all_files)
                print(f"WARNING: {len(extra)} .opus file(s) not in file_order.txt, "
                      f"appended at end: {extra}")
                ordered.extend(extra)
            return ordered

    # No file_order.txt — use alphabetical sort
    return sorted(all_files)


def main():
    parser = argparse.ArgumentParser(
        description="Build opus_data.bin for embedding in firmware")
    parser.add_argument("opus_dir", nargs="?",
                        default=None,
                        help="Directory with .opus files (default: ../opus_audio/)")
    parser.add_argument("output", nargs="?",
                        default=None,
                        help="Output binary (default: ../main/opus_data.bin)")
    args = parser.parse_args()

    script_dir = os.path.dirname(os.path.abspath(__file__))
    opus_dir = args.opus_dir
    if opus_dir is None:
        opus_dir = os.path.join(script_dir, "..", "opus_audio")
    output = args.output
    if output is None:
        output = os.path.join(script_dir, "..", "..", "flash_writer", "main", "opus_data.bin")

    opus_files = get_opus_files(opus_dir)
    if not opus_files:
        print(f"ERROR: No .opus files in {opus_dir}")
        sys.exit(1)

    print(f"Found {len(opus_files)} Opus files:")
    for i, fn in enumerate(opus_files):
        fpath = os.path.join(opus_dir, fn)
        fsize = os.path.getsize(fpath)
        print(f"  [{i}] {fn} ({fsize:,} bytes)")

    # Read all files, calculate offsets
    file_meta = []
    current_offset = 0
    total = 0

    for fn in opus_files:
        fpath = os.path.join(opus_dir, fn)
        fsize = os.path.getsize(fpath)
        total += fsize
        file_meta.append({
            "name": fn,
            "size": fsize,
            "offset": current_offset,
            "sample_rate": 48000,
        })
        current_offset += fsize

    print(f"\nBuilding opus_data.bin")
    print(f"  Files:  {len(opus_files)}")
    print(f"  Data:   {total:,} bytes ({total/1024:.1f} KB)")
    print(f"  Output: {output}")

    # Build TOC
    toc = build_toc(file_meta)

    # Concatenate: TOC + file data
    blob = bytearray(toc)

    for fn in opus_files:
        fpath = os.path.join(opus_dir, fn)
        with open(fpath, "rb") as f:
            blob.extend(f.read())

    bin_size = len(blob)
    print(f"  TOC:    {len(toc):,} bytes")
    print(f"  Total:  {bin_size:,} bytes ({bin_size/1024:.1f} KB)")

    with open(output, "wb") as f:
        f.write(blob)

    print(f"\nDone! Embed this by adding to CMakeLists.txt:")
    print(f"  EMBED_FILES opus_data.bin")
    print(f"Firmware size impact: +{bin_size:,} bytes ({bin_size/1024:.1f} KB)")


if __name__ == "__main__":
    main()
