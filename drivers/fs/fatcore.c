/* SPDX-License-Identifier: MIT */
/*
 * Portable FAT12/16/32 file system core (see fatcore.h).
 *
 * Layout reminders:
 *   reserved sectors | FAT 1 | FAT 2 ... | root directory (FAT12/16) | data
 *   cluster N starts at data_lba + (N - 2) * sectors_per_cluster
 *
 * Metadata (FAT and directory sectors) is read through a small sector cache
 * and always written straight through, to every FAT copy. File data is read
 * and written directly, in runs of contiguous clusters.
 *
 * Every opened file or directory is a fat_node. Nodes are shared: opening the
 * same entry twice gives the same node, so all users see one size and one
 * cluster chain. A node is identified by its parent directory and the slot
 * number of its short directory entry.
 */
#include "fatcore.h"

/* Declared here so the file builds both in the kernel and with a host libc. */
void *memcpy(void *dst, const void *src, size_t n);
void *memmove(void *dst, const void *src, size_t n);
void *memset(void *dst, int c, size_t n);
int memcmp(const void *a, const void *b, size_t n);
size_t strlen(const char *s);

#define MCACHE_SLOTS 16
#define FREE_CLUSTER 0
#define BAD12 0xFF7u
#define BAD16 0xFFF7u
#define BAD32 0x0FFFFFF7u
#define NO_CLUSTER 0xFFFFFFFFu

struct fat_node {
    fat_volume *v;
    fat_node *parent;            /* referenced while the node exists */
    fat_node *next;              /* volume's node list */
    int refs;
    bool is_dir, is_root, unlinked;
    uint32_t first_cluster;
    uint32_t size;
    uint8_t attr, ntres;
    uint32_t slot;               /* short entry slot in the parent */
    uint32_t first_slot;         /* first LFN slot (== slot if no long name) */
    uint8_t short_name[11];
    uint8_t crt_tenth;
    uint16_t crt_time, crt_date, acc_date, wrt_time, wrt_date;
    uint32_t chain_index, chain_cluster;   /* cluster chain position cache */
};

typedef struct {
    uint64_t lba;
    uint32_t age;
    bool valid;
    uint8_t *data;
} msector;

struct fat_volume {
    fat_dev dev;
    bool ro;
    int type;
    uint32_t bps, spc, cluster_bytes, eps;      /* eps = directory entries per sector */
    uint32_t reserved, nfats, fat_sectors;
    uint32_t root_entries, root_sectors;
    uint64_t fat_lba, root_lba, data_lba;
    uint32_t total_clusters, max_cluster;       /* valid clusters: 2 .. max_cluster */
    uint32_t root_cluster;
    uint32_t fsinfo_sector;
    uint32_t free_count, next_free;
    uint32_t serial;
    char label[12];
    uint32_t eoc;
    bool marked_dirty;
    bool fat1_clean_bit;                        /* FAT[1] had the "clean shutdown" bit */
    uint8_t dirty_offset;                       /* boot sector byte with the dirty flag */
    msector mc[MCACHE_SLOTS];
    uint32_t mc_clock;
    uint8_t *tmp;                               /* one sector of scratch */
    fat_node *nodes;
    fat_node *root;
};

/* ---- Little-endian helpers ----------------------------------------------- */

static inline uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static inline uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
static inline void wr16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static inline void wr32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }

const char *fat_strerror(int e)
{
    switch (e) {
    case FAT_OK: return "success";
    case FAT_EIO: return "I/O error";
    case FAT_ENOENT: return "not found";
    case FAT_EEXIST: return "already exists";
    case FAT_ENOTDIR: return "not a directory";
    case FAT_EISDIR: return "is a directory";
    case FAT_ENOTEMPTY: return "directory not empty";
    case FAT_ENOSPC: return "no space left";
    case FAT_EINVAL: return "invalid name or argument";
    case FAT_ENOMEM: return "out of memory";
    case FAT_EROFS: return "read-only volume";
    case FAT_ECORRUPT: return "file system damaged";
    case FAT_EBUSY: return "in use";
    case FAT_EFBIG: return "file too large";
    case FAT_ENAMETOOLONG: return "name too long";
    case FAT_ENOTFAT: return "not a FAT file system";
    default: return "unknown error";
    }
}

/* ---- Sector access -------------------------------------------------------- */

static int dev_read(fat_volume *v, uint64_t lba, uint32_t count, void *buf)
{
    if (lba + count > v->dev.sectors)
        return FAT_ECORRUPT;
    return v->dev.read(v->dev.ctx, lba, count, buf) < 0 ? FAT_EIO : FAT_OK;
}

static int dev_write(fat_volume *v, uint64_t lba, uint32_t count, const void *buf)
{
    if (v->ro)
        return FAT_EROFS;
    if (lba + count > v->dev.sectors)
        return FAT_ECORRUPT;
    return v->dev.write(v->dev.ctx, lba, count, buf) < 0 ? FAT_EIO : FAT_OK;
}

/* Metadata cache: returns a cached copy of one sector (read only). */
static int meta_get(fat_volume *v, uint64_t lba, uint8_t **out)
{
    msector *victim = NULL;
    for (int i = 0; i < MCACHE_SLOTS; i++) {
        msector *m = &v->mc[i];
        if (m->valid && m->lba == lba) {
            m->age = ++v->mc_clock;
            *out = m->data;
            return FAT_OK;
        }
        /* Replace an empty slot if there is one, else the least recently used. */
        if (!victim || (victim->valid && (!m->valid || m->age < victim->age)))
            victim = m;
    }
    int e = dev_read(v, lba, 1, victim->data);
    if (e) {
        victim->valid = false;
        return e;
    }
    victim->lba = lba;
    victim->valid = true;
    victim->age = ++v->mc_clock;
    *out = victim->data;
    return FAT_OK;
}

/* Write a (modified) metadata sector, keeping the cache in step. */
static int meta_put(fat_volume *v, uint64_t lba, const uint8_t *data)
{
    for (int i = 0; i < MCACHE_SLOTS; i++)
        if (v->mc[i].valid && v->mc[i].lba == lba && v->mc[i].data != data)
            memcpy(v->mc[i].data, data, v->bps);
    return dev_write(v, lba, 1, data);
}

static void meta_drop(fat_volume *v, uint64_t lba, uint32_t count)
{
    for (int i = 0; i < MCACHE_SLOTS; i++)
        if (v->mc[i].valid && v->mc[i].lba >= lba && v->mc[i].lba < lba + count)
            v->mc[i].valid = false;
}

static inline uint64_t cluster_lba(fat_volume *v, uint32_t c)
{
    return v->data_lba + (uint64_t)(c - 2) * v->spc;
}

/* ---- The FAT ---------------------------------------------------------------- */

static int fat_get(fat_volume *v, uint32_t c, uint32_t *out)
{
    if (c < 2 || c > v->max_cluster)
        return FAT_ECORRUPT;
    uint8_t *s;
    int e;
    if (v->type == 12) {
        uint32_t off = c + c / 2;
        uint64_t lba = v->fat_lba + off / v->bps;
        uint32_t in = off % v->bps;
        if ((e = meta_get(v, lba, &s)))
            return e;
        uint16_t val = s[in];
        if (in == v->bps - 1) {
            if ((e = meta_get(v, lba + 1, &s)))
                return e;
            val |= (uint16_t)(s[0] << 8);
        } else {
            val |= (uint16_t)(s[in + 1] << 8);
        }
        *out = (c & 1) ? (uint32_t)(val >> 4) : (uint32_t)(val & 0xFFF);
        return FAT_OK;
    }
    uint32_t off = c * (v->type == 16 ? 2 : 4);
    if ((e = meta_get(v, v->fat_lba + off / v->bps, &s)))
        return e;
    *out = v->type == 16 ? rd16(s + off % v->bps) : (rd32(s + off % v->bps) & 0x0FFFFFFF);
    return FAT_OK;
}

/* Write one byte-range change of FAT sector 'sec' (relative) to every copy. */
static int fat_write_sector(fat_volume *v, uint32_t sec, const uint8_t *data)
{
    for (uint32_t f = 0; f < v->nfats; f++) {
        int e = meta_put(v, v->fat_lba + (uint64_t)f * v->fat_sectors + sec, data);
        if (e)
            return e;
    }
    return FAT_OK;
}

static int fat_set(fat_volume *v, uint32_t c, uint32_t val)
{
    if (c < 2 || c > v->max_cluster)
        return FAT_ECORRUPT;
    uint8_t *s;
    int e;
    if (v->type == 12) {
        uint32_t off = c + c / 2;
        uint32_t sec = off / v->bps, in = off % v->bps;
        val &= 0xFFF;
        if ((e = meta_get(v, v->fat_lba + sec, &s)))
            return e;
        memcpy(v->tmp, s, v->bps);
        if (in == v->bps - 1) {
            /* The entry straddles two sectors. */
            if (c & 1) v->tmp[in] = (uint8_t)((v->tmp[in] & 0x0F) | ((val & 0x0F) << 4));
            else v->tmp[in] = (uint8_t)val;
            if ((e = fat_write_sector(v, sec, v->tmp)))
                return e;
            if ((e = meta_get(v, v->fat_lba + sec + 1, &s)))
                return e;
            memcpy(v->tmp, s, v->bps);
            if (c & 1) v->tmp[0] = (uint8_t)(val >> 4);
            else v->tmp[0] = (uint8_t)((v->tmp[0] & 0xF0) | (val >> 8));
            return fat_write_sector(v, sec + 1, v->tmp);
        }
        if (c & 1) {
            v->tmp[in] = (uint8_t)((v->tmp[in] & 0x0F) | ((val & 0x0F) << 4));
            v->tmp[in + 1] = (uint8_t)(val >> 4);
        } else {
            v->tmp[in] = (uint8_t)val;
            v->tmp[in + 1] = (uint8_t)((v->tmp[in + 1] & 0xF0) | (val >> 8));
        }
        return fat_write_sector(v, sec, v->tmp);
    }
    uint32_t off = c * (v->type == 16 ? 2 : 4);
    uint32_t sec = off / v->bps, in = off % v->bps;
    if ((e = meta_get(v, v->fat_lba + sec, &s)))
        return e;
    memcpy(v->tmp, s, v->bps);
    if (v->type == 16)
        wr16(v->tmp + in, (uint16_t)val);
    else
        wr32(v->tmp + in, (rd32(v->tmp + in) & 0xF0000000) | (val & 0x0FFFFFFF));
    return fat_write_sector(v, sec, v->tmp);
}

