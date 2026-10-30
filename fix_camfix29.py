#!/usr/bin/env python3
"""camfix29: 跳过模块符号 CRC 校验

camfix28 解掉了 vermagic, 现在卡在符号 CRC:
  wmt_drv: disagrees about version of symbol netlink_unicast
  wmt_drv: disagrees about version of symbol regmap_read
  ...
原因: check_version() 的 CRC 校验由模块自身的 __versions section 触发
      (versindex != 0 就进比较循环), 与 CONFIG_MODVERSIONS 无关。

做法: check_version() 在 !crc 的提前返回之后直接返回 1, 等价于
      modprobe --force 的效果 —— 不比较 CRC, 内核打 TAINT_FORCED_MODULE。

风险: 若模块与内核 ABI 真实不兼容会崩溃。两内核同为 4.19.191 / MT6765 /
      同 ALPS 树, 结构体布局应一致; 异常则刷回 boot 备份。
"""
import sys

F = "kernel/module.c"
s = open(F, encoding="utf-8").read()

if "CAMDBG_NO_CRC" in s:
    print("  [skip] 补丁已存在")
    sys.exit(0)

old = """\t/* Exporting module didn't supply crcs?  OK, we're already tainted. */
\tif (!crc)
\t\treturn 1;

\t/* No versions at all?  modprobe --force does this. */
\tif (versindex == 0)
\t\treturn try_to_force_load(mod, symname) == 0;
"""

new = """\t/* Exporting module didn't supply crcs?  OK, we're already tainted. */
\tif (!crc)
\t\treturn 1;

\t/* CAMDBG_NO_CRC: vendor .ko carry their own __versions CRCs that were
\t * generated against the stock kernel. Our LOCALVERSION differs, so
\t * every symbol reports "disagrees about version". Skip the CRC
\t * comparison entirely - same effect as modprobe --force. Kernel is
\t * tainted TAINT_FORCED_MODULE by try_to_force_load below.
\t */
\tif (try_to_force_load(mod, symname) == 0)
\t\treturn 1;

\t/* No versions at all?  modprobe --force does this. */
\tif (versindex == 0)
\t\treturn try_to_force_load(mod, symname) == 0;
"""

if s.count(old) != 1:
    print("  [FAIL] 命中 %d 次" % s.count(old))
    sys.exit(1)

s = s.replace(old, new, 1)
open(F, "w", encoding="utf-8").write(s)
print("  [ok] check_version 已跳过 CRC 校验")
k = s.find("CAMDBG_NO_CRC")
print(s[max(0, k - 180):k + 780])
