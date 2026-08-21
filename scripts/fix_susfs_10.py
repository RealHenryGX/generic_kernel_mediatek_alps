#!/usr/bin/env python3
"""Fix susfs 10_ patch Hunk#7 failure in KernelSU kernel/selinux/selinux.c.
Idempotent. Insertion point: right after ksu_is_zygote() (or is_zygote()).
Only inserts the SUSFS sid helper FUNCTIONS; the u32 variable/define block
is supplied by Hunk#1 of the same patch (already applied when Hunk#7 failed),
so we guard against duplicate definitions."""
import os, sys

target = 'KernelSU/kernel/selinux/selinux.c'
if not os.path.exists(target):
    print('no', target, '- skip')
    sys.exit(0)

src = open(target, encoding='utf-8', errors='replace').read()
if 'susfs_set_sid' in src:
    print('already patched - skip')
    sys.exit(0)

# --- find insertion point: end of ksu_is_zygote() / is_zygote() ---
marker_fn = None
for cand in ('bool ksu_is_zygote(void *sec)', 'bool is_zygote(void *sec)'):
    i = src.find(cand)
    if i >= 0:
        marker_fn = cand
        break
if marker_fn is None:
    print('zygote fn not found, abort')
    sys.exit(1)

# find the closing "return result;\n}" after the function start
fn_start = src.find(marker_fn)
ret_idx = src.find('return result;', fn_start)
if ret_idx < 0:
    print('return result; not found after zygote fn, abort')
    sys.exit(1)
brace_idx = src.find('}', ret_idx)
if brace_idx < 0:
    print('closing brace not found, abort')
    sys.exit(1)
insert_at = brace_idx + 1  # right after "}"

funcs_block = '''
#ifdef CONFIG_KSU_SUSFS
static inline void susfs_set_sid(const char *secctx_name, u32 *out_sid)
{
	int err;

	if (!secctx_name || !out_sid) {
		pr_err("secctx_name || out_sid is NULL\\n");
		return;
	}

	err = security_secctx_to_secid(secctx_name, strlen(secctx_name),
				       out_sid);
	if (err) {
		pr_err("failed setting sid for '%s', err: %d\\n", secctx_name, err);
		return;
	}
	pr_info("sid '%u' is set for secctx_name '%s'\\n", *out_sid, secctx_name);
}

bool susfs_is_sid_equal(void *sec, u32 sid2) {
	struct task_security_struct *tsec = (struct task_security_struct *)sec;
	if (!tsec) {
		return false;
	}
	return tsec->sid == sid2;
}

u32 susfs_get_sid_from_name(const char *secctx_name)
{
	u32 out_sid = 0;
	int err;

	if (!secctx_name) {
		pr_err("secctx_name is NULL\\n");
		return 0;
	}
	err = security_secctx_to_secid(secctx_name, strlen(secctx_name),
				       &out_sid);
	if (err) {
		pr_err("failed getting sid from secctx_name: %s, err: %d\\n", secctx_name, err);
		return 0;
	}
	return out_sid;
}

u32 susfs_get_current_sid(void) {
	return current_sid();
}

void susfs_set_zygote_sid(void)
{
	susfs_set_sid(KERNEL_ZYGOTE_DOMAIN, &susfs_zygote_sid);
}

bool susfs_is_current_zygote_domain(void) {
	return unlikely(current_sid() == susfs_zygote_sid);
}

void susfs_set_ksu_sid(void)
{
	susfs_set_sid(KERNEL_SU_DOMAIN, &susfs_ksu_sid);
}

bool susfs_is_current_ksu_domain(void)
{
	return unlikely(current_sid() == susfs_ksu_sid);
}

bool susfs_is_current_init_domain(void)
{
	return unlikely(current_sid() == susfs_init_sid);
}

void susfs_set_init_sid(void)
{
	susfs_set_sid(KERNEL_INIT_DOMAIN, &susfs_init_sid);
}
#endif
'''

out = src[:insert_at] + funcs_block + src[insert_at:]
open(target, 'w', encoding='utf-8', newline='\n').write(out)
print('inserted susfs sid funcs after', marker_fn, 'at char', insert_at)
