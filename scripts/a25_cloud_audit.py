#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""A25 v5 cold compile audit. No patching, packaging, flashing or release.

Python 3.10+; Linux build host. --self-test only exercises failure propagation
with an explicitly fake make, without downloading or compiling a kernel.
"""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import signal
import struct
import subprocess
import sys
import tempfile
import threading
import time
import traceback

HERE = Path(__file__).resolve().parent
AUDIT = HERE / 'a25_audit'
BRANCH = 'refs/heads/audit/a25-v5-compile'
PREPARED_COMMIT = '33abce43e7df3a46e8082276b12a279405e2ac29'
DEFCONFIG = 'a25_gravity_compile_audit_defconfig'
TOOLCHAIN_URL = ('https://github.com/Mandi-Sa/clang/releases/download/amd64-kernel-arm_static-22/'
                 'llvm22.0.0-binutils2.45.1_amd64-kernel-arm_static-20260111.7z')
TOOLCHAIN_SIZE = 146471475
TOOLCHAIN_SHA256 = 'a5b93f8a8814cbf800f79025f72d3a7eeba15eb7d212723e34be941f4e0797bf'
GATE_HASHES = {
    'audit_final.py': '0064e31c2b377b0bad097b8b57fe820a043a911f17c40afa75a3fb5668ada534',
    'policy.json': 'cb5bb8065f9ed1e39172df4c51e01b1e115d34ab85648f03bbbe78de54694c25',
    'expected_crc.json': 'f50414f983954aa8b9dba2ac73f81b0955102b80c7cd3bc240fc940134d23b7c',
    'source_lock.json': '885f9cf108b225c6285064c4c7e296d60ca16b9922faaaa966780db7f726f65a',
}
# Retain diagnostics as warnings, not silence. Original workflow's list plus
# incompatible-pointer-types: the top-level Makefile explicitly promotes it.
# A specific -Wno-error=foo still wins over a later vendor-wide bare -Werror.
DEMOTIONS = (
    'implicit-function-declaration', 'implicit-int',
    'incompatible-function-pointer-types', 'deprecated-non-prototype',
    'array-bounds', 'pointer-bool-conversion', 'string-plus-int',
    'sizeof-array-div', 'tautological-compare', 'deprecated-declarations',
    'strict-prototypes', 'date-time', 'int-conversion',
    'single-bit-bitfield-constant-conversion', 'pointer-to-int-cast',
    'declaration-after-statement', 'unused-variable', 'unused-but-set-variable',
    'unused-function', 'sign-compare', 'missing-prototypes', 'enum-conversion',
    'constant-conversion', 'incompatible-pointer-types',
)
STUB_WARNING = (
    'Vendored drivers/kernelsu/embed_ksud.c contains ksud_size=0: compile-audit stub, '
    'NOT a usable embedded ksud or a KernelSU userspace/module-functionality claim. '
    'This warning is independent of allow_unverified_hw; the hardware gate remains mandatory. '
    'No boot/recovery/flashable package is produced, and no hardware is certified.'
)


def require(ok, text):
    if not ok:
        raise ValueError(text)


def write_json(path, value):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, indent=2, sort_keys=True) + '\n', encoding='utf-8')


def digest(path):
    h = hashlib.sha256()
    with path.open('rb') as f:
        for chunk in iter(lambda: f.read(1024 * 1024), b''):
            h.update(chunk)
    return h.hexdigest()


def terminate_tree(process):
    if process.poll() is None:
        if os.name == 'posix':
            os.killpg(process.pid, signal.SIGTERM)
        else:
            process.terminate()
        try:
            process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            if os.name == 'posix':
                os.killpg(process.pid, signal.SIGKILL)
            else:
                process.kill()
            process.wait()


class Runner:
    """Stream unfiltered stdout+stderr to both console and durable log.

    There is no shell/tee success code to mask make failure. Every child exit
    is recorded and a nonzero child, timeout or logging error raises.
    """
    def __init__(self, evidence, cwd, env, budget=4400):
        self.evidence = evidence
        self.evidence.mkdir(parents=True, exist_ok=True)
        self.cwd, self.env = cwd, env
        self.deadline = time.monotonic() + budget
        self.commands = []

    def run(self, argv, label, timeout=300):
        argv = [str(x) for x in argv]
        limit = min(timeout, self.deadline - time.monotonic())
        require(limit > 0, 'Audit wall-clock budget exhausted')
        entry = dict(label=label, argv=argv, started_utc=time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime()))
        self.commands.append(entry)
        write_json(self.evidence / 'commands.json', self.commands)
        print('\n>>> ' + shlex.join(argv), flush=True)
        errors = []
        started = time.monotonic()
        process = None
        reader = None
        with (self.evidence / (label + '.log')).open('wb', buffering=0) as logfile:
            try:
                process = subprocess.Popen(argv, cwd=self.cwd, env=self.env,
                    stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                    start_new_session=(os.name == 'posix'), bufsize=0)

                def copy_stream():
                    try:
                        while True:
                            chunk = os.read(process.stdout.fileno(), 65536)
                            if not chunk:
                                break
                            logfile.write(chunk)
                            sys.stdout.buffer.write(chunk)
                            sys.stdout.buffer.flush()
                    except BaseException as exc:
                        errors.append(repr(exc))
                        terminate_tree(process)

                reader = threading.Thread(target=copy_stream, daemon=True)
                reader.start()
                try:
                    rc = process.wait(timeout=limit)
                except subprocess.TimeoutExpired:
                    entry['timed_out'] = True
                    raise
                reader.join(timeout=15)
                require(not reader.is_alive(), 'Child output pipe did not close: ' + label)
                require(not errors, 'Output logging failed: ' + repr(errors))
                entry['returncode'] = rc
                if rc:
                    raise subprocess.CalledProcessError(rc, argv)
                return self.evidence / (label + '.log')
            finally:
                if process is not None:
                    terminate_tree(process)
                    if reader is not None:
                        reader.join(timeout=10)
                    entry['returncode'] = process.returncode
                    if not reader or not reader.is_alive():
                        process.stdout.close()
                entry['elapsed_seconds'] = round(time.monotonic() - started, 3)
                write_json(self.evidence / 'commands.json', self.commands)
                print('\n<<< %s exit=%s' % (label, entry.get('returncode', 'not-started')), flush=True)


def git(source, *args):
    return subprocess.check_output(['/usr/bin/git', '-C', str(source), *args],
        stderr=subprocess.STDOUT, timeout=30, env=dict(os.environ, GIT_OPTIONAL_LOCKS='0')).decode('utf-8')


def verify_source(source, evidence):
    lock = json.loads((AUDIT / 'source_lock.json').read_text(encoding='utf-8'))
    require(lock['prepared_commit'] == PREPARED_COMMIT, 'Incorrect source-lock lineage')
    actual_root = [line for line in git(source, 'ls-tree', 'HEAD').splitlines()
                   if line.split('\t')[1] not in ('.github', 'scripts')]
    require(actual_root == lock['root'], 'Kernel root tree differs from locked v5 source')
    scripts = git(source, 'ls-tree', 'HEAD:scripts').splitlines()
    scripts = [line for line in scripts if line.split('\t')[1] not in ('a25_cloud_audit.py', 'a25_audit')]
    require(scripts == lock['scripts'], 'Existing scripts changed relative to v5 source')
    github = git(source, 'ls-tree', '-r', 'HEAD', '.github').splitlines()
    github = [line for line in github if line.split('\t')[1] != '.github/workflows/a25-compile-audit.yml']
    require(github == lock['github'], 'Existing workflow changed relative to v5 source')
    require(not git(source, 'status', '--porcelain', '--untracked-files=all').strip(),
            'Require a clean tracked/untracked checkout (output must be outside source)')
    require((source / 'arch/arm64/configs' / DEFCONFIG).is_file(), 'Missing committed compile-audit defconfig')
    require((source / 'drivers/kernelsu').is_dir() and not (source / 'drivers/kernelsu').is_symlink(),
            'Expected already-vendored drivers/kernelsu, not an integration step')
    ksu_make = (source / 'drivers/kernelsu/Makefile').read_text()
    require('KSU_VERSION := 11872' in ksu_make, 'Vendored KSU version is not pinned')
    require('sed -i' not in ksu_make and 'git fetch' not in ksu_make, 'Active KSU build must not patch/fetch source')
    require(re.search(r'ksud_size\s*=\s*0\s*;', (source / 'drivers/kernelsu/embed_ksud.c').read_text()),
            'ksud stub premise changed: review source-lock and warning before proceeding')
    head = git(source, 'rev-parse', 'HEAD').strip()
    if os.environ.get('GITHUB_ACTIONS') == 'true':
        require(os.environ.get('GITHUB_REF') == BRANCH, 'Wrong audit branch')
        require(os.environ.get('GITHUB_SHA') == head, 'Checkout does not match triggering commit')
    write_json(evidence / 'source.json', dict(head=head, prepared_commit=PREPARED_COMMIT,
        upstream_baseline=lock['upstream_baseline'], content_lock='all original tracked paths',
        input_hashes={p: digest(AUDIT / p) for p in (*GATE_HASHES, 'source_lock.json')},
        defconfig_sha256=digest(source / 'arch/arm64/configs' / DEFCONFIG)))
    print('::warning title=Embedded ksud stub::' + STUB_WARNING, flush=True)
    write_json(evidence / 'limitations.json', dict(warnings=[STUB_WARNING],
        hardware_certified=False, boot_certified=False, embedded_ksud_usable=False))


def load_gate():
    for name, expected in GATE_HASHES.items():
        require(digest(AUDIT / name) == expected, 'Audit input hash mismatch: ' + name)
    spec = importlib.util.spec_from_file_location('a25_final_gate', AUDIT / 'audit_final.py')
    gate = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(gate)
    baseline = gate.load_expected(AUDIT / 'expected_crc.json')
    policy = json.loads((AUDIT / 'policy.json').read_text(encoding='utf-8'))
    require(len(policy['config']) == 29 and baseline['counts']['records'] == 977,
            'Must retain all 29 policy settings and 977 ABI records')
    return gate, policy


def config_gate(gate, policy, out, evidence):
    shutil.copyfile(out / '.config', evidence / 'resolved.config')
    cfg = gate.config_parse((out / '.config').read_text(encoding='utf-8'))
    rows = [dict(key=key, expected=value, actual=cfg.get(key), passed=cfg.get(key) == value)
            for key, value in policy['config'].items()]
    opt_in = bool(re.search(r'\ballow_unverified_hw\s*=\s*(?:1|y|true|on)\b',
                           cfg.get('CONFIG_CMDLINE', ''), re.I))
    write_json(evidence / 'config-policy.json', dict(checked=29, settings=rows,
        passed=all(row['passed'] for row in rows) and not opt_in, hardware_opt_in=opt_in))
    for row in rows:
        print('CONFIG POLICY ' + json.dumps(row), flush=True)
    require(all(row['passed'] for row in rows) and not opt_in, 'Resolved config policy failed before full build')
    return cfg


def download_toolchain(runner, work):
    archive = work / 'toolchain.7z'
    attempts = []
    for attempt in range(1, 4):
        info = dict(attempt=attempt)
        attempts.append(info)
        try:
            runner.run(['/usr/bin/curl', '--fail', '--location', '--show-error',
                '--proto', '=https', '--proto-redir', '=https', '--connect-timeout', '15',
                '--max-time', '180', '--retry', '0', '--output', archive, TOOLCHAIN_URL],
                'download-%d' % attempt, timeout=180)
            info.update(size=archive.stat().st_size, sha256=digest(archive))
            require(info['size'] == TOOLCHAIN_SIZE, 'Toolchain size mismatch')
            require(info['sha256'] == TOOLCHAIN_SHA256, 'Toolchain SHA256 mismatch')
            info['verified'] = True
            break
        except (OSError, ValueError, subprocess.SubprocessError) as exc:
            info['error'] = str(exc)
            if archive.exists():
                info['downloaded_bytes'] = archive.stat().st_size
                archive.unlink()
            if attempt == 3:
                raise
            print('::warning::Toolchain attempt failed; bounded retry: ' + str(exc), flush=True)
        finally:
            write_json(work / 'evidence/toolchain-download.json', dict(url=TOOLCHAIN_URL,
                expected_bytes=TOOLCHAIN_SIZE, expected_sha256=TOOLCHAIN_SHA256, attempts=attempts))
    unpack = work / 'toolchain'
    unpack.mkdir()
    runner.run(['/usr/bin/7z', 'x', '-y', archive, '-o' + str(unpack)], 'toolchain-extract', timeout=180)
    # clang may be a symlink. Check bin directories, not find -type f clang.
    bins = [p for p in unpack.rglob('bin') if (p / 'clang').is_file()]
    require(len(bins) == 1, 'Expected one toolchain/bin; found ' + repr(bins))
    tc = bins[0]
    for name in ('clang', 'ld.lld', 'llvm-ar', 'llvm-nm', 'llvm-objcopy', 'llvm-objdump',
                 'llvm-strip', 'llvm-readelf', 'llvm-size'):
        require(os.access(tc / name, os.X_OK), 'Missing executable tool: ' + name)
    return tc


def make_arguments(tc, out):
    cc = str(tc / 'clang') + ' --target=aarch64-linux-gnu'
    values = dict(O=str(out), ARCH='arm64', LLVM='1', LLVM_IAS='1', CROSS_COMPILE='',
        CLANG_TRIPLE='aarch64-linux-gnu-', CROSS_COMPILE_COMPAT='arm-linux-gnueabi-',
        CC=cc, AS=cc, CC_COMPAT=str(tc / 'clang') + ' --target=arm-linux-gnueabi',
        LD_COMPAT=str(tc / 'ld.lld'), HOSTCC='/usr/bin/gcc', HOSTCXX='/usr/bin/g++',
        HOSTLD='/usr/bin/ld', HOSTAR='/usr/bin/ar', HOSTLDFLAGS='',
        LOCALVERSION='', KBUILD_BUILD_USER='gravity', KBUILD_BUILD_HOST='a25',
        KBUILD_BUILD_VERSION='1', KBUILD_MODPOST_WARN='', KBUILD_MODPOST_NOFINAL='',
        KCFLAGS=' '.join('-Wno-error=' + w for w in DEMOTIONS))
    for key, name in dict(LD='ld.lld', AR='llvm-ar', NM='llvm-nm', OBJCOPY='llvm-objcopy',
                          OBJDUMP='llvm-objdump', STRIP='llvm-strip', READELF='llvm-readelf',
                          OBJSIZE='llvm-size').items():
        values[key] = str(tc / name)
    return ['/usr/bin/make'] + [key + '=' + value for key, value in values.items()]


def smoke_tools(runner, tc, work):
    version = runner.run([tc / 'clang', '--version'], 'clang-version').read_text()
    require(re.search(r'clang version 22\.0\.0(?:git)?\b', version), 'Expected clang 22.0.0 family')
    runner.run([tc / 'ld.lld', '--version'], 'lld-version')
    runner.run(['/usr/bin/gcc', '--version'], 'host-gcc-version')
    runner.run(['/usr/bin/ld', '--version'], 'host-ld-version')
    probe = work / 'probes'
    probe.mkdir()
    c = probe / 'hello.c'
    c.write_text('int main(void) { return 0; }\n', encoding='ascii')
    runner.run(['/usr/bin/gcc', c, '-o', probe / 'host-hello'], 'host-link-smoke')
    runner.run([probe / 'host-hello'], 'host-exec-smoke')
    runner.run([tc / 'clang', '--target=aarch64-linux-gnu', '-c', c, '-o', probe / 'arm64.o'],
               'target-compile-smoke')
    data = (probe / 'arm64.o').read_bytes()
    require(data[:6] == b'\x7fELF\x02\x01' and struct.unpack_from('<H', data, 18)[0] == 183,
            'Target probe is not AArch64 ELF')


def verify_final_report(report):
    require(report.get('passed') is True and report.get('scope') == 'final_artifact_gate',
            'Final gate did not pass in final-artifact mode')
    require(report['abi']['checked_records'] == 977 and report['abi']['unique_symbols'] == 543,
            'Final gate did not cover the full ABI baseline')
    require(not report['abi']['errors'] and not report['errors'], 'Final gate contains failures')
    require(isinstance(report['verification'].get('linked_crc_readback'), int) and
            report['verification']['linked_crc_readback'] > 0, 'Missing linked CRC readback')
    require(report['verification']['embedded_config'] == 'compared_all_entries', 'Missing full IKCONFIG comparison')
    require(report['verification']['hardware_gate'] == 'linked_initial_byte_zero', 'Missing final hardware gate')


def verify_modules(out, policy, gate, evidence):
    order = (out / 'modules.order').read_text(encoding='utf-8').splitlines()
    require(order and len(order) == len(set(order)), 'Empty or duplicate modules.order')
    paths = []
    rows = []
    cfg = gate.config_parse((out / '.config').read_text())
    want_vm = policy['release'] + ' ' + ''.join(token + ' ' for key, token in
        [('CONFIG_SMP', 'SMP'), ('CONFIG_PREEMPT', 'preempt'), ('CONFIG_MODULE_UNLOAD', 'mod_unload'),
         ('CONFIG_MODVERSIONS', 'modversions')] if cfg.get(key) == 'y') + 'aarch64'
    for name in order:
        entry = Path(name)
        # This 4.19 tree emits "kernel/<object-path>.ko" for INSTALL layout
        # (scripts/Makefile.build); the output file lacks that one prefix.
        require(not entry.is_absolute() and '..' not in entry.parts and entry.suffix == '.ko'
                and entry.parts[0] == 'kernel', 'Bad 4.19 modules.order path: ' + name)
        rel = Path(*entry.parts[1:])
        path = out / rel
        data = path.read_bytes()
        require(data[:6] == b'\x7fELF\x02\x01' and struct.unpack_from('<HH', data, 16) == (1, 183),
                'Module is not AArch64 ET_REL: ' + name)
        vermagic = re.findall(rb'\x00vermagic=([^\x00]+)', data)
        require(len(vermagic) == 1 and vermagic[0].decode('ascii').strip() == want_vm,
                'Wrong/missing module vermagic: ' + name)
        rows.append(dict(path=str(rel), modules_order_entry=name, size=len(data), sha256=hashlib.sha256(data).hexdigest(),
                         vermagic=vermagic[0].decode('ascii')))
        paths.append(path.resolve())
    require(set(paths) == {p.resolve() for p in out.rglob('*.ko')}, 'KO set differs from modules.order')
    write_json(evidence / 'modules.json', dict(count=len(rows), modules=rows))


def preserve_small_evidence(out, evidence):
    for relative, target in [('.config', 'resolved.config'),
        ('include/config/kernel.release', 'kernel.release'), ('include/generated/compile.h', 'compile.h'),
        ('include/generated/utsrelease.h', 'utsrelease.h'), ('modules.order', 'modules.order'),
        ('modules.builtin', 'modules.builtin')]:
        src = out / relative
        if src.is_file():
            shutil.copyfile(src, evidence / target)
    # Big outputs are uploaded directly from out/, without copying or stripping.
    files = [out / p for p in ('vmlinux', 'Module.symvers', 'System.map',
                              'arch/arm64/boot/Image', 'arch/arm64/boot/Image.gz')]
    files += sorted(out.rglob('*.ko')) if out.is_dir() else []
    write_json(evidence / 'available-products.json', [dict(path=str(p.relative_to(out)),
        size=p.stat().st_size, sha256=digest(p)) for p in files if p.is_file()])


def compile_audit(args):
    source, work = args.source.resolve(), args.work.resolve()
    require(sys.platform.startswith('linux'), 'Real compilation requires Linux')
    require(work != source and source not in work.parents, 'Work must be outside source')
    require(source != work and work not in source.parents, 'Source must be outside work')
    require(not any(c.isspace() for c in str(source) + str(work)), 'Kbuild paths must not contain whitespace')
    work.mkdir(parents=True, exist_ok=True)
    evidence, out = work / 'evidence', work / 'out'
    evidence.mkdir(exist_ok=True)
    # Refuse stale output; never declare a previous Image a successful new build.
    require(not out.exists() and not (work / 'toolchain').exists(), 'Require fresh work dir; no cached kernel outputs')
    status = dict(passed=False, scope='compile_and_offline_audit_only', stage='preflight',
                  full_make_succeeded=False, final_gate_succeeded=False, warnings=[STUB_WARNING])
    write_json(evidence / 'status.json', status)
    env = dict(os.environ, PATH='/usr/bin:/bin', LC_ALL='C', LANG='C',
               LOCALVERSION='', KBUILD_BUILD_USER='gravity', KBUILD_BUILD_HOST='a25',
               PYTHONDONTWRITEBYTECODE='1', GIT_OPTIONAL_LOCKS='0')
    # Remove inherited compiler/make overrides; never touch TMP/TEMP/TMPDIR.
    for key in list(env):
        if key in {'MAKEFLAGS', 'MFLAGS', 'MAKEOVERRIDES', 'KBUILD_OUTPUT', 'KBUILD_EXTMOD',
                   'KCONFIG_CONFIG', 'KCONFIG_ALLCONFIG', 'KBUILD_KCONFIG', 'KBUILD_SRC',
                   'CC', 'CXX', 'LD', 'AR', 'NM', 'CFLAGS', 'CPPFLAGS', 'LDFLAGS',
                   'KCFLAGS', 'KAFLAGS', 'CPATH', 'C_INCLUDE_PATH', 'CPLUS_INCLUDE_PATH',
                   'LIBRARY_PATH', 'GCC_EXEC_PREFIX', 'COMPILER_PATH'}:
            del env[key]
    runner = Runner(evidence, source, env)
    source_verified = False
    try:
        gate, policy = load_gate()
        verify_source(source, evidence)
        source_verified = True
        require(json.loads((source / 'scripts/a25_compile_audit_policy.json').read_text()) == policy,
                'Prepared-tree policy differs from final audit policy')
        write_json(evidence / 'warning-policy.json', dict(kcflags_demotions=list(DEMOTIONS),
            changed_source_files=[], blanket_no_error=False,
            reason='Original workflow compatibility list plus top-level incompatible-pointer-types',
            existing_ksu_suppressions=[l for l in (source / 'drivers/kernelsu/Makefile').read_text().splitlines()
                                       if l.startswith('ccflags-y') and '-Wno-' in l]))
        status['stage'] = 'toolchain'
        tc = download_toolchain(runner, work)
        smoke_tools(runner, tc, work)
        out.mkdir()
        make = make_arguments(tc, out)
        write_json(evidence / 'build-settings.json', dict(make_argv=make, jobs=args.jobs,
            PATH=env['PATH'], LOCALVERSION='', KBUILD_BUILD_USER='gravity', KBUILD_BUILD_HOST='a25',
            toolchain_url=TOOLCHAIN_URL, toolchain_sha256=TOOLCHAIN_SHA256,
            targets=['Image.gz', 'modules']))
        status['stage'] = 'configure'
        runner.run(make + [DEFCONFIG], 'make-defconfig', timeout=180)
        config_gate(gate, policy, out, evidence)
        runner.run(make + ['-j%d' % args.jobs, 'V=1', 'prepare', 'modules_prepare'], 'make-prepare', timeout=300)
        release_log = runner.run(make + ['-s', '--no-print-directory', 'kernelrelease'], 'make-kernelrelease')
        release_lines = [line.strip() for line in release_log.read_text().splitlines() if line.strip()]
        # Vendor $(info ...) is printed even by make -s. Require the final
        # kernelrelease result exactly, rather than confusing chatter with it.
        require(release_lines and release_lines[-1] == policy['release'], 'Pre-build release is not exact')
        status['stage'] = 'full-build'
        runner.run(make + ['-j%d' % args.jobs, 'V=1', 'Image.gz', 'modules'], 'make-full', timeout=3900)
        status['full_make_succeeded'] = True
        preserve_small_evidence(out, evidence)
        status['stage'] = 'final-gate'
        config_gate(gate, policy, out, evidence)
        # No fixture mode, no CRC rewriting: final binary tables/config must match.
        runner.run([sys.executable, '-B', AUDIT / 'audit_final.py',
            '--module-symvers', out / 'Module.symvers', '--system-map', out / 'System.map',
            '--vmlinux', out / 'vmlinux', '--image', out / 'arch/arm64/boot/Image.gz',
            '--config', out / '.config', '--release', out / 'include/config/kernel.release',
            '--expected', AUDIT / 'expected_crc.json', '--policy', AUDIT / 'policy.json',
            '--report', evidence / 'abi-final-audit.json'], 'audit-final', timeout=300)
        report = json.loads((evidence / 'abi-final-audit.json').read_text())
        verify_final_report(report)
        status['final_gate_succeeded'] = True
        status['stage'] = 'module-products'
        verify_modules(out, policy, gate, evidence)
        status['passed'] = True
        status['stage'] = 'complete'
    except BaseException as exc:
        status['error'] = repr(exc)
        (evidence / 'exception.log').write_text(traceback.format_exc(), encoding='utf-8')
        print('::error::A25 audit failed in %s: %s' % (status['stage'], exc), flush=True)
    finally:
        try:
            preserve_small_evidence(out, evidence)
            if source_verified:
                dirty = git(source, 'status', '--porcelain', '--untracked-files=all')
                (evidence / 'source-status-after.txt').write_text(dirty, encoding='utf-8')
                if dirty:
                    status['passed'] = False
                    status['source_mutation'] = dirty
            else:
                status['source_check'] = 'preflight did not pass'
        except BaseException as exc:
            status['passed'] = False
            status['evidence_error'] = repr(exc)
        status['commands'] = runner.commands
        write_json(evidence / 'status.json', status)
        text = ('## A25 compile audit\n\n- Result: **%s**\n- Stage: `%s`\n'
                '- Full Image.gz + modules make: `%s`\n- Final 977-record gate: `%s`\n'
                '- **WARNING:** %s\n' % ('PASS' if status['passed'] else 'FAIL', status['stage'],
                    status['full_make_succeeded'], status['final_gate_succeeded'], STUB_WARNING))
        (evidence / 'summary.md').write_text(text, encoding='utf-8')
        if os.environ.get('GITHUB_STEP_SUMMARY'):
            with open(os.environ['GITHUB_STEP_SUMMARY'], 'a', encoding='utf-8') as f:
                f.write(text)
    return 0 if status['passed'] else 1


def self_test(work):
    """No kernel/tools download: fake-make negative controls, clearly labelled."""
    work.mkdir(parents=True, exist_ok=True)
    gate, policy = load_gate()
    require(len(policy['config']) == 29, 'Wrong policy count')
    with tempfile.TemporaryDirectory(prefix='mock-make-', dir=work) as tmp:
        root = Path(tmp)
        mock = root / 'mock_make.py'
        mock.write_text('import sys\nprint("MOCK MAKE STDOUT", flush=True)\n'
                        'print("MOCK MAKE STDERR", file=sys.stderr, flush=True)\n'
                        'sys.exit(int(sys.argv[1]))\n', encoding='utf-8')
        runner = Runner(root / 'logs', root, dict(os.environ), budget=60)
        # A stale filename cannot mask a nonzero child exit.
        (root / 'Image.gz').write_text('TEST-ONLY stale filename, NOT a kernel\n')
        try:
            runner.run([sys.executable, mock, '23'], 'mock-make-failed')
        except subprocess.CalledProcessError as exc:
            require(exc.returncode == 23, 'Child exit code lost')
        else:
            raise AssertionError('Mock make failure was reported as success')
        log = (root / 'logs/mock-make-failed.log').read_text()
        require('MOCK MAKE STDOUT' in log and 'MOCK MAKE STDERR' in log, 'Incomplete merged pipe log')
        runner.run([sys.executable, mock, '0'], 'mock-make-zero')
        try:
            verify_final_report(dict(passed=True, scope='fixture_only_NOT_build_acceptance'))
        except ValueError:
            pass
        else:
            raise AssertionError('Fixture report accepted as final gate')
        try:
            runner.run([sys.executable, '-c', 'import time; print("MOCK TIMEOUT",flush=True); time.sleep(10)'],
                       'mock-timeout', timeout=0.25)
        except subprocess.TimeoutExpired:
            require(runner.commands[-1].get('timed_out'), 'Timeout evidence lost')
        else:
            raise AssertionError('Timeout reported as success')
        write_json(work / 'self-test-result.json', dict(passed=True, kernel_build_performed=False,
            checks=['nonzero exit propagated (23)', 'stdout and stderr retained',
                    'stale Image cannot mask failed make', 'zero exit recorded',
                    'fixture final-report rejected', 'timeout terminates child and fails',
                    '29 policy entries', '977 baseline records']))
    print('PASS: failure-propagation self-test only; NO kernel build or ABI acceptance', flush=True)
    return 0


def interrupted(signum, frame):
    raise InterruptedError('Interrupted by signal %d' % signum)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, default=HERE.parent)
    parser.add_argument('--work', type=Path, required=True, help='Fresh output outside source; test scratch for --self-test')
    parser.add_argument('--jobs', type=int, choices=(2, 3, 4), default=4)
    parser.add_argument('--self-test', action='store_true', help='Mock make only; no download or kernel compilation')
    args = parser.parse_args()
    signal.signal(signal.SIGTERM, interrupted)
    try:
        return self_test(args.work.resolve()) if args.self_test else compile_audit(args)
    except (ValueError, OSError, subprocess.SubprocessError) as exc:
        print('::error::' + str(exc), file=sys.stderr)
        return 1


if __name__ == '__main__':
    sys.exit(main())
