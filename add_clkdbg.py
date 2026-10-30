#!/usr/bin/env python3
"""camfix16: 诊断 MCLK 时钟链路
1. imgsensor_clk_init: 打印每个时钟 devm_clk_get 是否成功(ERR/OK)
2. imgsensor_clk_set: 打印 HAL 实际请求的 TG / freq / on, 以及最终返回值
这能确定: HAL 有没有请求 MCLK; 请求了对不对; 时钟句柄是否拿到。
"""
import sys

F = "drivers/misc/mediatek/imgsensor/src/mt6765/imgsensor_clk.c"
s = open(F, encoding="utf-8").read()

if "CAMDBG_CLK" in s:
    print("  [skip] 诊断补丁已存在")
    sys.exit(0)

# --- 1. init: 打印时钟获取结果 ---
old_init = """\t/* get all possible using clocks */
\tfor (i = 0; i < IMGSENSOR_CCF_MAX_NUM; i++)
\t\tpclk->imgsensor_ccf[i] =
\t\t    devm_clk_get(&pplatform_dev->dev, gimgsensor_mclk_name[i]);

\treturn IMGSENSOR_RETURN_SUCCESS;
"""
new_init = """\t/* get all possible using clocks */
\tfor (i = 0; i < IMGSENSOR_CCF_MAX_NUM; i++)
\t\tpclk->imgsensor_ccf[i] =
\t\t    devm_clk_get(&pplatform_dev->dev, gimgsensor_mclk_name[i]);

\tfor (i = 0; i < IMGSENSOR_CCF_MAX_NUM; i++)
\t\tpr_info("[CAMDBG_CLK] init[%02d] %-24s %s\\n", i,
\t\t\tgimgsensor_mclk_name[i],
\t\t\tIS_ERR(pclk->imgsensor_ccf[i]) ? "ERR" : "OK");

\treturn IMGSENSOR_RETURN_SUCCESS;
"""
if s.count(old_init) != 1:
    print("  [FAIL] init 代码块命中 %d 次" % s.count(old_init))
    sys.exit(1)
s = s.replace(old_init, new_init, 1)

# --- 2. set: 入口打印参数 ---
old_head = """\tint ret = 0;
\tint mclk_index = MCLK_ENU_START;
\tconst int supported_mclk_freq[MCLK_MAX] = {
"""
new_head = """\tint ret = 0;
\tint mclk_index = MCLK_ENU_START;
\tconst int supported_mclk_freq[MCLK_MAX] = {
"""
# 在 supported_mclk_freq 数组之后、for 之前插入
anchor = """\t\tFREQ_26MHZ, FREQ_48MHZ, FREQ_52MHZ };
"""
if s.count(anchor) != 1:
    print("  [FAIL] 频率表锚点命中 %d 次" % s.count(anchor))
    sys.exit(1)
s = s.replace(anchor,
              anchor + "\n\tpr_info(\"[CAMDBG_CLK] set TG=%d freq=%d on=%d\\n\",\n"
                      "\t\tpmclk->TG, pmclk->freq, pmclk->on);\n", 1)

# --- 3. set: 末尾打印结果 ---
old_tail = """\t\tclk_disable_unprepare(pclk->imgsensor_ccf[mclk_index]);
\t\tatomic_dec(&pclk->enable_cnt[mclk_index]);
\t}

\treturn ret;
}
"""
new_tail = """\t\tclk_disable_unprepare(pclk->imgsensor_ccf[mclk_index]);
\t\tatomic_dec(&pclk->enable_cnt[mclk_index]);
\t}

\tpr_info("[CAMDBG_CLK] set done TG=%d freq=%d on=%d idx=%d ret=%d\\n",
\t\tpmclk->TG, pmclk->freq, pmclk->on, mclk_index, ret);

\treturn ret;
}
"""
if s.count(old_tail) != 1:
    print("  [FAIL] set 尾部锚点命中 %d 次" % s.count(old_tail))
    sys.exit(1)
s = s.replace(old_tail, new_tail, 1)

open(F, "w", encoding="utf-8").write(s)
print("  [ok] MCLK 时钟链路诊断已插入")
