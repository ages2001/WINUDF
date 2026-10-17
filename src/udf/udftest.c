/* udftest.c - test program of the UDF core on the build host: image files as devices.
   udftest <image> [-s sectorsize] [-o sector] <command> ...
     ls [path]            list a directory (recursive with -R)
     cat <path>           file to standard output
     verify <dir>         compare the whole volume with a directory tree
     info                 volume information
     put <src> <path>     copy a host file onto the volume
     mkdir <path>         rm <path>  rmdir <path>  mv <old> <new>
     trunc <path> <size>  pwrite <path> <offset> <src>
     attr <path> <dosattr>
     stress <n>           random create/write/delete with checks
   Build: cc -DUDF_HOST -o udftest udftest.c udfcore.c udfwr.c */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>
#include <dirent.h>
#include <sys/stat.h>
#include "udf.h"

static FILE *img;
static struct udf_dev dev;
static struct udf_vol vol;

static int hread(struct udf_dev *d, u64 lba, u16 n, u8 *buf)
{
    if (fseeko(img, (off_t)(lba * d->ssize), SEEK_SET)) return 1;
    if (fread(buf, d->ssize, n, img) != n) {
        memset(buf, 0, (size_t)n * d->ssize);
        return lba + n <= d->sectors ? 0 : 1;
    }
    return 0;
}

static int hwrite(struct udf_dev *d, u64 lba, u16 n, const u8 *buf)
{
    if (fseeko(img, (off_t)(lba * d->ssize), SEEK_SET)) return 1;
    if (fwrite(buf, d->ssize, n, img) != n) return 1;
    return 0;
}

void udf_now(struct udf_time *t)
{
    time_t now = time(0);
    struct tm *tm = localtime(&now);
    t->year = (u16)(tm->tm_year + 1900);
    t->month = (u8)(tm->tm_mon + 1);
    t->day = (u8)tm->tm_mday;
    t->hour = (u8)tm->tm_hour;
    t->min = (u8)tm->tm_min;
    t->sec = (u8)tm->tm_sec;
    t->cs = 0;
    t->tz = UDF_NOTZ;
}

void udf_log(const char *fmt, ...)
{
    va_list a;
    va_start(a, fmt);
    vfprintf(stderr, fmt, a);
    va_end(a);
}

static void u2utf8(const u16 *s, u16 n, char *out)
{
    u16 i;
    char *o = out;
    for (i = 0; i < n; i++) {
        u16 c = s[i];
        if (c < 0x80) *o++ = (char)c;
        else if (c < 0x800) {
            *o++ = (char)(0xC0 | (c >> 6));
            *o++ = (char)(0x80 | (c & 63));
        } else {
            *o++ = (char)(0xE0 | (c >> 12));
            *o++ = (char)(0x80 | ((c >> 6) & 63));
            *o++ = (char)(0x80 | (c & 63));
        }
    }
    *o = 0;
}

static u16 utf82u(const char *s, u16 *out)
{
    u16 n = 0;
    const unsigned char *p = (const unsigned char *)s;
    while (*p && n < UDF_NAMEMAX) {
        u16 c;
        if (*p < 0x80) c = *p++;
        else if ((*p & 0xE0) == 0xC0) {
            c = (u16)(((p[0] & 31) << 6) | (p[1] & 63));
            p += 2;
        } else {
            c = (u16)(((p[0] & 15) << 12) | ((p[1] & 63) << 6) | (p[2] & 63));
            p += 3;
        }
        out[n++] = c;
    }
    return n;
}

static void die(const char *m, int r)
{
    fprintf(stderr, "udftest: %s (%d)\n", m, r);
    exit(1);
}

