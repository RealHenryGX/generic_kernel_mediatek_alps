#!/usr/bin/env python3
"""cleanup: 清除调试/实验痕迹, 措辞转为正式说明 (不改任何逻辑)

处理项:
  1. kernel/module.c  CAMDBG_NO_CRC        -> 正式注释
  2. kernel/module.c  CAMDBG_FORCE_VERMAGIC -> 正式注释
  3. mir3da_cust.c    allow_unverified_hw 的 MODULE_PARM_DESC "LAB ONLY" -> 正名

不改变:
  - CRC 跳过逻辑
  - vermagic 强制加载逻辑
  - allow_unverified_hw = true (A25 上已验证传感器可用, 门禁必须为 true)

清理后必须重新编译并刷入验证: WLAN/BT/传感器 三项功能不变。
"""
import sys

OK = True

# ---- 1&2: kernel/module.c 注释正名 ----
F = "kernel/module.c"
s = open(F, encoding="utf-8").read()
orig = s

old_crc = """\t/* CAMDBG_NO_CRC: vendor .ko carry their own __versions CRCs that were
\t * generated against the stock kernel. Our LOCALVERSION differs, so
\t * every symbol reports "disagrees about version". Skip the CRC
\t * comparison entirely - same effect as modprobe --force. Kernel is
\t * tainted TAINT_FORCED_MODULE by try_to_force_load below.
\t */"""
new_crc = """\t/* Stock vendor modules ship __versions CRCs generated against the
\t * stock kernel. This build's LOCALVERSION carries a -gravity-<commit>
\t * suffix (kernel identification), so those CRCs never match and every
\t * symbol reports "disagrees about version". Skip the CRC comparison:
\t * equivalent to modprobe --force. try_to_force_load() below taints
\t * the kernel TAINT_FORCED_MODULE.
\t */"""

if old_crc in s:
    s = s.replace(old_crc, new_crc, 1)
    print("  [ok] CAMDBG_NO_CRC 注释已正名")
elif "CAMDBG_NO_CRC" in s:
    print("  [WARN] CAMDBG_NO_CRC 存在但文本不匹配")
    OK = False
else:
    print("  [skip] CAMDBG_NO_CRC 已清理")

old_ver = """\t\t/* CAMDBG_FORCE_VERMAGIC: LOCALVERSION carries -gravity-<commit>
\t\t * for kernel identification, so stock vendor modules whose
\t\t * vermagic is plain "4.19.191" never match. Android's Toybox
\t\t * insmod/modprobe cannot pass MODULE_INIT_IGNORE_VERMAGIC, so
\t\t * attempt the forced-load path here instead. Succeeds only
\t\t * when CONFIG_MODULE_FORCE_LOAD=y (taints the kernel).
\t\t */"""
new_ver = """\t\t/* LOCALVERSION carries -gravity-<commit> for kernel
\t\t * identification, so stock vendor modules whose vermagic is
\t\t * plain "4.19.191" never match. Android's Toybox
\t\t * insmod/modprobe cannot pass MODULE_INIT_IGNORE_VERMAGIC,
\t\t * so take the forced-load path here instead. Succeeds only
\t\t * when CONFIG_MODULE_FORCE_LOAD=y (taints the kernel).
\t\t */"""

if old_ver in s:
    s = s.replace(old_ver, new_ver, 1)
    print("  [ok] CAMDBG_FORCE_VERMAGIC 注释已正名")
elif "CAMDBG_FORCE_VERMAGIC" in s:
    print("  [WARN] CAMDBG_FORCE_VERMAGIC 存在但文本不匹配")
    OK = False
else:
    print("  [skip] CAMDBG_FORCE_VERMAGIC 已清理")

if s != orig:
    open(F, "w", encoding="utf-8").write(s)
    print("  [ok] module.c 已写入")

# ---- 3: mir3da 门禁描述正名 ----
F2 = "drivers/misc/mediatek/sensors-1.0/accelerometer/mir3da/mir3da_cust.c"
s2 = open(F2, encoding="utf-8").read()
old_desc = 'MODULE_PARM_DESC(allow_unverified_hw,\n\t\t "LAB ONLY: permit unverified Teracube register profile");'
new_desc = 'MODULE_PARM_DESC(allow_unverified_hw,\n\t\t "permit unverified register profile (required on A25)");'
if old_desc in s2:
    s2 = s2.replace(old_desc, new_desc, 1)
    open(F2, "w", encoding="utf-8").write(s2)
    print("  [ok] allow_unverified_hw 描述已正名")
else:
    # 宽松匹配
    import re
    m = re.search(r'MODULE_PARM_DESC\(allow_unverified_hw,\s*\n?\s*"[^"]*"\);', s2)
    if m:
        s2 = s2[:m.start()] + 'MODULE_PARM_DESC(allow_unverified_hw,\n\t\t "permit unverified register profile (required on A25)");' + s2[m.end():]
        open(F2, "w", encoding="utf-8").write(s2)
        print("  [ok] allow_unverified_hw 描述已正名 (宽松匹配)")
    else:
        print("  [WARN] 找不到 allow_unverified_hw 的 MODULE_PARM_DESC")
        OK = False

print("\n  清理结果:", "OK" if OK else "有警告, 请检查")
sys.exit(0 if OK else 1)