static bool is_eoc(fat_volume *v, uint32_t val)
{
    switch (v->type) {
    case 12: return val >= 0xFF8;
    case 16: return val >= 0xFFF8;
    default: return val >= 0x0FFFFFF8;
    }
}

static bool is_bad(fat_volume *v, uint32_t val)
{
    return val == (v->type == 12 ? BAD12 : v->type == 16 ? BAD16 : BAD32);
}

/* Mark the volume "in use" in the boot sector (and FAT[1]) before the first change. */
static int mark_dirty(fat_volume *v, bool dirty)
{
    if (v->ro || v->marked_dirty == dirty)
        return FAT_OK;
    uint8_t *s;
    int e = meta_get(v, 0, &s);
    if (e)
        return e;
    memcpy(v->tmp, s, v->bps);
    if (dirty) v->tmp[v->dirty_offset] |= 1;
    else v->tmp[v->dirty_offset] &= (uint8_t)~1;
    if ((e = meta_put(v, 0, v->tmp)))
        return e;
    if (v->fat1_clean_bit) {
        uint32_t f1;
        if (v->type == 16) {
            if ((e = meta_get(v, v->fat_lba, &s)))
                return e;
            memcpy(v->tmp, s, v->bps);
            f1 = rd16(v->tmp + 2);
            f1 = dirty ? (f1 & ~0x8000u) : (f1 | 0x8000u);
            wr16(v->tmp + 2, (uint16_t)f1);
            if ((e = fat_write_sector(v, 0, v->tmp)))
                return e;
        } else if (v->type == 32) {
            if ((e = meta_get(v, v->fat_lba, &s)))
                return e;
            memcpy(v->tmp, s, v->bps);
            f1 = rd32(v->tmp + 4);
            f1 = dirty ? (f1 & ~0x08000000u) : (f1 | 0x08000000u);
            wr32(v->tmp + 4, f1);
            if ((e = fat_write_sector(v, 0, v->tmp)))
                return e;
        }
    }
    v->marked_dirty = dirty;
    return FAT_OK;
}

static int begin_change(fat_volume *v)
{
    if (v->ro)
        return FAT_EROFS;
    return mark_dirty(v, true);
}

static int count_free(fat_volume *v)
{
    uint32_t n = 0, val;
    for (uint32_t c = 2; c <= v->max_cluster; c++) {
        int e = fat_get(v, c, &val);
        if (e)
            return e;
        if (val == FREE_CLUSTER)
            n++;
    }
    v->free_count = n;
    return FAT_OK;
}

/* Allocate one cluster, preferably right after 'hint'. It is marked as the end of a chain. */
static int alloc_cluster(fat_volume *v, uint32_t hint, uint32_t *out)
{
    if (v->free_count == 0)
        return FAT_ENOSPC;
    uint32_t start = (hint >= 2 && hint < v->max_cluster) ? hint + 1 : v->next_free;
    if (start < 2 || start > v->max_cluster)
        start = 2;
    uint32_t c = start, val;
    for (uint32_t i = 0; i < v->total_clusters; i++) {
        int e = fat_get(v, c, &val);
        if (e)
            return e;
        if (val == FREE_CLUSTER) {
            if ((e = fat_set(v, c, v->eoc)))
                return e;
            if (v->free_count != NO_CLUSTER)
                v->free_count--;
            v->next_free = c + 1 > v->max_cluster ? 2 : c + 1;
            meta_drop(v, cluster_lba(v, c), v->spc);
            *out = c;
            return FAT_OK;
        }
        c = c + 1 > v->max_cluster ? 2 : c + 1;
    }
    v->free_count = 0;
    return FAT_ENOSPC;
}

static int free_chain(fat_volume *v, uint32_t c)
{
    uint32_t guard = 0;
    while (c >= 2 && c <= v->max_cluster) {
        uint32_t next;
        int e = fat_get(v, c, &next);
        if (e)
            return e;
        if (next == FREE_CLUSTER)
            return FAT_ECORRUPT;               /* chain runs into a free cluster */
        if ((e = fat_set(v, c, FREE_CLUSTER)))
            return e;
        meta_drop(v, cluster_lba(v, c), v->spc);
        if (v->free_count != NO_CLUSTER)
            v->free_count++;
        if (is_eoc(v, next) || is_bad(v, next))
            break;
        c = next;
        if (++guard > v->total_clusters)
            return FAT_ECORRUPT;
    }
    return FAT_OK;
}

/* Cluster number at position 'index' of a node's chain. */
static int chain_at(fat_node *n, uint32_t index, uint32_t *out)
{
    fat_volume *v = n->v;
    uint32_t i = 0, c = n->first_cluster;
    if (c < 2)
        return FAT_ENOENT;
    if (n->chain_cluster != NO_CLUSTER && n->chain_index <= index) {
        i = n->chain_index;
        c = n->chain_cluster;
    }
    while (i < index) {
        uint32_t next;
        int e = fat_get(v, c, &next);
        if (e)
            return e;
        if (is_eoc(v, next))
            return FAT_ENOENT;
        if (next < 2 || next > v->max_cluster)
            return FAT_ECORRUPT;
        c = next;
        if (++i > v->total_clusters)
            return FAT_ECORRUPT;
    }
    n->chain_index = index;
    n->chain_cluster = c;
    *out = c;
    return FAT_OK;
}

static uint32_t chain_length(fat_node *n, int *err)
{
    fat_volume *v = n->v;
    uint32_t len = 0, c = n->first_cluster;
    *err = FAT_OK;
    while (c >= 2 && c <= v->max_cluster) {
        len++;
        uint32_t next;
        if ((*err = fat_get(v, c, &next)))
            return 0;
        if (is_eoc(v, next))
            break;
        if (next < 2 || next > v->max_cluster || len > v->total_clusters) {
            *err = FAT_ECORRUPT;
            return 0;
        }
        c = next;
    }
    return len;
}

/* ---- Times ------------------------------------------------------------------ */

static void now_fat(uint16_t *date, uint16_t *time, uint8_t *tenth)
{
    fat_time t;
    fat_env_now(&t);
    if (t.year < 1980) t.year = 1980;
    if (t.year > 2107) t.year = 2107;
    *date = (uint16_t)(((t.year - 1980) << 9) | (t.month << 5) | t.day);
    *time = (uint16_t)((t.hour << 11) | (t.minute << 5) | (t.second / 2));
    if (tenth)
        *tenth = (uint8_t)((t.second % 2) * 100 + t.ms / 10);
}

static void unpack_time(uint16_t date, uint16_t time, uint8_t tenth, fat_time *t)
{
    t->year = (uint16_t)(1980 + (date >> 9));
    t->month = (date >> 5) & 0xF;
    t->day = date & 0x1F;
    t->hour = time >> 11;
    t->minute = (time >> 5) & 0x3F;
    t->second = (uint8_t)((time & 0x1F) * 2 + tenth / 100);
    t->ms = (uint16_t)((tenth % 100) * 10);
}

static void pack_time(const fat_time *t, uint16_t *date, uint16_t *time, uint8_t *tenth)
{
    uint16_t y = t->year < 1980 ? 1980 : t->year > 2107 ? 2107 : t->year;
    *date = (uint16_t)(((y - 1980) << 9) | (t->month << 5) | t->day);
    if (time)
        *time = (uint16_t)((t->hour << 11) | (t->minute << 5) | (t->second / 2));
    if (tenth)
        *tenth = (uint8_t)((t->second % 2) * 100 + t->ms / 10);
}

/* ---- Names ------------------------------------------------------------------- */

/* UTF-8 -> UTF-16. Returns the number of units or a negative error. */
static int utf8_to_16(const char *s, size_t len, uint16_t *out, int max)
{
    int n = 0;
    for (size_t i = 0; i < len;) {
        uint32_t cp;
        uint8_t c = (uint8_t)s[i];
        if (c < 0x80) { cp = c; i += 1; }
        else if ((c & 0xE0) == 0xC0 && i + 1 < len) { cp = ((c & 0x1Fu) << 6) | (s[i + 1] & 0x3F); i += 2; }
        else if ((c & 0xF0) == 0xE0 && i + 2 < len) { cp = ((c & 0x0Fu) << 12) | ((s[i + 1] & 0x3Fu) << 6) | (s[i + 2] & 0x3F); i += 3; }
        else if ((c & 0xF8) == 0xF0 && i + 3 < len) {
            cp = ((c & 0x07u) << 18) | ((s[i + 1] & 0x3Fu) << 12) | ((s[i + 2] & 0x3Fu) << 6) | (s[i + 3] & 0x3F);
            i += 4;
        } else return FAT_EINVAL;
        if (cp >= 0x10000) {
            if (n + 2 > max) return FAT_ENAMETOOLONG;
            cp -= 0x10000;
            out[n++] = (uint16_t)(0xD800 | (cp >> 10));
            out[n++] = (uint16_t)(0xDC00 | (cp & 0x3FF));
        } else {
            if (n + 1 > max) return FAT_ENAMETOOLONG;
            out[n++] = (uint16_t)cp;
        }
    }
    return n;
}

