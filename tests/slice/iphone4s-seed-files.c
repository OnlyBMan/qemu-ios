/* The restore writes seeded files through native HFS after creating the keybag.
 * SLICE contrib/it-n94seed/it_n94seed.c fn seed_files
 */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef uint32_t u32;
static unsigned char buf[65536], archive[100000], output[100000];
static size_t position, length, written;
static unsigned owner, group, mode, opens;
static int fail_write;
static int readall(int fd, void *p, unsigned n)
{
    if (position + n > length)
        return 0;
    memcpy(p, archive + position, n);
    position += n;
    return 1;
}
static int seed_open(const char *path, int flags, unsigned permissions)
{
    assert(strcmp(path, "/mnt2/mobile/seed") == 0);
    opens++;
    return 1;
}
static int seed_write(int fd, const void *p, unsigned n)
{
    if (fail_write)
        return -1;
    assert(written + n <= sizeof(output));
    memcpy(output + written, p, n);
    written += n;
    return n;
}
static int seed_unlink(const char *path)
{
    return 0;
}
static int seed_close(int fd)
{
    return 0;
}
static int seed_chown(int fd, unsigned uid, unsigned gid)
{
    owner = uid;
    group = gid;
    return 0;
}
static int seed_chmod(int fd, unsigned permissions)
{
    mode = permissions;
    return 0;
}
static int seed_sync(int fd)
{
    return 0;
}
#define open seed_open
#define write seed_write
#define unlink seed_unlink
#define close seed_close
#define fchown seed_chown
#define fchmod seed_chmod
#define fsync seed_sync
#include "slice.h"
static void make_archive(const char *path)
{
    memset(archive, 0, sizeof(archive));
    position = written = opens = 0;
    u32 h[] = {strlen(path), 90000, 501, 20, 0100640};
    memcpy(archive, h, sizeof(h));
    memcpy(archive + 20, path, h[0]);
    memset(archive + 20 + h[0], 0x5a, h[1]);
    length = 40 + h[0] + h[1];
}
int main(void)
{
    make_archive("mobile/seed");
    assert(seed_files(0) == 0 && position == length && opens == 1 && written == 90000);
    assert(owner == 501 && group == 20 && mode == 0640);
    for (unsigned i = 0; i < written; i++)
        assert(output[i] == 0x5a);
    make_archive("../escape");
    assert(seed_files(0) != 0 && opens == 0);
    make_archive("mobile/../../escape");
    assert(seed_files(0) != 0 && opens == 0);
    make_archive("/escape");
    assert(seed_files(0) != 0 && opens == 0);
    make_archive("mobile/seed");
    fail_write = 1;
    assert(seed_files(0) != 0);
    return 0;
}
