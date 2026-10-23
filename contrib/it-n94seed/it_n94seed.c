/* Restore-ramdisk disk writer for N94. The PPN formatter and FTL remain stock guest
 * firmware. A single semihosting command-line argument names the host's sparse stream:
 * little-endian { uint64_t diskOffset; uint32_t length; bytes[length]; }, terminated
 * by length zero. Only preparation enables semihosting; normal device boots do not.
 */
typedef unsigned int u32;
typedef unsigned long long u64;
extern int open(const char *, int, ...);
extern int write(int, const void *, unsigned int);
extern long long lseek(int, long long, int);
extern unsigned int sleep(unsigned int);
extern void sync(void);
extern int reboot(int);
extern int printf(const char *, ...);
extern int fflush(void *);
static unsigned char buf[65536];
static int semi(int op, void *arg)
{
    register int r0 __asm__("r0") = op;
    register void *r1 __asm__("r1") = arg;
    __asm__ volatile("svc #0x123456" : "+r"(r0) : "r"(r1) : "memory", "cc");
    return r0;
}
static int readall(int fd, void *p, unsigned int size)
{
    /* Debug memory writes cannot fault in Darwin's demand-zero/COW pages. */
    volatile unsigned char *bytes = p;
    for (unsigned int at = 0; at < size; at += 4096)
        bytes[at] = 0xa5;
    if (size)
        bytes[size - 1] = 0xa5;
    u32 a[3] = {fd, (u32)p, size};
    return semi(6, a) == 0;
}
extern int close(int);
extern int unlink(const char *);
extern int fchmod(int, unsigned short);
extern int fchown(int, unsigned int, unsigned int);
extern int fsync(int);
extern int unmount(const char *, int);
extern int vfork(void);
extern int execl(const char *, const char *, ...);
extern int waitpid(int, int *, int);
extern void _exit(int);
extern void *dlopen(const char *, int);
extern void *dlsym(void *, const char *);

static int seed_files(int in)
{
    unsigned count = 0;
    while (1) {
        u32 h[5];
        if (!readall(in, h, sizeof(h)))
            return 1;
        if (!h[0]) {
            printf("it_n94seed: files complete %u\n", count);
            return 0;
        }
        if (h[0] >= 1024 || h[1] > 512 * 1024 * 1024)
            return 1;
        char path[1030] = "/mnt2/";
        if (!readall(in, path + 6, h[0]))
            return 1;
        path[6 + h[0]] = 0;
        /* The archive is generated from HFS catalog paths, never followed outside /mnt2. */
        for (unsigned i = 6; i < 6 + h[0]; i++) {
            if (!path[i] ||
                (path[i] == '.' && (i == 6 || path[i - 1] == '/') &&
                 (path[i + 1] == '/' || !path[i + 1] || (path[i + 1] == '.' && (path[i + 2] == '/' || !path[i + 2])))))
                return 1;
        }
        if (path[6] == '/')
            return 1;
        unlink(path);
        int out = open(path, 0x601, h[4] & 07777); /* O_WRONLY | O_CREAT | O_TRUNC */
        if (out < 0) {
            printf("it_n94seed: cannot create %s\n", path);
            return 1;
        }
        u32 left = h[1];
        while (left) {
            u32 n = left > sizeof(buf) ? sizeof(buf) : left;
            if (!readall(in, buf, n) || write(out, buf, n) != (int)n) {
                close(out);
                return 1;
            }
            left -= n;
        }
        int error = fchown(out, h[2], h[3]) || fchmod(out, h[4] & 07777) || fsync(out);
        close(out);
        if (error)
            return 1;
        count++;
    }
}
static int keybag_and_files(int in)
{
    void *iok = dlopen("/System/Library/Frameworks/IOKit.framework/Versions/A/IOKit", 2);
    void *mkb = dlopen("/System/Library/PrivateFrameworks/MobileKeyBag.framework/MobileKeyBag", 2);
    void *(*matching)(const char *) = iok ? dlsym(iok, "IOServiceMatching") : 0;
    u32 (*getService)(u32, void *) = iok ? dlsym(iok, "IOServiceGetMatchingService") : 0;
    int (*ioOpen)(u32, u32, u32, u32 *) = iok ? dlsym(iok, "IOServiceOpen") : 0;
    int (*scalar)(u32, u32, const u64 *, u32, u64 *, u32 *) = iok ? dlsym(iok, "IOConnectCallScalarMethod") : 0;
    int (*create)(void *, const char *) = mkb ? dlsym(mkb, "MKBKeyBagCreateSystem") : 0;
    u32 *taskSelf = dlsym(iok, "mach_task_self_");
    if (!matching || !getService || !ioOpen || !scalar || !create || !taskSelf)
        return 1;
    u32 service = 0, connection = 0;
    for (int i = 0; i < 120 && !service; i++) {
        service = getService(0, matching("AppleEffaceableStorage"));
        if (!service)
            sleep(1);
    }
    if (!service || ioOpen(service, *taskSelf, 0, &connection))
        return 1;
    u64 formatted = 0;
    u32 n = 1;
    int error = scalar(connection, 3, 0, 0, &formatted, &n);
    if (!error && !formatted) {
        n = 0;
        error = scalar(connection, 4, 0, 0, 0, &n);
    }
    if (error)
        return 1;
    int status = 0, pid = vfork();
    if (pid == 0) {
        execl("/sbin/mount_hfs", "mount_hfs", "/dev/disk0s2", "/mnt2", (char *)0);
        _exit(127);
    }
    if (pid < 0 || waitpid(pid, &status, 0) != pid || status) {
        printf("it_n94seed: mount failed %d\n", status);
        return 1;
    }
    error = create(0, "/mnt2");
    printf("it_n94seed: keybag %d\n", error);
    fflush(0);
    if (!error)
        error = seed_files(in);
    sync();
    if (unmount("/mnt2", 0))
        error = 1;
    return error;
}

