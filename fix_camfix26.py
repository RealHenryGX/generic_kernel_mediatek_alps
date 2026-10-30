#!/usr/bin/env python3
"""camfix26: 清理调试代码, 固化地址探测 (交付版)

根因: DT 声明 reg=<0x26>, 硬件实际在 0x27 (SA0=1)。DA380/DA312 由 SA0 选择
      7-bit 地址, 两个候选只差 bit0。

改动:
  1. 删除 camfix22 的 6x200ms 重试循环 (真正的根因是地址, 不是时序)
  2. CAMDBG_ALT 转正: 声明地址失败后立即试 sibling 地址, 无延时
  3. 去掉所有 CAMDBG_* 调试打印
  4. 保留 allow_unverified_hw = true (Teracube 寄存器配置门禁)

效果: 开机不再多等 ~1.2s, 同时保留地址自适配。
"""
import sys

F = "drivers/misc/mediatek/sensors-1.0/accelerometer/mir3da/mir3da_cust.c"
s = open(F, encoding="utf-8").read()

if "CAMDBG" not in s:
    print("  [skip] 已是干净版")
    sys.exit(0)

# 定位整个检测块: 从 "Family detection" 注释行开始, 到 "if (ret) {\n\t\tif (ret == -1)" 之前
start = s.find("\t/* Family detection happens before reset/init writes")
if start < 0:
    print("  [FAIL] 找不到 Family detection 注释")
    sys.exit(1)

end_mark = "\tif (ret) {\n\t\tif (ret == -1)\n\t\t\tret = -ENODEV;\n\t\tgoto unlock_free;\n\t}\n"
end = s.find(end_mark, start)
if end < 0:
    print("  [FAIL] 找不到 if (ret) 收尾块")
    sys.exit(1)

new_block = """\t/* Family detection happens before reset/init writes.
\t * A25: the module answers at 0x27 (SA0=1) although the DT node
\t * declares reg=<0x26>. DA380/DA312 select their 7-bit address with
\t * the SA0 pin, so the two candidates differ only in bit0. Probe the
\t * declared address first, then its sibling, and keep whichever
\t * responds. No artificial delay: the address was the issue, not
\t * power-up timing.
\t */
\tret = mir3da_module_detect(client);
\tif (ret) {
\t\tu8 dt_addr = (u8)client->addr;

\t\tclient->addr = dt_addr ^ 0x01;
\t\tret = mir3da_module_detect(client);
\t\tif (ret) {
\t\t\tclient->addr = dt_addr;
\t\t\tdev_err(&client->dev,
\t\t\t\t"no sensor at i2c 0x%02x or 0x%02x\\n",
\t\t\t\tdt_addr, dt_addr ^ 0x01);
\t\t} else {
\t\t\tdev_info(&client->dev,
\t\t\t\t "using i2c 0x%02x (DT declared 0x%02x)\\n",
\t\t\t\t (u8)client->addr, dt_addr);
\t\t}
\t}
"""

s = s[:start] + new_block + s[end:]
open(F, "w", encoding="utf-8").write(s)

left = s.count("CAMDBG")
print("  [ok] 检测块已整理, 剩余 CAMDBG 引用: %d" % left)
k = s.find("Family detection")
print(s[max(0, k - 120):k + 950])
