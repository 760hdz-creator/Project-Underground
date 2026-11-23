"""Extract an Xbox (XDVDFS / XISO) image.

Usage: python -I tools/extract_xiso.py <image.iso> <out_dir>
"""
import os
import struct
import sys

SECTOR = 2048
MAGIC = b"MICROSOFT*XBOX*MEDIA"
# Game partition offsets: trimmed xiso, then full redump image.
PARTITION_OFFSETS = (0, 0x18300000, 0x0FD90000, 0x02080000)


def find_partition(f):
    for base in PARTITION_OFFSETS:
        f.seek(base + 32 * SECTOR)
        if f.read(len(MAGIC)) == MAGIC:
            return base
    raise SystemExit("XDVDFS magic not found")


def read_dir(f, base, sector, size, out, rel=""):
    f.seek(base + sector * SECTOR)
    table = f.read(size)
    seen = set()
    stack = [0]
    while stack:
        off = stack.pop()
        if off in seen or off + 14 > len(table):
            continue
        seen.add(off)
        left, right, start, length, attr, nlen = struct.unpack_from("<HHIIBB", table, off)
        if left == 0xFFFF and right == 0xFFFF:
            continue  # sector padding
        name = table[off + 14 : off + 14 + nlen].decode("latin-1")
        path = os.path.join(rel, name)
        if attr & 0x10:
            os.makedirs(os.path.join(out, path), exist_ok=True)
            if length:
                read_dir(f, base, start, length, out, path)
        else:
            f.seek(base + start * SECTOR)
            with open(os.path.join(out, path), "wb") as o:
                left_bytes = length
                while left_bytes:
                    chunk = f.read(min(left_bytes, 1 << 20))
                    if not chunk:
                        raise SystemExit(f"short read in {path}")
                    o.write(chunk)
                    left_bytes -= len(chunk)
            print(f"{length:>11} {path}")
        if left:
            stack.append(left * 4)
        if right:
            stack.append(right * 4)


def main():
    if len(sys.argv) != 3:
        raise SystemExit(__doc__)
    image, out = sys.argv[1], sys.argv[2]
    os.makedirs(out, exist_ok=True)
    with open(image, "rb") as f:
        base = find_partition(f)
        f.seek(base + 32 * SECTOR + len(MAGIC))
        root_sector, root_size = struct.unpack("<II", f.read(8))
        print(f"partition at 0x{base:X}, root sector {root_sector}, size {root_size}")
        read_dir(f, base, root_sector, root_size, out)


if __name__ == "__main__":
    main()
