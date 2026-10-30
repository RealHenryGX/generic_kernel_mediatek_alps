#!/usr/bin/env python3
"""Repack Android boot image: 用新 kernel 重新紧凑布局 kernel/ramdisk/dtb 段。

Usage: repack_boot.py <stock_boot.img> <new_kernel[.gz]> <output.img>

要点:
- 各段(page 对齐)位置由 header 里的 size 字段决定 ⇒ 换 kernel 后 ramdisk/dtb 必须跟着前移,
  否则 LK 会按 header 去读 dtb、读到旧偏移上的垃圾 ⇒ 内核拿到坏设备树 ⇒ 第一屏后重启循环。
- 分区大小与分区末尾内容(AVB footer 等绝对位置数据)原样保留,只更新 header 的 kernel_size。
"""
import struct, sys


def align(v, page):
    return ((v + page - 1) // page) * page


def main():
    src, kernel, dst = sys.argv[1], sys.argv[2], sys.argv[3]
    d = open(src, 'rb').read()
    if d[0:8] != b'ANDROID!':
        sys.exit('not an android bootimg')

    kernel_size = struct.unpack_from('<I', d, 8)[0]
    ramdisk_size = struct.unpack_from('<I', d, 16)[0]
    page_size = struct.unpack_from('<I', d, 36)[0]
    hdr_ver = struct.unpack_from('<I', d, 40)[0]
    dtb_size = struct.unpack_from('<I', d, 1648)[0] if hdr_ver >= 2 else 0

    new_k = open(kernel, 'rb').read()

    # 源偏移:按 stock header 声明的 size 紧凑对齐
    k_src = page_size
    r_src = k_src + align(kernel_size, page_size)
    d_src = r_src + align(ramdisk_size, page_size)
    # 目标偏移:按新 kernel 的实际大小
    k_dst = page_size
    r_dst = k_dst + align(len(new_k), page_size)
    d_dst = r_dst + align(ramdisk_size, page_size)

    need = d_dst + align(dtb_size, page_size)
    if need > len(d):
        sys.exit('layout overflow: need %d > partition %d' % (need, len(d)))

    print('page=%d hdr=v%d  kernel %d -> %d' % (page_size, hdr_ver, kernel_size, len(new_k)))
    print('  ramdisk @%d -> @%d (size %d)' % (r_src, r_dst, ramdisk_size))
    print('  dtb     @%d -> @%d (size %d)' % (d_src, d_dst, dtb_size))

    out = bytearray(d)                 # 保留 header 与分区尾部(AVB footer 等在末尾的绝对位置)
    for i in range(page_size, need):   # 清掉旧的段区
        out[i] = 0
    struct.pack_into('<I', out, 8, len(new_k))          # kernel_size
    out[k_dst:k_dst + len(new_k)] = new_k
    out[r_dst:r_dst + ramdisk_size] = d[r_src:r_src + ramdisk_size]
    if dtb_size:
        out[d_dst:d_dst + dtb_size] = d[d_src:d_src + dtb_size]

    open(dst, 'wb').write(out)
    print('wrote %s (%d bytes)' % (dst, len(out)))


if __name__ == '__main__':
    main()
