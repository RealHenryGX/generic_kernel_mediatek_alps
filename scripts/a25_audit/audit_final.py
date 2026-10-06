#!/usr/bin/env python3
"""Fail-closed A25 final-build ABI/config gate (Python 3.10+, stdlib only).
No build, device, packing, network or CRC rewriting. See README.md.
"""
import argparse
from collections import Counter, defaultdict
import gzip
import hashlib
import json
from pathlib import Path
import re
import struct
import sys

HERE = Path(__file__).resolve().parent
SYMBOL = re.compile(r'[A-Za-z_][A-Za-z0-9_.$]*\Z')
CRC = re.compile(r'0x[0-9a-fA-F]{8}\Z')
FIXTURE = 'ABI_AUDIT_FIXTURE'


def require(condition, message):
    if not condition:
        raise ValueError(message)


def config_parse(text):
    result = {}
    for line in text.splitlines():
        if line.startswith('CONFIG_'):
            m = re.fullmatch(r'(CONFIG_[A-Za-z0-9_]+)=(.+)', line)
            require(m is not None, 'Malformed config line: ' + line)
            key, value = m.groups()
        else:
            m = re.fullmatch(r'# (CONFIG_[A-Za-z0-9_]+) is not set', line)
            if not m:
                continue
            key, value = m[1], 'n'
        require(key not in result, 'Duplicate config entry: ' + key)
        result[key] = value
    require(bool(result), 'Empty config')
    return result


def load_expected(path):
    d = json.loads(path.read_text(encoding='utf-8'))
    require(d['schema'] == 1, 'Unsupported expected-CRC schema')
    records, modules = d['records'], d['modules']
    require(len(modules) == 9 and len(records) == 977, 'Expected 9 modules / 977 records')
    require(len({(r['module'], r['symbol']) for r in records}) == 977, 'Duplicate import record')
    require(len({r['symbol'] for r in records}) == 543, 'Expected 543 unique symbols')
    k = [r for r in records if r['provider'] == 'vmlinux']
    m = [r for r in records if r['provider'] != 'vmlinux']
    counts = dict(modules=len(modules), records=len(records), unique_symbols=543,
                  kernel_records=len(k), module_records=len(m),
                  module_symbols=len({r['symbol'] for r in m}))
    require(counts == d['counts'] and len(k) == 901 and len(m) == 76 and counts['module_symbols'] == 44,
            'Baseline provider/count assertions failed')
    require(all(re.fullmatch(r'module_0[1-9]', name) for name in modules), 'Non-anonymous module ID')
    for name, exports in modules.items():
        require(all(SYMBOL.fullmatch(s) and CRC.fullmatch(c) for s, c in exports.items()), 'Bad KO export metadata')
    for r in records:
        require(set(r) == {'module', 'symbol', 'crc', 'weak', 'provider'}, 'Unexpected import metadata')
        require(r['module'] in modules and SYMBOL.fullmatch(r['symbol']) and CRC.fullmatch(r['crc']), 'Bad import')
        require(type(r['weak']) is bool, 'weak must be boolean')
        if r['provider'] != 'vmlinux':
            require(r['provider'] in modules, 'Unknown KO provider')
            require(modules[r['provider']].get(r['symbol']) == r['crc'], 'KO provider CRC baseline conflict')
    return d


def symvers_parse(text, fixture=False):
    require(fixture or FIXTURE not in text, 'Fixture input is forbidden in final mode')
    result = {}
    for no, line in enumerate(text.splitlines(), 1):
        if not line.strip() or line.startswith('#'):
            continue
        fields = line.split()
        require(len(fields) in (4, 5), 'Malformed Module.symvers line %d' % no)
        crc, name, provider, kind = fields[:4]
        require(CRC.fullmatch(crc) and SYMBOL.fullmatch(name), 'Invalid CRC or symbol at line %d' % no)
        require(kind in {'EXPORT_SYMBOL', 'EXPORT_SYMBOL_GPL', 'EXPORT_SYMBOL_GPL_FUTURE',
                         'EXPORT_UNUSED_SYMBOL', 'EXPORT_UNUSED_SYMBOL_GPL'}, 'Unknown export kind: ' + kind)
        require(name not in result, 'Duplicate export in Module.symvers: ' + name)
        result[name] = dict(crc=crc.lower(), provider=provider, kind=kind,
                            namespace=fields[4] if len(fields) == 5 else '')
    require(bool(result), 'Empty Module.symvers')
    return result


