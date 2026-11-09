#include <stdint.h>

#define __NR_write 64
#define __NR_exit 93
#define __NR_bpf 280
#define BPF_MAP_LOOKUP_ELEM 1
#define BPF_MAP_UPDATE_ELEM 2
#define BPF_MAP_DELETE_ELEM 3
#define BPF_OBJ_GET 7
#define BPF_MAP_GET_NEXT_KEY 4

struct bpf_obj_get_attr { uint64_t pathname; uint32_t bpf_fd; uint32_t file_flags; unsigned char reserved[0x98 - 0x18]; };
struct bpf_next_key_attr { uint32_t map_fd; uint32_t reserved; uint64_t key; uint64_t next_key; uint64_t flags; };
struct bpf_lookup_attr { uint32_t map_fd; uint32_t reserved; uint64_t key; uint64_t value; uint64_t flags; };
struct bpf_update_attr { uint32_t map_fd; uint32_t reserved; uint64_t key; uint64_t value; uint64_t flags; };

static long syscall3(long n, long a, long b, long c) {
    register long x0 asm("x0") = a;
    register long x1 asm("x1") = b;
    register long x2 asm("x2") = c;
    register long x8 asm("x8") = n;
    asm volatile("svc 0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x8) : "memory");
    return x0;
}
static void out(const char *s, unsigned long n) { syscall3(__NR_write, 1, (long)s, n); }
static void zero_bytes(void *p, unsigned long n) { unsigned char *b = p; while (n--) *b++ = 0; }
static void dec(long v) {
    char b[32]; int i = 0;
    if (v < 0) { out("-", 1); v = -v; }
    do { b[i++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (i) out(&b[--i], 1);
}
static long open_map(const char *path) {
    struct bpf_obj_get_attr a; zero_bytes(&a, sizeof(a)); a.pathname = (uint64_t)path;
    return syscall3(__NR_bpf, BPF_OBJ_GET, (long)&a, sizeof(a));
}
static long lookup(long fd, void *key, void *value) {
    struct bpf_lookup_attr a; zero_bytes(&a, sizeof(a));
    a.map_fd = (uint32_t)fd; a.key = (uint64_t)key; a.value = (uint64_t)value;
    return syscall3(__NR_bpf, BPF_MAP_LOOKUP_ELEM, (long)&a, sizeof(a));
}
static long next_key(long fd, void *key, void *next) {
    struct bpf_next_key_attr a; zero_bytes(&a, sizeof(a));
    a.map_fd = (uint32_t)fd; a.key = (uint64_t)key; a.next_key = (uint64_t)next;
    return syscall3(__NR_bpf, BPF_MAP_GET_NEXT_KEY, (long)&a, sizeof(a));
}
static long update(long fd, void *key, void *value) {
    struct bpf_update_attr a; zero_bytes(&a, sizeof(a));
    a.map_fd = (uint32_t)fd; a.key = (uint64_t)key; a.value = (uint64_t)value;
    a.flags = 0;
    return syscall3(__NR_bpf, BPF_MAP_UPDATE_ELEM, (long)&a, sizeof(a));
}
static long delete_key(long fd, void *key) {
    struct bpf_update_attr a; zero_bytes(&a, sizeof(a));
    a.map_fd = (uint32_t)fd; a.key = (uint64_t)key;
    return syscall3(__NR_bpf, BPF_MAP_DELETE_ELEM, (long)&a, sizeof(a));
}
static void dump_config(void) {
    static const char path[] = "/sys/fs/bpf/netd_shared/map_netd_configuration_map";
    uint32_t key, value; long fd = open_map(path);
    out("configuration fd=", 19); dec(fd); out("\n", 1);
    if (fd < 0) return;
    for (key = 0; key < 2; ++key) {
        value = 0; out("configuration key=", 19); dec(key); out(" lookup=", 8);
        dec(lookup(fd, &key, &value)); out(" value=", 7); dec(value); out("\n", 1);
    }
}
static void dump_uid_owner(void) {
    static const char path[] = "/sys/fs/bpf/netd_shared/map_netd_uid_owner_map";
    uint32_t key, next, value[2]; long fd = open_map(path), result; unsigned long count = 0;
    out("uid_owner fd=", 14); dec(fd); out("\n", 1);
    if (fd < 0) return;
    result = next_key(fd, (void *)0, &next); out("uid_owner first=", 17); dec(result); out("\n", 1);
    while (result >= 0 && count < 128) {
        key = next; value[0] = value[1] = 0; out("uid_owner uid=", 15); dec(key);
        out(" lookup=", 8); dec(lookup(fd, &key, value)); out(" iif=", 5); dec(value[0]);
        out(" rule=", 6); dec(value[1]); out("\n", 1); ++count;
        result = next_key(fd, &key, &next);
    }
    out("uid_owner count=", 16); dec(count); out(" next_result=", 13); dec(result); out("\n", 1);
}
static void probe_uid_owner_update(void) {
    static const char path[] = "/sys/fs/bpf/netd_shared/map_netd_uid_owner_map";
    uint32_t key = 19999, value[2] = { 0, 4096 }, readback[2] = { 0, 0 };
    long fd = open_map(path), result;
    out("uid_owner_update fd=", 22); dec(fd); out(" update=", 8);
    if (fd < 0) { dec(fd); out("\n", 1); return; }
    result = update(fd, &key, value); dec(result); out(" lookup=", 8);
    dec(lookup(fd, &key, readback)); out(" rule=", 6); dec(readback[1]); out(" delete=", 8);
    dec(delete_key(fd, &key)); out("\n", 1);
}
void _start(void) { dump_config(); dump_uid_owner(); probe_uid_owner_update(); syscall3(__NR_exit, 0, 0, 0); }
