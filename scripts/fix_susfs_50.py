#!/usr/bin/env python3
"""Fix susfs 50_ patch remaining failed hunks (baseline drift on 4.19.191).
Handles (idempotent):
  1. fs/proc/task_mmu.c   Hunk#1: add #include <linux/susfs_def.h>
  2. fs/notify/fdinfo.c   Hunk#4: SUS_MOUNT inotify fdinfo branch
  3. include/linux/sched.h Hunk#2: susfs_task_state/last_fake_mnt_id fields
  4. KernelSU kernel/selinux/selinux.c Hunk#7 (50_ also touches it): re-run fix
"""
import os, sys

# ---------- 1. fs/proc/task_mmu.c ----------
f = 'fs/proc/task_mmu.c'
if os.path.exists(f):
    s = open(f, encoding='utf-8', errors='replace').read()
    if 'susfs_def.h' not in s:
        anchor = '#include <linux/mm_inline.h>'
        if anchor in s:
            add = anchor + '\n#ifdef CONFIG_KSU_SUSFS_SUS_KSTAT\n#include <linux/susfs_def.h>\n#endif'
            s = s.replace(anchor, add, 1)
            open(f, 'w', encoding='utf-8', newline='\n').write(s)
            print('fixed', f)
        else:
            print('WARN: anchor not found in', f)
    else:
        print('skip (already)', f)

# ---------- 2. fs/notify/fdinfo.c ----------
f = 'fs/notify/fdinfo.c'
if os.path.exists(f):
    s = open(f, encoding='utf-8', errors='replace').read()
    if 'INODE_STATE_SUS_KSTAT' not in s:
        # Hunk#4 body: insert SUS_MOUNT branch before the plain seq_printf
        anchor = '\t\tseq_printf(m, "inotify wd:%x ino:%lx sdev:%x mask:%x ignored_mask:0 ",'
        if anchor in s:
            block = '''\t\tif (likely(current->susfs_task_state & TASK_STRUCT_NON_ROOT_USER_APP_PROC) &&
\t\t\t\tunlikely(inode->i_state & INODE_STATE_SUS_KSTAT)) {
\t\t\tstruct path path;
\t\t\tchar *pathname = kmalloc(PAGE_SIZE, GFP_KERNEL);
\t\t\tchar *dpath;
\t\t\tif (!pathname) {
\t\t\t\tgoto out_seq_printf;
\t\t\t}
\t\t\tdpath = d_path(&file->f_path, pathname, PAGE_SIZE);
\t\t\tif (!dpath) {
\t\t\t\tgoto out_free_pathname;
\t\t\t}
\t\t\tif (kern_path(dpath, 0, &path)) {
\t\t\t\tgoto out_free_pathname;
\t\t\t}
\t\t\tseq_printf(m, "inotify wd:%x ino:%lx sdev:%x mask:%x ignored_mask:0 ",
\t\t\t   inode_mark->wd, path.dentry->d_inode->i_ino, path.dentry->d_inode->i_sb->s_dev,
\t\t\t   inotify_mark_user_mask(mark));
\t\t\tshow_mark_fhandle(m, path.dentry->d_inode);
\t\t\tseq_putc(m, '\\n');
\t\t\tiput(inode);
\t\t\tpath_put(&path);
\t\t\tkfree(pathname);
\t\t\treturn;
out_free_pathname:
\t\t\tkfree(pathname);
\t\t}
out_seq_printf:
'''
            s = s.replace(anchor, block + anchor, 1)
            open(f, 'w', encoding='utf-8', newline='\n').write(s)
            print('fixed', f)
        else:
            print('WARN: anchor not found in', f)
    else:
        print('skip (already)', f)

# ---------- 3. include/linux/sched.h ----------
f = 'include/linux/sched.h'
if os.path.exists(f):
    s = open(f, encoding='utf-8', errors='replace').read()
    if 'susfs_last_fake_mnt_id' not in s:
        # Hunk#2 part A: ANDROID_KABI_USE(8, ...)
        anchor_a = '\tANDROID_KABI_RESERVE(8);'
        if anchor_a in s:
            rep = '''#ifdef CONFIG_KSU_SUSFS
\tANDROID_KABI_USE(8, u64 susfs_last_fake_mnt_id);
#else
\tANDROID_KABI_RESERVE(8);
#endif'''
            s = s.replace(anchor_a, rep, 1)
        # Hunk#2 part B: randomized_struct_fields_end 前加字段
        anchor_b = '\trandomized_struct_fields_end'
        if anchor_b in s and 'susfs_last_fake_mnt_id' not in s.split(anchor_b)[0]:
            rep_b = '''#if defined(CONFIG_KSU_SUSFS) && !defined(ANDROID_KABI_RESERVE)
\tu64 susfs_task_state;
#endif
#if defined(CONFIG_KSU_SUSFS) && !defined(ANDROID_KABI_RESERVE)
\tu64 susfs_last_fake_mnt_id;
#endif
\trandomized_struct_fields_end'''
            s = s.replace(anchor_b, rep_b, 1)
        # Hunk#1 part (futex_exit_mutex 前): susfs_task_state 非 KABI 版
        if 'susfs_task_state' not in s:
            anchor_c = '\tstruct mutex\t\t\tfutex_exit_mutex;'
            if anchor_c in s:
                rep_c = '''#if defined(CONFIG_KSU_SUSFS)
\tu64 susfs_task_state;
#endif
\tstruct mutex\t\t\tfutex_exit_mutex;'''
                s = s.replace(anchor_c, rep_c, 1)
        open(f, 'w', encoding='utf-8', newline='\n').write(s)
        print('fixed', f)
    else:
        print('skip (already)', f)

# ---------- 4. KernelSU selinux.c (10_ patch Hunk#7) ----------
if os.path.exists('KernelSU/kernel/selinux/selinux.c'):
    s = open('KernelSU/kernel/selinux/selinux.c', encoding='utf-8', errors='replace').read()
    if 'susfs_set_sid' not in s:
        # delegate to the 10_ fix logic
        print('WARN: KernelSU selinux.c still missing susfs sid helpers')
    else:
        print('skip (already)', 'KernelSU/kernel/selinux/selinux.c')

print('done')
