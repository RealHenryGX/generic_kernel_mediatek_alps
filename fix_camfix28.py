#!/usr/bin/env python3
"""camfix28: 允许 vermagic 不匹配的模块强制加载

背景:
  本内核 LOCALVERSION 带 -gravity-g53d31dab5 (用户要求, 用于区分自编译内核),
  原厂 .ko 的 vermagic 是 4.19.191, 不匹配 -> insmod 返回 EINVAL。
  Android 的 insmod/modprobe 都是 Toybox 版, 不支持 --force 传
  MODULE_INIT_IGNORE_VERMAGIC 标志。

做法:
  kernel/module.c 的 check_modinfo() 里, vermagic 不匹配时先走
  try_to_force_load()。在 CONFIG_MODULE_FORCE_LOAD=y 下该函数返回 0,
  模块得以加载并给内核打 TAINT_FORCED_MODULE 标记。

效果: 原厂 vendor/mediatek connectivity .ko (wmt_drv / wlan_drv_gen4m /
      bt_drv_connac1x 等) 可以直接加载, 无需移植源码。

风险: 若模块与内核 ABI 实际不兼容, 加载后可能崩溃。两内核同为 4.19.191
      同平台(MT6765), ABI 大概率一致; 异常时回退刷机即可。
"""
import sys

F = "kernel/module.c"
s = open(F, encoding="utf-8").read()

if "CAMDBG_FORCE_VERMAGIC" in s:
    print("  [skip] 补丁已存在")
    sys.exit(0)

old = """\t} else if (!same_magic(modmagic, vermagic, info->index.vers)) {
\t\tpr_err("%s: version magic '%s' should be '%s'\\n",
\t\t       info->name, modmagic, vermagic);
\t\treturn -ENOEXEC;
\t}
"""

new = """\t} else if (!same_magic(modmagic, vermagic, info->index.vers)) {
\t\t/* CAMDBG_FORCE_VERMAGIC: LOCALVERSION carries -gravity-<commit>
\t\t * for kernel identification, so stock vendor modules whose
\t\t * vermagic is plain "4.19.191" never match. Android's Toybox
\t\t * insmod/modprobe cannot pass MODULE_INIT_IGNORE_VERMAGIC, so
\t\t * attempt the forced-load path here instead. Succeeds only
\t\t * when CONFIG_MODULE_FORCE_LOAD=y (taints the kernel).
\t\t */
\t\terr = try_to_force_load(mod, "bad vermagic");
\t\tif (err) {
\t\t\tpr_err("%s: version magic '%s' should be '%s'\\n",
\t\t\t       info->name, modmagic, vermagic);
\t\t\treturn -ENOEXEC;
\t\t}
\t\tpr_warn("%s: vermagic '%s' != '%s', force loading\\n",
\t\t\tinfo->name, modmagic, vermagic);
\t}
"""

if s.count(old) != 1:
    print("  [FAIL] 命中 %d 次" % s.count(old))
    sys.exit(1)

s = s.replace(old, new, 1)
open(F, "w", encoding="utf-8").write(s)
print("  [ok] check_modinfo 已放宽 vermagic 校验")
k = s.find("CAMDBG_FORCE_VERMAGIC")
print(s[max(0, k - 200):k + 950])
