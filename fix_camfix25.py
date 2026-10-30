#!/usr/bin/env python3
"""camfix25: 探测备用 i2c 地址 (根因修复)

实测证据 (i2cdetect/i2cget on bus1):
  0x26 -> Remote I/O error   (无设备)
  0x27 -> reg0x01 = 0x13     (WHO_AM_I, 正是期望值)
  0x60 -> 有响应             (wusb3801)

DA380/DA312 用 SA0 引脚选择 7-bit 地址 0x26/0x27, 二者只差 bit0。
A25 这颗接成 SA0=1 -> 0x27, 但 DT 节点写的是 reg=<0x26>,
i2c_client 因此只对 0x26 发事务, 必然 ACK error (-121)。

原厂 DT 同样写 0x26 却能工作, 说明原厂驱动自己做了地址探测。

修法: DT 地址探测失败后翻转 bit0 再试一次, 成功则沿用该地址。
"""
import sys

F = "drivers/misc/mediatek/sensors-1.0/accelerometer/mir3da/mir3da_cust.c"
s = open(F, encoding="utf-8").read()

if "CAMDBG_ALT" in s:
    print("  [skip] 地址探测补丁已存在")
    sys.exit(0)

old = """\t\t\tdev_info(&client->dev,
\t\t\t\t "CAMDBG_DETECT attempt %d failed ret=%d\\n",
\t\t\t\t attempt, ret);
\t\t}
\t}
"""

new = """\t\t\tdev_info(&client->dev,
\t\t\t\t "CAMDBG_DETECT attempt %d failed ret=%d\\n",
\t\t\t\t attempt, ret);
\t\t}

\t\t/* CAMDBG_ALT: A25 hardware answers at 0x27 while the DT node
\t\t * declares reg=<0x26>. DA380/DA312 pick the 7-bit address with
\t\t * the SA0 pin, so the two candidates differ only in bit0. Try
\t\t * the sibling address before failing; the stock driver
\t\t * evidently does the same (same DT, working probe).
\t\t */
\t\tif (ret) {
\t\t\tu8 dt_addr = (u8)client->addr;

\t\t\tclient->addr = dt_addr ^ 0x01;
\t\t\tret = mir3da_module_detect(client);
\t\t\tdev_info(&client->dev,
\t\t\t\t "CAMDBG_ALT dt=0x%02x try=0x%02x ret=%d\\n",
\t\t\t\t dt_addr, (u8)client->addr, ret);
\t\t\tif (ret)
\t\t\t\tclient->addr = dt_addr;
\t\t}
\t}
"""

if s.count(old) != 1:
    print("  [FAIL] 命中 %d 次" % s.count(old))
    sys.exit(1)

s = s.replace(old, new, 1)
open(F, "w", encoding="utf-8").write(s)
print("  [ok] 备用地址探测已加入")
k = s.find("CAMDBG_ALT")
print(s[max(0, k - 500):k + 420])
