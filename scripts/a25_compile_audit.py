#!/usr/bin/env python3
"""Offline preparation: python3 replay_prepare.py /path/kernel [--toolchain-dir /path/bin]."""
import argparse, os, subprocess, json, hashlib, shutil
from pathlib import Path
p=argparse.ArgumentParser();p.add_argument('kernel',type=Path);p.add_argument('--toolchain-dir',type=Path,default=Path('/usr/bin'));p.add_argument('--objects',action='store_true');a=p.parse_args()
k=a.kernel.resolve();o=k/'out-compile-audit';tc=a.toolchain_dir.resolve();o.mkdir(exist_ok=True)
cc=str(tc/'clang')+' --target=aarch64-linux-gnu'
args=['/usr/bin/make','O='+str(o),'ARCH=arm64','LLVM=1','LLVM_IAS=1','CROSS_COMPILE=','CC='+cc,'AS='+cc,'HOSTCC=/usr/bin/gcc','HOSTCXX=/usr/bin/g++','HOSTAR=/usr/bin/ar','HOSTLD=/usr/bin/ld','HOSTLDFLAGS=','LOCALVERSION=','KBUILD_BUILD_USER=gravity','KBUILD_BUILD_HOST=a25']
for key,name in {'LD':'ld.lld','AR':'llvm-ar','NM':'llvm-nm','OBJCOPY':'llvm-objcopy','OBJDUMP':'llvm-objdump','STRIP':'llvm-strip','READELF':'llvm-readelf','OBJSIZE':'llvm-size'}.items():args.append(key+'='+str(tc/name))
env=dict(os.environ,LOCALVERSION='',KBUILD_BUILD_USER='gravity',KBUILD_BUILD_HOST='a25',PATH=str(tc)+':/usr/bin:/bin')
def run(targets,label):
 r=subprocess.run(args+targets,cwd=k,env=env,stdout=subprocess.PIPE,stderr=subprocess.STDOUT);(o/(label+'.log')).write_bytes(r.stdout);print(label,r.returncode);r.check_returncode();return r.stdout
run(['a25_gravity_compile_audit_defconfig'],'defconfig')
shutil.copy2(o/'.config',o/'resolved.config')
expected=json.loads((k/'scripts/a25_compile_audit_policy.json').read_text())['config']
s=(o/'resolved.config').read_text();failed=[key for key,v in expected.items() if (('# '+key+' is not set') if v=='n' else key+'='+v) not in s.splitlines()];assert not failed,failed
run(['-j4','prepare','modules_prepare'],'prepare')
release=run(['-s','kernelrelease'],'release').decode().strip();assert release=='4.19.191-gravity',release
if a.objects:
 run(['scripts/selinux/genheaders/'],'genheaders-build');(o/'security/selinux').mkdir(parents=True,exist_ok=True)
 subprocess.run([str(o/'scripts/selinux/genheaders/genheaders'),str(o/'security/selinux/flask.h'),str(o/'security/selinux/av_permissions.h')],check=True,cwd=k)
 run(['-j4','V=1','drivers/kernelsu/','fs/namei.o','fs/susfs.o','fs/notify/fdinfo.o','fs/proc/task_mmu.o','init/version.o'],'selected-objects')
print('PASS: offline prepare only; no full Image, ABI or hardware certification')
