#!/usr/bin/env python3
#
# Copyright (c) 2024 Apple Inc. All rights reserved.
#
# @APPLE_LICENSE_HEADER_START@
#
# This file contains Original Code and/or Modifications of Original Code
# as defined in and that are subject to the Apple Public Source License
# Version 2.0 (the 'License'). You may not use this file except in
# compliance with the License. Please obtain a copy of the License at
# http://www.opensource.apple.com/apsl/ and read it before using this
# file.
#
# The Original Code and all software distributed under the License are
# distributed on an 'AS IS' basis, WITHOUT WARRANTY OF ANY KIND, EITHER
# EXPRESS OR IMPLIED, AND APPLE HEREBY DISCLAIMS ALL SUCH WARRANTIES,
# INCLUDING WITHOUT LIMITATION, ANY WARRANTIES OF MERCHANTABILITY,
# FITNESS FOR A PARTICULAR PURPOSE, QUIET ENJOYMENT OR NON-INFRINGEMENT.
# Please see the License for the specific language governing rights and
# limitations under the License.
#
# @APPLE_LICENSE_HEADER_END@

"""
archive-crcer: Stamps CRC32c checksums into AppleArchive files for testing.

This tool scans an AppleArchive file for entries containing the
"com.apple.dyld.crc32c" extended attribute and fills in the record
length and CRC32c checksum fields.

Usage:
    archive-crcer.py <archive.aa>

The archive should be created with placeholder xattr data:
    1. Create files with: xattr -wx com.apple.dyld.crc32c "00 00 00 00 00 00 00 00" <file>
    2. Archive with: aa archive -d <dir> -o <out.aa> -a raw \\
         -exclude-field all -include-field typ,pat,dat,lnk,xat
    3. Run this tool to stamp the CRC32c values: ./archive-crcer.py <out.aa>
"""

import mmap
import struct
import sys

# CRC32c (Castagnoli) lookup table - polynomial 0x1EDC6F41 (reflected: 0x82F63B78)
# Generated at module load time
def _generate_crc32c_table():
    table = []
    for i in range(256):
        crc = i
        for _ in range(8):
            if crc & 1:
                crc = (crc >> 1) ^ 0x82F63B78
            else:
                crc >>= 1
        table.append(crc)
    return table

_CRC32C_TABLE = _generate_crc32c_table()

def crc32c(data: bytes, crc: int = 0) -> int:
    """Compute CRC32c checksum of data."""
    crc = crc ^ 0xFFFFFFFF
    for byte in data:
        crc = _CRC32C_TABLE[(crc ^ byte) & 0xFF] ^ (crc >> 8)
    return crc ^ 0xFFFFFFFF

XATTR_MARKER = b"com.apple.dyld.crc32c"
AA_MAGIC = b"AA01"

def process_archive(filepath: str) -> int:
    """Process an AppleArchive file and stamp CRC32c checksums."""
    with open(filepath, "r+b") as f:
        with mmap.mmap(f.fileno(), 0) as mm:
            pos = 0
            while pos < len(mm):
                # Verify AA01 magic
                if mm[pos:pos+4] != AA_MAGIC:
                    print(f"Malformed buffer: expected AA01 magic at offset 0x{pos:x}",
                          file=sys.stderr)
                    return 2

                # Look for our xattr in this record
                remaining = mm[pos:]
                xattr_offset = remaining.find(XATTR_MARKER)

                if xattr_offset == -1:
                    # No xattr found, skip to next AA01 record
                    next_record = remaining[1:].find(AA_MAGIC)
                    if next_record == -1:
                        skip_len = len(remaining)
                    else:
                        skip_len = next_record + 1
                    print(f"Skipping record at offset 0x{pos:x} (no crc32c xattr)")
                    pos += skip_len
                    continue

                # Found xattr - position after null terminator
                record_end_offset = xattr_offset + len(XATTR_MARKER) + 1  # +1 for null
                record_len_exclusive = record_end_offset

                print(f"Processing record at offset 0x{pos:x}, length {record_len_exclusive}")

                # Write record length (4 bytes after xattr string null terminator)
                # Per AppleArchive convention, length is INCLUSIVE of the length and CRC fields
                length_pos = pos + record_end_offset
                record_len_inclusive = record_len_exclusive + 8  # +8 for length field (4) + CRC field (4)
                mm[length_pos:length_pos+4] = struct.pack('<I', record_len_inclusive)
                print(f"  Wrote length ({record_len_inclusive}) at offset 0x{length_pos:x}")

                # Compute and write CRC32c (includes the length field we just wrote)
                # For magic residue validation: CRC(data + CRC(data)) = magic_residue
                crc_pos = length_pos + 4
                crc_data = mm[pos:crc_pos]
                crc = crc32c(crc_data)
                mm[crc_pos:crc_pos+4] = struct.pack('<I', crc)
                print(f"  Wrote CRC32c (0x{crc:08x}) at offset 0x{crc_pos:x}")

                # Move to next record
                pos += record_len_inclusive

    print("Done.")
    return 0

def main():
    if len(sys.argv) != 2:
        print(f"Usage: {sys.argv[0]} <archive.aa>", file=sys.stderr)
        return 1

    return process_archive(sys.argv[1])

if __name__ == "__main__":
    sys.exit(main())
