#include <stdint.h>

#define __NR_write 64
#define __NR_exit 93
#define __NR_bpf 280
#define BPF_OBJ_GET 7
#define BPF_OBJ_GET_INFO_BY_FD 15

struct bpf_obj_get_attr {
    uint64_t pathname;
    uint32_t bpf_fd;
    uint32_t file_flags;
    unsigned char reserved[0x98 - 0x18];
};

struct bpf_info_attr {
    uint32_t bpf_fd;
    uint32_t info_len;
    uint64_t info;
    unsigned char reserved[0x98 - 0x10];
};

struct bpf_map_info {
    uint32_t type, id, key_size, value_size, max_entries, map_flags;
    uint32_t name[4];
    uint32_t ifindex, btf_vmlinux_id, netns_dev, netns_ino;
    uint32_t btf_id, btf_key_type_id, btf_value_type_id, map_extra;
    uint64_t map_extra2;
};

static long syscall3(long n, long a, long b, long c) {
    register long x0 asm("x0") = a;
    register long x1 asm("x1") = b;
    register long x2 asm("x2") = c;
    register long x8 asm("x8") = n;
    asm volatile("svc 0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x8) : "memory");
    return x0;
}

static void zero_bytes(void *ptr, unsigned long n) {
    unsigned char *p = ptr;
    while (n--) *p++ = 0;
}

static void out(const char *s, unsigned long n) {
    syscall3(__NR_write, 1, (long)s, n);
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

void _start(void) {
    static const char path[] =
        "/sys/fs/bpf/netd_shared/map_netd_configuration_map";
    struct bpf_obj_get_attr get;
    struct bpf_info_attr info_attr;
    unsigned char info_buffer[0x58];
    struct bpf_map_info *info = (struct bpf_map_info *)info_buffer;
    long fd;
    long result;

    zero_bytes(&get, sizeof(get));
    zero_bytes(&info_attr, sizeof(info_attr));
    zero_bytes(info_buffer, sizeof(info_buffer));
    get.pathname = (uint64_t)path;
    fd = syscall3(__NR_bpf, BPF_OBJ_GET, (long)&get, sizeof(get));
    out("get=", 4);
    dec(fd);
    if (fd >= 0) {
        info_attr.bpf_fd = (uint32_t)fd;
        info_attr.info_len = 0x58;
        info_attr.info = (uint64_t)info_buffer;
        result = syscall3(__NR_bpf, BPF_OBJ_GET_INFO_BY_FD,
                          (long)&info_attr, sizeof(info_attr));
        out(" info=", 6);
        dec(result);
        out(" type=", 6);
        dec(info->type);
        out(" key=", 5);
        dec(info->key_size);
        out(" value=", 7);
        dec(info->value_size);
        out("\n", 1);
    }
    syscall3(__NR_exit, 0, 0, 0);
}