def compare_abi(expected, exports):
    statuses = Counter()
    errors = []
    for r in expected['records']:
        name = r['symbol']
        provider = r['provider']
        if provider != 'vmlinux':
            actual = expected['modules'][provider][name]
            # These are the PRECOMPILED vendor modules, not newly built providers.
            if name in exports:
                status = 'provider_collision'
            else:
                status = 'match' if actual == r['crc'] else 'mismatch'
        else:
            entry = exports.get(name)
            actual = entry['crc'] if entry else None
            if entry is None:
                status = 'weak_missing' if r['weak'] else 'missing'
            elif entry['provider'].replace('\\', '/').split('/')[-1] != 'vmlinux':
                status = 'wrong_provider'
            elif entry['namespace']:
                status = 'unexpected_namespace'
            else:
                status = 'match' if actual == r['crc'] else 'mismatch'
        statuses[status] += 1
        if status not in ('match', 'weak_missing'):
            errors.append(dict(module=r['module'], symbol=name, expected=r['crc'],
                               actual=actual, provider=provider, status=status))
    return dict(counts=dict(statuses), checked_records=sum(statuses.values()),
                unique_symbols=len({r['symbol'] for r in expected['records']}),
                mismatch_unique=len({e['symbol'] for e in errors if e['status'] == 'mismatch'}), errors=errors)


def map_parse(text):
    result = defaultdict(list)
    for line in text.splitlines():
        fields = line.split()
        if len(fields) == 3 and re.fullmatch(r'[0-9a-fA-F]+', fields[0]) and len(fields[1]) == 1:
            result[fields[2]].append((int(fields[0], 16), fields[1]))
        elif line.strip() and not line.startswith('#'):
            raise ValueError('Malformed System.map line')
    require(bool(result), 'Empty System.map')
    return result


class Elf:
    """Minimal ELF64 little-endian AArch64 reader; no external nm needed."""
    def __init__(self, data):
        require(data[:6] == b'\x7fELF\x02\x01', 'vmlinux must be ELF64 little-endian')
        h = struct.unpack_from('<HHIQQQIHHHHHH', data, 16)
        require(h[0] in (2, 3) and h[1] == 183,
                'vmlinux must be linked AArch64 ET_EXEC/ET_DYN, not a partial ET_REL object')
        off, size, num, names_index = h[5], h[10], h[11], h[12]
        require(size == 64 and num > 0, 'Unsupported/missing ELF sections')
        self.data = data
        self.sections = [struct.unpack_from('<IIQQQQIIQQ', data, off + i * size) for i in range(num)]
        require(names_index < num, 'Invalid ELF section-name index')
        strings = self.section_data(names_index)
        self.section_names = [self.cstring(strings, s[0]) for s in self.sections]
        self.symbols = defaultdict(list)
        for i, s in enumerate(self.sections):
            if s[1] != 2:  # SHT_SYMTAB; stripped vmlinux is not acceptable
                continue
            require(s[9] == 24, 'Invalid ELF symbol size')
            names = self.section_data(s[6])
            raw = self.section_data(i)
            for j in range(0, len(raw), 24):
                n, info, other, index, value, length = struct.unpack_from('<IBBHQQ', raw, j)
                if n and index:
                    name = self.cstring(names, n)
                    self.symbols[name].append((value, index, length))
        require(bool(self.symbols), 'vmlinux must retain .symtab')

    @staticmethod
    def cstring(data, start):
        end = data.index(b'\0', start)
        return data[start:end].decode('ascii')

    def section_data(self, index):
        s = self.sections[index]
        require(s[1] != 8 and s[4] + s[5] <= len(self.data), 'Invalid ELF section data')
        return self.data[s[4]:s[4] + s[5]]

    def read(self, address, size):
        for s in self.sections:
            if s[2] & 2 and s[3] <= address and address + size <= s[3] + s[5]:
                if s[1] == 8:  # SHT_NOBITS => zero-initialized .bss
                    return bytes(size)
                start = s[4] + address - s[3]
                require(start + size <= len(self.data), 'Truncated vmlinux')
                return self.data[start:start + size]
        raise ValueError('ELF address not in allocated section: 0x%x' % address)

    def address(self, name):
        hits = self.symbols.get(name, [])
        require(len(hits) == 1, 'Missing/ambiguous linked symbol: ' + name)
        return hits[0][0]