int main(void)
{
    int con = open("/dev/console", 1);
    write(con, "it_n94seed: entered\n", 20);
    char path[4096];
    for (unsigned i = 0; i < sizeof(path); i++)
        ((volatile char *)path)[i] = 0;
    u32 command[2] = {(u32)path, sizeof(path)};
    if (semi(0x15, command) || !command[1] || command[1] >= sizeof(path))
        return 1;
    u32 a[3] = {(u32)path, 1, command[1]};
    int in = semi(1, a);
    printf("it_n94seed: host stream %d\n", in);
    fflush(0);
    u32 first[3];
    if (in < 0 || !readall(in, first, 12))
        return 1;
    if (first[0] == 0x4634394e && first[1] == 1 && first[2] == 0) {
        int error = keybag_and_files(in);
        printf("it_n94seed: %s\n", error ? "FAILED: keybag/files" : "keybag and files complete");
        sync();
        fflush(0);
        reboot(0x408);
        return error;
    }
    int out = -1;
    for (int i = 0; i < 120 && out < 0; i++) {
        out = open("/dev/rdisk0", 2);
        if (out < 0)
            sleep(1);
    }
    printf("it_n94seed: disk fd %d\n", out);
    fflush(0);
    if (in < 0 || out < 0) {
        fflush(0);
        reboot(0x408);
        return 1;
    }
    unsigned int count = 0;
    while (1) {
        u32 h[3];
        if (count == 0) {
            for (unsigned i = 0; i < 3; i++)
                h[i] = first[i];
        } else if (!readall(in, h, 12)) {
            printf("it_n94seed: header read failed\n");
            break;
        }
        if (!h[2]) {
            printf("it_n94seed: complete %u chunks\n", count);
            sync();
            fflush(0);
            reboot(0x408);
            break;
        }
        u64 off = ((u64)h[1] << 32) | h[0];
        if (h[2] > sizeof(buf) || (off & 4095) || (h[2] & 4095) || off + h[2] < off || off + h[2] > 3906250ULL * 4096 ||
            !readall(in, buf, h[2]) || lseek(out, off, 0) != (long long)off || write(out, buf, h[2]) != (int)h[2]) {
            printf("it_n94seed: IO failed chunk %u off %llx len %u\n", count, off, h[2]);
            break;
        }
        if ((++count & 255) == 0) {
            printf("it_n94seed: %u chunks off %llx\n", count, off);
            fflush(0);
        }
    }
    fflush(0);
    sync();
    reboot(0x408);
    return 1;
}
