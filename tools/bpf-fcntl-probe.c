#include <stdint.h>

#define __NR_write 64
#define __NR_close 57
#define __NR_exit 93
#define __NR_fcntl 25
#define __NR_bpf 280
#define BPF_OBJ_GET 7
#define F_OFD_SETLK 37
#define F_WRLCK 1
#define F_SETLK 6

struct bpf_attr_obj_get {
    uint64_t pathname;
    uint32_t bpf_fd;
    uint32_t file_flags;
};

struct flock64_aarch64 {
    int16_t l_type;
    int16_t l_whence;
    int64_t l_start;
    int64_t l_len;
    int32_t l_pid;
    int32_t pad;
};

static long syscall3(long n, long a, long b, long c) {
    register long x0 asm("x0") = a;
    register long x1 asm("x1") = b;
    register long x2 asm("x2") = c;
    register long x8 asm("x8") = n;
    asm volatile("svc 0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x8) : "memory");
    return x0;
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

static void probe(const char *name, unsigned long name_len, const char *path) {
    struct bpf_attr_obj_get get = { (uint64_t)path, 0, 0 };
    struct flock64_aarch64 lock = { F_WRLCK, 0, 0, 1, 0, 0 };
    long fd1 = syscall3(__NR_bpf, BPF_OBJ_GET, (long)&get, sizeof(get));
    long fd2 = syscall3(__NR_bpf, BPF_OBJ_GET, (long)&get, sizeof(get));
    long first = -999;
    long second = -999;

    /* The device JNI uses the local BPF fd as l_start and locks one byte. */
    if (fd1 >= 0) {
        lock.l_start = fd1;
        first = syscall3(__NR_fcntl, fd1, F_OFD_SETLK, (long)&lock);
    }
    if (fd2 >= 0) {
        lock.l_start = fd2;
        second = syscall3(__NR_fcntl, fd2, F_OFD_SETLK, (long)&lock);
    }

    out(name, name_len);
    out(" fd1=", 5); dec(fd1);
    out(" fd2=", 5); dec(fd2);
    out(" first=", 7); dec(first);
    out(" second=", 8); dec(second);
    out("\n", 1);
    if (fd1 >= 0) syscall3(__NR_close, fd1, 0, 0);
    if (fd2 >= 0) syscall3(__NR_close, fd2, 0, 0);
}

void _start(void) {
    probe("configuration", 13, "/sys/fs/bpf/netd_shared/map_netd_configuration_map");
    probe("uid_owner", 9, "/sys/fs/bpf/netd_shared/map_netd_uid_owner_map");
    syscall3(__NR_exit, 0, 0, 0);
}
