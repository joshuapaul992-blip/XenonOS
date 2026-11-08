/* SPDX-License-Identifier: MIT */
/*
 * xfat - the XenonOS FAT core (drivers/fs/fatcore.c) as a Linux program.
 *
 *   xfat IMAGE[@OFFSET] ls [PATH]          list a directory
 *   xfat IMAGE[@OFFSET] manifest           every file: path, size, CRC32
 *   xfat IMAGE[@OFFSET] cat PATH           write a file to stdout
 *   xfat IMAGE[@OFFSET] put HOSTFILE PATH  copy a host file in
 *   xfat IMAGE[@OFFSET] mkdir PATH | rm PATH | rmdir PATH | mv SRC DST
 *   xfat IMAGE[@OFFSET] truncate PATH SIZE | label TEXT | df
 *   xfat IMAGE[@OFFSET] stress SEED OPS [MANIFEST]
 *        random operations checked against an in-memory model, with
 *        unmount/remount and full verification along the way; writes the
 *        final tree as a manifest so tests can compare it with mtools.
 *
 * OFFSET is the byte offset of the file system inside IMAGE (partitions).
 */
#include "../../../drivers/fs/fatcore.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <zlib.h>

/* ---- Environment ----------------------------------------------------------- */

void *fat_env_alloc(size_t size) { return calloc(1, size); }
void fat_env_free(void *p) { free(p); }
void fat_env_now(fat_time *t)
{
    time_t now = time(NULL);
    struct tm tm;
    gmtime_r(&now, &tm);
    t->year = (uint16_t)(tm.tm_year + 1900);
    t->month = (uint8_t)(tm.tm_mon + 1);
    t->day = (uint8_t)tm.tm_mday;
    t->hour = (uint8_t)tm.tm_hour;
    t->minute = (uint8_t)tm.tm_min;
    t->second = (uint8_t)tm.tm_sec;
    t->ms = 0;
}

typedef struct { int fd; off_t base; } IMG;

static int img_read(void *ctx, uint64_t lba, uint32_t count, void *buf)
{
    IMG *i = ctx;
    ssize_t n = pread(i->fd, buf, (size_t)count * 512, i->base + (off_t)lba * 512);
    return n == (ssize_t)count * 512 ? 0 : -1;
}

static int img_write(void *ctx, uint64_t lba, uint32_t count, const void *buf)
{
    IMG *i = ctx;
    ssize_t n = pwrite(i->fd, buf, (size_t)count * 512, i->base + (off_t)lba * 512);
    return n == (ssize_t)count * 512 ? 0 : -1;
}

static IMG g_img;
static fat_dev g_dev;

static fat_volume *mount_image(const char *spec, bool ro)
{
    char path[1024];
    snprintf(path, sizeof(path), "%s", spec);
    char *at = strrchr(path, '@');
    g_img.base = 0;
    if (at) {
        *at = 0;
        g_img.base = strtoll(at + 1, NULL, 0);
    }
    g_img.fd = open(path, ro ? O_RDONLY : O_RDWR);
    if (g_img.fd < 0) {
        perror(path);
        exit(2);
    }
    off_t size = lseek(g_img.fd, 0, SEEK_END) - g_img.base;
    g_dev = (fat_dev){ img_read, img_write, &g_img, 512, (uint64_t)size / 512 };
    fat_volume *v;
    int e = fat_mount(&g_dev, ro, &v);
    if (e) {
        fprintf(stderr, "xfat: mount: %s\n", fat_strerror(e));
        exit(1);
    }
    return v;
}

static void die(const char *what, int e)
{
    fprintf(stderr, "xfat: %s: %s\n", what, fat_strerror(e));
    exit(1);
}

/* Split "a/b/c" into the parent directory node and the last name. */
static int open_parent(fat_volume *v, const char *path, fat_node **dir, const char **leaf)
{
    const char *slash = strrchr(path, '/');
    char parent[2048];
    if (!slash) {
        *dir = fat_root(v);
        *leaf = path;
        return FAT_OK;
    }
    snprintf(parent, sizeof(parent), "%.*s", (int)(slash - path), path);
    *leaf = slash + 1;
    return fat_open(v, parent, dir);
}

