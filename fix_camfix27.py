#!/usr/bin/env python3
"""camfix27: 解决原厂 .ko 无法加载的问题 (WLAN/BT 修复)

根因:
  1. CONFIG_MODVERSIONS=y -> 原厂 .ko 的符号 CRC 与本内核不一致,
     报 "disagrees about version of symbol genl_*"
  2. vermagic 不匹配 -> 本内核带 -gravity-g53d31dab5 后缀,
     原厂 .ko 是 4.19.191, 无法直接 insmod

做法:
  - 关闭 CONFIG_MODVERSIONS (不再校验符号版本)
  - 打开 CONFIG_MODULE_FORCE_LOAD (允许 insmod --force 忽略 vermagic)
  仅改这两个选项, 不动 LOCALVERSION (用户要求保留 gravity 标识)。

加载方式变更: 不能用 modprobe 常规加载, 需 modprobe --force 或 insmod --force。
"""
import pathlib, sys

D = pathlib.Path("arch/arm64/configs/a25_gravity_compile_audit_defconfig")
s = D.read_text(encoding="utf-8")
orig = s

changes = [
    ("# CONFIG_MODVERSIONS is not set", None),        # 确认状态
    ("CONFIG_MODVERSIONS=y",            "# CONFIG_MODVERSIONS is not set"),
    ("# CONFIG_MODULE_FORCE_LOAD is not set", "CONFIG_MODULE_FORCE_LOAD=y"),
]

applied = []
for old, new in changes:
    if new is None:
        if old in s:
            print("  [info] 当前: " + old)
        continue
    if new.replace("# ", "").replace(" is not set", "") in s and new.startswith("#"):
        pass
    if old in s:
        s = s.replace(old, new, 1)
        applied.append("%s -> %s" % (old, new))
    else:
        print("  [warn] 未找到: " + old)

if not applied:
    print("  [skip] 无改动")
    sys.exit(0)

D.write_text(s, encoding="utf-8")
for a in applied:
    print("  [ok] " + a)

print("\n  当前相关项:")
for i, l in enumerate(D.read_text(encoding="utf-8").split("\n")):
    if "MODVERSIONS" in l or "MODULE_FORCE_LOAD" in l or "LOCALVERSION" in l:
        print("    行 %d: %s" % (i + 1, l.strip()))
