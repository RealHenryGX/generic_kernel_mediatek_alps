#!/usr/bin/env python3
"""camfix17: 在 gc5025sub get_imgsensor_id 里打印每次的写地址与读回的 sensor_id
用来确认真实行为: 0x6e 那次到底成功读到什么值, 为什么判定失败。
"""
import sys

F = ("drivers/misc/mediatek/imgsensor/src/common/v1/"
     "gc5025sub_mipi_raw/gc5025sub_mipi_sensor.c")
s = open(F, encoding="utf-8").read()

if "CAMDBG_ID" in s:
    print("  [skip] 诊断已存在")
    sys.exit(0)

old = "*sensor_id = return_sensor_id();"
if s.count(old) != 1:
    print("  [FAIL] return_sensor_id 调用点命中 %d 次" % s.count(old))
    sys.exit(1)

dbg = (
    '\n\t\t\tpr_info("[CAMDBG_ID] i=%d write_id=0x%x got=0x%x expect=0x%x\\n",\n'
    '\t\t\t\ti, imgsensor.i2c_write_id, *sensor_id,\n'
    '\t\t\t\timgsensor_info.sensor_id);'
)
s = s.replace(old, old + dbg, 1)

# 顺带打印 addr_table 实际内容
old2 = "while (imgsensor_info.i2c_addr_table[i] != 0xff) {"
if s.count(old2) >= 1:
    tbl = (
        '\n\tpr_info("[CAMDBG_ID] addr_table = {0x%x, 0x%x, 0x%x, 0x%x, 0x%x} init_write_id=0x%x\\n",\n'
        '\t\timgsensor_info.i2c_addr_table[0], imgsensor_info.i2c_addr_table[1],\n'
        '\t\timgsensor_info.i2c_addr_table[2], imgsensor_info.i2c_addr_table[3],\n'
        '\t\timgsensor_info.i2c_addr_table[4], imgsensor.i2c_write_id);'
    )
    s = s.replace(old2, old2 + tbl, 1)

open(F, "w", encoding="utf-8").write(s)
print("  [ok] sensor_id 诊断已插入")
