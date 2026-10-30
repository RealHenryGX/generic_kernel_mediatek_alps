#!/usr/bin/env python3
"""camfix19: 修正后摄(IMX278)方向
模组倒立安装 -> 需要 180 度旋转 = mirror + flip = 0x0101 bit1|bit0 = 0x03
(之前试过 0x02 仅垂直翻转, 不够)
插入到 sensor_init 末尾, 保留原有寄存器表不动。
"""
import sys

F = ("drivers/misc/mediatek/imgsensor/src/common/v1/"
     "imx278_mipi_raw/imx278mipiraw_Sensor.c")
s = open(F, encoding="utf-8").read()

if "A25 rear" in s or "0x0101, 0x03" in s:
    print("  [skip] 方向补丁已存在")
    sys.exit(0)

anchor = 'LOG_INF("Exit sensor_init\\n");'
if s.count(anchor) != 1:
    print("  [FAIL] 锚点命中 %d 次" % s.count(anchor))
    sys.exit(1)

patch = ('/* A25 rear: module mounted upside down -> 180deg = mirror+flip */\n'
         '\twrite_cmos_sensor(0x0101, 0x03);\n')
s = s.replace(anchor, patch + anchor, 1)
open(F, "w", encoding="utf-8").write(s)
print("  [ok] IMX278 sensor_init 末尾加入 0x0101=0x03 (mirror+flip)")

# 校验
for i, l in enumerate(open(F, encoding="utf-8").read().split("\n")):
    if "0x0101, 0x03" in l:
        print("  行 %d: %s" % (i + 1, l.strip()))
