/*
 * ports.c
 * List listening ports and the owning process (if possible), and allow searching
 * by port or process name. Parses /proc/net/* and maps socket inode -> pid.
 * Supports JSON output (-j), sorting (-s), and reverse (-r).
 *
 * Build: make
 * Usage:
 *   ./ports            # list all listening listening ports
 *   ./ports -a         # show all entries
 *   ./ports -p 22      # search for a specific port
 *   ./ports -n ssh     # search for a process name substring
 *   ./ports -s pid     # sort by pid (port default)
 *   ./ports -s proto -r # sort by proto then port, reverse order
 *   ./ports -j         # output JSON instead of a table
 */

/* _GNU_SOURCE made optional for Synology compatibility */
#ifdef USE_GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <arpa/inet.h>
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
#include <ctype.h>

typedef struct owner_info { pid_t pid; char name[256]; struct owner_info *next; } owner_info_t;

typedef struct sock_entry {
    char proto[8];            // tcp/tcp6/udp/udp6
    char local_hex[64];       // hex local address as in /proc/net
    char local_ip[64];        // human readable IP
    unsigned port;
    unsigned long inode;
    owner_info_t *owners;    // linked list of owners
    struct sock_entry *next;
} sock_entry_t;

// config
static bool g_show_all = false;
static int g_search_port = 0;
static const char *g_search_name = NULL;
static bool g_json = false;
typedef enum { SORT_PORT, SORT_PID, SORT_PROTO } sort_field_t;
static sort_field_t g_sort_field = SORT_PORT;
static bool g_sort_reverse = false;

/* Portable case-insensitive substring search (replaces GNU strcasestr) */
static const char *portable_strcasestr(const char *haystack, const char *needle) {
    if (!haystack || !needle) return NULL;
    if (!*needle) return haystack;
    
    size_t needle_len = strlen(needle);
    for (const char *p = haystack; *p; p++) {
        size_t i;
        for (i = 0; i < needle_len && p[i]; i++) {
            if (tolower((unsigned char)p[i]) != tolower((unsigned char)needle[i])) {
                break;
            }
        }
        if (i == needle_len) return p;
    }
    return NULL;
}

// helpers
static unsigned hex_to_port(const char *hex) { return (unsigned)strtoul(hex, NULL, 16); }

static void hex_to_ipstr(const char *hex, bool is_v6, char *out, size_t out_len) {
    if (!hex || !*hex) { snprintf(out, out_len, "-"); return; }
    if (!is_v6) {
        unsigned long val = strtoul(hex, NULL, 16);
        unsigned a = val & 0xff, b = (val>>8)&0xff, c = (val>>16)&0xff, d = (val>>24)&0xff;
        snprintf(out, out_len, "%u.%u.%u.%u", a,b,c,d);
        return;
    }
    // simple IPv6 parse: hex string 32 bytes -> bytes
    unsigned char buf[16]={0};
    size_t len = strlen(hex);
    char tmp[33];
    if (len < 32) {
        memset(tmp, '0', 32); tmp[32]=0; memcpy(tmp + (32 - len), hex, len); hex = tmp; len=32;
    }
    for (int i=0;i<16;i++) { unsigned int b=0; sscanf(&hex[i*2], "%2x", &b); buf[i]=b; }
    struct in6_addr a6; memcpy(&a6.s6_addr, buf, 16);
    if (!inet_ntop(AF_INET6, &a6, out, out_len)) snprintf(out, out_len, "::");
}

static void add_owner(sock_entry_t *e, pid_t pid, const char *name)
{
    owner_info_t *n = malloc(sizeof(*n));
    if (!n)
        return;

    n->pid = pid;
    strncpy(n->name, name ? name : "", sizeof(n->name) - 1);
    n->name[sizeof(n->name) - 1] = '\0';
    n->next = e->owners;
    e->owners = n;
}

static void free_entries(sock_entry_t *head) {
    while (head) { sock_entry_t *n = head->next; owner_info_t *o=head->owners; while(o){owner_info_t *no=o->next; free(o); o=no;} free(head); head=n; }
}

