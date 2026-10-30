#!/usr/bin/env python3
"""camfix20: 修正后摄(IMX278) Bayer 相位
camfix19 用 0x0101=0x03 做 180 度旋转解决倒立, 但 180 度旋转会让首像素从 R 移到
对角(奇数行奇数列), 该位置在原 Bayer 阵里是 B -> 必须把输出格式同步改成 RAW_B,
否则 ISP 仍按 RAW_R 解码, 导致红蓝通道互换(橙/黄变蓝)。
"""
import sys

F = ("drivers/misc/mediatek/imgsensor/src/common/v1/"
     "imx278_mipi_raw/imx278mipiraw_Sensor.c")
s = open(F, encoding="utf-8").read()

old = ".sensor_output_dataformat = SENSOR_OUTPUT_FORMAT_RAW_R,"
new = ".sensor_output_dataformat = SENSOR_OUTPUT_FORMAT_RAW_B,"

if old not in s and new in s:
    print("  [skip] 输出格式已是 RAW_B")
    sys.exit(0)

if s.count(old) != 1:
    print("  [FAIL] RAW_R 初始化命中 %d 次" % s.count(old))
    sys.exit(1)

s = s.replace(old, new, 1)
open(F, "w", encoding="utf-8").write(s)
print("  [ok] IMX278 sensor_output_dataformat: RAW_R -> RAW_B")

for i, l in enumerate(open(F, encoding="utf-8").read().split("\n")):
    if "sensor_output_dataformat =" in l:
        print("  行 %d: %s" % (i + 1, l.strip()))
    if "0x0101, 0x03" in l:
        print("  行 %d: %s  (方向补丁仍在)" % (i + 1, l.strip()))
