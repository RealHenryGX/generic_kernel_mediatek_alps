#include <stdint.h>

typedef unsigned long size_t;

#define __NR_write 64
#define __NR_exit 93
#define __NR_bpf 280
#define BPF_OBJ_GET 7
#define BPF_OBJ_GET_INFO_BY_FD 15

struct bpf_attr_obj_get {
    uint64_t pathname;
    uint32_t bpf_fd;
    uint32_t file_flags;
};

struct bpf_attr_info {
    uint32_t bpf_fd;
    uint32_t info_len;
    uint64_t info;
};

struct bpf_map_info {
    uint32_t type, id, key_size, value_size, max_entries, map_flags;
    uint32_t name[4];
    uint32_t ifindex, btf_vmlinux_id, netns_dev, netns_ino;
    uint32_t btf_id, btf_key_type_id, btf_value_type_id, map_extra;
};

struct map_entry {
    const char *name;
    const char *path;
};

static long syscall3(long n, long a, long b, long c) {
    register long x0 asm("x0") = a;
    register long x1 asm("x1") = b;
    register long x2 asm("x2") = c;
    register long x8 asm("x8") = n;
    asm volatile("svc 0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x8) : "memory");
    return x0;
}

static long bpf(long cmd, void *attr, uint32_t size) {
    return syscall3(__NR_bpf, cmd, (long)attr, size);
}

static void out(const char *s, size_t n) {
    syscall3(__NR_write, 1, (long)s, n);
}

static void zero_bytes(void *ptr, size_t n) {
    unsigned char *bytes = (unsigned char *)ptr;
    while (n--) *bytes++ = 0;
}

static size_t str_len(const char *s) {
    size_t n = 0;
    while (s[n]) ++n;
    return n;
}

static void dec(long value) {
    char buf[32];
    int i = 0;

    if (value < 0) {
        out("-", 1);
        value = -value;
    }
    do {
        buf[i++] = (char)('0' + value % 10);
        value /= 10;
    } while (value);
    while (i) out(&buf[--i], 1);
}

static void probe(const struct map_entry *entry) {
    struct bpf_attr_obj_get get = { (uint64_t)entry->path, 0, 0 };
    struct bpf_map_info map;
    struct bpf_attr_info info;
    long fd = bpf(BPF_OBJ_GET, &get, sizeof(get));

    zero_bytes(&map, sizeof(map));
    zero_bytes(&info, sizeof(info));

    out(entry->name, str_len(entry->name));
    out(" ", 1);
    out("fd=", 3);
    dec(fd);
    if (fd < 0) {
        out("\n", 1);
        return;
    }

    info.bpf_fd = (uint32_t)fd;
    info.info_len = sizeof(map);
    info.info = (uint64_t)&map;
    out(" info=", 6);
    dec(bpf(BPF_OBJ_GET_INFO_BY_FD, &info, sizeof(info)));
    out(" id=", 4);
    dec(map.id);
    out(" type=", 6);
    dec(map.type);
    out(" key=", 5);
    dec(map.key_size);
    out(" value=", 7);
    dec(map.value_size);
    out(" max=", 5);
    dec(map.max_entries);
    out(" flags=", 7);
    dec(map.map_flags);
    out("\n", 1);
}

void _start(void) {
    static const struct map_entry maps[] = {
        { "configuration", "/sys/fs/bpf/netd_shared/map_netd_configuration_map" },
        { "cookie_tag", "/sys/fs/bpf/netd_shared/map_netd_cookie_tag_map" },
        { "uid_owner", "/sys/fs/bpf/netd_shared/map_netd_uid_owner_map" },
        { "uid_permission", "/sys/fs/bpf/netd_shared/map_netd_uid_permission_map" },
        { "data_saver", "/sys/fs/bpf/netd_shared/map_netd_data_saver_enabled_map" },
        { "local_net_access", "/sys/fs/bpf/netd_shared/map_netd_local_net_access_map" },
        { "local_net_blocked_uid", "/sys/fs/bpf/netd_shared/map_netd_local_net_blocked_uid_map" },
    };
    size_t i;

    for (i = 0; i < sizeof(maps) / sizeof(maps[0]); ++i) probe(&maps[i]);
    syscall3(__NR_exit, 0, 0, 0);
}