/* node of a path ("" or "/" root); parent directory and entry position too */
static int walk(const char *path, struct udf_node *n, struct udf_node *parent, u32 *fidpos, u8 *fchar)
{
    char buf[1024], *t, *save;
    struct udf_fid f;
    struct udf_node cur;
    int r;
    r = udf_iget(&vol, vol.root, &cur);
    if (r) return r;
    if (parent) *parent = cur;
    if (fidpos) *fidpos = 0xFFFFFFFFUL;
    if (fchar) *fchar = FC_DIR;
    strncpy(buf, path, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = 0;
    for (t = strtok_r(buf, "/", &save); t; t = strtok_r(NULL, "/", &save)) {
        u16 nm[UDF_NAMEMAX + 1], nl = utf82u(t, nm);
        if (cur.ftype != FT_DIR && cur.ftype != FT_STREAMDIR) return UE_NOTDIR;
        r = udf_lookup(&vol, &cur, nm, nl, &f, NULL);
        if (r < 0) return r;
        if (r == 0) return UE_NOTFOUND;
        if (parent) *parent = cur;
        if (fidpos) *fidpos = f.pos;
        if (fchar) *fchar = f.fchar;
        r = udf_iget(&vol, f.icb, &cur);
        if (r) return r;
    }
    *n = cur;
    return 0;
}

static void split(const char *path, char *dir, char *name)
{
    const char *s = strrchr(path, '/');
    if (!s) {
        dir[0] = 0;
        strcpy(name, path);
    } else {
        memcpy(dir, path, (size_t)(s - path));
        dir[s - path] = 0;
        strcpy(name, s + 1);
    }
}

static void ls(struct udf_node *d, const char *prefix, int rec)
{
    struct udf_fid f;
    u32 pos = 0;
    int r;
    while ((r = udf_readdir(&vol, d, &pos, &f)) > 0) {
        struct udf_node n;
        char nm[800], dn[13], full[2048];
        if (f.fchar & (FC_PARENT | FC_DELETED)) continue;
        u2utf8(f.name, f.nlen, nm);
        udf_dosname(f.name, f.nlen, dn, NULL);
        r = udf_iget(&vol, f.icb, &n);
        if (r) {
            printf("%s%s  <error %d>\n", prefix, nm, r);
            continue;
        }
        printf("%-12s %c%c %10llu %04u-%02u-%02u %02u:%02u  %s%s%s\n", dn,
               n.ftype == FT_DIR ? 'd' : '-', "0123"[n.adtype], (unsigned long long)n.size,
               n.mtime.year, n.mtime.month, n.mtime.day, n.mtime.hour, n.mtime.min,
               prefix, nm, n.ftype == FT_DIR ? "/" : "");
        if (rec && n.ftype == FT_DIR) {
            snprintf(full, sizeof(full), "%s%s/", prefix, nm);
            ls(&n, full, 1);
        }
    }
    if (r < 0) printf("%s: <readdir error %d>\n", prefix, r);
}

static long cmpfile(struct udf_node *n, const char *host)
{
    FILE *f = fopen(host, "rb");
    static u8 a[65536], b[65536];
    u32 off = 0;
    long bad = 0;
    if (!f) return -1;
    for (;;) {
        size_t k = fread(a, 1, sizeof(a), f);
        s32 r;
        u32 chunk = 1 + (u32)(rand() % 65535);
        if (chunk > k) chunk = (u32)k;
        if (!k) break;
        r = udf_read(&vol, n, off, (u32)k, b);
        if (r != (s32)k || memcmp(a, b, k)) {
            bad++;
            break;
        }
        (void)chunk;
        off += (u32)k;
    }
    fclose(f);
    if ((u64)off != n->size) bad++;
    return bad;
}

static long verify(struct udf_node *d, const char *host)
{
    struct udf_fid f;
    u32 pos = 0;
    long bad = 0, cnt = 0;
    int r;
    DIR *dh;
    struct dirent *e;
    while ((r = udf_readdir(&vol, d, &pos, &f)) > 0) {
        struct udf_node n;
        char nm[800], full[4096];
        struct stat st;
        if (f.fchar & (FC_PARENT | FC_DELETED)) continue;
        cnt++;
        u2utf8(f.name, f.nlen, nm);
        snprintf(full, sizeof(full), "%s/%s", host, nm);
        if (udf_iget(&vol, f.icb, &n) || stat(full, &st)) {
            printf("MISSING/ERR %s\n", full);
            bad++;
            continue;
        }
        if (n.ftype == FT_DIR) {
            if (!S_ISDIR(st.st_mode)) {
                printf("TYPE %s\n", full);
                bad++;
            } else {
                bad += verify(&n, full);
            }
        } else if (cmpfile(&n, full)) {
            printf("DIFF %s\n", full);
            bad++;
        }
    }
    if (r < 0) {
        printf("readdir error %d in %s\n", r, host);
        bad++;
    }
    dh = opendir(host);
    if (dh) {
        long hn = 0;
        while ((e = readdir(dh)) != NULL)
            if (strcmp(e->d_name, ".") && strcmp(e->d_name, "..")) hn++;
        closedir(dh);
        if (hn != cnt) {
            printf("COUNT %s: host %ld volume %ld\n", host, hn, cnt);
            bad++;
        }
    }
    return bad;
}


/* ---- stress: random operations against an in-memory model ---- */
#define SF 48
struct sfile { char name[64]; int dir; u8 *data; u32 size; int used; };
static struct sfile sf[SF];
static char sdirs[6][32] = { "", "sa", "sb", "sa/deep", "sc", "sb/x" };

static int s_lookup(const char *path, struct udf_node *n, struct udf_node *par, u32 *pos)
{
    return walk(path, n, par, pos, 0);
}

static void s_path(int i, char *out)
{
    if (sdirs[sf[i].dir][0]) sprintf(out, "%s/%s", sdirs[sf[i].dir], sf[i].name);
    else strcpy(out, sf[i].name);
}

static int s_check(void)
{
    int i, bad = 0;
    static u8 b[1 << 20];
    for (i = 0; i < SF; i++) {
        char p[128];
        struct udf_node n;
        if (!sf[i].used) continue;
        s_path(i, p);
        if (s_lookup(p, &n, 0, 0)) {
            printf("missing %s\n", p);
            bad++;
            continue;
        }
        if (n.size != sf[i].size) {
            printf("size %s %llu != %u\n", p, (unsigned long long)n.size, (unsigned)sf[i].size);
            bad++;
            continue;
        }
        if (sf[i].size && (udf_read(&vol, &n, 0, sf[i].size, b) != (s32)sf[i].size || memcmp(b, sf[i].data, sf[i].size))) {
            printf("data %s\n", p);
            bad++;
        }
    }
    return bad;
}

static int stress(int nops, unsigned seed)
{
    int op, k, r, bad = 0;
    static u8 rnd[300000];
    srand(seed);
    for (k = 0; k < (int)sizeof(rnd); k++) rnd[k] = (u8)rand();
    for (k = 1; k < 6; k++) {
        struct udf_node d, n;
        char par[64], nm[32];
        u16 w[40], wl;
        u32 fp;
        split(sdirs[k], par, nm);
        if (walk(sdirs[k], &n, 0, 0, 0) == 0) continue;
        r = walk(par, &d, 0, 0, 0);
        if (r) die("stress dir", r);
        wl = utf82u(nm, w);
        r = udf_create(&vol, &d, w, wl, 1, 0, 0, &n, &fp);
        if (r) die("stress mkdir", r);
    }
    for (op = 0; op < nops; op++) {
        int i = rand() % SF, what = rand() % 10;
        char p[128];
        struct udf_node n, par;
        u32 fp;
        if (!sf[i].used) {
            struct udf_node d;
            u16 w[80], wl;
            sprintf(sf[i].name, rand() % 2 ? "file %d with a long name.dat" : "F%d.TXT", i);
            sf[i].dir = rand() % 6;
            r = walk(sdirs[sf[i].dir], &d, 0, 0, 0);
            if (r) die("stress walk", r);
            wl = utf82u(sf[i].name, w);
            r = udf_create(&vol, &d, w, wl, 0, 0x20, 0, &n, &fp);
            if (r) { printf("create %s: %d\n", sf[i].name, r); bad++; break; }
            sf[i].used = 1;
            sf[i].size = 0;
            sf[i].data = 0;
            continue;
        }
        s_path(i, p);
        r = s_lookup(p, &n, &par, &fp);
        if (r) { printf("lookup %s: %d\n", p, r); bad++; break; }
        if (what < 4) {                 /* append */
            u32 len = (u32)(rand() % (what == 0 ? 120000 : 3000)), src = (u32)(rand() % 100000);
            sf[i].data = realloc(sf[i].data, sf[i].size + len + 1);
            memcpy(sf[i].data + sf[i].size, rnd + src, len);
            vol.nopen++;
            r = (int)udf_write(&vol, &n, sf[i].size, len, rnd + src);
            vol.nopen--;
            if (r == UE_NOSPACE || r == UE_UNSUPP) { /* volume full / too fragmented */
                printf("append %s: %d (ok)\n", p, r);
                if (s_lookup(p, &n, 0, 0) == 0) {
                    sf[i].size = (u32)n.size;
                    if (udf_read(&vol, &n, 0, sf[i].size, sf[i].data) != (s32)sf[i].size) bad++;
                }
                continue;
            }
            if (r != (int)len) { printf("append %s: %d\n", p, r); bad++; break; }
            sf[i].size += len;
        } else if (what < 6) {          /* overwrite */
            u32 off, len, src = (u32)(rand() % 100000);
            if (!sf[i].size) continue;
            off = (u32)(rand() % sf[i].size);
            len = (u32)(rand() % 20000);
            if (off + len > sf[i].size) {
                sf[i].data = realloc(sf[i].data, off + len + 1);
            }
            memcpy(sf[i].data + off, rnd + src, len);
            vol.nopen++;
            r = (int)udf_write(&vol, &n, off, len, rnd + src);
            vol.nopen--;
            if (r != (int)len) { printf("overwrite %s: %d\n", p, r); bad++; break; }
            if (off + len > sf[i].size) sf[i].size = off + len;
        } else if (what == 6) {         /* truncate */
            u32 ns = sf[i].size ? (u32)(rand() % (sf[i].size + 1)) : 0;
            r = udf_truncate(&vol, &n, ns);
            if (r) { printf("trunc %s: %d\n", p, r); bad++; break; }
            sf[i].size = ns;
        } else if (what == 7) {         /* delete */
            r = udf_unlink(&vol, &par, fp);
            if (r) { printf("unlink %s: %d\n", p, r); bad++; break; }
            free(sf[i].data);
            sf[i].used = 0;
        } else if (what == 8) {         /* rename / move */
            struct udf_node d;
            int nd = rand() % 6, j;
            char nn[64];
            u16 w[80], wl;
            sprintf(nn, "moved %d %d.bin", i, op);
            for (j = 0; j < SF; j++)
                if (sf[j].used && sf[j].dir == nd && !strcasecmp(sf[j].name, nn)) break;
            if (j < SF) continue;
            r = walk(sdirs[nd], &d, 0, 0, 0);
            if (r) die("stress walk2", r);
            wl = utf82u(nn, w);
            r = udf_rename(&vol, &par, fp, &d, w, wl, 0xFFFFFFFFUL);
            if (r) { printf("rename %s -> %s: %d\n", p, nn, r); bad++; break; }
            strcpy(sf[i].name, nn);
            sf[i].dir = nd;
        } else {                        /* attributes */
            r = udf_setattr(&vol, &par, fp, &n, (u8)(rand() % 2 ? 0x21 : 0x22));
            if (r) { printf("attr %s: %d\n", p, r); bad++; break; }
        }
        r = udf_sync(&vol);
        if (r) { printf("sync %d\n", r); bad++; break; }
        if (op % 50 == 49) {
            int b = s_check();
            if (b) { printf("check failed after op %d\n", op); bad += b; break; }
        }
    }
    bad += s_check();
    printf("stress: %d ops, %d problems\n", nops, bad);
    return bad;
}

int main(int argc, char **argv)
{
    int i = 1, r;
    u16 ss = 0;
    u64 off = 0;
    const char *cmd;
    static u8 cache[32][UDF_MAXBS], tmp[UDF_MAXBS];
    if (argc < 3) {
        fprintf(stderr, "usage: udftest image [-s ss] [-o sector] cmd ...\n");
        return 2;
    }
    img = fopen(argv[i++], "r+b");
    if (!img) die("cannot open image", 0);
    while (i < argc && argv[i][0] == '-') {
        if (!strcmp(argv[i], "-s")) ss = (u16)atoi(argv[i + 1]);
        else if (!strcmp(argv[i], "-o")) off = strtoull(argv[i + 1], 0, 0);
        i += 2;
    }
    fseeko(img, 0, SEEK_END);
    dev.ssize = ss ? ss : 2048;
    dev.sectors = (u64)ftello(img) / dev.ssize;
    dev.read = hread;
    dev.write = hwrite;
    dev.optical = ss == 0;
    udf_cache_init(UDF_MAXBS);
    for (r = 0; r < 32; r++) udf_cache_slot(cache[r]);
    udf_tmp = tmp;
    r = udf_mount(&vol, &dev, off, (u32)(dev.sectors - off));
    if (r) {
        if (ss == 0) {
            dev.ssize = 512;
            dev.optical = 0;
            dev.sectors *= 4;
            r = udf_mount(&vol, &dev, off, (u32)(dev.sectors - off));
        }
        if (r) die("mount failed", r);
    }
    cmd = argv[i++];
    if (!strcmp(cmd, "info")) {
        int k;
        printf("label '%s' rev %x bs %u maps %u flags %x serial %08x files %u dirs %u uniq %llu\n",
               vol.label, vol.rev, vol.bs, vol.npm, vol.flags, (unsigned)vol.serial,
               (unsigned)vol.nfiles, (unsigned)vol.ndirs, (unsigned long long)vol.uniq);
        for (k = 0; k < vol.npm; k++) {
            struct udf_pmap *pm = &vol.pm[k];
            printf(" map %d type %u num %u start %u len %u access %u ubm %u/%u free %u size %u",
                   k, pm->type, pm->num, (unsigned)pm->start, (unsigned)pm->len, pm->access,
                   (unsigned)pm->ubm_lbn, (unsigned)pm->ubm_len, (unsigned)pm->free, (unsigned)pm->size);
            if (pm->type == PM_VIRT) printf(" vat %u n %u", (unsigned)pm->vat_abs, (unsigned)pm->vat_n);
            if (pm->type == PM_SPAR) printf(" packet %u st %u entries %u", pm->packet, pm->nst, (unsigned)pm->st_len);
            if (pm->type == PM_META) printf(" meta %u/%u/%u dup %u", (unsigned)pm->meta_lbn[0], (unsigned)pm->meta_lbn[1], (unsigned)pm->meta_lbn[2], pm->dup);
            printf("\n");
        }
        printf("free %u total %u\n", (unsigned)udf_freeblocks(&vol), (unsigned)udf_totalblocks(&vol));
        return 0;
    }
    if (!strcmp(cmd, "ls")) {
        struct udf_node n;
        int rec = 0;
        if (i < argc && !strcmp(argv[i], "-R")) {
            rec = 1;
            i++;
        }
        r = walk(i < argc ? argv[i] : "", &n, 0, 0, 0);
        if (r) die("path", r);
        ls(&n, "", rec);
        return 0;
    }
    if (!strcmp(cmd, "stress")) {
        r = stress(atoi(argv[i]), (unsigned)(i + 1 < argc ? atoi(argv[i + 1]) : 1));
        udf_sync(&vol);
        return r != 0;
    }
    if (!strcmp(cmd, "stat")) {
        struct udf_node n;
        r = walk(argv[i], &n, 0, 0, 0);
        if (r) die("path", r);
        printf("loc %u:%u abs %u ftype %u efe %u ad %u size %llu links %u lea %u lad %u blocks %llu uniq %llu perm %x flags %x\n",
               n.loc.part, (unsigned)n.loc.lbn, (unsigned)n.feabs, n.ftype, n.efe, n.adtype,
               (unsigned long long)n.size, n.links, (unsigned)n.lea, (unsigned)n.lad,
               (unsigned long long)n.blocks, (unsigned long long)n.uniq, (unsigned)n.perm, n.icbflags);
        return 0;
    }
    if (!strcmp(cmd, "cat")) {
        struct udf_node n;
        static u8 b[65536];
        u32 o = 0;
        s32 k;
        r = walk(argv[i], &n, 0, 0, 0);
        if (r) die("path", r);
        while ((k = udf_read(&vol, &n, o, sizeof(b), b)) > 0) {
            fwrite(b, 1, (size_t)k, stdout);
            o += (u32)k;
        }
        if (k < 0) die("read", (int)k);
        return 0;
    }
    if (!strcmp(cmd, "verify")) {
        struct udf_node n;
        long bad;
        r = udf_iget(&vol, vol.root, &n);
        if (r) die("root", r);
        bad = verify(&n, argv[i]);
        printf("verify: %ld differences\n", bad);
        return bad != 0;
    }
    if (!strcmp(cmd, "put") || !strcmp(cmd, "mkdir")) {
        struct udf_node d, n;
        char dir[1024], name[512];
        u16 nm[UDF_NAMEMAX + 1], nl;
        struct udf_time t;
        int isdir = !strcmp(cmd, "mkdir");
        const char *path = isdir ? argv[i] : argv[i + 1];
        u32 fp;
        split(path, dir, name);
        r = walk(dir, &d, 0, 0, 0);
        if (r) die("dir", r);
        nl = utf82u(name, nm);
        udf_now(&t);
        r = udf_create(&vol, &d, nm, nl, isdir, 0x20, &t, &n, &fp);
        if (r) die("create", r);
        if (!isdir) {
            FILE *f = fopen(argv[i], "rb");
            static u8 b[65536];
            size_t k;
            u32 o = 0;
            if (!f) die("source", 0);
            vol.nopen++;
            while ((k = fread(b, 1, 1 + (size_t)(rand() % sizeof(b)), f)) > 0) {
                s32 w = udf_write(&vol, &n, o, (u32)k, b);
                if (w != (s32)k) die("write", (int)w);
                o += (u32)k;
            }
            fclose(f);
            vol.nopen--;
        }
        r = udf_sync(&vol);
        if (r) die("sync", r);
        return 0;
    }
    if (!strcmp(cmd, "pwrite")) {
        struct udf_node n;
        FILE *f;
        static u8 b[1 << 20];
        size_t k;
        r = walk(argv[i], &n, 0, 0, 0);
        if (r) die("path", r);
        f = fopen(argv[i + 2], "rb");
        if (!f) die("source", 0);
        k = fread(b, 1, sizeof(b), f);
        fclose(f);
        vol.nopen++;
        r = (int)udf_write(&vol, &n, (u32)strtoul(argv[i + 1], 0, 0), (u32)k, b);
        vol.nopen--;
        if (r != (int)k) die("write", r);
        r = udf_sync(&vol);
        if (r) die("sync", r);
        return 0;
    }
    if (!strcmp(cmd, "trunc")) {
        struct udf_node n;
        r = walk(argv[i], &n, 0, 0, 0);
        if (r) die("path", r);
        r = udf_truncate(&vol, &n, (u32)strtoul(argv[i + 1], 0, 0));
        if (r) die("truncate", r);
        r = udf_sync(&vol);
        if (r) die("sync", r);
        return 0;
    }
    if (!strcmp(cmd, "rm") || !strcmp(cmd, "rmdir")) {
        struct udf_node n, p;
        u32 fp;
        r = walk(argv[i], &n, &p, &fp, 0);
        if (r) die("path", r);
        r = udf_unlink(&vol, &p, fp);
        if (r) die("unlink", r);
        r = udf_sync(&vol);
        if (r) die("sync", r);
        return 0;
    }
    if (!strcmp(cmd, "mv")) {
        struct udf_node n, p, d, x;
        u32 fp, xp = 0xFFFFFFFFUL;
        char dir[1024], name[512];
        u16 nm[UDF_NAMEMAX + 1], nl;
        r = walk(argv[i], &n, &p, &fp, 0);
        if (r) die("path", r);
        split(argv[i + 1], dir, name);
        r = walk(dir, &d, 0, 0, 0);
        if (r) die("dest dir", r);
        (void)x;
        nl = utf82u(name, nm);
        r = udf_rename(&vol, &p, fp, &d, nm, nl, xp);
        if (r) die("rename", r);
        r = udf_sync(&vol);
        if (r) die("sync", r);
        return 0;
    }
    if (!strcmp(cmd, "attr")) {
        struct udf_node n, p;
        u32 fp;
        r = walk(argv[i], &n, &p, &fp, 0);
        if (r) die("path", r);
        r = udf_setattr(&vol, &p, fp, &n, (u8)strtoul(argv[i + 1], 0, 0));
        if (r) die("setattr", r);
        r = udf_sync(&vol);
        if (r) die("sync", r);
        return 0;
    }
    die("unknown command", 0);
    return 1;
}
