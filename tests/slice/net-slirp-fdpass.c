/* The user netdev's guestfwd fdpass:FD (net/slirp.c): each guest connection's socket is one end of a new pair,
 * the other end passed over FD with one byte, so the host's server gets it with no path anyone could take; a
 * descriptor that isn't a Unix stream socket is refused, and a closed FD refuses the connection.
 *
 * SLICE net/slirp.c fn guestfwd_fdpass_connect guestfwd_fdpass_socket
 */
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>
#include "slice.h"

/* What the host's server does: take the descriptor that came with the byte. */
static int receive(int via)
{
    char byte = 0;
    struct iovec iov = { .iov_base = &byte, .iov_len = 1 };
    union {
        struct cmsghdr header;
        char space[CMSG_SPACE(sizeof(int))];
    } control;
    struct msghdr msg = { .msg_iov = &iov, .msg_iovlen = 1, .msg_control = control.space,
                          .msg_controllen = sizeof(control.space) };
    int fd = -1;
    if (recvmsg(via, &msg, 0) != 1) {
        return -1;
    }
    struct cmsghdr *c = CMSG_FIRSTHDR(&msg);
    if (c && c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS) {
        memcpy(&fd, CMSG_DATA(c), sizeof(int));
    }
    assert(byte == 'C');
    return fd;
}

int main(void)
{
    int via[2];
    signal(SIGPIPE, SIG_IGN);
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, via) == 0);
    assert(guestfwd_fdpass_socket(via[0]));

    /* two guest connections, each its own pair: bytes cross both ways, and each stays its own */
    for (int i = 0; i < 2; i++) {
        int guest = guestfwd_fdpass_connect((void *)(intptr_t)via[0]);
        assert(guest >= 0);
        assert(fcntl(guest, F_GETFD) & FD_CLOEXEC);
        int server = receive(via[1]);
        assert(server >= 0 && server != guest);
        char request[8], reply[8];
        snprintf(request, sizeof(request), "GET %d", i);
        assert(write(guest, request, 6) == 6);
        assert(read(server, reply, 6) == 6 && memcmp(reply, request, 6) == 0);
        assert(write(server, "200", 3) == 3);
        assert(read(guest, reply, 3) == 3 && memcmp(reply, "200", 3) == 0);
        close(server);
        assert(read(guest, reply, 1) == 0); /* the server's close ends the guest's connection */
        close(guest);
    }

    /* not a Unix stream socket: refused when the rule is parsed */
    int udp = socket(AF_INET, SOCK_DGRAM, 0), pipes[2];
    assert(pipe(pipes) == 0);
    int dgram[2];
    assert(socketpair(AF_UNIX, SOCK_DGRAM, 0, dgram) == 0);
    assert(!guestfwd_fdpass_socket(udp));
    assert(!guestfwd_fdpass_socket(pipes[0]));
    assert(!guestfwd_fdpass_socket(dgram[0]));
    assert(!guestfwd_fdpass_socket(-1));
    assert(!guestfwd_fdpass_socket(9999));

    /* the server gone: the connection is refused, nothing leaks */
    close(via[1]);
    int before = dup(0);
    close(before);
    assert(guestfwd_fdpass_connect((void *)(intptr_t)via[0]) < 0);
    int after = dup(0);
    assert(after == before);
    close(after);
    printf("ok\n");
    return 0;
}
