#!/usr/bin/env python3
"""camfix18 = camfix13(已对齐的供电序列) + 真正修复:
  A. GC5025SUB_SENSOR_ID 0x5025 -> 0x5026  (硬件实际返回 0x5026, 原厂 kdSensorList 也是 0x5026)
  B. i2c_addr_table {0x6e,0x7e,0xff} -> {0x6e,0xff}  (0x7e 是早期实验残留, 原厂只用 0x6e)
  C. 清掉所有 CAMDBG_* 诊断打印, 交付干净版本
保留 camfix13 的供电序列改动(imgsensor_cfg_table.c 不动)。
"""
import sys, re

IDS  = "drivers/misc/mediatek/imgsensor/inc/kd_imgsensor.h"
SUB  = "drivers/misc/mediatek/imgsensor/src/common/v1/gc5025sub_mipi_raw/gc5025sub_mipi_sensor.c"
HW   = "drivers/misc/mediatek/imgsensor/src/common/v1/imgsensor_hw.c"
CLK  = "drivers/misc/mediatek/imgsensor/src/mt6765/imgsensor_clk.c"
I2C  = "drivers/i2c/busses/i2c-mtk.c"


def strip_camdbg(path):
    """删除所有含 [CAMDBG 的 pr_info 语句(括号配平), 连同紧邻的空行"""
    try:
        lines = open(path, encoding="utf-8").read().split("\n")
    except FileNotFoundError:
        return 0
    out, i, removed = [], 0, 0
    while i < len(lines):
        if "[CAMDBG" in lines[i]:
            depth = 0
            j = i
            while j < len(lines):
                depth += lines[j].count("(") - lines[j].count(")")
                j += 1
                if depth <= 0:
                    break
            # 回退掉紧跟在后面的一行纯空白
            if j < len(lines) and lines[j].strip() == "":
                j += 1
            removed += 1
            i = j
            continue
        out.append(lines[i])
        i += 1
    if removed:
        open(path, "w", encoding="utf-8").write("\n".join(out))
    return removed


print("== camfix18 ==")

# A. sensor id
s = open(IDS, encoding="utf-8").read()
old = "#define GC5025SUB_SENSOR_ID                     0x5025"
if old in s:
    s = s.replace(old, "#define GC5025SUB_SENSOR_ID                     0x5026", 1)
    open(IDS, "w", encoding="utf-8").write(s)
    print("  [ok] GC5025SUB_SENSOR_ID 0x5025 -> 0x5026")
elif "GC5025SUB_SENSOR_ID                     0x5026" in s:
    print("  [skip] sensor_id 已是 0x5026")
else:
    print("  [FAIL] 找不到 GC5025SUB_SENSOR_ID 定义")
    sys.exit(1)

# B. addr_table
s = open(SUB, encoding="utf-8").read()
old_tbl = ".i2c_addr_table = {0x6e, 0x7e, 0xff}"
if old_tbl in s:
    s = s.replace(old_tbl, ".i2c_addr_table = {0x6e, 0xff}", 1)
    open(SUB, "w", encoding="utf-8").write(s)
    print("  [ok] addr_table -> {0x6e, 0xff}")
elif ".i2c_addr_table = {0x6e, 0xff}" in s:
    print("  [skip] addr_table 已干净")
else:
    print("  [FAIL] 找不到 addr_table")
    sys.exit(1)

# C. 清诊断
for f in (SUB, HW, CLK, I2C):
    n = strip_camdbg(f)
    if n:
        print("  [ok] %s: 清除 %d 处诊断打印" % (f.split("/")[-1], n))

# 校验
print("  --- 校验 ---")
print("  ", [l.strip() for l in open(IDS, encoding="utf-8").read().split("\n")
             if "GC5025SUB_SENSOR_ID" in l][:1])
print("  ", [l.strip() for l in open(SUB, encoding="utf-8").read().split("\n")
             if "i2c_addr_table" in l][:1])
left = sum(open(f, encoding="utf-8").read().count("CAMDBG") for f in (SUB, HW, CLK, I2C))
print("  剩余 CAMDBG 引用: %d (应为 0)" % left)
print("== done ==")
