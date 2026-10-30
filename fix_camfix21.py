#!/usr/bin/env python3
"""camfix21: 打开 mir3da 的 allow_unverified_hw 门禁

背景: 之前移植 mir3da 驱动时, 寄存器配置来自 Teracube 设备、未经 A25 验证,
     所以加了 module_param 门禁默认关闭。结果 probe 返回 -EPERM, 连带
     accel 框架整体初始化失败 -> 全部传感器不可用。

依据 A25 硬件确认为 MIR3DA (原厂内核 dmesg: mir3da_probe chip_id 0x13),
打开门禁。probe 后要通过 chip_id 读数验证寄存器配置是否真的匹配。
"""
import sys

F = ("drivers/misc/mediatek/sensors-1.0/accelerometer/mir3da/mir3da_cust.c")
s = open(F, encoding="utf-8").read()

old = "static bool allow_unverified_hw;"
new = "static bool allow_unverified_hw = true;"

if new in s:
    print("  [skip] 门禁已开启")
    sys.exit(0)
if s.count(old) != 1:
    print("  [FAIL] 门禁变量命中 %d 次" % s.count(old))
    sys.exit(1)

s = s.replace(old, new, 1)
open(F, "w", encoding="utf-8").write(s)
print("  [ok] allow_unverified_hw 默认值 false -> true")

for i, l in enumerate(open(F, encoding="utf-8").read().split("\n")):
    if "allow_unverified_hw" in l:
        print("  行 %d: %s" % (i + 1, l.strip()))
