#!/usr/bin/env python3
"""revert camfix22: 移除已推翻的检测重试逻辑, 恢复原始单次检测"""
import sys

F = "drivers/misc/mediatek/sensors-1.0/accelerometer/mir3da/mir3da_cust.c"
s = open(F, encoding="utf-8").read()

start_mark = "\t/* CAMDBG_DETECT: A25 gsensor VDD"
if start_mark not in s:
    print("  [skip] 未找到 camfix22 重试块")
    sys.exit(0)

# 定位整块: 从注释开始到闭合的 "\t}\n"
i = s.index(start_mark)
# 前一行注释 "/* Family detection ... */" 保留, 从 CAMDBG 注释开始删
j = s.index("\t}\n", s.index("dev_info(&client->dev,", i)) + 3
blk = s[i:j]
# 该块以 "\t{\n\t\tint attempt;..." 开头, 删到 "\t}\n"
s = s[:i] + s[j:]

# 恢复单次调用
needle = "\t/* Family detection happens before reset/init writes. Never scan 0x27. */\n\n\tif (ret) {"
n2 = "\t/* Family detection happens before reset/init writes. Never scan 0x27. */\n\tret = mir3da_module_detect(client);\n\tif (ret) {"
if needle in s:
    s = s.replace(needle, n2, 1)
else:
    # 可能是注释后直接 if
    needle2 = "\t/* Family detection happens before reset/init writes. Never scan 0x27. */\n\tif (ret) {"
    if needle2 in s:
        s = s.replace(needle2, "\t/* Family detection happens before reset/init writes. Never scan 0x27. */\n\tret = mir3da_module_detect(client);\n\tif (ret) {", 1)
    else:
        print("  [WARN] 恢复点未匹配, 请人工确认")
        print("  附近内容:")
        k = s.find("Family detection")
        print(s[max(0,k-200):k+400])
        sys.exit(1)

open(F, "w", encoding="utf-8").write(s)
print("  [ok] camfix22 重试已移除, 恢复单次检测")
k = s.find("Family detection")
print("  当前代码:")
print(s[max(0,k-100):k+320])
