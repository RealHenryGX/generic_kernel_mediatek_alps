#!/usr/bin/env python3
"""撤销 camfix15 的 CCU 通道实验(已验证失败), 恢复 i2c4 默认通道。"""
import sys, re

F = "drivers/i2c/busses/i2c-mtk.c"
s = open(F, encoding="utf-8").read()

if "CAMDBG_EXP" not in s:
    print("  [skip] CCU 实验补丁不存在")
    sys.exit(0)

pat = re.compile(
    r'\n\t\t/\* CAMDBG_EXP.*?\n\t\tif \(i2c->id == 4 && i2c->ccu_offset\) \{.*?\n\t\t\}\n',
    re.S)
s2, n = pat.subn("\n", s)
if n != 1:
    print("  [FAIL] 匹配 %d 处" % n)
    sys.exit(1)
open(F, "w", encoding="utf-8").write(s2)
print("  [ok] 已撤销 CCU 通道实验")