// parse /proc/net/* and build initial entries
static void parse_proc_net(sock_entry_t **head, const char *path, const char *proto, bool only_listen) {
    FILE *f = fopen(path, "r"); if (!f) return; char line[1024]; if (!fgets(line, sizeof(line), f)) { fclose(f); return; }
    while (fgets(line, sizeof(line), f)) {
        // tokens
        char local[128]={0}, rem[128]={0}, st[16]={0}; unsigned long inode=0; 
        char *tokens[32];
        int tokc = 0;
        char *p = line, *save = NULL;

        while ((tokc < 32) && (tokens[tokc] = strtok_r(p, " \t\n", &save))) {
            p = NULL;
            tokc++;
        }

        if (tokc < 4)
            continue;

        strncpy(local, tokens[1], sizeof(local) - 1);
        strncpy(rem, tokens[2], sizeof(rem) - 1);
        strncpy(st, tokens[3], sizeof(st) - 1);

        if (tokc > 9 && tokens[9])
            inode = strtoul(tokens[9], NULL, 10);

        if (only_listen && strcmp(st, "0A") != 0)
            continue; // LISTEN is 0A
        char *colon = strchr(local, ':'); if (!colon) continue; *colon=0; const char *porthex = colon+1; unsigned port = hex_to_port(porthex);
        sock_entry_t *e = malloc(sizeof(*e)); if (!e) continue; memset(e,0,sizeof(*e)); strncpy(e->proto, proto, sizeof(e->proto)-1); strncpy(e->local_hex, local, sizeof(e->local_hex)-1);
        bool v6 = (strstr(proto, "6")!=NULL); hex_to_ipstr(e->local_hex, v6, e->local_ip, sizeof(e->local_ip)); e->port=port; e->inode=inode; e->owners=NULL; e->next = *head; *head = e;
    }
    fclose(f);
}

// populate owners by scanning /proc/<pid>/fd for socket:[inode]
static void populate_owners(sock_entry_t *head) {
    DIR *proc = opendir("/proc"); if (!proc) return; struct dirent *d;
    while ((d = readdir(proc)) != NULL) {
        // numeric dirs
        char *endptr; long pid = strtol(d->d_name, &endptr, 10); if (*endptr) continue;
        char fdpath[512]; snprintf(fdpath, sizeof(fdpath), "/proc/%ld/fd", pid);
        DIR *fd = opendir(fdpath); if (!fd) continue; struct dirent *fdent; 
        while ((fdent = readdir(fd)) != NULL) {
            if (strcmp(fdent->d_name, ".") == 0 || strcmp(fdent->d_name, "..") == 0) continue;
            char linkpath[1024], target[1024]; snprintf(linkpath, sizeof(linkpath), "%s/%s", fdpath, fdent->d_name);
            ssize_t r = readlink(linkpath, target, sizeof(target)-1); if (r <= 0) continue; target[r]=0;
            unsigned long inode = 0; if (sscanf(target, "socket:[%lu]", &inode) == 1) {
                // find matching entry
                for (sock_entry_t *e = head; e; e = e->next) {
                    if (!e->inode || e->inode != inode)
                        continue;

                    // read comm or cmdline
                    char comm[256] = "?";
                    char tmp[256];

                    snprintf(tmp, sizeof(tmp), "/proc/%ld/comm", pid);
                    FILE *c = fopen(tmp, "r");
                    if (c) {
                        if (fgets(comm, sizeof(comm), c)) {
                            size_t L = strlen(comm);
                            if (L && comm[L - 1] == '\n')
                                comm[L - 1] = 0;
                        }
                        fclose(c);
                    } else {
                        tmp[0] = 0;
                        snprintf(tmp, sizeof(tmp), "/proc/%ld/cmdline", pid);
                        FILE *cl = fopen(tmp, "r");
                        if (cl) {
                            if (fgets(comm, sizeof(comm), cl)) {
                                for (size_t i = 0; i < strlen(comm); ++i)
                                    if (comm[i] == 0)
                                        comm[i] = ' ';
                            }
                            fclose(cl);
                        }
                    }

                    add_owner(e, (pid_t)pid, comm);
                }
            }
        }
        closedir(fd);
    }
    closedir(proc);
}

// comparator
static pid_t entry_primary_pid(const sock_entry_t *e);
static int cmp_entries(const void *a, const void *b) {
    const sock_entry_t *ea = *(const sock_entry_t **)a; const sock_entry_t *eb = *(const sock_entry_t **)b; int r=0;
    switch (g_sort_field) {
    case SORT_PORT: if (ea->port < eb->port) r=-1; else if (ea->port>eb->port) r=1; else r = strcmp(ea->proto, eb->proto); break;
    case SORT_PID: { pid_t pa=entry_primary_pid(ea), pb=entry_primary_pid(eb); if (pa<pb) r=-1; else if (pa>pb) r=1; else r = strcmp(ea->proto, eb->proto); break; }
    case SORT_PROTO: r = strcmp(ea->proto, eb->proto); if (!r) r = (ea->port < eb->port)?-1:(ea->port>eb->port)?1:0; break;
    }
    if (g_sort_reverse)
        r = -r;

    return r;
}

// JSON helper
static void json_escape(const char *s, FILE *out)
{
    if (!s)
        return;

    for (; *s; ++s) {
        unsigned char c = *s;
        switch (c) {
        case '\\':
            fputs("\\\\", out);
            break;
        case '"':
            fputs("\\\"", out);
            break;
        case '\b':
            fputs("\\b", out);
            break;
        case '\f':
            fputs("\\f", out);
            break;
        case '\n':
            fputs("\\n", out);
            break;
        case '\r':
            fputs("\\r", out);
            break;
        case '\t':
            fputs("\\t", out);
            break;
        default:
            if (c < 0x20)
                fprintf(out, "\\u%04x", c);
            else
                fputc(c, out);
            break;
        }
    }
}


