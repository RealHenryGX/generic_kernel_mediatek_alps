#!/usr/bin/env python3
"""camfix24: 打开 CONFIG_I2C_CHARDEV, 提供 /dev/i2c-* 用于用户态实测

目的: 用 i2cget 直接读 bus1 addr 0x26 reg 0x01(WHO_AM_I), 取得决定性判据:
  - 读到 0x13  -> 总线正常, 问题在驱动代码
  - 读失败     -> 总线层面不通, 转向硬件/上电序列
仅增加用户态接口, 不改驱动逻辑。
"""
import pathlib, sys

D = pathlib.Path("arch/arm64/configs/a25_gravity_compile_audit_defconfig")
s = D.read_text(encoding="utf-8")

if "CONFIG_I2C_CHARDEV=y" in s:
    print("  [skip] I2C_CHARDEV 已开启")
    sys.exit(0)

old = "# CONFIG_I2C_CHARDEV is not set"
if old in s:
    s = s.replace(old, "CONFIG_I2C_CHARDEV=y", 1)
    D.write_text(s, encoding="utf-8")
    print("  [ok] CONFIG_I2C_CHARDEV=y 已写入")
else:
    # 没有这一行, 追加
    s = s.rstrip("\n") + "\nCONFIG_I2C_CHARDEV=y\n"
    D.write_text(s, encoding="utf-8")
    print("  [ok] CONFIG_I2C_CHARDEV=y 已追加")

for i, l in enumerate(D.read_text(encoding="utf-8").split("\n")):
    if "I2C_CHARDEV" in l or "DEBUG_FS" in l:
        print("  行 %d: %s" % (i + 1, l.strip()))