static size_t utf16_to_8(const uint16_t *s, int len, char *out, size_t max)
{
    size_t n = 0;
    for (int i = 0; i < len; i++) {
        uint32_t cp = s[i];
        if (cp >= 0xD800 && cp < 0xDC00 && i + 1 < len && s[i + 1] >= 0xDC00 && s[i + 1] < 0xE000) {
            cp = 0x10000 + ((cp - 0xD800) << 10) + (s[i + 1] - 0xDC00);
            i++;
        }
        if (cp < 0x80 && n + 1 <= max) out[n++] = (char)cp;
        else if (cp < 0x800 && n + 2 <= max) { out[n++] = (char)(0xC0 | (cp >> 6)); out[n++] = (char)(0x80 | (cp & 0x3F)); }
        else if (cp < 0x10000 && n + 3 <= max) {
            out[n++] = (char)(0xE0 | (cp >> 12)); out[n++] = (char)(0x80 | ((cp >> 6) & 0x3F)); out[n++] = (char)(0x80 | (cp & 0x3F));
        } else if (n + 4 <= max) {
            out[n++] = (char)(0xF0 | (cp >> 18)); out[n++] = (char)(0x80 | ((cp >> 12) & 0x3F));
            out[n++] = (char)(0x80 | ((cp >> 6) & 0x3F)); out[n++] = (char)(0x80 | (cp & 0x3F));
        } else break;
    }
    out[n] = 0;
    return n;
}

static uint16_t fold(uint16_t c)
{
    if (c >= 'a' && c <= 'z') return (uint16_t)(c - 32);
    if (c >= 0xE0 && c <= 0xFE && c != 0xF7) return (uint16_t)(c - 32);   /* Latin-1 */
    return c;
}

static bool names_equal(const uint16_t *a, int alen, const uint16_t *b, int blen)
{
    if (alen != blen)
        return false;
    for (int i = 0; i < alen; i++)
        if (fold(a[i]) != fold(b[i]))
            return false;
    return true;
}

static bool valid_long_char(uint16_t c)
{
    if (c < 0x20)
        return false;
    switch (c) {
    case '"': case '*': case '/': case ':': case '<': case '>': case '?': case '\\': case '|':
        return false;
    default:
        return true;
    }
}

static bool valid_short_char(uint16_t c)
{
    if (c >= 'A' && c <= 'Z') return true;
    if (c >= '0' && c <= '9') return true;
    switch (c) {
    case '$': case '%': case '\'': case '-': case '_': case '@': case '~': case '`': case '!':
    case '(': case ')': case '{': case '}': case '^': case '#': case '&':
        return true;
    default:
        return false;
    }
}

/* Clean a requested name: UTF-16, trailing spaces and dots removed (as on
   Windows) unless 'exact', characters checked. Returns the length or an error. */
static int prepare_name_ex(const char *name, size_t len, uint16_t *out, bool exact)
{
    if (len == 0)
        return FAT_EINVAL;
    int n = utf8_to_16(name, len, out, 256);
    if (n < 0)
        return n;
    while (!exact && n > 0 && (out[n - 1] == ' ' || out[n - 1] == '.'))
        n--;
    if (n == 0)
        return FAT_EINVAL;
    if (n > 255)
        return FAT_ENAMETOOLONG;
    for (int i = 0; i < n; i++)
        if (!valid_long_char(out[i]))
            return FAT_EINVAL;
    return n;
}

static int prepare_name(const char *name, size_t len, uint16_t *out)
{
    return prepare_name_ex(name, len, out, false);
}

/* Short names store characters 0x80-0xFF in the OEM code page; XenonOS
   reads them as code page 437, the default of Windows and Linux. */
static const uint16_t g_cp437_high[128] = {
    0x00C7, 0x00FC, 0x00E9, 0x00E2, 0x00E4, 0x00E0, 0x00E5, 0x00E7,
    0x00EA, 0x00EB, 0x00E8, 0x00EF, 0x00EE, 0x00EC, 0x00C4, 0x00C5,
    0x00C9, 0x00E6, 0x00C6, 0x00F4, 0x00F6, 0x00F2, 0x00FB, 0x00F9,
    0x00FF, 0x00D6, 0x00DC, 0x00A2, 0x00A3, 0x00A5, 0x20A7, 0x0192,
    0x00E1, 0x00ED, 0x00F3, 0x00FA, 0x00F1, 0x00D1, 0x00AA, 0x00BA,
    0x00BF, 0x2310, 0x00AC, 0x00BD, 0x00BC, 0x00A1, 0x00AB, 0x00BB,
    0x2591, 0x2592, 0x2593, 0x2502, 0x2524, 0x2561, 0x2562, 0x2556,
    0x2555, 0x2563, 0x2551, 0x2557, 0x255D, 0x255C, 0x255B, 0x2510,
    0x2514, 0x2534, 0x252C, 0x251C, 0x2500, 0x253C, 0x255E, 0x255F,
    0x255A, 0x2554, 0x2569, 0x2566, 0x2560, 0x2550, 0x256C, 0x2567,
    0x2568, 0x2564, 0x2565, 0x2559, 0x2558, 0x2552, 0x2553, 0x256B,
    0x256A, 0x2518, 0x250C, 0x2588, 0x2584, 0x258C, 0x2590, 0x2580,
    0x03B1, 0x00DF, 0x0393, 0x03C0, 0x03A3, 0x03C3, 0x00B5, 0x03C4,
    0x03A6, 0x0398, 0x03A9, 0x03B4, 0x221E, 0x03C6, 0x03B5, 0x2229,
    0x2261, 0x00B1, 0x2265, 0x2264, 0x2320, 0x2321, 0x00F7, 0x2248,
    0x00B0, 0x2219, 0x00B7, 0x221A, 0x207F, 0x00B2, 0x25A0, 0x00A0,
};

static uint16_t lower_cp(uint16_t c)
{
    if (c >= 'A' && c <= 'Z') return (uint16_t)(c + 32);
    if (c >= 0xC0 && c <= 0xDE && c != 0xD7) return (uint16_t)(c + 32);   /* Latin-1 */
    return c;
}

/* Display form of an 8.3 entry, with the NT lowercase flags applied. */
static int short_to_name(const uint8_t sn[11], uint8_t ntres, uint16_t *out)
{
    int n = 0, base = 8, ext = 3;
    while (base > 0 && sn[base - 1] == ' ') base--;
    while (ext > 0 && sn[8 + ext - 1] == ' ') ext--;
    for (int i = 0; i < base; i++) {
        uint8_t c = (i == 0 && sn[0] == 0x05) ? 0xE5 : sn[i];
        uint16_t u = c >= 0x80 ? g_cp437_high[c - 0x80] : c;
        out[n++] = (ntres & 0x08) ? lower_cp(u) : u;
    }
    if (ext) {
        out[n++] = '.';
        for (int i = 0; i < ext; i++) {
            uint8_t c = sn[8 + i];
            uint16_t u = c >= 0x80 ? g_cp437_high[c - 0x80] : c;
            out[n++] = (ntres & 0x10) ? lower_cp(u) : u;
        }
    }
    return n;
}

static uint8_t lfn_checksum(const uint8_t sn[11])
{
    uint8_t sum = 0;
    for (int i = 0; i < 11; i++)
        sum = (uint8_t)(((sum & 1) << 7) + (sum >> 1) + sn[i]);
    return sum;
}

/* If the name is a plain 8.3 name (each part all upper or all lower case),
   fill sn/ntres and return true: no long name entries are needed. */
static bool fits_short(const uint16_t *name, int n, uint8_t sn[11], uint8_t *ntres)
{
    int dot = -1;
    for (int i = 0; i < n; i++) {
        if (name[i] == '.') {
            if (dot >= 0) return false;
            dot = i;
        }
    }
    int blen = dot < 0 ? n : dot, elen = dot < 0 ? 0 : n - dot - 1;
    if (blen < 1 || blen > 8 || elen > 3 || (dot >= 0 && elen == 0))
        return false;
    bool bl = false, bu = false, el = false, eu = false;
    memset(sn, ' ', 11);
    for (int i = 0; i < blen; i++) {
        uint16_t c = name[i];
        if (c >= 'a' && c <= 'z') { bl = true; c = (uint16_t)(c - 32); }
        else if (c >= 'A' && c <= 'Z') bu = true;
        if (!valid_short_char(c)) return false;
        sn[i] = (uint8_t)c;
    }
    for (int i = 0; i < elen; i++) {
        uint16_t c = name[dot + 1 + i];
        if (c >= 'a' && c <= 'z') { el = true; c = (uint16_t)(c - 32); }
        else if (c >= 'A' && c <= 'Z') eu = true;
        if (!valid_short_char(c)) return false;
        sn[8 + i] = (uint8_t)c;
    }
    if ((bl && bu) || (el && eu))
        return false;                          /* mixed case needs a long name */
    *ntres = (uint8_t)((bl ? 0x08 : 0) | (el ? 0x10 : 0));
    if (sn[0] == 0xE5)
        sn[0] = 0x05;
    return true;
}

/* ---- Directory slots ---------------------------------------------------------- */

