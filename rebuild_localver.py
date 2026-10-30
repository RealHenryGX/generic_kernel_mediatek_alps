import pathlib, subprocess, importlib.util, json, time, hashlib
K = pathlib.Path('/home/builder/work/build/a25-wusb-kbuild-integration/kernel')
OUT = pathlib.Path('/home/builder/work/build/a25-wusb-kbuild-integration/out/kbuild')
spec = importlib.util.spec_from_file_location('ci', K / 'scripts/a25_cloud_audit.py')
ci = importlib.util.module_from_spec(spec)
spec.loader.exec_module(ci)
argv = ci.make_arguments(pathlib.Path('/usr/bin'), OUT)

# 1. defconfig（更新 .config 的 CONFIG_LOCALVERSION）
t = time.time()
r1 = subprocess.run(argv + [ci.DEFCONFIG], cwd=K)
print('DEFCONFIG_EXIT', r1.returncode, 'ELAPSED', round(time.time()-t, 1), flush=True)

# 2. 编译 Image.gz
t = time.time()
r2 = subprocess.run(argv + ['-j12', 'V=1', 'Image.gz'], cwd=K)
print('BUILD_EXIT', r2.returncode, 'ELAPSED', round(time.time()-t, 1), flush=True)

img = OUT / 'arch/arm64/boot/Image.gz'
if img.exists():
    print('RESULT', json.dumps({'size': img.stat().st_size,
                                'sha256': hashlib.sha256(img.read_bytes()).hexdigest()}), flush=True)
