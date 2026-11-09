#include <stdint.h>

#define __NR_write 64
#define __NR_exit 93
#define __NR_flock 32
#define __NR_bpf 280
#define BPF_OBJ_GET 7
#define LOCK_EX 2
#define LOCK_NB 4

struct bpf_attr_obj_get {
    uint64_t pathname;
    uint32_t bpf_fd;
    uint32_t file_flags;
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

void _start(void) {
    static const char path[] =
        "/sys/fs/bpf/netd_shared/map_netd_configuration_map";
    struct bpf_attr_obj_get get = { (uint64_t)path, 0, 0 };
    long fd1 = syscall3(__NR_bpf, BPF_OBJ_GET, (long)&get, sizeof(get));
    long fd2 = syscall3(__NR_bpf, BPF_OBJ_GET, (long)&get, sizeof(get));
    long lock1 = syscall3(__NR_flock, fd1, LOCK_EX | LOCK_NB, 0);
    long lock2 = syscall3(__NR_flock, fd2, LOCK_EX | LOCK_NB, 0);

    out("fd1=", 4);
    dec(fd1);
    out(" fd2=", 5);
    dec(fd2);
    out(" lock1=", 7);
    dec(lock1);
    out(" lock2=", 7);
    dec(lock2);
    out("\n", 1);
    syscall3(__NR_exit, 0, 0, 0);
}