/* Sector and byte offset of directory slot 'slot'. FAT_ENOENT past the end. */
static int slot_pos(fat_node *dir, uint32_t slot, uint64_t *lba, uint32_t *off)
{
    fat_volume *v = dir->v;
    if (dir->is_root && v->type != 32) {
        if (slot >= v->root_entries)
            return FAT_ENOENT;
        *lba = v->root_lba + slot / v->eps;
        *off = (slot % v->eps) * 32;
        return FAT_OK;
    }
    uint32_t per_cluster = v->eps * v->spc;
    uint32_t c;
    int e = chain_at(dir, slot / per_cluster, &c);
    if (e)
        return e;
    uint32_t in = slot % per_cluster;
    *lba = cluster_lba(v, c) + in / v->eps;
    *off = (in % v->eps) * 32;
    return FAT_OK;
}

static int slot_read(fat_node *dir, uint32_t slot, uint8_t ent[32])
{
    uint64_t lba;
    uint32_t off;
    uint8_t *s;
    int e = slot_pos(dir, slot, &lba, &off);
    if (e)
        return e;
    if ((e = meta_get(dir->v, lba, &s)))
        return e;
    memcpy(ent, s + off, 32);
    return FAT_OK;
}

static int slot_write(fat_node *dir, uint32_t slot, const uint8_t ent[32])
{
    fat_volume *v = dir->v;
    uint64_t lba;
    uint32_t off;
    uint8_t *s;
    int e = slot_pos(dir, slot, &lba, &off);
    if (e)
        return e;
    if ((e = meta_get(v, lba, &s)))
        return e;
    memcpy(v->tmp, s, v->bps);
    memcpy(v->tmp + off, ent, 32);
    return meta_put(v, lba, v->tmp);
}

/* One directory entry as found while scanning. */
typedef struct {
    uint32_t slot, first_slot;
    uint8_t ent[32];
    uint16_t lname[256];
    int llen;                   /* 0: no (valid) long name */
} dirent_t;

/* Scan from *slot. Returns 1 with the next real entry (volume labels skipped
   unless want_label), 0 at the end, < 0 on error. */
static int dir_next(fat_node *dir, uint32_t *slot, dirent_t *d, bool want_label)
{
    uint16_t lfn[260];
    int lfn_count = 0, lfn_expect = 0;
    uint8_t lfn_sum = 0;
    uint32_t lfn_start = 0;
    for (;;) {
        uint8_t ent[32];
        int e = slot_read(dir, *slot, ent);
        if (e == FAT_ENOENT)
            return 0;
        if (e)
            return e;
        uint32_t this_slot = (*slot)++;
        if (ent[0] == 0x00)
            return 0;                                /* end of directory */
        if (ent[0] == 0xE5) {
            lfn_expect = lfn_count = 0;
            continue;
        }
        if (ent[11] == FAT_ATTR_LFN) {
            uint8_t ord = ent[0];
            if (ord & 0x40) {
                lfn_expect = ord & 0x3F;
                lfn_count = lfn_expect;
                lfn_sum = ent[13];
                lfn_start = this_slot;
                if (lfn_expect == 0 || lfn_expect > 20) {
                    lfn_expect = lfn_count = 0;
                    continue;
                }
                memset(lfn, 0xFF, sizeof(lfn));
            } else if (lfn_expect == 0 || (ord & 0x3F) != lfn_expect || ent[13] != lfn_sum) {
                lfn_expect = lfn_count = 0;
                continue;
            }
            int idx = ((ord & 0x3F) - 1) * 13;
            static const uint8_t pos[13] = { 1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30 };
            for (int i = 0; i < 13 && idx + i < 260; i++)
                lfn[idx + i] = rd16(ent + pos[i]);
            lfn_expect--;
            continue;
        }
        bool label = (ent[11] & FAT_ATTR_VOLUME) && !(ent[11] & FAT_ATTR_DIRECTORY);
        if (label && !want_label) {
            lfn_expect = lfn_count = 0;
            continue;
        }
        d->slot = this_slot;
        memcpy(d->ent, ent, 32);
        d->llen = 0;
        d->first_slot = this_slot;
        if (lfn_count && lfn_expect == 0 && lfn_checksum(ent) == lfn_sum) {
            int n = 0;
            while (n < lfn_count * 13 && n < 255 && lfn[n] != 0x0000 && lfn[n] != 0xFFFF)
                n++;
            memcpy(d->lname, lfn, (size_t)n * 2);
            d->llen = n;
            d->first_slot = lfn_start;
        }
        return 1;
    }
}

static int entry_name(const dirent_t *d, uint16_t *out)
{
    if (d->llen) {
        memcpy(out, d->lname, (size_t)d->llen * 2);
        return d->llen;
    }
    return short_to_name(d->ent, d->ent[12], out);
}

/* Find a name in a directory. */
static int dir_find(fat_node *dir, const uint16_t *name, int n, dirent_t *d)
{
    uint32_t slot = 0;
    uint16_t buf[256];
    for (;;) {
        int r = dir_next(dir, &slot, d, false);
        if (r <= 0)
            return r == 0 ? FAT_ENOENT : r;
        if (d->llen && names_equal(d->lname, d->llen, name, n))
            return FAT_OK;
        int sl = short_to_name(d->ent, 0, buf);
        if (names_equal(buf, sl, name, n))
            return FAT_OK;
    }
}

static bool short_exists(fat_node *dir, const uint8_t sn[11])
{
    uint32_t slot = 0;
    dirent_t d;
    while (dir_next(dir, &slot, &d, true) == 1)
        if (!memcmp(d.ent, sn, 11))
            return true;
    return false;
}

/* Windows-style alias for a long name: BASIS~N.EXT */
static int make_alias(fat_node *dir, const uint16_t *name, int n, uint8_t sn[11])
{
    int last_dot = -1;
    for (int i = 0; i < n; i++)
        if (name[i] == '.')
            last_dot = i;
    if (last_dot == 0)
        last_dot = -1;                             /* ".profile": no extension */
    uint8_t base[8], ext[3];
    int bl = 0, el = 0;
    int bend = last_dot < 0 ? n : last_dot;
    for (int i = 0; i < bend && bl < 8; i++) {
        uint16_t c = fold(name[i]);
        if (c == ' ' || c == '.')
            continue;
        base[bl++] = valid_short_char(c) ? (uint8_t)c : '_';
    }
    if (last_dot >= 0)
        for (int i = last_dot + 1; i < n && el < 3; i++) {
            uint16_t c = fold(name[i]);
            if (c == ' ' || c == '.')
                continue;
            ext[el++] = valid_short_char(c) ? (uint8_t)c : '_';
        }
    if (bl == 0)
        base[bl++] = '_';
    for (uint32_t num = 1; num < 1000000; num++) {
        char tail[8];
        int tl = 0;
        uint32_t x = num;
        char digits[7];
        int dl = 0;
        do { digits[dl++] = (char)('0' + x % 10); x /= 10; } while (x);
        tail[tl++] = '~';
        while (dl) tail[tl++] = digits[--dl];
        int keep = bl < 8 - tl ? bl : 8 - tl;
        memset(sn, ' ', 11);
        memcpy(sn, base, (size_t)keep);
        memcpy(sn + keep, tail, (size_t)tl);
        memcpy(sn + 8, ext, (size_t)el);
        if (sn[0] == 0xE5)
            sn[0] = 0x05;
        if (!short_exists(dir, sn))
            return FAT_OK;
    }
    return FAT_EEXIST;
}

/* Grow a (non-root FAT12/16) directory by one zeroed cluster. */
static int dir_extend(fat_node *dir)
{
    fat_volume *v = dir->v;
    if (dir->is_root && v->type != 32)
        return FAT_ENOSPC;
    int e;
    uint32_t len = chain_length(dir, &e);
    if (e)
        return e;
    uint32_t last = 0;
    if (len && (e = chain_at(dir, len - 1, &last)))
        return e;
    uint32_t c;
    if ((e = alloc_cluster(v, last, &c)))
        return e;
    memset(v->tmp, 0, v->bps);
    for (uint32_t i = 0; i < v->spc; i++)
        if ((e = dev_write(v, cluster_lba(v, c) + i, 1, v->tmp)))
            return e;
    if (last)
        return fat_set(v, last, c);
    dir->first_cluster = c;
    return FAT_OK;
}

/* Find 'count' consecutive free slots, growing the directory if needed. */
static int find_free_slots(fat_node *dir, uint32_t count, uint32_t *first)
{
    uint32_t run = 0, start = 0;
    for (uint32_t slot = 0;; slot++) {
        uint8_t ent[32];
        int e = slot_read(dir, slot, ent);
        if (e == FAT_ENOENT) {
            if ((e = dir_extend(dir)))
                return e;
            slot--;                                   /* read it again, now zeroed */
            continue;
        }
        if (e)
            return e;
        if (ent[0] == 0x00 || ent[0] == 0xE5) {
            if (run == 0)
                start = slot;
            if (++run == count) {
                *first = start;
                return FAT_OK;
            }
        } else {
            run = 0;
        }
        if (slot > 65535)                             /* FAT limit on directory size */
            return FAT_ENOSPC;
    }
}

