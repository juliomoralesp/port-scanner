/*
 * ports.c
 * List listening ports and the owning process (if possible), and allow searching
 * by port or process name. Parses /proc/net/* and maps socket inode -> pid.
 *
 * Build: make
 * Usage:
 *   ./ports            # list all listening ports
 *   ./ports -p 22      # search for a specific port
 *   ./ports -n ssh     # search for a process name substring
 *   ./ports -a         # show all protocols including non-listening
 */

#define _GNU_SOURCE

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <arpa/inet.h>

typedef struct owner_info {
    pid_t pid;
    char name[256];
    struct owner_info *next;
} owner_info_t;

typedef struct sock_entry {
    char proto[8];
    char local_addr[64]; // original hex local address
    char local_ip[64];   // human readable IP (IPv4 or IPv6)
    unsigned port;
    unsigned long inode;
    owner_info_t *owners; // linked list of processes that reference the socket
    struct sock_entry *next;
} sock_entry_t;

// Helper: hex string to ip:port text
static unsigned hex_to_port(const char *hex)
{
    unsigned long p = strtoul(hex, NULL, 16);
    return (unsigned) p;
}

// Convert a hex local address string into a human readable IP string
// For IPv4 the hex string is 8 hex digits (little-endian) like 0100007F => 127.0.0.1
// For IPv6 the hex string is 32 hex digits representing 16 bytes; we'll convert
// pairs of hex into bytes in big-endian order and call inet_ntop.
static void hex_to_ipstr(const char *hex, bool is_ipv6, char *out, size_t out_len)
{
    if (!hex || !*hex) { snprintf(out, out_len, "-"); return; }
    if (!is_ipv6) {
        // IPv4: parse up to 8 hex chars
        unsigned long val = strtoul(hex, NULL, 16);
        unsigned a = val & 0xff;
        unsigned b = (val >> 8) & 0xff;
        unsigned c = (val >> 16) & 0xff;
        unsigned d = (val >> 24) & 0xff;
        snprintf(out, out_len, "%u.%u.%u.%u", a, b, c, d);
        return;
    }

    // IPv6: expect 32 hex chars. Convert two hex chars to each byte.
    unsigned char buf[16];
    size_t len = strlen(hex);
    // pad left if shorter
    size_t start = 0;
    if (len < 32) {
        // left-pad with zeros: treat existing digits as rightmost
        char tmp[33]; memset(tmp, '0', 32); tmp[32] = 0;
        memcpy(tmp + (32 - len), hex, len);
        hex = strdup(tmp);
        start = 0;
    }
    for (int i = 0; i < 16; ++i) {
        unsigned int byte = 0;
        char pair[3] = {0,0,0};
        pair[0] = hex[i*2]; pair[1] = hex[i*2 + 1];
        sscanf(pair, "%2x", &byte);
        buf[i] = (unsigned char)byte;
    }
    struct in6_addr a6;
    memcpy(&a6.s6_addr, buf, 16);
    if (!inet_ntop(AF_INET6, &a6, out, out_len)) strncpy(out, "::", out_len-1);
}

static void add_owner(sock_entry_t *e, pid_t pid, const char *name)
{
    owner_info_t *n = malloc(sizeof(*n));
    if (!n) return;
    n->pid = pid;
    strncpy(n->name, name ? name : "", sizeof(n->name)-1);
    n->name[sizeof(n->name)-1] = '\0';
    n->next = e->owners;
    e->owners = n;
}

// helper: return smallest pid in owners or 0 if none
static pid_t entry_primary_pid(const sock_entry_t *e)
{
    if (!e || !e->owners) return 0;
    pid_t min = e->owners->pid;
    for (owner_info_t *o = e->owners; o; o = o->next) if (o->pid < min) min = o->pid;
    return min;
}

// Parse /proc/net/{tcp,tcp6,udp,udp6}, collect listening sockets by inode.
static void parse_proc_net(sock_entry_t **head, const char *path, const char *proto, bool only_listening)
{
    FILE *f = fopen(path, "r");
    if (!f) return;

    char line[512];
    // skip header
    if (!fgets(line, sizeof(line), f)) { fclose(f); return; }

    while (fgets(line, sizeof(line), f)) {
        // Tokenize line (whitespace) and extract fields safely.
        // Expected fields (0-based): 0=sl, 1=local, 2=rem, 3=st, .., 9=inode
        char local[128] = {0}, rem[128] = {0}, st[8] = {0};
        unsigned long inode = 0;
        char *tokens[32];
        int tokc = 0;
        char *p = line;
        char *save = NULL;
        while (tokc < (int)(sizeof(tokens)/sizeof(tokens[0])) && (tokens[tokc] = strtok_r(p, " \t\n", &save))) {
            p = NULL; tokc++;
        }
        if (tokc < 4) continue;
        strncpy(local, tokens[1], sizeof(local)-1);
        strncpy(rem, tokens[2], sizeof(rem)-1);
        strncpy(st, tokens[3], sizeof(st)-1);
        if (tokc > 9 && tokens[9]) inode = strtoul(tokens[9], NULL, 10);

        // Only include LISTEN (0A) when asked
        if (only_listening && strcmp(st, "0A") != 0) continue;

        // local has form '0100007F:0016' (ip:port hex)
        char *colon = strchr(local, ':');
        if (!colon) continue;
        *colon = '\0';
        const char *porthex = colon + 1;

        unsigned port = hex_to_port(porthex);

        sock_entry_t *e = malloc(sizeof(*e));
        if (!e) continue;
        strncpy(e->proto, proto, sizeof(e->proto)-1);
        e->proto[sizeof(e->proto)-1] = '\0';
        snprintf(e->local_addr, sizeof(e->local_addr), "%s", local);
        bool is_v6 = (strstr(proto, "6") != NULL);
        hex_to_ipstr(e->local_addr, is_v6, e->local_ip, sizeof(e->local_ip));
        e->port = port;
        e->inode = inode;
        e->owners = NULL;
        e->next = *head;
        *head = e;
    }

    fclose(f);
}

// Convert socket inode to pid(s) by scanning /proc/*/fd
// Try to match socket owners in two ways:
// 1) by scanning each process's fd links for socket:[inode]
// 2) if inode not matching or missing, parse /proc/<pid>/net/{tcp,tcp6,udp,udp6}
//    for local address:port matches and associate that PID
static void populate_owners(sock_entry_t *head)
{
    struct dirent *d;
    DIR *proc = opendir("/proc");
    if (!proc) return;

