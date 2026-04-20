#!/usr/bin/env python3
"""
Merge a SoftDevice hex + application hex into a single UF2 file.

Usage:
  python3 make_combined_uf2.py --app APP.hex --sd SD.hex --out OUTPUT.uf2 [--family 0x239a00b3]
"""

import argparse
import struct
import sys


def read_hex(path, lo=0x00000000, hi=0xFFFFFFFF):
    """Parse an Intel HEX file (type-02 and type-04 addressing) into {addr: byte}."""
    data = {}
    upper = 0
    with open(path) as f:
        for line in f:
            line = line.strip()
            if not line.startswith(':'):
                continue
            length  = int(line[1:3],  16)
            offset  = int(line[3:7],  16)
            rectype = int(line[7:9],  16)
            if rectype == 1:    # EOF
                break
            elif rectype == 2:  # Extended Segment Address
                upper = int(line[9:13], 16) << 4
            elif rectype == 4:  # Extended Linear Address
                upper = int(line[9:13], 16) << 16
            elif rectype == 0:  # Data
                base = upper + offset
                for i in range(length):
                    addr = base + i
                    if lo <= addr < hi:
                        data[addr] = int(line[9 + i*2 : 11 + i*2], 16)
    return data


def write_uf2(data, path, family):
    """Write {addr: byte} dict to a UF2 file."""
    if not data:
        print("ERROR: no data to write", file=sys.stderr)
        sys.exit(1)

    addrs = sorted(data)
    blocks = []
    addr = (addrs[0] >> 8) << 8          # align down to 256-byte boundary
    end  = ((addrs[-1] >> 8) + 1) << 8   # align up

    while addr < end:
        chunk = bytearray(256)
        has_data = False
        for i in range(256):
            if addr + i in data:
                chunk[i] = data[addr + i]
                has_data = True
            else:
                chunk[i] = 0xFF           # unwritten flash = 0xFF
        if has_data:
            blocks.append((addr, chunk))
        addr += 256

    total = len(blocks)
    with open(path, 'wb') as f:
        for n, (blk_addr, chunk) in enumerate(blocks):
            blk = bytearray(512)
            struct.pack_into('<IIII', blk,  0, 0x0A324655, 0x9E5D5157, 0x00002000, blk_addr)
            struct.pack_into('<IIII', blk, 16, 256, n, total, family)
            blk[32:288] = chunk
            struct.pack_into('<I',   blk, 508, 0x0AB16F30)
            f.write(blk)

    print(f"  {total} blocks  "
          f"{blocks[0][0]:#010x} – {blocks[-1][0] + 255:#010x}  "
          f"({total * 512 // 1024} KB)  →  {path}")


def main():
    parser = argparse.ArgumentParser(description="Merge SoftDevice + app hex into UF2")
    parser.add_argument("--app",    required=True,  help="Application .hex file")
    parser.add_argument("--sd",     required=True,  help="SoftDevice .hex file (or combined bootloader hex)")
    parser.add_argument("--out",    required=True,  help="Output .uf2 file")
    parser.add_argument("--family", default="0x239a00b3",
                        help="UF2 family ID (default: 0x239a00b3 for nRF52840)")
    args = parser.parse_args()

    family = int(args.family, 16) if args.family.startswith("0x") else int(args.family)

    print(f"Reading SoftDevice  (0x001000–0x025FFF): {args.sd}")
    sd = read_hex(args.sd, lo=0x001000, hi=0x026000)
    print(f"  {len(sd):,} bytes")

    print(f"Reading application (0x026000–0x0DFFFF): {args.app}")
    app = read_hex(args.app, lo=0x026000, hi=0x0E0000)
    print(f"  {len(app):,} bytes")

    merged = {**sd, **app}
    print(f"Writing combined UF2 ({len(merged):,} bytes total):")
    write_uf2(merged, args.out, family)


if __name__ == "__main__":
    main()
