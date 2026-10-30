#!/usr/bin/env python3
"""camfix15 实验: 强制 i2c4 使用 CCU 通道偏移(0x200)
假设: 副摄 sensor 物理挂在 CCU i2c 通道上, 默认的 ch_offset_default(0x100) 走不通,
      所以 NACK。CCU 固件正常时由 ccu_i2c_buf_mode_en() 调用 i2c_ccu_enable(0x200) 切换,
      我们这里直接静态切过去做验证。
只影响 id==4 的控制器, 不影响 i2c2(主摄)。
"""
import sys, re

F = "drivers/i2c/busses/i2c-mtk.c"
s = open(F, encoding="utf-8").read()

if "CAMDBG_EXP" in s:
    print("  [skip] 实验补丁已存在")
    sys.exit(0)

old = """\t} else {
\t\ti2c->ch_offset = i2c->ch_offset_default;
\t\ti2c->ch_offset_dma = i2c->ch_offset_dma_default;
\t}
"""
new = """\t} else {
\t\ti2c->ch_offset = i2c->ch_offset_default;
\t\ti2c->ch_offset_dma = i2c->ch_offset_dma_default;
\t\t/* CAMDBG_EXP: 强制 i2c4 走 CCU 通道, 验证副摄是否挂在 CCU 上 */
\t\tif (i2c->id == 4 && i2c->ccu_offset) {
\t\t\tdev_info(i2c->dev,
\t\t\t\t"[CAMDBG_EXP] id4 ch_offset 0x%x -> ccu 0x%x\\n",
\t\t\t\ti2c->ch_offset, i2c->ccu_offset);
\t\t\ti2c->ch_offset = i2c->ccu_offset;
\t\t}
\t}
"""
n = s.count(old)
if n != 1:
    print("  [FAIL] 目标代码块命中 %d 次" % n)
    sys.exit(1)
s = s.replace(old, new, 1)
open(F, "w", encoding="utf-8").write(s)
print("  [ok] 已强制 i2c4 使用 CCU 通道")