/* Write the entries for a new name. Returns the first slot and the short slot. */
static int dir_add(fat_node *dir, const uint16_t *name, int n, uint8_t short_ent[32],
                   uint32_t *first_slot, uint32_t *short_slot)
{
    uint8_t sn[11], ntres = 0;
    bool need_lfn = !fits_short(name, n, sn, &ntres);
    int e;
    if (need_lfn) {
        if ((e = make_alias(dir, name, n, sn)))
            return e;
        ntres = 0;
    } else if (short_exists(dir, sn)) {
        return FAT_EEXIST;
    }
    uint32_t lfn_entries = need_lfn ? (uint32_t)(n + 12) / 13 : 0;
    uint32_t first;
    if ((e = find_free_slots(dir, lfn_entries + 1, &first)))
        return e;
    uint8_t sum = lfn_checksum(sn);
    static const uint8_t pos[13] = { 1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30 };
    for (uint32_t i = 0; i < lfn_entries; i++) {
        uint32_t ord = lfn_entries - i;               /* physical order: last part first */
        uint8_t ent[32];
        memset(ent, 0, 32);
        ent[0] = (uint8_t)(ord | (i == 0 ? 0x40 : 0));
        ent[11] = FAT_ATTR_LFN;
        ent[13] = sum;
        for (int k = 0; k < 13; k++) {
            int idx = (int)(ord - 1) * 13 + k;
            uint16_t ch = idx < n ? name[idx] : idx == n ? 0x0000 : 0xFFFF;
            wr16(ent + pos[k], ch);
        }
        if ((e = slot_write(dir, first + i, ent)))
            return e;
    }
    memcpy(short_ent, sn, 11);
    short_ent[12] = ntres;
    if ((e = slot_write(dir, first + lfn_entries, short_ent)))
        return e;
    *first_slot = first;
    *short_slot = first + lfn_entries;
    return FAT_OK;
}

static int dir_remove_slots(fat_node *dir, uint32_t first, uint32_t last)
{
    for (uint32_t s = first; s <= last; s++) {
        uint8_t ent[32];
        int e = slot_read(dir, s, ent);
        if (e)
            return e;
        ent[0] = 0xE5;
        if ((e = slot_write(dir, s, ent)))
            return e;
    }
    return FAT_OK;
}

/* ---- Nodes ---------------------------------------------------------------------- */

static uint32_t ent_cluster(fat_volume *v, const uint8_t *ent)
{
    uint32_t c = rd16(ent + 26);
    if (v->type == 32)
        c |= (uint32_t)rd16(ent + 20) << 16;
    return c;
}

static void node_from_entry(fat_node *n, const dirent_t *d)
{
    const uint8_t *e = d->ent;
    memcpy(n->short_name, e, 11);
    n->attr = e[11];
    n->ntres = e[12];
    n->crt_tenth = e[13];
    n->crt_time = rd16(e + 14);
    n->crt_date = rd16(e + 16);
    n->acc_date = rd16(e + 18);
    n->wrt_time = rd16(e + 22);
    n->wrt_date = rd16(e + 24);
    n->first_cluster = ent_cluster(n->v, e);
    n->size = rd32(e + 28);
    n->is_dir = (n->attr & FAT_ATTR_DIRECTORY) != 0;
    n->slot = d->slot;
    n->first_slot = d->first_slot;
    if (n->is_dir)
        n->size = 0;
}

static fat_node *find_node(fat_volume *v, fat_node *parent, uint32_t slot)
{
    for (fat_node *n = v->nodes; n; n = n->next)
        if (n->parent == parent && n->slot == slot && !n->unlinked)
            return n;
    return NULL;
}

static fat_node *new_node(fat_volume *v, fat_node *parent)
{
    fat_node *n = fat_env_alloc(sizeof(fat_node));
    if (!n)
        return NULL;
    n->v = v;
    n->parent = parent;
    if (parent)
        parent->refs++;
    n->refs = 1;
    n->chain_cluster = NO_CLUSTER;
    n->next = v->nodes;
    v->nodes = n;
    return n;
}

/* Write a node's short directory entry from its fields. */
static int node_write_entry(fat_node *n)
{
    if (n->is_root || n->unlinked)
        return FAT_OK;
    uint8_t ent[32];
    int e = slot_read(n->parent, n->slot, ent);
    if (e)
        return e;
    memcpy(ent, n->short_name, 11);
    ent[11] = n->attr;
    ent[12] = n->ntres;
    ent[13] = n->crt_tenth;
    wr16(ent + 14, n->crt_time);
    wr16(ent + 16, n->crt_date);
    wr16(ent + 18, n->acc_date);
    wr16(ent + 20, n->v->type == 32 ? (uint16_t)(n->first_cluster >> 16) : 0);
    wr16(ent + 22, n->wrt_time);
    wr16(ent + 24, n->wrt_date);
    wr16(ent + 26, (uint16_t)n->first_cluster);
    wr32(ent + 28, n->is_dir ? 0 : n->size);
    return slot_write(n->parent, n->slot, ent);
}

void fat_ref(fat_node *n)
{
    n->refs++;
}

void fat_close(fat_node *n)
{
    while (n && --n->refs == 0) {
        fat_volume *v = n->v;
        if (n->unlinked && n->first_cluster >= 2 && !v->ro) {
            if (begin_change(v) == FAT_OK)
                free_chain(v, n->first_cluster);
        }
        for (fat_node **pp = &v->nodes; *pp; pp = &(*pp)->next) {
            if (*pp == n) {
                *pp = n->next;
                break;
            }
        }
        fat_node *parent = n->parent;
        fat_env_free(n);
        n = parent;                  /* drop the reference this node held */
    }
}

fat_volume *fat_node_volume(fat_node *n) { return n->v; }
bool fat_is_dir(fat_node *n) { return n->is_dir; }
fat_node *fat_parent(fat_node *n) { return n->parent; }

fat_node *fat_root(fat_volume *v)
{
    v->root->refs++;
    return v->root;
}

int fat_open_count(fat_volume *v)
{
    int count = 0;
    for (fat_node *n = v->nodes; n; n = n->next)
        if (!n->is_root)
            count++;
    return count;
}

/* Split off one path component. */
static const char *next_component(const char *p, size_t *len)
{
    while (*p == '/' || *p == '\\')
        p++;
    const char *s = p;
    while (*p && *p != '/' && *p != '\\')
        p++;
    *len = (size_t)(p - s);
    return s;
}

static int lookup_n(fat_node *dir, const char *name, size_t len, fat_node **out)
{
    if (!dir->is_dir)
        return FAT_ENOTDIR;
    if (len == 1 && name[0] == '.') {
        dir->refs++;
        *out = dir;
        return FAT_OK;
    }
    if (len == 2 && name[0] == '.' && name[1] == '.') {
        fat_node *p = dir->is_root ? dir : dir->parent;
        p->refs++;
        *out = p;
        return FAT_OK;
    }
    /* Exact name first: other systems can write names that end in spaces
       or dots. Then the name as Windows would clean it. */
    uint16_t u[260];
    dirent_t d;
    int e = FAT_ENOENT;
    int n = prepare_name_ex(name, len, u, true);
    if (n > 0)
        e = dir_find(dir, u, n, &d);
    if (e == FAT_ENOENT) {
        n = prepare_name(name, len, u);
        if (n < 0)
            return n;
        e = dir_find(dir, u, n, &d);
    }
    if (e)
        return e;
    fat_node *node = find_node(dir->v, dir, d.slot);
    if (node) {
        node->refs++;
        *out = node;
        return FAT_OK;
    }
    node = new_node(dir->v, dir);
    if (!node)
        return FAT_ENOMEM;
    node_from_entry(node, &d);
    *out = node;
    return FAT_OK;
}

int fat_lookup(fat_node *dir, const char *name, fat_node **out)
{
    return lookup_n(dir, name, strlen(name), out);
}

int fat_open(fat_volume *v, const char *path, fat_node **out)
{
    fat_node *cur = fat_root(v);
    const char *p = path;
    for (;;) {
        size_t len;
        const char *c = next_component(p, &len);
        if (len == 0)
            break;
        fat_node *next;
        int e = lookup_n(cur, c, len, &next);
        fat_close(cur);
        if (e)
            return e;
        cur = next;
        p = c + len;
    }
    *out = cur;
    return FAT_OK;
}

int fat_create(fat_node *dir, const char *name, bool directory, fat_node **out)
{
    fat_volume *v = dir->v;
    if (!dir->is_dir)
        return FAT_ENOTDIR;
    uint16_t u[260];
    int n = prepare_name(name, strlen(name), u);
    if (n < 0)
        return n;
    if ((n == 1 && u[0] == '.') || (n == 2 && u[0] == '.' && u[1] == '.'))
        return FAT_EEXIST;
    dirent_t d;
    int e = dir_find(dir, u, n, &d);
    if (e == FAT_OK)
        return FAT_EEXIST;
    if (e != FAT_ENOENT)
        return e;
    if ((e = begin_change(v)))
        return e;

    uint32_t cluster = 0;
    uint16_t date, time;
    uint8_t tenth;
    now_fat(&date, &time, &tenth);
    if (directory) {
        /* New directory: one cluster with "." and "..". */
        if ((e = alloc_cluster(v, 0, &cluster)))
            return e;
        memset(v->tmp, 0, v->bps);
        for (uint32_t i = 1; i < v->spc; i++)
            if ((e = dev_write(v, cluster_lba(v, cluster) + i, 1, v->tmp)))
                goto undo;
        uint32_t parent_cluster = dir->is_root ? 0 : dir->first_cluster;
        uint8_t *s = v->tmp;
        memset(s, ' ', 11);
        s[0] = '.';
        s[11] = FAT_ATTR_DIRECTORY;
        wr16(s + 14, time); wr16(s + 16, date); wr16(s + 18, date); wr16(s + 22, time); wr16(s + 24, date);
        wr16(s + 20, (uint16_t)(cluster >> 16));
        wr16(s + 26, (uint16_t)cluster);
        memcpy(s + 32, s, 32);
        s[33] = '.';
        wr16(s + 32 + 20, (uint16_t)(parent_cluster >> 16));
        wr16(s + 32 + 26, (uint16_t)parent_cluster);
        if ((e = dev_write(v, cluster_lba(v, cluster), 1, v->tmp)))
            goto undo;
    }
    uint8_t ent[32];
    memset(ent, 0, 32);
    ent[11] = directory ? FAT_ATTR_DIRECTORY : FAT_ATTR_ARCHIVE;
    ent[13] = tenth;
    wr16(ent + 14, time); wr16(ent + 16, date); wr16(ent + 18, date);
    wr16(ent + 22, time); wr16(ent + 24, date);
    wr16(ent + 20, v->type == 32 ? (uint16_t)(cluster >> 16) : 0);
    wr16(ent + 26, (uint16_t)cluster);
    uint32_t first, slot;
    if ((e = dir_add(dir, u, n, ent, &first, &slot)))
        goto undo;
    if (dir->is_root ? false : dir->is_dir) {
        /* Keep the parent's modification time current, like other systems do. */
        dir->wrt_date = date;
        dir->wrt_time = time;
        node_write_entry(dir);
    }
    fat_node *node = new_node(v, dir);
    if (!node)
        return FAT_ENOMEM;
    d.slot = slot;
    d.first_slot = first;
    memcpy(d.ent, ent, 32);
    d.llen = 0;
    node_from_entry(node, &d);
    *out = node;
    return FAT_OK;
undo:
    if (cluster)
        free_chain(v, cluster);
    return e;
}

