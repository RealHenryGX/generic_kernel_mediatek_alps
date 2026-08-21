#!/usr/bin/env python3
"""Repack Android boot image: replace kernel segment in-place, keep partition size & AVB tail.
Usage: repack_boot.py <stock_boot.img> <new_kernel[.gz]> <output.img>
Requires new_kernel <= original kernel slot (aligned to page). Preserves ramdisk/dtb/AVB footer.
"""
import struct, sys

def align(v, page):
    return ((v + page - 1) // page) * page

def main():
    src, kernel, dst = sys.argv[1], sys.argv[2], sys.argv[3]
    with open(src, 'rb') as f:
        d = f.read()
    if d[0:8] != b'ANDROID!':
        sys.exit('not an android bootimg')

    kernel_size, kernel_addr = struct.unpack_from('<II', d, 8)
    ramdisk_size, ramdisk_addr = struct.unpack_from('<II', d, 16)
    second_size, second_addr = struct.unpack_from('<II', d, 24)
    tags_addr = struct.unpack_from('<I', d, 32)[0]
    page_size = struct.unpack_from('<I', d, 36)[0]
    hdr_ver = struct.unpack_from('<I', d, 40)[0]
    os_version = struct.unpack_from('<I', d, 44)[0]
    hdr_len = 1660 if hdr_ver >= 2 else (1632 if hdr_ver >= 1 else 512)

    with open(kernel, 'rb') as f:
        new_k = f.read()

    # kernel slot: starts at page_size, occupies align(kernel_size, page_size) bytes
    kslot = page_size
    kslot_end = kslot + align(kernel_size, page_size)
    if len(new_k) > kslot_end - kslot:
        sys.exit(f'new kernel {len(new_k)} larger than slot {kslot_end-kslot}; need re-layout, abort')

    print(f'header v{hdr_ver} page={page_size} k={kernel_size}@{hex(kernel_addr)} r={ramdisk_size}@{hex(ramdisk_addr)} s={second_size} slot={kslot}..{kslot_end}')

    out = bytearray(d)  # copy whole partition incl. AVB tail
    # update header kernel_size
    struct.pack_into('<I', out, 8, len(new_k))
    # write new kernel in-place
    out[kslot:kslot+len(new_k)] = new_k
    # zero rest of kernel slot (in case new kernel shorter)
    for i in range(kslot+len(new_k), kslot_end):
        out[i] = 0

    with open(dst, 'wb') as f:
        f.write(out)
    print(f'wrote {dst}: {len(out)} bytes (kernel {len(new_k)}, tail preserved)')

if __name__ == '__main__':
    main()