static pid_t entry_primary_pid(const sock_entry_t *e) { if (!e || !e->owners) return 0; pid_t min=e->owners->pid; for (owner_info_t *o=e->owners; o; o=o->next) if (o->pid < min) min = o->pid; return min; }

static void print_json(sock_entry_t **arr, size_t n)
{
    printf("[");

    bool firstEntry = true;
    for (size_t i = 0; i < n; ++i) {
        sock_entry_t *e = arr[i];

        if (g_search_port > 0 && e->port != (unsigned)g_search_port)
            continue;

        if (g_search_name && *g_search_name) {
            bool match = false;
            for (owner_info_t *o = e->owners; o && !match; o = o->next)
                if (portable_strcasestr(o->name, g_search_name))
                    match = true;
            if (!match)
                continue;
        }

        if (!firstEntry)
            printf(",\n");

        firstEntry = false;
        printf("  {\n");
        printf("    \"proto\": \"");
        json_escape(e->proto, stdout);
        printf("\",\n");
        printf("    \"port\": %u,\n", e->port);
        printf("    \"local_ip\": \"");
        json_escape(e->local_ip, stdout);
        printf("\",\n");
        printf("    \"inode\": %lu,\n", e->inode);
        printf("    \"owners\": [");

        bool fo = true;
        for (owner_info_t *o = e->owners; o; o = o->next) {
            if (!fo)
                printf(", ");
            fo = false;
            printf("{\"pid\": %d, \"name\": \"", o->pid);
            json_escape(o->name, stdout);
            printf("\"}");
        }

        printf("]\n  }");
    }

    printf("\n]\n");
}


static void print_table(sock_entry_t **arr, size_t n) {
    printf("Proto  Port   Local IP        Inode       Owner(s)\n");
    printf("-----  -----  --------------- ----------  ----------------------------\n");
    for (size_t i=0;i<n;++i) {
        sock_entry_t *e = arr[i]; if (g_search_port>0 && e->port != (unsigned)g_search_port) continue; if (g_search_name && *g_search_name) { bool match=false; for (owner_info_t *o=e->owners;o;o=o->next) if (portable_strcasestr(o->name, g_search_name)) { match=true; break; } if (!match) continue; }
        printf("%-5s  %-5u  %-15s  %-10lu  ", e->proto, e->port, e->local_ip, e->inode);
        if (!e->owners) { printf("(no owner found)\n"); continue; }
        bool first=true; for (owner_info_t *o=e->owners;o;o=o->next) { if (!first) printf(", "); first=false; printf("%d/%s", o->pid, o->name); } printf("\n");
    }
}

static void print_entries(sock_entry_t *head) {
    size_t n=0; for (sock_entry_t *e=head; e; e=e->next) ++n; if (n==0) { if (g_json) puts("[]"); return; }
    sock_entry_t **arr = malloc(n*sizeof(*arr)); if (!arr) return; size_t i=0; for (sock_entry_t *e=head; e; e=e->next) arr[i++]=e; qsort(arr, n, sizeof(*arr), cmp_entries);
    if (g_json) print_json(arr, n); else print_table(arr, n);
    free(arr);
}

static void usage(const char *p) { fprintf(stderr, "usage: %s [-a] [-p port] [-n name] [-s port|pid|proto] [-r] [-j]\n", p); }

int main(int argc, char **argv) {
    int opt; while ((opt = getopt(argc, argv, "ap:n:s:rj")) != -1) {
        switch (opt) {
        case 'a': g_show_all = true; break;
        case 'p': g_search_port = atoi(optarg); break;
        case 'n': g_search_name = optarg; break;
        case 's': if (strcmp(optarg, "port") == 0) g_sort_field = SORT_PORT; else if (strcmp(optarg, "pid") == 0) g_sort_field = SORT_PID; else if (strcmp(optarg, "proto") == 0) g_sort_field = SORT_PROTO; else { fprintf(stderr, "unknown sort: %s\n", optarg); usage(argv[0]); return 2; } break;
        case 'r': g_sort_reverse = true; break;
        case 'j': g_json = true; break;
        default: usage(argv[0]); return 2; }
    }

    sock_entry_t *head = NULL;
    // default: only LISTEN (0A); show all if requested
    parse_proc_net(&head, "/proc/net/tcp", "tcp", !g_show_all);
    parse_proc_net(&head, "/proc/net/tcp6", "tcp6", !g_show_all);
    parse_proc_net(&head, "/proc/net/udp", "udp", !g_show_all);
    parse_proc_net(&head, "/proc/net/udp6", "udp6", !g_show_all);

    populate_owners(head);

    print_entries(head);

    free_entries(head);
    return 0;
}