/* ---- Commands ----------------------------------------------------------------- */

static void cmd_ls(fat_volume *v, const char *path)
{
    fat_node *d;
    int e = fat_open(v, path, &d);
    if (e) die(path, e);
    uint32_t cookie = 0;
    fat_info info;
    while ((e = fat_readdir(d, &cookie, &info)) == 1)
        printf("%c %10u %-12s %s\n", (info.attr & FAT_ATTR_DIRECTORY) ? 'd' : '-', info.size, info.short_name,
               info.name);
    if (e < 0) die("readdir", e);
    fat_close(d);
}

static void manifest_dir(fat_node *d, const char *prefix, FILE *out)
{
    uint32_t cookie = 0;
    fat_info info;
    int e;
    while ((e = fat_readdir(d, &cookie, &info)) == 1) {
        if (!strcmp(info.name, ".") || !strcmp(info.name, ".."))
            continue;
        char path[4096];
        snprintf(path, sizeof(path), "%s%s%s", prefix, *prefix ? "/" : "", info.name);
        fat_node *n;
        if ((e = fat_lookup(d, info.name, &n)))
            die(path, e);
        if (info.attr & FAT_ATTR_DIRECTORY) {
            fprintf(out, "D %s\n", path);
            manifest_dir(n, path, out);
        } else {
            uLong crc = crc32(0, NULL, 0);
            uint8_t buf[65536];
            uint64_t off = 0;
            uint32_t got;
            do {
                if ((e = fat_read(n, off, buf, sizeof(buf), &got)))
                    die(path, e);
                crc = crc32(crc, buf, got);
                off += got;
            } while (got);
            fprintf(out, "F %u %08lx %s\n", info.size, crc, path);
        }
        fat_close(n);
    }
    if (e < 0) die("readdir", e);
}

static void cmd_cat(fat_volume *v, const char *path)
{
    fat_node *n;
    int e = fat_open(v, path, &n);
    if (e) die(path, e);
    uint8_t buf[65536];
    uint64_t off = 0;
    uint32_t got;
    do {
        if ((e = fat_read(n, off, buf, sizeof(buf), &got))) die(path, e);
        fwrite(buf, 1, got, stdout);
        off += got;
    } while (got);
    fat_close(n);
}

static void cmd_put(fat_volume *v, const char *host, const char *path)
{
    FILE *f = fopen(host, "rb");
    if (!f) { perror(host); exit(2); }
    fat_node *dir, *n;
    const char *leaf;
    int e = open_parent(v, path, &dir, &leaf);
    if (e) die(path, e);
    e = fat_create(dir, leaf, false, &n);
    if (e == FAT_EEXIST) {
        if ((e = fat_lookup(dir, leaf, &n)) || (e = fat_truncate(n, 0))) die(path, e);
    } else if (e) {
        die(path, e);
    }
    uint8_t buf[65536];
    uint64_t off = 0;
    size_t got;
    while ((got = fread(buf, 1, sizeof(buf), f)) > 0) {
        uint32_t done;
        if ((e = fat_write(n, off, buf, (uint32_t)got, &done))) die(path, e);
        off += got;
    }
    fclose(f);
    fat_close(n);
    fat_close(dir);
}

/* ---- Stress test against a model ----------------------------------------------- */

#define MAXN 600
typedef struct {
    bool used, dir;
    char path[1024];
    uint8_t *data;
    uint32_t size;
} MNODE;

static MNODE g_m[MAXN];
static uint64_t g_rng;
static unsigned long g_ops_done[16];

static uint32_t rnd(void)
{
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 7;
    g_rng ^= g_rng << 17;
    return (uint32_t)(g_rng >> 11);
}

/* Fold like the FAT core: ASCII and Latin-1 letters, compared as code points. */
static int next_cp(const char **s)
{
    const unsigned char *p = (const unsigned char *)*s;
    int cp;
    if (p[0] < 0x80) { cp = p[0]; *s += 1; }
    else if ((p[0] & 0xE0) == 0xC0) { cp = ((p[0] & 0x1F) << 6) | (p[1] & 0x3F); *s += 2; }
    else if ((p[0] & 0xF0) == 0xE0) { cp = ((p[0] & 0x0F) << 12) | ((p[1] & 0x3F) << 6) | (p[2] & 0x3F); *s += 3; }
    else { cp = ((p[0] & 0x07) << 18) | ((p[1] & 0x3F) << 12) | ((p[2] & 0x3F) << 6) | (p[3] & 0x3F); *s += 4; }
    return cp;
}