class Image:
    def __init__(self, data, symbols):
        require(len(data) >= 64 and data[56:60] == b'ARM\x64', 'Expected uncompressed ARM64 Image')
        self.data, self.symbols = data, symbols
        self.base = self.address('_text')  # NOT _stext (A25 differs by 0x800)

    def address(self, name):
        hits = self.symbols.get(name, [])
        require(len(hits) == 1, 'Missing/ambiguous System.map symbol: ' + name)
        return hits[0][0]

    def read(self, address, size):
        if '__bss_start' in self.symbols and '__bss_stop' in self.symbols:
            if self.address('__bss_start') <= address and address + size <= self.address('__bss_stop'):
                return bytes(size)
        off = address - self.base
        require(0 <= off and off + size <= len(self.data), 'Image address out of bounds: 0x%x' % address)
        return self.data[off:off + size]


def linked_crcs(binary, config):
    """Read the actual linked kcrc tables, including CONFIG_MODULE_REL_CRCS.
Uses section-boundary symbols and __ksymtab_NAME addresses, not kallsyms CRC guesses.
"""
    result = {}
    for suffix, kind in [('', 'EXPORT_SYMBOL'), ('_gpl', 'EXPORT_SYMBOL_GPL'),
                         ('_gpl_future', 'EXPORT_SYMBOL_GPL_FUTURE'),
                         ('_unused', 'EXPORT_UNUSED_SYMBOL'), ('_unused_gpl', 'EXPORT_UNUSED_SYMBOL_GPL')]:
        start = '__start___ksymtab' + suffix
        stop = '__stop___ksymtab' + suffix
        if start not in binary.symbols or stop not in binary.symbols:
            continue
        lo, hi = binary.address(start), binary.address(stop)
        entries = sorted((hits[0][0], n[len('__ksymtab_'):]) for n, hits in binary.symbols.items()
                         if n.startswith('__ksymtab_') and len(hits) == 1 and lo <= hits[0][0] < hi)
        width = 8 if config.get('CONFIG_HAVE_ARCH_PREL32_RELOCATIONS') == 'y' else 16
        require(hi - lo == len(entries) * width, 'Incomplete linked export symbols: ' + start)
        if not entries:
            continue
        cstart, cstop = binary.address('__start___kcrctab' + suffix), binary.address('__stop___kcrctab' + suffix)
        require(cstop - cstart == len(entries) * 4, 'Linked CRC/export count mismatch')
        for i, (address, name) in enumerate(entries):
            require(address == lo + i * width, 'Non-contiguous linked ksymtab')
            if width == 8:
                name_at = address + 4 + struct.unpack('<i', binary.read(address + 4, 4))[0]
            else:
                name_at = struct.unpack('<Q', binary.read(address + 8, 8))[0]
            require(binary.read(name_at, len(name) + 1) == name.encode('ascii') + b'\0',
                    'Linked export name disagrees with symbol table: ' + name)
            at = cstart + i * 4
            raw = binary.read(at, 4)
            if config.get('CONFIG_MODULE_REL_CRCS') == 'y':
                raw = binary.read(at + struct.unpack('<i', raw)[0], 4)
            value = struct.unpack('<I', raw)[0]
            require(name not in result, 'Duplicate linked export: ' + name)
            result[name] = dict(crc='0x%08x' % value, kind=kind)
    require(bool(result), 'Missing linked ksymtab/kcrctab; preserve full System.map / unstripped vmlinux')
    return result