/* ---- File data ------------------------------------------------------------------- */

int fat_read(fat_node *n, uint64_t offset, void *buf, uint32_t len, uint32_t *done)
{
    fat_volume *v = n->v;
    uint8_t *out = buf;
    *done = 0;
    if (n->is_dir)
        return FAT_EISDIR;
    if (offset >= n->size)
        return FAT_OK;
    if (offset + len > n->size)
        len = (uint32_t)(n->size - offset);
    while (len) {
        uint32_t ci = (uint32_t)(offset / v->cluster_bytes);
        uint32_t in = (uint32_t)(offset % v->cluster_bytes);
        uint32_t c;
        int e = chain_at(n, ci, &c);
        if (e)
            return e == FAT_ENOENT ? FAT_ECORRUPT : e;
        /* Extend over physically consecutive clusters. */
        uint32_t span = v->cluster_bytes - in;
        uint32_t run = 1;
        while (span < len && run < 128) {
            uint32_t next;
            if (chain_at(n, ci + run, &next) || next != c + run)
                break;
            span += v->cluster_bytes;
            run++;
        }
        uint32_t chunk = span < len ? span : len;
        uint64_t lba = cluster_lba(v, c) + in / v->bps;
        uint32_t sin = in % v->bps;
        if (sin == 0 && chunk >= v->bps) {
            uint32_t whole = chunk / v->bps;
            if ((e = dev_read(v, lba, whole, out)))
                return e;
            chunk = whole * v->bps;
        } else {
            if ((e = dev_read(v, lba, 1, v->tmp)))
                return e;
            uint32_t take = v->bps - sin < chunk ? v->bps - sin : chunk;
            memcpy(out, v->tmp + sin, take);
            chunk = take;
        }
        out += chunk;
        offset += chunk;
        len -= chunk;
        *done += chunk;
    }
    return FAT_OK;
}

/* Make sure the chain has at least 'clusters' clusters. */
static int ensure_clusters(fat_node *n, uint32_t clusters)
{
    fat_volume *v = n->v;
    int e;
    uint32_t have = chain_length(n, &e);
    if (e)
        return e;
    uint32_t last = 0;
    if (have && (e = chain_at(n, have - 1, &last)))
        return e;
    while (have < clusters) {
        uint32_t c;
        if ((e = alloc_cluster(v, last, &c)))
            return e;
        if (last) {
            if ((e = fat_set(v, last, c)))
                return e;
        } else {
            n->first_cluster = c;
            n->chain_cluster = NO_CLUSTER;
        }
        last = c;
        have++;
    }
    return FAT_OK;
}

static int write_bytes(fat_node *n, uint64_t offset, const uint8_t *in, uint32_t len, bool zero)
{
    fat_volume *v = n->v;
    while (len) {
        uint32_t ci = (uint32_t)(offset / v->cluster_bytes);
        uint32_t cin = (uint32_t)(offset % v->cluster_bytes);
        uint32_t c;
        int e = chain_at(n, ci, &c);
        if (e)
            return e == FAT_ENOENT ? FAT_ECORRUPT : e;
        uint32_t span = v->cluster_bytes - cin, run = 1;
        while (span < len && run < 128 && !zero) {
            uint32_t next;
            if (chain_at(n, ci + run, &next) || next != c + run)
                break;
            span += v->cluster_bytes;
            run++;
        }
        uint32_t chunk = span < len ? span : len;
        uint64_t lba = cluster_lba(v, c) + cin / v->bps;
        uint32_t sin = cin % v->bps;
        if (sin == 0 && chunk >= v->bps && !zero) {
            uint32_t whole = chunk / v->bps;
            meta_drop(v, lba, whole);
            if ((e = dev_write(v, lba, whole, in)))
                return e;
            chunk = whole * v->bps;
        } else {
            uint32_t take = v->bps - sin < chunk ? v->bps - sin : chunk;
            if (take < v->bps) {
                if ((e = dev_read(v, lba, 1, v->tmp)))
                    return e;
            }
            if (zero) memset(v->tmp + sin, 0, take);
            else memcpy(v->tmp + sin, in, take);
            meta_drop(v, lba, 1);
            if ((e = dev_write(v, lba, 1, v->tmp)))
                return e;
            chunk = take;
        }
        if (!zero)
            in += chunk;
        offset += chunk;
        len -= chunk;
    }
    return FAT_OK;
}

int fat_write(fat_node *n, uint64_t offset, const void *buf, uint32_t len, uint32_t *done)
{
    fat_volume *v = n->v;
    *done = 0;
    if (n->is_dir)
        return FAT_EISDIR;
    if (len == 0)
        return FAT_OK;
    if (offset + len > 0xFFFFFFFFull)
        return FAT_EFBIG;
    int e = begin_change(v);
    if (e)
        return e;
    uint64_t end = offset + len;
    uint32_t need = (uint32_t)((end + v->cluster_bytes - 1) / v->cluster_bytes);
    if ((e = ensure_clusters(n, need)))
        return e;
    if (offset > n->size) {
        /* Writing past the end: the gap reads back as zeros. */
        if ((e = write_bytes(n, n->size, NULL, (uint32_t)(offset - n->size), true)))
            return e;
    }
    if ((e = write_bytes(n, offset, buf, len, false)))
        return e;
    *done = len;
    if (end > n->size)
        n->size = (uint32_t)end;
    now_fat(&n->wrt_date, &n->wrt_time, NULL);
    n->acc_date = n->wrt_date;
    n->attr |= FAT_ATTR_ARCHIVE;
    return node_write_entry(n);
}

int fat_truncate(fat_node *n, uint64_t size)
{
    fat_volume *v = n->v;
    if (n->is_dir)
        return FAT_EISDIR;
    if (size > 0xFFFFFFFFull)
        return FAT_EFBIG;
    if (size == n->size)
        return FAT_OK;
    int e = begin_change(v);
    if (e)
        return e;
    if (size > n->size) {
        uint32_t need = (uint32_t)((size + v->cluster_bytes - 1) / v->cluster_bytes);
        if ((e = ensure_clusters(n, need)))
            return e;
        if ((e = write_bytes(n, n->size, NULL, (uint32_t)(size - n->size), true)))
            return e;
    } else {
        uint32_t keep = (uint32_t)((size + v->cluster_bytes - 1) / v->cluster_bytes);
        if (keep == 0) {
            if (n->first_cluster >= 2 && (e = free_chain(v, n->first_cluster)))
                return e;
            n->first_cluster = 0;
        } else {
            uint32_t last, next;
            if ((e = chain_at(n, keep - 1, &last)) || (e = fat_get(v, last, &next)))
                return e;
            if (!is_eoc(v, next)) {
                if ((e = fat_set(v, last, v->eoc)) || (e = free_chain(v, next)))
                    return e;
            }
        }
        n->chain_cluster = NO_CLUSTER;
    }
    n->size = (uint32_t)size;
    now_fat(&n->wrt_date, &n->wrt_time, NULL);
    n->attr |= FAT_ATTR_ARCHIVE;
    return node_write_entry(n);
}

int fat_flush(fat_node *n)
{
    if (n->v->ro)
        return FAT_OK;
    return node_write_entry(n);
}

/* ---- Information ------------------------------------------------------------------ */

static void info_from(fat_volume *v, const dirent_t *d, fat_info *info)
{
    uint16_t u[256];
    int n = entry_name(d, u);
    utf16_to_8(u, n, info->name, sizeof(info->name) - 1);
    n = short_to_name(d->ent, 0, u);
    for (int i = 0; i < n && i < 12; i++)
        info->short_name[i] = u[i] < 0x80 ? (char)u[i] : '?';   /* ASCII view of the 8.3 name */
    info->short_name[n < 12 ? n : 12] = 0;
    info->attr = d->ent[11];
    info->size = (d->ent[11] & FAT_ATTR_DIRECTORY) ? 0 : rd32(d->ent + 28);
    info->first_cluster = ent_cluster(v, d->ent);
    unpack_time(rd16(d->ent + 16), rd16(d->ent + 14), d->ent[13], &info->created);
    unpack_time(rd16(d->ent + 24), rd16(d->ent + 22), 0, &info->modified);
    unpack_time(rd16(d->ent + 18), 0, 0, &info->accessed);
}