static int foldcp(int c)
{
    if (c >= 'a' && c <= 'z') return c - 32;
    if (c >= 0xE0 && c <= 0xFE && c != 0xF7) return c - 32;
    return c;
}

static bool same_path(const char *a, const char *b)
{
    while (*a && *b)
        if (foldcp(next_cp(&a)) != foldcp(next_cp(&b)))
            return false;
    return !*a && !*b;
}

static int mfind(const char *path)
{
    for (int i = 0; i < MAXN; i++)
        if (g_m[i].used && same_path(g_m[i].path, path))
            return i;
    return -1;
}

static int mcount(void)
{
    int n = 0;
    for (int i = 0; i < MAXN; i++)
        n += g_m[i].used;
    return n;
}

static void random_name(char *out)
{
    static const char *pieces[] = {
        "a", "b", "C", "d", "E", "x", "y", "Z", "0", "1", "7", "9", "-", "_", "~", "+", ",", ";", "=", "[", "]",
        " ", ".", "é", "É", "ü", "ß", "中", "文", "🎉", "readme", "README", "Photo", "notes", "TXT", "txt",
        "very long name part ", "!", "#", "$", "%", "&", "(", ")", "'",
    };
    int style = rnd() % 4;
    char buf[400] = "";
    if (style == 0) {
        /* Plain 8.3, all upper or all lower case. */
        bool lower = rnd() & 1;
        int bl = 1 + rnd() % 8, el = rnd() % 4;
        int n = 0;
        for (int i = 0; i < bl; i++) buf[n++] = (char)((lower ? 'a' : 'A') + rnd() % 26);
        if (el) {
            buf[n++] = '.';
            for (int i = 0; i < el; i++) buf[n++] = (char)((lower ? 'a' : 'A') + rnd() % 26);
        }
        buf[n] = 0;
    } else {
        int parts = 1 + rnd() % (style == 3 ? 40 : 8);
        for (int i = 0; i < parts && strlen(buf) < 300; i++)
            strcat(buf, pieces[rnd() % (sizeof(pieces) / sizeof(pieces[0]))]);
    }
    /* No leading spaces, no trailing spaces or dots (Windows would strip them). */
    char *s = buf;
    while (*s == ' ') s++;
    size_t n = strlen(s);
    while (n && (s[n - 1] == ' ' || s[n - 1] == '.')) s[--n] = 0;
    if (!n || !strcmp(s, ".") || !strcmp(s, "..")) strcpy(s, "x");
    /* Keep within 255 UTF-16 units: cut on character boundaries. */
    int units = 0;
    const char *p = s;
    while (*p) {
        const char *q = p;
        int cp = next_cp(&q);
        int u = cp >= 0x10000 ? 2 : 1;
        if (units + u > 200) break;
        units += u;
        p = q;
    }
    snprintf(out, 300, "%.*s", (int)(p - s), s);
    n = strlen(out);
    while (n && (out[n - 1] == ' ' || out[n - 1] == '.')) out[--n] = 0;
    if (!n) strcpy(out, "y");
}

static int random_dir(void)
{
    /* -1 = root */
    int dirs[MAXN], nd = 0;
    for (int i = 0; i < MAXN; i++)
        if (g_m[i].used && g_m[i].dir && strlen(g_m[i].path) < 600)
            dirs[nd++] = i;
    if (!nd || rnd() % 3 == 0)
        return -1;
    return dirs[rnd() % nd];
}

static int random_node(bool want_dir)
{
    int c[MAXN], n = 0;
    for (int i = 0; i < MAXN; i++)
        if (g_m[i].used && g_m[i].dir == want_dir)
            c[n++] = i;
    return n ? c[rnd() % n] : -2;
}

static void join(char *out, int dir, const char *name)
{
    if (dir < 0) snprintf(out, 1024, "%s", name);
    else snprintf(out, 1024, "%s/%s", g_m[dir].path, name);
}

