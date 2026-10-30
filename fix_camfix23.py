#!/usr/bin/env python3
"""camfix23: 打开 CONFIG_DEBUG_FS，用于运行时诊断

目的:
  1. /sys/kernel/debug/pinctrl/*/pinmux-pins -> 查 GPIO81(SCL)/GPIO84(SDA) 归属
  2. /sys/kernel/debug/regulator/regulator_summary -> 查 gsensor 供电请求
  3. /sys/kernel/debug/clk/clk_summary -> 查 i2c1 时钟

不改任何驱动逻辑, 仅打开调试文件系统。
"""
import pathlib, sys

D = pathlib.Path("arch/arm64/configs/a25_gravity_compile_audit_defconfig")
s = D.read_text(encoding="utf-8")

old = "# CONFIG_DEBUG_FS is not set"
if old not in s:
    if "CONFIG_DEBUG_FS=y" in s:
        print("  [skip] DEBUG_FS 已开启")
        sys.exit(0)
    print("  [FAIL] 找不到 DEBUG_FS 行")
    sys.exit(1)

s = s.replace(old, "CONFIG_DEBUG_FS=y", 1)
D.write_text(s, encoding="utf-8")
print("  [ok] CONFIG_DEBUG_FS=y 已写入 compile-audit defconfig")
for i, l in enumerate(D.read_text(encoding="utf-8").split("\n")):
    if "DEBUG_FS" in l:
        print("  行 %d: %s" % (i + 1, l.strip()))
