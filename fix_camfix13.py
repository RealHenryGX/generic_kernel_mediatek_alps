#!/usr/bin/env python3
"""camfix13: 一次性对齐原厂
1. IMX278 供电序列 -> 原厂完整序列(含 AFVDD 15ms, DVDD 1200)
2. GC5025SUB 供电序列 -> 原厂完整序列(补上 AFVDD 15ms)
3. 撤掉 imx278 sensor_init 里硬写的 0x0101=0x02(覆盖了 HAL 下发的 mirror/flip)
匹配串含驱动名, 每处必须命中且只命中 1 次。幂等。
"""
import sys

CFG = "drivers/misc/mediatek/imgsensor/src/mt6765/camera_hw/imgsensor_cfg_table.c"
IMX = "drivers/misc/mediatek/imgsensor/src/common/v1/imx278_mipi_raw/imx278mipiraw_Sensor.c"

T = "\t\t\t"
OEM_STEPS = [
    "{SensorMCLK, Vol_High, 0},",
    "{DOVDD, Vol_1800, 10},",
    "{AVDD, Vol_2800, 10},",
    "{DVDD, Vol_1200, 10},",
    "{AFVDD, Vol_2800, 15},",
    "{PDN, Vol_Low, 10},",
    "{RST, Vol_Low, 10},",
    "{PDN, Vol_High, 10},",
    "{RST, Vol_High, 10},",
]
OLD_IMX_STEPS = [
    "{AVDD, Vol_2800, 0},",
    "{DVDD, Vol_1100, 0},",
    "{DOVDD, Vol_1800, 0},",
    "{SensorMCLK, Vol_High, 1},",
    "{PDN, Vol_Low, 0},",
    "{PDN, Vol_High, 0},",
    "{RST, Vol_Low, 0},",
    "{RST, Vol_High, 5},",
]
# 副摄当前状态 = 原厂序列去掉 AFVDD 行
OLD_SUB_STEPS = [s for s in OEM_STEPS if "AFVDD" not in s]


def block(drvname, steps):
    body = "\n".join(T + s for s in steps)
    return "SENSOR_DRVNAME_%s,\n\t\t{\n%s\n" % (drvname, body)


OLD_IMX = block("IMX278_MIPI_RAW", OLD_IMX_STEPS)
NEW_IMX = block("IMX278_MIPI_RAW", OEM_STEPS)
OLD_SUB = block("GC5025SUB_MIPI_RAW", OLD_SUB_STEPS)
NEW_SUB = block("GC5025SUB_MIPI_RAW", OEM_STEPS)

INJECT = "\twrite_cmos_sensor(0x0101, 0x02); //mirror/flip: flip (vertical) for A25 rear\n"


def patch(path, old, new, tag):
    s = open(path, encoding="utf-8").read()
    if new in s and old not in s:
        print("  [skip] %s: 已是目标状态" % tag)
        return False
    n = s.count(old)
    if n != 1:
        print("  [FAIL] %s: 命中 %d 次(应为 1)" % (tag, n))
        sys.exit(1)
    open(path, "w", encoding="utf-8").write(s.replace(old, new, 1))
    print("  [ok] %s" % tag)
    return True


print("== camfix13 ==")
patch(CFG, OLD_IMX, NEW_IMX, "IMX278 序列 -> 原厂完整(含 AFVDD)")
patch(CFG, OLD_SUB, NEW_SUB, "GC5025SUB 序列 -> 原厂完整(补 AFVDD)")

s = open(IMX, encoding="utf-8").read()
if INJECT in s:
    open(IMX, "w", encoding="utf-8").write(s.replace(INJECT, "", 1))
    print("  [ok] 撤销 imx278 0x0101 硬写")
else:
    print("  [skip] 0x0101 硬写不存在")
print("== done ==")
