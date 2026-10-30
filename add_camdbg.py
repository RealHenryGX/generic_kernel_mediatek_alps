#!/usr/bin/env python3
"""camfix14: 在 imgsensor_hw_power_sequence 里加诊断打印
验证 pin -> pdev 映射(id[])是否正确, 以及每个 step 实际调用了哪个模块。
只加 printk, 不改逻辑。
"""
import sys, re

F = "drivers/misc/mediatek/imgsensor/src/common/v1/imgsensor_hw.c"
s = open(F, encoding="utf-8").read()

if "CAMDBG_STEP" in s:
    print("  [skip] 诊断打印已存在")
    sys.exit(0)

# 找 pdev = phw->pdev[psensor_pwr->id[ppwr_info->pin]];
pat = re.compile(
    r'(?P<indent>[ \t]*)pdev = phw->pdev\[psensor_pwr->id\[ppwr_info->pin\]\];\n')
m = pat.search(s)
if not m:
    print("  [FAIL] 未找到 pdev 赋值点")
    sys.exit(1)

ind = m.group("indent")
dbg = (ind + 'pr_info("[CAMDBG_STEP] idx=%d pin=%d id=%d pdev=%px state=%d delay=%u\\n",\n'
       + ind + '\t(int)sensor_idx, (int)ppwr_info->pin,\n'
       + ind + '\t(int)psensor_pwr->id[ppwr_info->pin], pdev,\n'
       + ind + '\t(int)((pwr_status == IMGSENSOR_HW_POWER_STATUS_ON) ?\n'
       + ind + '\t\tppwr_info->pin_state_on : ppwr_info->pin_state_off),\n'
       + ind + '\t(pwr_status == IMGSENSOR_HW_POWER_STATUS_ON) ?\n'
       + ind + '\t\tppwr_info->pin_on_delay : ppwr_info->pin_off_delay);\n')

s2 = s[:m.end()] + dbg + s[m.end():]
open(F, "w", encoding="utf-8").write(s2)
print("  [ok] 已插入 [CAMDBG_STEP] 打印")