int fat_stat(fat_node *n, fat_info *info)
{
    memset(info, 0, sizeof(*info));
    if (n->is_root) {
        info->name[0] = '\\';
        info->attr = FAT_ATTR_DIRECTORY;
        info->first_cluster = n->first_cluster;
        unpack_time(0x21, 0, 0, &info->created);       /* 1980-01-01 */
        info->modified = info->accessed = info->created;
        return FAT_OK;
    }
    dirent_t d;
    int e = slot_read(n->parent, n->slot, d.ent);
    if (e)
        return e;
    /* Reassemble the long name from the slots before the short entry. */
    uint32_t slot = n->first_slot;
    dirent_t full;
    if (dir_next(n->parent, &slot, &full, false) == 1 && full.slot == n->slot)
        d = full;
    else {
        d.slot = n->slot;
        d.llen = 0;
    }
    info_from(n->v, &d, info);
    /* The node's in-memory fields are the newest. */
    info->size = n->is_dir ? 0 : n->size;
    info->attr = n->attr;
    info->first_cluster = n->first_cluster;
    unpack_time(n->crt_date, n->crt_time, n->crt_tenth, &info->created);
    unpack_time(n->wrt_date, n->wrt_time, 0, &info->modified);
    unpack_time(n->acc_date, 0, 0, &info->accessed);
    return FAT_OK;
}

int fat_set_attr(fat_node *n, uint8_t attr)
{
    if (n->is_root)
        return FAT_EINVAL;
    int e = begin_change(n->v);
    if (e)
        return e;
    uint8_t keep = n->attr & (FAT_ATTR_DIRECTORY | FAT_ATTR_VOLUME);
    n->attr = (uint8_t)(keep | (attr & (FAT_ATTR_READONLY | FAT_ATTR_HIDDEN | FAT_ATTR_SYSTEM | FAT_ATTR_ARCHIVE)));
    return node_write_entry(n);
}

int fat_set_times(fat_node *n, const fat_time *created, const fat_time *modified, const fat_time *accessed)
{
    if (n->is_root)
        return FAT_EINVAL;
    int e = begin_change(n->v);
    if (e)
        return e;
    if (created) pack_time(created, &n->crt_date, &n->crt_time, &n->crt_tenth);
    if (modified) pack_time(modified, &n->wrt_date, &n->wrt_time, NULL);
    if (accessed) pack_time(accessed, &n->acc_date, NULL, NULL);
    return node_write_entry(n);
}

int fat_readdir(fat_node *dir, uint32_t *cookie, fat_info *info)
{
    if (!dir->is_dir)
        return FAT_ENOTDIR;
    dirent_t d;
    int r = dir_next(dir, cookie, &d, false);
    if (r != 1)
        return r;
    memset(info, 0, sizeof(*info));
    info_from(dir->v, &d, info);
    return 1;
}

/* ---- Removing and renaming --------------------------------------------------------- */

static int dir_is_empty(fat_node *dir)
{
    uint32_t slot = 0;
    dirent_t d;
    for (;;) {
        int r = dir_next(dir, &slot, &d, false);
        if (r < 0)
            return r;
        if (r == 0)
            return 1;
        if (d.ent[0] == '.' && (d.ent[1] == ' ' || (d.ent[1] == '.' && d.ent[2] == ' ')))
            continue;
        return 0;
    }
}

int fat_unlink(fat_node *dir, const char *name)
{
    fat_volume *v = dir->v;
    fat_node *n;
    int e = fat_lookup(dir, name, &n);
    if (e)
        return e;
    if (n == dir || n == dir->parent || n->is_root) {
        fat_close(n);
        return FAT_EINVAL;
    }
    if (n->is_dir) {
        int empty = dir_is_empty(n);
        if (empty <= 0) {
            fat_close(n);
            return empty < 0 ? empty : FAT_ENOTEMPTY;
        }
        if (n->refs > 1) {                     /* a directory someone has open */
            fat_close(n);
            return FAT_EBUSY;
        }
    }
    if ((e = begin_change(v)) || (e = dir_remove_slots(dir, n->first_slot, n->slot))) {
        fat_close(n);
        return e;
    }
    n->unlinked = true;                        /* clusters go when the last user closes it */
    fat_close(n);
    return FAT_OK;
}

int fat_remove(fat_node *n)
{
    if (n->is_root || n->unlinked || !n->parent)
        return FAT_EINVAL;
    if (n->is_dir) {
        int empty = dir_is_empty(n);
        if (empty <= 0)
            return empty < 0 ? empty : FAT_ENOTEMPTY;
    }
    int e = begin_change(n->v);
    if (e)
        return e;
    if ((e = dir_remove_slots(n->parent, n->first_slot, n->slot)))
        return e;
    n->unlinked = true;
    return FAT_OK;
}

static bool is_ancestor(fat_node *a, fat_node *b)
{
    for (fat_node *p = b; p; p = p->parent)
        if (p == a)
            return true;
    return false;
}

int fat_rename(fat_node *src_dir, const char *src, fat_node *dst_dir, const char *dst, bool replace)
{
    fat_volume *v = src_dir->v;
    if (!dst_dir->is_dir)
        return FAT_ENOTDIR;
    uint16_t u[260];
    int n = prepare_name(dst, strlen(dst), u);
    if (n < 0)
        return n;
    fat_node *node;
    int e = fat_lookup(src_dir, src, &node);
    if (e)
        return e;
    if (node->is_root || node == src_dir || node == src_dir->parent) {
        fat_close(node);
        return FAT_EINVAL;
    }
    if (node->is_dir && is_ancestor(node, dst_dir)) {
        fat_close(node);
        return FAT_EINVAL;                     /* moving a directory into itself */
    }
    dirent_t d;
    bool same_entry = false;
    e = dir_find(dst_dir, u, n, &d);
    if (e == FAT_OK) {
        if (dst_dir == src_dir && d.slot == node->slot) {
            same_entry = true;                 /* only the case or spelling changes */
        } else if (!replace) {
            fat_close(node);
            return FAT_EEXIST;
        } else {
            fat_node *victim;
            if ((e = fat_lookup(dst_dir, dst, &victim)) == FAT_OK) {
                bool dir_victim = victim->is_dir;
                fat_close(victim);
                if (dir_victim || node->is_dir) {
                    fat_close(node);
                    return FAT_EEXIST;
                }
            }
            if ((e = fat_unlink(dst_dir, dst))) {
                fat_close(node);
                return e;
            }
        }
    } else if (e != FAT_ENOENT) {
        fat_close(node);
        return e;
    }
    if ((e = begin_change(v))) {
        fat_close(node);
        return e;
    }
    /* Keep a copy of the old entries (long name parts and short entry). */
    uint32_t old_first = node->first_slot, old_slot = node->slot;
    uint32_t old_count = old_slot - old_first + 1;
    uint8_t old[21][32];
    if (old_count > 21) {
        fat_close(node);
        return FAT_ECORRUPT;
    }
    for (uint32_t i = 0; i < old_count; i++) {
        if ((e = slot_read(src_dir, old_first + i, old[i]))) {
            fat_close(node);
            return e;
        }
    }
    uint8_t ent[32];
    memcpy(ent, old[old_count - 1], 32);
    uint32_t first, slot;
    if (same_entry) {
        /* The new name may need the same slots: free them first, and put the
           exact old entries back if the new ones cannot be written. */
        if ((e = dir_remove_slots(src_dir, old_first, old_slot))) {
            fat_close(node);
            return e;
        }
        if ((e = dir_add(dst_dir, u, n, ent, &first, &slot))) {
            for (uint32_t i = 0; i < old_count; i++)
                slot_write(src_dir, old_first + i, old[i]);
            fat_close(node);
            return e;
        }
    } else {
        /* Write the new entry first; the old one goes only once that worked,
           so a failure (e.g. a full directory) changes nothing. */
        if ((e = dir_add(dst_dir, u, n, ent, &first, &slot))) {
            fat_close(node);
            return e;
        }
        if ((e = dir_remove_slots(src_dir, old_first, old_slot))) {
            fat_close(node);
            return e;
        }
    }
    uint8_t sn[11], ntres;
    if ((e = slot_read(dst_dir, slot, ent))) {
        fat_close(node);
        return e;
    }
    memcpy(sn, ent, 11);
    ntres = ent[12];
    memcpy(node->short_name, sn, 11);
    node->ntres = ntres;
    node->slot = slot;
    node->first_slot = first;
    if (dst_dir != src_dir) {
        dst_dir->refs++;
        node->parent = dst_dir;
        if (node->is_dir) {
            /* Update ".." in the moved directory. */
            uint8_t dot[32];
            uint32_t pc = dst_dir->is_root ? 0 : dst_dir->first_cluster;
            if (slot_read(node, 1, dot) == FAT_OK && dot[0] == '.' && dot[1] == '.') {
                wr16(dot + 20, v->type == 32 ? (uint16_t)(pc >> 16) : 0);
                wr16(dot + 26, (uint16_t)pc);
                slot_write(node, 1, dot);
            }
        }
        fat_close(src_dir);                    /* the node no longer references it */
    }
    e = node_write_entry(node);
    fat_close(node);
    return e;
}