static void verify_all(fat_volume *v, const char *when)
{
    for (int i = 0; i < MAXN; i++) {
        if (!g_m[i].used)
            continue;
        fat_node *n;
        int e = fat_open(v, g_m[i].path, &n);
        if (e) { fprintf(stderr, "verify %s: open %s: %s\n", when, g_m[i].path, fat_strerror(e)); exit(1); }
        fat_info info;
        fat_stat(n, &info);
        if (strcmp(info.name, strrchr(g_m[i].path, '/') ? strrchr(g_m[i].path, '/') + 1 : g_m[i].path)) {
            fprintf(stderr, "verify %s: name of %s reads back as '%s'\n", when, g_m[i].path, info.name);
            exit(1);
        }
        if (fat_is_dir(n) != g_m[i].dir) { fprintf(stderr, "verify %s: type of %s\n", when, g_m[i].path); exit(1); }
        if (!g_m[i].dir) {
            if (info.size != g_m[i].size) {
                fprintf(stderr, "verify %s: size of %s is %u, expected %u\n", when, g_m[i].path, info.size, g_m[i].size);
                exit(1);
            }
            uint8_t *buf = malloc(g_m[i].size + 1);
            uint32_t got;
            if ((e = fat_read(n, 0, buf, g_m[i].size + 1, &got)) || got != g_m[i].size ||
                (got && memcmp(buf, g_m[i].data, got))) {
                fprintf(stderr, "verify %s: content of %s differs (%s, %u bytes)\n", when, g_m[i].path, fat_strerror(e), got);
                exit(1);
            }
            free(buf);
        }
        fat_close(n);
    }
    /* No extra entries: count everything on disk. */
    int on_disk = 0;
    fat_node *stack[64];
    int sp = 0;
    stack[sp++] = fat_root(v);
    while (sp) {
        fat_node *d = stack[--sp];
        uint32_t cookie = 0;
        fat_info info;
        while (fat_readdir(d, &cookie, &info) == 1) {
            if (!strcmp(info.name, ".") || !strcmp(info.name, ".."))
                continue;
            on_disk++;
            if ((info.attr & FAT_ATTR_DIRECTORY) && sp < 63) {
                fat_node *c;
                if (fat_lookup(d, info.name, &c) == FAT_OK)
                    stack[sp++] = c;
            }
        }
        fat_close(d);
    }
    if (on_disk != mcount()) {
        fprintf(stderr, "verify %s: %d entries on disk, model has %d\n", when, on_disk, mcount());
        exit(1);
    }
    if (fat_open_count(v) != 0) {
        fprintf(stderr, "verify %s: %d nodes still open (reference leak)\n", when, fat_open_count(v));
        exit(1);
    }
}