def embedded_config(data):
    start = data.find(b'IKCFG_ST')
    require(start >= 0, 'Missing IKCONFIG in linked artifact')
    stop = data.find(b'IKCFG_ED', start + 8)
    require(stop > start, 'Missing IKCONFIG end marker')
    require(data.find(b'IKCFG_ST', stop + 8) < 0, 'Ambiguous IKCONFIG payload')
    return config_parse(gzip.decompress(data[start + 8:stop]).decode('utf-8'))


def read_binary(path):
    data = path.read_bytes()
    return gzip.decompress(data) if data[:2] == b'\x1f\x8b' else data


def audit(args):
    expected = load_expected(args.expected)
    policy = json.loads(args.policy.read_text(encoding='utf-8'))
    exports = symvers_parse(args.module_symvers.read_text(encoding='utf-8'), args.fixture_mode)
    config_text = args.config.read_text(encoding='utf-8')
    require(args.fixture_mode or FIXTURE not in config_text, 'Fixture config forbidden in final mode')
    cfg = config_parse(config_text)
    report = dict(scope='fixture_only_NOT_build_acceptance' if args.fixture_mode else 'final_artifact_gate',
                  passed=False, abi=compare_abi(expected, exports), errors=[], verification={})
    errors = report['errors']
    def check(ok, code, detail):
        if not ok:
            errors.append(dict(code=code, detail=detail))
    vendor_exports = {name for module in expected['modules'].values() for name in module}
    for name in sorted(vendor_exports & exports.keys()):
        check(False, 'vendor_export_collision', name + ': precompiled KO export also present in final symvers')
    for name, wanted in policy['config'].items():
        check(cfg.get(name) == wanted, 'config', '%s expected=%s actual=%s' % (name, wanted, cfg.get(name, 'ABSENT')))
    check(not re.search(r'\ballow_unverified_hw\s*=\s*(?:1|y|true|on)\b', cfg.get('CONFIG_CMDLINE', ''), re.I),
          'hardware_opt_in', 'CONFIG_CMDLINE must not opt into unverified hardware')
    release = args.release.read_text(encoding='utf-8')
    check(release in (policy['release'], policy['release'] + '\n'), 'release', 'kernel.release must equal ' + policy['release'])
    binary = None
    maps = map_parse(args.system_map.read_text(encoding='utf-8')) if args.system_map else None
    if args.vmlinux:
        binary = Elf(args.vmlinux.read_bytes())
        if maps:
            for name in ('_text', 'linux_banner', policy['hardware_gate_symbol']):
                check(name in maps and len(maps[name]) == 1 and maps[name][0][0] == binary.address(name),
                      'artifact_pair', 'System.map/vmlinux disagree: ' + name)
    elif args.image and maps:
        binary = Image(read_binary(args.image), maps)
    elif not args.fixture_mode:
        raise ValueError('Final gate requires --vmlinux OR --system-map with --image (Image or Image.gz)')
    if args.fixture_mode and binary is None:
        require(args.banner is not None, 'Fixture test requires --banner text fixture')
        require(maps is not None, 'Fixture test requires --system-map')
        banner = args.banner.read_bytes().strip()
        symbols = maps
        report['verification']['linked_crc_readback'] = 'NOT_TESTED_fixture'
        gate = symbols.get(policy['hardware_gate_symbol'], [])
        check(len(gate) == 1 and gate[0][1] in 'bB', 'hardware_gate', 'Fixture must model false/.bss hardware gate')
    else:
        symbols = binary.symbols
        # Read actual linux_banner, not a matching substring elsewhere in .rodata.
        at = binary.address('linux_banner')
        banner_bytes = bytearray()
        for i in range(2048):
            value = binary.read(at + i, 1)
            if value == b'\0':
                break
            banner_bytes.extend(value)
        else:
            raise ValueError('Unterminated linux_banner')
        banner = bytes(banner_bytes).strip()
        check(embedded_config(binary.data) == cfg, 'embedded_config', 'Resolved .config differs from linked IKCONFIG')
        check(binary.read(binary.address(policy['hardware_gate_symbol']), 1) == b'\0',
              'hardware_gate', 'allow_unverified_hw must default false in final data/BSS')
        actual_exports = linked_crcs(binary, cfg)
        # Verify every vmlinux entry, not only module_layout or affected imports.
        wanted_exports = {name: e for name, e in exports.items()
                          if e['provider'].replace('\\', '/').split('/')[-1] == 'vmlinux'}
        check(actual_exports.keys() == wanted_exports.keys(), 'linked_export_set',
              dict(missing_from_binary=sorted(wanted_exports.keys() - actual_exports.keys()),
                   missing_from_symvers=sorted(actual_exports.keys() - wanted_exports.keys())))
        for name in sorted(actual_exports.keys() & wanted_exports.keys()):
            check(all(actual_exports[name][k] == wanted_exports[name][k] for k in ('crc', 'kind')),
                  'linked_crc', name)
        for name in {r['symbol'] for r in expected['records'] if r['provider'] != 'vmlinux'}:
            check(name not in actual_exports, 'provider_collision', name)
        report['verification']['linked_crc_readback'] = len(actual_exports)
        report['verification']['embedded_config'] = 'compared_all_entries'
        report['verification']['hardware_gate'] = 'linked_initial_byte_zero'
        if args.image and args.vmlinux:
            require(maps is not None, '--image with --vmlinux requires --system-map for raw Image readback')
            image = Image(read_binary(args.image), maps)
            check(linked_crcs(image, cfg) == actual_exports, 'image_crc', 'Image and vmlinux CRC tables differ')
            check(embedded_config(image.data) == cfg, 'image_config', 'Image config differs')
            check(image.read(image.address('linux_banner'), len(banner)) == banner,
                  'image_banner', 'Image banner differs')
            check(image.read(image.address(policy['hardware_gate_symbol']), 1) == b'\0',
                  'image_hardware_gate', 'Image enables unverified hardware')
    prefix = ('Linux version %s (%s) ' % (policy['release'], policy['builder'])).encode('ascii')
    check(banner.startswith(prefix), 'banner', 'Expected exact release and (gravity@a25) builder')
    for group, names in policy['symbol_groups'].items():
        for name in names:
            check(name in symbols, 'candidate_symbol', group + ': ' + name)
    report['verification']['candidate_symbols'] = sum(map(len, policy['symbol_groups'].values()))
    report['verification']['resolved_config_entries'] = len(cfg)
    report['verification']['baseline'] = expected['counts']
    report['inputs_sha256'] = {key: hashlib.sha256(getattr(args, key).read_bytes()).hexdigest()
                              for key in ('module_symvers', 'config', 'release', 'system_map', 'vmlinux', 'image', 'expected', 'policy')
                              if getattr(args, key, None)}
    report['passed'] = not errors and not report['abi']['errors']
    return report


def parser():
    p = argparse.ArgumentParser(description=__doc__)
    for key in ('module-symvers', 'config', 'release'):
        p.add_argument('--' + key, type=Path, required=True)
    for key in ('system-map', 'vmlinux', 'image', 'banner', 'report'):
        p.add_argument('--' + key, type=Path)
    p.add_argument('--expected', type=Path, default=HERE / 'expected_crc.json')
    p.add_argument('--policy', type=Path, default=HERE / 'policy.json')
    p.add_argument('--fixture-mode', action='store_true', help='test-only; NEVER build acceptance')
    return p


def main():
    args = parser().parse_args()
    try:
        report = audit(args)
        code = 0 if report['passed'] else 1
    except (ValueError, OSError, KeyError, TypeError, struct.error, UnicodeError, EOFError) as e:
        report = dict(passed=False, scope='input_error', errors=[dict(code='input', detail=str(e))])
        code = 2
    text = json.dumps(report, indent=2, sort_keys=True) + '\n'
    if args.report:
        args.report.write_text(text, encoding='utf-8')
    print(text, end='')
    return code


if __name__ == '__main__':
    sys.exit(main())