int fat_set_label(fat_volume *v, const char *label)
{
    int e = begin_change(v);
    if (e)
        return e;
    uint8_t name[11];
    memset(name, ' ', 11);
    size_t len = strlen(label);
    for (size_t i = 0; i < len && i < 11; i++) {
        char c = label[i];
        if (c >= 'a' && c <= 'z') c = (char)(c - 32);
        name[i] = (uint8_t)c;
    }
    /* Root directory entry. */
    uint32_t slot = 0;
    dirent_t d;
    bool found = false;
    while (dir_next(v->root, &slot, &d, true) == 1) {
        if ((d.ent[11] & FAT_ATTR_VOLUME) && !(d.ent[11] & FAT_ATTR_DIRECTORY)) {
            found = true;
            break;
        }
    }
    uint8_t ent[32];
    if (found) {
        memcpy(ent, d.ent, 32);
        memcpy(ent, name, 11);
        if ((e = slot_write(v->root, d.slot, ent)))
            return e;
    } else {
        uint32_t first;
        if ((e = find_free_slots(v->root, 1, &first)))
            return e;
        memset(ent, 0, 32);
        memcpy(ent, name, 11);
        ent[11] = FAT_ATTR_VOLUME;
        uint16_t date, time;
        now_fat(&date, &time, NULL);
        wr16(ent + 22, time);
        wr16(ent + 24, date);
        if ((e = slot_write(v->root, first, ent)))
            return e;
    }
    /* Boot sector copy of the label. */
    uint8_t *s;
    if ((e = meta_get(v, 0, &s)))
        return e;
    memcpy(v->tmp, s, v->bps);
    memcpy(v->tmp + (v->type == 32 ? 71 : 43), name, 11);
    if ((e = meta_put(v, 0, v->tmp)))
        return e;
    int n = 11;
    while (n > 0 && name[n - 1] == ' ') n--;
    memcpy(v->label, name, (size_t)n);
    v->label[n] = 0;
    return FAT_OK;
}

/* ---- Mounting --------------------------------------------------------------------- */

static int write_fsinfo(fat_volume *v)
{
    if (v->type != 32 || !v->fsinfo_sector || v->ro)
        return FAT_OK;
    uint8_t *s;
    int e = meta_get(v, v->fsinfo_sector, &s);
    if (e)
        return e;
    if (rd32(s) != 0x41615252 || rd32(s + 484) != 0x61417272)
        return FAT_OK;                         /* no valid FSInfo to update */
    memcpy(v->tmp, s, v->bps);
    wr32(v->tmp + 488, v->free_count);
    wr32(v->tmp + 492, v->next_free);
    return meta_put(v, v->fsinfo_sector, v->tmp);
}

int fat_mount(const fat_dev *dev, bool read_only, fat_volume **out)
{
    if (dev->sector_size < 512 || dev->sector_size > 4096)
        return FAT_EINVAL;
    uint8_t *b = fat_env_alloc(dev->sector_size);
    if (!b)
        return FAT_ENOMEM;
    int e = dev->read(dev->ctx, 0, 1, b) < 0 ? FAT_EIO : FAT_OK;
    if (e) {
        fat_env_free(b);
        return e;
    }
    uint32_t bps = rd16(b + 11), spc = b[13], reserved = rd16(b + 14), nfats = b[16];
    uint32_t root_entries = rd16(b + 17);
    uint32_t total = rd16(b + 19) ? rd16(b + 19) : rd32(b + 32);
    uint32_t fatsz = rd16(b + 22) ? rd16(b + 22) : rd32(b + 36);
    bool ok = (b[0] == 0xEB || b[0] == 0xE9) && bps == dev->sector_size && spc && !(spc & (spc - 1)) &&
              reserved && nfats >= 1 && nfats <= 4 && fatsz && total && total <= dev->sectors &&
              (b[510] == 0x55 && b[511] == 0xAA);
    if (!ok) {
        fat_env_free(b);
        return FAT_ENOTFAT;
    }
    fat_volume *v = fat_env_alloc(sizeof(fat_volume));
    if (!v) {
        fat_env_free(b);
        return FAT_ENOMEM;
    }
    v->dev = *dev;
    v->ro = read_only;
    v->bps = bps;
    v->spc = spc;
    v->cluster_bytes = bps * spc;
    v->eps = bps / 32;
    v->reserved = reserved;
    v->nfats = nfats;
    v->fat_sectors = fatsz;
    v->root_entries = root_entries;
    v->root_sectors = (root_entries * 32 + bps - 1) / bps;
    v->fat_lba = reserved;
    v->root_lba = reserved + (uint64_t)nfats * fatsz;
    v->data_lba = v->root_lba + v->root_sectors;
    if (v->data_lba >= total) {
        fat_env_free(b);
        fat_env_free(v);
        return FAT_ENOTFAT;
    }
    v->total_clusters = (uint32_t)((total - v->data_lba) / spc);
    v->type = v->total_clusters < 4085 ? 12 : v->total_clusters < 65525 ? 16 : 32;
    v->max_cluster = v->total_clusters + 1;
    /* The FAT must be big enough for all clusters. */
    uint64_t fat_bytes = (uint64_t)fatsz * bps;
    uint64_t need = v->type == 12 ? ((uint64_t)(v->max_cluster + 1) * 3 + 1) / 2
                  : (uint64_t)(v->max_cluster + 1) * (v->type == 16 ? 2 : 4);
    if (fat_bytes < need || (v->type == 32 && root_entries) || (v->type != 32 && !root_entries)) {
        fat_env_free(b);
        fat_env_free(v);
        return FAT_ENOTFAT;
    }
    v->eoc = v->type == 12 ? 0xFFF : v->type == 16 ? 0xFFFF : 0x0FFFFFFF;
    const uint8_t *ext = b + (v->type == 32 ? 64 : 36);   /* extended boot record */
    v->dirty_offset = (uint8_t)(v->type == 32 ? 65 : 37);
    if (ext[2] == 0x29) {
        v->serial = rd32(ext + 3);
        int n = 11;
        memcpy(v->label, ext + 7, 11);
        while (n > 0 && v->label[n - 1] == ' ') n--;
        v->label[n] = 0;
        if (!memcmp(v->label, "NO NAME", 8))
            v->label[0] = 0;
    }
    v->free_count = NO_CLUSTER;
    v->next_free = 2;
    if (v->type == 32) {
        v->root_cluster = rd32(b + 44);
        v->fsinfo_sector = rd16(b + 48);
        if (v->root_cluster < 2 || v->root_cluster > v->max_cluster) {
            fat_env_free(b);
            fat_env_free(v);
            return FAT_ENOTFAT;
        }
    }
    fat_env_free(b);

    v->tmp = fat_env_alloc(bps);
    for (int i = 0; i < MCACHE_SLOTS; i++)
        v->mc[i].data = fat_env_alloc(bps);
    v->root = fat_env_alloc(sizeof(fat_node));
    bool alloc_ok = v->tmp && v->root;
    for (int i = 0; i < MCACHE_SLOTS; i++)
        alloc_ok = alloc_ok && v->mc[i].data;
    if (!alloc_ok) {
        fat_unmount(v);
        return FAT_ENOMEM;
    }
    fat_node *r = v->root;
    r->v = v;
    r->refs = 1;                           /* held by the volume */
    r->is_dir = r->is_root = true;
    r->attr = FAT_ATTR_DIRECTORY;
    r->first_cluster = v->type == 32 ? v->root_cluster : 0;
    r->chain_cluster = NO_CLUSTER;
    r->next = NULL;
    v->nodes = r;

    /* FSInfo (FAT32): trust the free count only if it is plausible. */
    if (v->type == 32 && v->fsinfo_sector && v->fsinfo_sector < reserved) {
        uint8_t *s;
        if (meta_get(v, v->fsinfo_sector, &s) == FAT_OK && rd32(s) == 0x41615252 && rd32(s + 484) == 0x61417272) {
            uint32_t fc = rd32(s + 488), nf = rd32(s + 492);
            if (fc <= v->total_clusters)
                v->free_count = fc;
            if (nf >= 2 && nf <= v->max_cluster)
                v->next_free = nf;
        }
    }
    if (v->free_count == NO_CLUSTER && (e = count_free(v)) != FAT_OK) {
        fat_unmount(v);
        return e;
    }
    /* Remember whether FAT[1] carries a clean-shutdown bit we must maintain. */
    if (v->type != 12) {
        uint32_t f1;
        uint8_t *s;
        if (meta_get(v, v->fat_lba, &s) == FAT_OK) {
            f1 = v->type == 16 ? rd16(s + 2) : rd32(s + 4);
            v->fat1_clean_bit = v->type == 16 ? (f1 & 0x8000) : (f1 & 0x08000000);
        }
    }
    *out = v;
    return FAT_OK;
}

int fat_sync(fat_volume *v)
{
    for (fat_node *n = v->nodes; n; n = n->next)
        if (!n->is_root && !n->unlinked && !v->ro)
            node_write_entry(n);
    return write_fsinfo(v);
}

int fat_unmount(fat_volume *v)
{
    int e = FAT_OK;
    if (v->root && !v->ro) {
        e = fat_sync(v);
        int e2 = mark_dirty(v, false);
        if (!e)
            e = e2;
    }
    /* Free remaining nodes (callers should have closed them). */
    while (v->nodes) {
        fat_node *n = v->nodes;
        v->nodes = n->next;
        fat_env_free(n);
    }
    for (int i = 0; i < MCACHE_SLOTS; i++)
        if (v->mc[i].data)
            fat_env_free(v->mc[i].data);
    if (v->tmp)
        fat_env_free(v->tmp);
    fat_env_free(v);
    return e;
}

void fat_discard(fat_volume *v)
{
    v->ro = true;                       /* nothing may be written to the new medium */
    fat_unmount(v);
}

int fat_mark_clean(fat_volume *v)
{
    if (v->ro || !v->marked_dirty)
        return FAT_OK;
    int e = fat_sync(v);
    int e2 = mark_dirty(v, false);
    return e ? e : e2;
}

void fat_volume_info(fat_volume *v, fat_volinfo *info)
{
    info->type = v->type;
    info->bytes_per_sector = v->bps;
    info->sectors_per_cluster = v->spc;
    info->cluster_bytes = v->cluster_bytes;
    info->total_clusters = v->total_clusters;
    if (v->free_count == NO_CLUSTER)
        count_free(v);
    info->free_clusters = v->free_count == NO_CLUSTER ? 0 : v->free_count;
    info->serial = v->serial;
    memcpy(info->label, v->label, sizeof(info->label));
    info->read_only = v->ro;
}