static void cmd_stress(const char *spec, unsigned seed, int ops, const char *manifest)
{
    g_rng = 0x9E3779B97F4A7C15ull ^ ((uint64_t)seed * 0x100000001B3ull);
    fat_volume *v = mount_image(spec, false);
    fat_volinfo vi;
    fat_volume_info(v, &vi);
    uint32_t max_file = vi.cluster_bytes * 40 + 1000;
    for (int op = 0; op < ops; op++) {
        int kind = rnd() % 100;
        char name[300], path[1024], path2[1024];
        int e;
        if (kind < 22 && mcount() < MAXN - 2) {                    /* create file or directory */
            bool dir = rnd() % 4 == 0;
            int parent = random_dir();
            random_name(name);
            join(path, parent, name);
            fat_node *pd, *n;
            if ((e = parent < 0 ? (pd = fat_root(v), 0) : fat_open(v, g_m[parent].path, &pd)))
                die("open parent", e);
            e = fat_create(pd, name, dir, &n);
            bool exists = mfind(path) >= 0;
            if (exists) {
                if (e != FAT_EEXIST) { fprintf(stderr, "op %d: create of existing %s gave %s\n", op, path, fat_strerror(e)); exit(1); }
            } else if (e == FAT_ENOSPC) {
                /* full: acceptable */
            } else if (e) {
                fprintf(stderr, "op %d: create %s: %s\n", op, path, fat_strerror(e));
                exit(1);
            } else {
                int slot = 0;
                while (g_m[slot].used) slot++;
                g_m[slot] = (MNODE){ .used = true, .dir = dir };
                snprintf(g_m[slot].path, sizeof(g_m[slot].path), "%s", path);
                fat_close(n);
                g_ops_done[0]++;
            }
            fat_close(pd);
        } else if (kind < 50) {                                     /* write */
            int i = random_node(false);
            if (i < 0) continue;
            fat_node *n;
            if ((e = fat_open(v, g_m[i].path, &n))) die(g_m[i].path, e);
            uint32_t off = g_m[i].size ? rnd() % (g_m[i].size + vi.cluster_bytes) : rnd() % 3000;
            uint32_t len = 1 + rnd() % (rnd() % 4 == 0 ? vi.cluster_bytes * 6 : 700);
            if (off + len > max_file) { fat_close(n); continue; }
            uint8_t *buf = malloc(len);
            for (uint32_t k = 0; k < len; k++) buf[k] = (uint8_t)rnd();
            uint32_t done;
            e = fat_write(n, off, buf, len, &done);
            if (e == FAT_ENOSPC) {
                /* The file may have grown in clusters but not in size: reload size. */
                fat_info info;
                fat_stat(n, &info);
                if (info.size != g_m[i].size) { fprintf(stderr, "op %d: size changed after ENOSPC\n", op); exit(1); }
            } else if (e) {
                die("write", e);
            } else {
                uint32_t end = off + len;
                if (end > g_m[i].size) {
                    g_m[i].data = realloc(g_m[i].data, end);
                    if (off > g_m[i].size) memset(g_m[i].data + g_m[i].size, 0, off - g_m[i].size);
                    g_m[i].size = end;
                }
                memcpy(g_m[i].data + off, buf, len);
                g_ops_done[1]++;
            }
            free(buf);
            fat_close(n);
        } else if (kind < 58) {                                     /* truncate */
            int i = random_node(false);
            if (i < 0) continue;
            fat_node *n;
            if ((e = fat_open(v, g_m[i].path, &n))) die(g_m[i].path, e);
            uint32_t size = rnd() % 3 == 0 ? 0 : rnd() % (g_m[i].size + vi.cluster_bytes * 2 + 1);
            e = fat_truncate(n, size);
            if (e == FAT_ENOSPC) { fat_close(n); continue; }
            if (e) die("truncate", e);
            g_m[i].data = realloc(g_m[i].data, size + 1);
            if (size > g_m[i].size) memset(g_m[i].data + g_m[i].size, 0, size - g_m[i].size);
            g_m[i].size = size;
            fat_close(n);
            g_ops_done[2]++;
        } else if (kind < 68) {                                     /* delete */
            bool dir = rnd() % 3 == 0;
            int i = random_node(dir);
            if (i < 0) continue;
            bool has_children = false;
            size_t pl = strlen(g_m[i].path);
            for (int k = 0; k < MAXN; k++)
                if (g_m[k].used && k != i && !strncmp(g_m[k].path, g_m[i].path, pl) && g_m[k].path[pl] == '/')
                    has_children = true;
            const char *slash = strrchr(g_m[i].path, '/');
            fat_node *pd;
            if (slash) {
                snprintf(path, sizeof(path), "%.*s", (int)(slash - g_m[i].path), g_m[i].path);
                if ((e = fat_open(v, path, &pd))) die(path, e);
            } else {
                pd = fat_root(v);
            }
            e = fat_unlink(pd, slash ? slash + 1 : g_m[i].path);
            fat_close(pd);
            if (has_children) {
                if (e != FAT_ENOTEMPTY) { fprintf(stderr, "op %d: rmdir non-empty %s gave %s\n", op, g_m[i].path, fat_strerror(e)); exit(1); }
            } else {
                if (e) { fprintf(stderr, "op %d: delete %s: %s\n", op, g_m[i].path, fat_strerror(e)); exit(1); }
                free(g_m[i].data);
                g_m[i] = (MNODE){ 0 };
                g_ops_done[3]++;
            }
        } else if (kind < 78) {                                     /* rename / move */
            int i = random_node(rnd() % 4 == 0);
            if (i < 0) continue;
            int nd = random_dir();
            if (nd >= 0 && g_m[i].dir) {
                size_t pl = strlen(g_m[i].path);
                if (nd == i || (!strncmp(g_m[nd].path, g_m[i].path, pl) && g_m[nd].path[pl] == '/'))
                    continue;                                       /* into itself */
            }
            random_name(name);
            join(path2, nd, name);
            if (strlen(path2) > 900) continue;
            const char *slash = strrchr(g_m[i].path, '/');
            fat_node *sd, *dd;
            if (slash) {
                snprintf(path, sizeof(path), "%.*s", (int)(slash - g_m[i].path), g_m[i].path);
                if ((e = fat_open(v, path, &sd))) die(path, e);
            } else sd = fat_root(v);
            if (nd < 0) dd = fat_root(v);
            else if ((e = fat_open(v, g_m[nd].path, &dd))) die("open dest", e);
            int target = mfind(path2);
            e = fat_rename(sd, slash ? slash + 1 : g_m[i].path, dd, name, false);
            fat_close(sd);
            fat_close(dd);
            if (target >= 0 && target != i) {
                if (e != FAT_EEXIST) { fprintf(stderr, "op %d: rename onto existing gave %s\n", op, fat_strerror(e)); exit(1); }
            } else if (e == FAT_ENOSPC) {
                /* directory could not grow */
            } else if (e) {
                fprintf(stderr, "op %d: rename %s -> %s: %s\n", op, g_m[i].path, path2, fat_strerror(e));
                exit(1);
            } else {
                /* Update this node and everything under it. */
                char old[1024];
                snprintf(old, sizeof(old), "%s", g_m[i].path);
                size_t ol = strlen(old);
                for (int k = 0; k < MAXN; k++) {
                    if (!g_m[k].used) continue;
                    if (k == i) snprintf(g_m[k].path, sizeof(g_m[k].path), "%s", path2);
                    else if (!strncmp(g_m[k].path, old, ol) && g_m[k].path[ol] == '/') {
                        char tmp[1024];
                        snprintf(tmp, sizeof(tmp), "%s%s", path2, g_m[k].path + ol);
                        snprintf(g_m[k].path, sizeof(g_m[k].path), "%s", tmp);
                    }
                }
                g_ops_done[4]++;
            }
        } else if (kind < 80) {                                     /* delete a file while it is open */
            int i = random_node(false);
            if (i < 0) continue;
            fat_node *n, *pd;
            if ((e = fat_open(v, g_m[i].path, &n))) die(g_m[i].path, e);
            const char *slash = strrchr(g_m[i].path, '/');
            if (slash) {
                snprintf(path, sizeof(path), "%.*s", (int)(slash - g_m[i].path), g_m[i].path);
                if ((e = fat_open(v, path, &pd))) die(path, e);
            } else pd = fat_root(v);
            if ((e = fat_unlink(pd, slash ? slash + 1 : g_m[i].path))) die("unlink open file", e);
            /* Still readable through the open node. */
            uint8_t *buf = malloc(g_m[i].size + 1);
            uint32_t got;
            if ((e = fat_read(n, 0, buf, g_m[i].size, &got)) || got != g_m[i].size ||
                (got && memcmp(buf, g_m[i].data, got))) {
                fprintf(stderr, "op %d: open file unreadable after delete\n", op);
                exit(1);
            }
            free(buf);
            fat_close(pd);
            fat_close(n);
            free(g_m[i].data);
            g_m[i] = (MNODE){ 0 };
            g_ops_done[5]++;
        } else if (kind < 82) {                                     /* remount */
            if ((e = fat_unmount(v))) die("unmount", e);
            close(g_img.fd);
            v = mount_image(spec, false);
            g_ops_done[6]++;
        } else if (kind < 84) {
            verify_all(v, "periodic");
            g_ops_done[7]++;
        }
        if (fat_open_count(v)) {
            fprintf(stderr, "op %d: %d nodes left open (reference leak)\n", op, fat_open_count(v));
            exit(1);
        }
    }
    verify_all(v, "final");
    fat_volume_info(v, &vi);
    int e = fat_unmount(v);
    if (e) die("unmount", e);
    close(g_img.fd);
    v = mount_image(spec, true);
    verify_all(v, "after remount");
    if (manifest) {
        FILE *f = fopen(manifest, "w");
        fat_node *r = fat_root(v);
        manifest_dir(r, "", f);
        fat_close(r);
        fclose(f);
    }
    fat_unmount(v);
    printf("stress: FAT%d, %d operations: %lu creates, %lu writes, %lu truncates, %lu deletes, %lu renames, "
           "%lu open-deletes, %lu remounts, %lu verifies; %d entries at the end, %u of %u clusters free\n",
           vi.type, ops, g_ops_done[0], g_ops_done[1], g_ops_done[2], g_ops_done[3], g_ops_done[4], g_ops_done[5],
           g_ops_done[6], g_ops_done[7], mcount(), vi.free_clusters, vi.total_clusters);
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: xfat IMAGE[@OFFSET] COMMAND ...\n");
        return 2;
    }
    const char *spec = argv[1], *cmd = argv[2];
    if (!strcmp(cmd, "stress")) {
        if (argc < 5) { fprintf(stderr, "usage: xfat IMAGE stress SEED OPS [MANIFEST]\n"); return 2; }
        cmd_stress(spec, (unsigned)strtoul(argv[3], NULL, 0), atoi(argv[4]), argc > 5 ? argv[5] : NULL);
        return 0;
    }
    bool ro = !strcmp(cmd, "ls") || !strcmp(cmd, "manifest") || !strcmp(cmd, "cat") || !strcmp(cmd, "df");
    fat_volume *v = mount_image(spec, ro);
    int e = 0;
    if (!strcmp(cmd, "ls")) {
        cmd_ls(v, argc > 3 ? argv[3] : "");
    } else if (!strcmp(cmd, "manifest")) {
        fat_node *r = fat_root(v);
        manifest_dir(r, "", stdout);
        fat_close(r);
    } else if (!strcmp(cmd, "cat") && argc > 3) {
        cmd_cat(v, argv[3]);
    } else if (!strcmp(cmd, "put") && argc > 4) {
        cmd_put(v, argv[3], argv[4]);
    } else if ((!strcmp(cmd, "mkdir") || !strcmp(cmd, "touch")) && argc > 3) {
        fat_node *d, *n;
        const char *leaf;
        if ((e = open_parent(v, argv[3], &d, &leaf)) || (e = fat_create(d, leaf, !strcmp(cmd, "mkdir"), &n)))
            die(argv[3], e);
        fat_close(n);
        fat_close(d);
    } else if ((!strcmp(cmd, "rm") || !strcmp(cmd, "rmdir")) && argc > 3) {
        fat_node *d;
        const char *leaf;
        if ((e = open_parent(v, argv[3], &d, &leaf)) || (e = fat_unlink(d, leaf)))
            die(argv[3], e);
        fat_close(d);
    } else if (!strcmp(cmd, "mv") && argc > 4) {
        fat_node *sd, *dd;
        const char *sl, *dl;
        if ((e = open_parent(v, argv[3], &sd, &sl)) || (e = open_parent(v, argv[4], &dd, &dl)) ||
            (e = fat_rename(sd, sl, dd, dl, false)))
            die("mv", e);
        fat_close(sd);
        fat_close(dd);
    } else if (!strcmp(cmd, "truncate") && argc > 4) {
        fat_node *n;
        if ((e = fat_open(v, argv[3], &n)) || (e = fat_truncate(n, strtoull(argv[4], NULL, 0))))
            die(argv[3], e);
        fat_close(n);
    } else if (!strcmp(cmd, "label") && argc > 3) {
        if ((e = fat_set_label(v, argv[3]))) die("label", e);
    } else if (!strcmp(cmd, "df")) {
        fat_volinfo vi;
        fat_volume_info(v, &vi);
        printf("FAT%d label='%s' serial=%08X cluster=%u total=%u free=%u\n", vi.type, vi.label, vi.serial,
               vi.cluster_bytes, vi.total_clusters, vi.free_clusters);
    } else {
        fprintf(stderr, "xfat: unknown command %s\n", cmd);
        return 2;
    }
    if ((e = fat_unmount(v))) die("unmount", e);
    return 0;
}
