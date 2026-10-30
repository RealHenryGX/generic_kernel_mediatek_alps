#!/usr/bin/env python3
"""camfix22: mir3da 检测加延时重试

现象: 地址阶段 ACK 成功(irq_stat 无 ACKERR bit), 数据阶段 NACK(-EREMOTEIO)。
      我的 probe 在 0.747s, 原厂在 1.385s 且耗时 45ms。
假设: gsensor 的 VDD 比 VDDIO 起得晚, probe 太早 -> 地址能 ACK 但内部未就绪。
做法: 检测阶段加重试+延时(最多约 1s), 并打印每次尝试结果以便判定。
"""
import sys

F = "drivers/misc/mediatek/sensors-1.0/accelerometer/mir3da/mir3da_cust.c"
s = open(F, encoding="utf-8").read()

if "CAMDBG_DETECT" in s:
    print("  [skip] 重试补丁已存在")
    sys.exit(0)

old = """\t/* Family detection happens before reset/init writes. Never scan 0x27. */
\tret = mir3da_module_detect(client);
\tif (ret) {
\t\tif (ret == -1)
\t\t\tret = -ENODEV;
\t\tgoto unlock_free;
\t}
"""

new = """\t/* Family detection happens before reset/init writes. Never scan 0x27. */
\t/* CAMDBG_DETECT: A25 gsensor VDD may settle later than our probe point.
\t * Retry with delay so an early probe does not permanently fail the
\t * whole accelerometer framework. Logs each attempt for diagnosis.
\t */
\t{
\t\tint attempt;

\t\tfor (attempt = 0; attempt < 6; attempt++) {
\t\t\tif (attempt)
\t\t\t\tmsleep(200);
\t\t\tret = mir3da_module_detect(client);
\t\t\tif (ret == 0)
\t\t\t\tbreak;
\t\t\tdev_info(&client->dev,
\t\t\t\t "CAMDBG_DETECT attempt %d failed ret=%d\\n",
\t\t\t\t attempt, ret);
\t\t}
\t}
\tif (ret) {
\t\tif (ret == -1)
\t\t\tret = -ENODEV;
\t\tgoto unlock_free;
\t}
"""

if s.count(old) != 1:
    print("  [FAIL] 检测代码块命中 %d 次" % s.count(old))
    sys.exit(1)

s = s.replace(old, new, 1)

# 确保 msleep 可用
if "#include <linux/delay.h>" not in s:
    s = s.replace("#include <linux/i2c.h>", "#include <linux/i2c.h>\n#include <linux/delay.h>", 1)
    print("  [ok] 补加 linux/delay.h")

open(F, "w", encoding="utf-8").write(s)
print("  [ok] 检测重试已加入 (最多 6 次 x 200ms)")
for i, l in enumerate(open(F, encoding="utf-8").read().split("\n")):
    if "CAMDBG_DETECT" in l or "attempt < 6" in l:
        print("  行 %d: %s" % (i + 1, l.strip()))
