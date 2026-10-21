#!/bin/bash
# The user netdev's destination rule (slirp_destination_class and slirp_destination_allowed, from
# subprojects/packagefiles/libslirp-set-restricted.patch, so no libslirp checkout is needed): the host itself in any
# spelling is HOST, the local networks LOCAL, the rest PUBLIC; an IPv6 address carrying an IPv4 one is judged as that
# address; the host's own public addresses are LOCAL when allowing. LightTouchMac's NetworkPolicyTests hold the same table.
set -eu
HERE="$(cd "$(dirname "$0")" && pwd)"
PATCH="$HERE/../../subprojects/packagefiles/libslirp-set-restricted.patch"
T="$(mktemp -d)"
trap 'rm -rf "$T"' EXIT
# The patch's added lines: the enum (libslirp.h) and the rule (slirp.c), without their leading "+".
awk '/^\+enum \{/,/^\+\};/' "$PATCH" | sed 's/^+//' > "$T/rule.h"
awk '/^\+static int slirp_destination_class4/ { on = 1 } on { print } on && /^\+bool slirp_destination_allowed/ { last = 1 }
     last && /^\+}/ { exit }' "$PATCH" | sed 's/^+//' >> "$T/rule.h"
grep -q 'int slirp_destination_class(const struct sockaddr' "$T/rule.h" || { echo "FAIL: no classifier in $PATCH" >&2; exit 1; }
cat > "$T/test.c" <<'EOF'
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <stdbool.h>
#include <stdint.h>
#include <time.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#define G_USEC_PER_SEC 1000000
static int64_t g_get_monotonic_time(void) { return (int64_t)clock_gettime_nsec_np(CLOCK_MONOTONIC) / 1000; }
typedef struct { bool local_network_blocked; } Slirp;
/* This host's interfaces: loopback and one public address, 203.0.113.5. */
static struct sockaddr_in fake_lo = { .sin_family = AF_INET }, fake_public = { .sin_family = AF_INET };
static struct ifaddrs fake_list[2];
static int fake_getifaddrs(struct ifaddrs **list)
{
    inet_pton(AF_INET, "127.0.0.1", &fake_lo.sin_addr);
    inet_pton(AF_INET, "203.0.113.5", &fake_public.sin_addr);
    fake_list[0] = (struct ifaddrs){ .ifa_next = &fake_list[1], .ifa_addr = (struct sockaddr *)&fake_lo };
    fake_list[1] = (struct ifaddrs){ .ifa_addr = (struct sockaddr *)&fake_public };
    *list = fake_list;
    return 0;
}
static void fake_freeifaddrs(struct ifaddrs *list) { (void)list; }
#define getifaddrs fake_getifaddrs
#define freeifaddrs fake_freeifaddrs
#include "rule.h"
static int allowed(const char *text, bool lan)
{
    Slirp s = { .local_network_blocked = !lan };
    struct sockaddr_in a4 = { .sin_family = AF_INET };
    struct sockaddr_in6 a6 = { .sin6_family = AF_INET6 };
    if (inet_pton(AF_INET, text, &a4.sin_addr) == 1) return slirp_destination_allowed(&s, (struct sockaddr *)&a4);
    inet_pton(AF_INET6, text, &a6.sin6_addr);
    return slirp_destination_allowed(&s, (struct sockaddr *)&a6);
}
static int class(const char *text)
{
    struct sockaddr_in a4 = { .sin_family = AF_INET };
    struct sockaddr_in6 a6 = { .sin6_family = AF_INET6 };
    if (inet_pton(AF_INET, text, &a4.sin_addr) == 1) return slirp_destination_class((struct sockaddr *)&a4);
    if (inet_pton(AF_INET6, text, &a6.sin6_addr) == 1) return slirp_destination_class((struct sockaddr *)&a6);
    return -1;
}
int main(void)
{
    static const char *host[] = { "127.0.0.1", "127.255.255.254", "0.0.0.0", "0.1.2.3", "::1", "::", "::ffff:127.0.0.1",
        "::127.0.0.1", "::ffff:0:127.0.0.1", "64:ff9b::7f00:1", "::ffff:0.0.0.0" };
    static const char *local[] = { "10.0.2.2", "10.1.2.3", "172.16.0.1", "172.31.255.255", "192.168.1.1", "169.254.169.254",
        "100.64.0.1", "100.127.255.255", "198.18.0.1", "198.19.255.255", "224.0.0.251", "240.0.0.1", "255.255.255.255",
        "::ffff:192.168.1.1", "::10.0.0.1", "64:ff9b::a00:1", "::ffff:0:a9fe:a9fe", "fe80::1", "fc00::1", "fd12:3456::1",
        "fec0::1", "ff02::1" };
    static const char *public[] = { "93.184.215.14", "1.1.1.1", "11.0.0.1", "100.63.255.255", "100.128.0.1", "172.15.0.1",
        "172.32.0.1", "192.169.0.1", "198.17.0.1", "198.20.0.1", "223.255.255.255", "2606:4700::1111", "::ffff:8.8.8.8",
        "64:ff9b::808:808" };
    struct { const char **list; size_t n; int want; } table[] = {
        { host, sizeof host / sizeof *host, SLIRP_DESTINATION_HOST },
        { local, sizeof local / sizeof *local, SLIRP_DESTINATION_LOCAL },
        { public, sizeof public / sizeof *public, SLIRP_DESTINATION_PUBLIC },
    };
    struct sockaddr unix_like = { .sa_family = AF_UNIX };
    int bad = slirp_destination_class(&unix_like) != SLIRP_DESTINATION_HOST, n = 0;
    for (size_t t = 0; t < 3; t++) {
        for (size_t i = 0; i < table[t].n; i++, n++) {
            int got = class(table[t].list[i]);
            if (got != table[t].want) {
                fprintf(stderr, "FAIL: %s is %d, not %d\n", table[t].list[i], got, table[t].want);
                bad = 1;
            }
        }
    }
    /* Allowing: the host never, its own public address and the LAN only with the LAN on, the rest always. */
    static const struct { const char *addr; int off, on; } rule[] = {
        { "127.0.0.1", 0, 0 }, { "::ffff:127.0.0.1", 0, 0 }, { "203.0.113.5", 0, 1 }, { "::ffff:203.0.113.5", 0, 1 },
        { "64:ff9b::cb00:7105", 0, 1 }, { "192.168.1.1", 0, 1 }, { "203.0.113.6", 1, 1 }, { "2606:4700::1111", 1, 1 },
    };
    for (size_t i = 0; i < sizeof rule / sizeof *rule; i++, n++) {
        if (allowed(rule[i].addr, false) != rule[i].off || allowed(rule[i].addr, true) != rule[i].on) {
            fprintf(stderr, "FAIL: %s allowed %d/%d with the LAN off/on, not %d/%d\n", rule[i].addr,
                    allowed(rule[i].addr, false), allowed(rule[i].addr, true), rule[i].off, rule[i].on);
            bad = 1;
        }
    }
    if (bad) return 1;
    printf("PASS: %d destinations judged (host, local network, public; IPv4 carried in IPv6 as itself; the host's own addresses)\n", n);
    return 0;
}
EOF
cc -std=gnu11 -Wall -Werror -fsanitize=address,undefined -o "$T/test" "$T/test.c"
"$T/test"
