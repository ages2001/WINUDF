/* udfcore.c - UDF file system core: block cache, volume recognition and mounting, partition
   mapping (physical, virtual, sparable, metadata), file entries, allocation descriptors,
   file data and directories. Writing is in udfwr.c. */
#include "udf.h"

/* ------------------------------------------------------------------ helpers */

void udf_p16(u8 FAR *p, u16 v)
{
    p[0] = (u8)v;
    p[1] = (u8)(v >> 8);
}

void udf_p32(u8 FAR *p, u32 v)
{
    udf_p16(p, (u16)v);
    udf_p16(p + 2, (u16)(v >> 16));
}

void udf_p64(u8 FAR *p, u64 v)
{
    udf_p32(p, (u32)v);
    udf_p32(p + 4, (u32)(v >> 32));
}

void udf_fcopy(u8 FAR *d, const u8 FAR *s, u16 n)
{
    while (n--) *d++ = *s++;
}

void udf_fset(u8 FAR *d, u8 c, u16 n)
{
    while (n--) *d++ = c;
}

/* CRC-ITU-T (polynomial 1021h, initial value 0) of the descriptor tags, four bits a step */
static const u16 crctab[16] = {
    0x0000, 0x1021, 0x2042, 0x3063, 0x4084, 0x50a5, 0x60c6, 0x70e7,
    0x8108, 0x9129, 0xa14a, 0xb16b, 0xc18c, 0xd1ad, 0xe1ce, 0xf1ef
};

u16 udf_crc(const u8 FAR *p, u16 len, u16 crc)
{
    while (len--) {
        u8 b = *p++;
        crc = (u16)((crc << 4) ^ crctab[(crc >> 12) ^ (b >> 4)]);
        crc = (u16)((crc << 4) ^ crctab[(crc >> 12) ^ (b & 15)]);
    }
    return crc;
}

/* a valid descriptor tag: checksum, identifier (0xFFFF any), location (0xFFFFFFFF any) and
   the CRC of the descriptor when it lies within maxlen bytes */
int udf_tagok(const u8 FAR *p, u16 ident, u32 loc, u16 maxlen)
{
    u8 sum = 0;
    u16 i, clen;
    for (i = 0; i < 16; i++)
        if (i != 4) sum = (u8)(sum + p[i]);
    if (sum != p[4]) return 0;
    if (ident != 0xFFFF && G16(p, 0) != ident) return 0;
    if (loc != 0xFFFFFFFFUL && G32(p, 12) != loc) return 0;
    clen = G16(p, 10);
    if (clen && (u32)clen + 16 <= maxlen && udf_crc(p + 16, clen, 0) != G16(p, 8)) return 0;
    return 1;
}

/* fill in a descriptor tag: len bytes of descriptor (tag included) for the CRC */
void udf_tagset(u8 FAR *p, u16 ident, u16 ver, u32 loc, u16 len)
{
    u8 sum = 0;
    u16 i;
    udf_p16(p, ident);
    udf_p16(p + 2, ver);
    p[5] = 0;
    udf_p32(p + 12, loc);
    udf_p16(p + 10, (u16)(len - 16));
    udf_p16(p + 8, udf_crc(p + 16, (u16)(len - 16), 0));
    for (i = 0; i < 16; i++)
        if (i != 4) sum = (u8)(sum + p[i]);
    p[4] = sum;
}

/* ------------------------------------------------------------------ block cache */

#ifdef UDF_DOS16
#define MAXSLOTS        32
#else
#define MAXSLOTS        64
#endif

struct cslot {
    struct udf_vol *v;
    u32 abs;
    u16 age;
    u8 FAR *data;
};

static struct cslot slots[MAXSLOTS];
static u16 nslots, slotsize, clock;
u8 FAR *udf_tmp;

void udf_cache_init(u16 size)
{
    nslots = 0;
    slotsize = size;
}

int udf_cache_slot(u8 FAR *buf)
{
    if (nslots >= MAXSLOTS) return -1;
    slots[nslots].v = 0;
    slots[nslots].data = buf;
    nslots++;
    return 0;
}

void udf_cache_drop(struct udf_vol *v)
{
    u16 i;
    for (i = 0; i < nslots; i++)
        if (slots[i].v == v) slots[i].v = 0;
}

static struct cslot *cfind(struct udf_vol *v, u32 abs)
{
    u16 i;
    for (i = 0; i < nslots; i++)
        if (slots[i].v == v && slots[i].abs == abs) return &slots[i];
    return 0;
}

/* pinned buffers stay in the cache until unpinned */
#define MAXPIN          4
static u8 FAR *pins[MAXPIN];
static u16 npins;

void udf_pin(u8 FAR *p)
{
    if (npins < MAXPIN) pins[npins++] = p;
}

void udf_unpin(void)
{
    if (npins) npins--;
}

static int pinned(u8 FAR *p)
{
    u16 i;
    for (i = 0; i < npins; i++)
        if (pins[i] == p) return 1;
    return 0;
}

static struct cslot *cvictim(void)
{
    u16 i, best = 0xFFFF, oldest = 0;
    for (i = 0; i < nslots; i++) {
        u16 a;
        if (pinned(slots[i].data)) continue;
        if (!slots[i].v) return &slots[i];
        a = (u16)(clock - slots[i].age);
        if (best == 0xFFFF || a >= oldest) {
            oldest = a;
            best = i;
        }
    }
    return &slots[best];
}

int udf_devread(struct udf_vol *v, u32 abs, u16 n, u8 FAR *buf)
{
    struct udf_dev *d = v->dev;
    if (v->nblocks && (abs >= v->nblocks || n > v->nblocks - abs)) return UE_IO;
    if (d->read(d, v->base + ((u64)abs << v->ssh), (u16)(n << v->ssh), buf)) return UE_IO;
    return 0;
}

int udf_devwrite(struct udf_vol *v, u32 abs, u16 n, const u8 FAR *buf)
{
    struct udf_dev *d = v->dev;
    if (!d->write || d->ro) return UE_RDONLY;
    if (v->nblocks && (abs >= v->nblocks || n > v->nblocks - abs)) return UE_IO;
    if (d->write(d, v->base + ((u64)abs << v->ssh), (u16)(n << v->ssh), buf)) return UE_IO;
    return 0;
}

/* block abs of the volume, NULL on a device error. The data stays valid until the next
   udf_bget / udf_bnew. */
u8 FAR *udf_bget(struct udf_vol *v, u32 abs)
{
    struct cslot *s = cfind(v, abs);
    if (!s) {
        s = cvictim();
        s->v = 0;
        if (udf_devread(v, abs, 1, s->data)) return 0;
        s->v = v;
        s->abs = abs;
    }
    s->age = ++clock;
    return s->data;
}

/* a cache buffer for block abs that the caller fills completely */
u8 FAR *udf_bnew(struct udf_vol *v, u32 abs)
{
    struct cslot *s = cfind(v, abs);
    if (!s) {
        s = cvictim();
        s->v = v;
        s->abs = abs;
    }
    s->age = ++clock;
    return s->data;
}

/* blocks written to the device without the cache: refresh the cached copies */
void udf_cache_update(struct udf_vol *v, u32 abs, u16 n, const u8 FAR *data)
{
    u16 i;
    for (i = 0; i < n; i++) {
        struct cslot *s = cfind(v, abs + i);
        if (s) udf_fcopy(s->data, data + ((u32)i << v->bsh), v->bs);
    }
}

/* write block abs (data is its cache buffer or any other buffer) */
int udf_bput(struct udf_vol *v, u32 abs, u8 FAR *data)
{
    struct cslot *s = cfind(v, abs);
    int r = udf_devwrite(v, abs, 1, data);
    if (s && s->data != data) {
        if (r) s->v = 0;
        else udf_fcopy(s->data, data, v->bs);
    } else if (s && r) {
        s->v = 0;
    }
    return r;
}

/* ------------------------------------------------------------------ time, names */

void udf_timeget(const u8 FAR *p, struct udf_time *t)
{
    t->year = G16(p, 2);
    t->month = p[4];
    t->day = p[5];
    t->hour = p[6];
    t->min = p[7];
    t->sec = p[8];
    t->cs = p[9];
    t->tz = UDF_NOTZ;
    if ((p[1] >> 4) == 1) {
        t->tz = (s16)(G16(p, 0) & 0x0FFF);
        if (t->tz & 0x0800) t->tz = (s16)(t->tz - 0x1000);
        if (t->tz < -1440 || t->tz > 1440) t->tz = UDF_NOTZ;
    }
    if (t->month < 1 || t->month > 12 || t->day < 1 || t->day > 31 || t->year < 1980) {
        t->year = 1980;
        t->month = 1;
        t->day = 1;
        t->hour = t->min = t->sec = t->cs = 0;
    }
}

/* local time with its offset from UTC (or none) */
void udf_timeset(u8 FAR *p, const struct udf_time *t)
{
    udf_p16(p, (u16)(0x1000 | ((u16)t->tz & 0x0FFF)));
    udf_p16(p + 2, t->year);
    p[4] = t->month;
    p[5] = t->day;
    p[6] = t->hour;
    p[7] = t->min;
    p[8] = t->sec;
    p[9] = t->cs;
    p[10] = 0;
    p[11] = 0;
}

/* OSTA compressed Unicode (d-characters of len bytes, compression ID first) */
void udf_cs0(const u8 FAR *p, u16 len, u16 *out, u16 *nout, u16 max)
{
    u16 n = 0, i;
    u8 id;
    *nout = 0;
    if (len < 1) return;
    id = p[0];
    if (id == 8 || id == 254) {
        for (i = 1; i < len && n < max; i++) out[n++] = p[i];
    } else if (id == 16 || id == 255) {
        for (i = 1; i + 1 < len && n < max; i += 2) out[n++] = (u16)((p[i] << 8) | p[i + 1]);
    }
    *nout = n;
}

u16 udf_upper(u16 c)
{
    if (c >= 'a' && c <= 'z') return (u16)(c - 32);
    if (c < 0xE0) return c;
    if (c <= 0xFE) return c == 0xF7 ? c : (u16)(c - 32);
    if (c == 0xFF) return 0x178;
    if (c >= 0x100 && c <= 0x17F) {
        if (c == 0x131) return 'I';
        if ((c >= 0x139 && c <= 0x148) || (c >= 0x179 && c <= 0x17E)) return (c & 1) ? c : (u16)(c - 1);
        if (c == 0x130 || c == 0x138 || c == 0x149 || c == 0x178 || c == 0x17F) return c;
        return (c & 1) ? (u16)(c - 1) : c;
    }
    if (c >= 0x3B1 && c <= 0x3C9 && c != 0x3C2) return (u16)(c - 32);
    if (c >= 0x430 && c <= 0x44F) return (u16)(c - 32);
    if (c >= 0x450 && c <= 0x45F) return (u16)(c - 80);
    return c;
}

/* Unicode character -> OEM byte of the code page (table of 0x80-0xFF), 0 when none */
static u8 oemchar(u16 c, const u16 *oemtab)
{
    u16 i;
    if (c < 0x80) return (u8)c;
    if (!oemtab) return 0;
    for (i = 0; i < 128; i++)
        if (oemtab[i] == c) return (u8)(0x80 + i);
    return 0;
}

static int dosbad(u8 c)
{
    static const char bad[] = "\"*+,/:;<=>?[\\]|. ";
    const char *b;
    if (c < 0x20 || c == 0x7F || c == 0xE5) return 1;
    for (b = bad; *b; b++)
        if ((u8)*b == c) return 1;
    return 0;
}

/* DOS 8.3 name of a UDF name: "NAME.EXT" in OEM characters. Names that do not fit become
   the first four characters, '#' and three hex digits of the CRC of the whole name (the
   method of the UDF specification, 6.7.1). Returns the length. */
int udf_dosname(const u16 *name, u16 len, char *out, const u16 *oemtab)
{
    static const char hex[] = "0123456789ABCDEF";
    char base[9], ext[4];
    int nb = 0, ne = 0, crc = 0, i, dot = -1, start = 0, o = 0;
    u16 k;
    for (k = 0; k < len; k++)
        if (name[k] == '.') dot = k;
    while (start < (int)len && name[start] == '.') {
        start++;
        crc = 1;
    }
    if (dot < start) dot = -1;
    for (i = start; i < (dot < 0 ? (int)len : dot); i++) {
        u8 c = oemchar(udf_upper(name[i]), oemtab);
        if (name[i] == ' ') {
            crc = 1;
            continue;
        }
        if (!c || dosbad(c)) {
            c = '_';
            crc = 1;
        }
        if (nb < 8) base[nb++] = (char)c;
        else crc = 1;
    }
    if (dot >= 0) {
        for (i = dot + 1; i < (int)len; i++) {
            u8 c = oemchar(udf_upper(name[i]), oemtab);
            if (name[i] == ' ') {
                crc = 1;
                continue;
            }
            if (!c || dosbad(c)) {
                c = '_';
                crc = 1;
            }
            if (ne < 3) ext[ne++] = (char)c;
            else crc = 1;
        }
    }
    if (!nb) {
        base[nb++] = '_';
        crc = 1;
    }
    if (crc) {
        u16 v = 0;
        for (k = 0; k < len; k++) {
            u8 b[2];
            b[0] = (u8)(name[k] >> 8);
            b[1] = (u8)name[k];
            v = udf_crc(b, 2, v);
        }
        if (nb > 4) nb = 4;
        base[nb++] = '#';
        base[nb++] = hex[(v >> 8) & 15];
        base[nb++] = hex[(v >> 4) & 15];
        base[nb++] = hex[v & 15];
    }
    for (i = 0; i < nb; i++) out[o++] = base[i];
    if (ne) {
        out[o++] = '.';
        for (i = 0; i < ne; i++) out[o++] = ext[i];
    }
    out[o] = 0;
    return o;
}

/* "NAME.EXT" against an 11-character FCB pattern with '?' */
int udf_fcbmatch(const char *name, const char *pat)
{
    char f[11];
    int i = 0, j;
    for (j = 0; j < 11; j++) f[j] = ' ';
    for (j = 0; name[i] && name[i] != '.' && j < 8; i++, j++) f[j] = name[i];
    while (name[i] && name[i] != '.') i++;
    if (name[i] == '.') i++;
    for (j = 8; name[i] && j < 11; i++, j++) f[j] = name[i];
    for (j = 0; j < 11; j++)
        if (pat[j] != '?' && pat[j] != f[j]) return 0;
    return 1;
}

u8 udf_dosattr(struct udf_node *n, u8 fchar)
{
    u8 a = 0;
    if (n->ftype == FT_DIR || (fchar & FC_DIR)) a |= 0x10;
    if (fchar & FC_HIDDEN) a |= 0x02;
    if (n->icbflags & ICBF_SYSTEM) a |= 0x04;
    if (n->icbflags & ICBF_ARCHIVE) a |= 0x20;
    if (!(n->perm & PERM_OWRITE) && n->ftype != FT_DIR) a |= 0x01;
    return a;
}

/* ------------------------------------------------------------------ partitions */

static int sparemap(struct udf_vol *v, struct udf_pmap *pm, u32 lbn, u32 *abs)
{
    u32 packet = lbn & ~(u32)(pm->packet - 1), lo = 0, hi = pm->st_len;
    u16 t;
    for (t = 0; t < pm->nst; t++) {
        u32 tab = pm->st_abs[t];
        int bad = 0;
        lo = 0;
        hi = pm->st_len;
        while (lo < hi) {
            u32 mid = (lo + hi) >> 1, off = 56 + mid * 8;
            u8 FAR *p = udf_bget(v, tab + (off >> v->bsh));
            u32 orig;
            if (!p) {
                bad = 1;
                break;
            }
            orig = G32(p, (u16)(off & (v->bs - 1)));
            if (orig == packet) {
                *abs = G32(p, (u16)((off & (v->bs - 1)) + 4)) + (lbn - packet);
                return 0;
            }
            if (orig < packet) lo = mid + 1;
            else hi = mid;
        }
        if (!bad) break;
    }
    *abs = pm->start + lbn;
    return 0;
}

int udf_map(struct udf_vol *v, u16 part, u32 lbn, u32 *abs)
{
    struct udf_pmap *pm;
    if (part >= v->npm) return UE_CORRUPT;
    pm = &v->pm[part];
    switch (pm->type) {
    case PM_PHYS:
        if (lbn >= pm->len) return UE_CORRUPT;
        *abs = pm->start + lbn;
        return 0;
    case PM_SPAR:
        if (lbn >= pm->len) return UE_CORRUPT;
        return sparemap(v, pm, lbn, abs);
    case PM_VIRT:
    {
        u8 e[4];
        u32 x;
        if (lbn >= pm->vat_n) return UE_CORRUPT;
        if (udf_read(v, &v->sys[0], pm->vat_off + lbn * 4, 4, e) != 4) return UE_IO;
        x = G32(e, 0);
        if (x == 0xFFFFFFFFUL) return UE_CORRUPT;
        return udf_map(v, pm->phys, x, abs);
    }
    case PM_META:
    {
        u32 run;
        int r;
        if (lbn >= (u32)(v->sys[0].size >> v->bsh) && lbn >= (u32)(v->sys[1].size >> v->bsh)) return UE_CORRUPT;
        if (!pm->badmeta) {
            r = udf_bmap(v, &v->sys[0], lbn << v->bsh, abs, &run);
            if (r == 0 && *abs) return 0;
        }
        if (v->sys[1].ftype != FT_MMIRROR) return UE_CORRUPT;
        r = udf_bmap(v, &v->sys[1], lbn << v->bsh, abs, &run);
        if (r == 0 && *abs) return 0;
        return UE_CORRUPT;
    }
    }
    return UE_CORRUPT;
}

/* ------------------------------------------------------------------ file entries */

void udf_node_parse(struct udf_vol *v, struct udf_node *n, const u8 FAR *p)
{
    n->ftype = p[27];
    n->icbflags = G16(p, 34);
    n->adtype = (u8)(n->icbflags & 7);
    n->perm = G32(p, 44);
    n->links = G16(p, 48);
    n->size = G64(p, 56);
    n->big = (u8)((n->size >> 32) != 0);
    n->stream.lbn = 0xFFFFFFFFUL;
    n->stream.part = 0;
    if (n->efe) {
        n->blocks = G64(p, 72);
        udf_timeget(p + 80, &n->atime);
        udf_timeget(p + 92, &n->mtime);
        udf_timeget(p + 104, &n->ctime);
        if (G32(p, 152) & 0x3FFFFFFFUL) {
            n->stream.lbn = G32(p, 156);
            n->stream.part = G16(p, 160);
        }
        n->uniq = G64(p, 200);
        n->lea = G32(p, 208);
        n->lad = G32(p, 212);
    } else {
        n->blocks = G64(p, 64);
        udf_timeget(p + 72, &n->atime);
        udf_timeget(p + 84, &n->mtime);
        n->ctime = n->mtime;
        n->uniq = G64(p, 160);
        n->lea = G32(p, 168);
        n->lad = G32(p, 172);
    }
    n->c_ok = 0;
    n->tset = 0;
    (void)v;
}


int udf_iget(struct udf_vol *v, struct udf_loc loc, struct udf_node *n)
{
    u32 abs;
    u8 FAR *p;
    int depth, r;
    for (depth = 0; depth < 16; depth++) {
        u16 id;
        r = udf_map(v, loc.part, loc.lbn, &abs);
        if (r) return r;
        p = udf_bget(v, abs);
        if (!p) return UE_IO;
        id = G16(p, 0);
        if (id == 259 && udf_tagok(p, 259, 0xFFFFFFFFUL, v->bs)) {      /* indirect entry */
            loc.lbn = G32(p, 40);
            loc.part = G16(p, 44);
            continue;
        }
        if ((id != 261 && id != 266) || !udf_tagok(p, id, 0xFFFFFFFFUL, v->bs)) return UE_CORRUPT;
        if (G16(p, 20) == 4096) {
            /* strategy 4096: a newer entry follows, through an indirect entry */
            u32 nabs;
            u8 FAR *q;
            if (udf_map(v, loc.part, loc.lbn + 1, &nabs) == 0 && (q = udf_bget(v, nabs)) != 0 &&
                G16(q, 0) == 259 && udf_tagok(q, 259, 0xFFFFFFFFUL, v->bs) &&
                (G32(q, 36) & 0x3FFFFFFFUL)) {
                loc.lbn = G32(q, 40);
                loc.part = G16(q, 44);
                continue;
            }
            p = udf_bget(v, abs);
            if (!p) return UE_IO;
        }
        n->loc = loc;
        n->feabs = abs;
        n->efe = (u8)(id == 266);
        udf_node_parse(v, n, p);
        if ((u32)FEHDR(n) + n->lea + n->lad > v->bs) return UE_CORRUPT;
        return 0;
    }
    return UE_CORRUPT;
}

/* descriptor size of the allocation descriptor type */
u16 udf_adsize(u8 t)
{
    return t == AD_SHORT ? 8 : t == AD_LONG ? 16 : 20;
}

/* cursor to the first descriptor */
void udf_ad_rewind(struct udf_vol *v, struct udf_node *n)
{
    n->c_off = 0;
    n->c_elen = 0;
    n->c_blk = n->feabs;
    n->c_pos = (u16)(FEHDR(n) + n->lea);
    n->c_end = (u16)(n->c_pos + n->lad);
    n->c_ok = 1;
    (void)v;
}

/* load the next extent at the cursor: 1 loaded, 0 end of the descriptors, < 0 error */
int udf_ad_next(struct udf_vol *v, struct udf_node *n)
{
    u16 sz = udf_adsize(n->adtype);
    int hops = 0;
    for (;;) {
        u8 FAR *p;
        u32 len, lbn;
        u16 part;
        u8 type;
        if ((u32)n->c_pos + sz > n->c_end) return 0;
        p = udf_bget(v, n->c_blk);
        if (!p) return UE_IO;
        p += n->c_pos;
        len = G32(p, 0);
        if (n->adtype == AD_SHORT) {
            lbn = G32(p, 4);
            part = n->loc.part;
        } else if (n->adtype == AD_LONG) {
            lbn = G32(p, 4);
            part = G16(p, 8);
        } else {
            lbn = G32(p, 12);
            part = G16(p, 16);
        }
        type = (u8)(len >> 30);
        len &= 0x3FFFFFFFUL;
        if (!len) return 0;
        if (type == 3) {
            u32 abs;
            u8 FAR *q;
            if (++hops > 64) return UE_CORRUPT;
            if (udf_map(v, part, lbn, &abs)) return UE_CORRUPT;
            q = udf_bget(v, abs);
            if (!q) return UE_IO;
            if (!udf_tagok(q, 258, 0xFFFFFFFFUL, v->bs) || G32(q, 20) + 24 > v->bs) return UE_CORRUPT;
            n->c_blk = abs;
            n->c_pos = 24;
            n->c_end = (u16)(24 + G32(q, 20));
            continue;
        }
        n->c_ablk = n->c_blk;
        n->c_apos = n->c_pos;
        n->c_pos = (u16)(n->c_pos + sz);
        n->c_elen = len;
        n->c_lbn = lbn;
        n->c_part = part;
        n->c_type = type;
        return 1;
    }
}

/* the block holding byte off of the file: abs (0 for a hole or an extent not recorded) and
   run, the bytes from off that continue in sequence on the device (at least to the end of
   the block) */
int udf_bmap(struct udf_vol *v, struct udf_node *n, u32 off, u32 *abs, u32 *run)
{
    int r;
    u32 in;
    if (n->adtype == AD_ICB) return UE_CORRUPT;
    if (!n->c_ok || off < n->c_off) {
        udf_ad_rewind(v, n);
        r = udf_ad_next(v, n);
        if (r <= 0) {
            n->c_ok = 0;
            return r ? r : UE_NOTFOUND;
        }
    }
    while (off - n->c_off >= n->c_elen) {
        n->c_off += n->c_elen;
        r = udf_ad_next(v, n);
        if (r <= 0) {
            n->c_ok = 0;
            return r ? r : UE_NOTFOUND;
        }
    }
    in = off - n->c_off;
    if (n->c_type != 0) {
        *abs = 0;
        *run = n->c_elen - in;
        return 0;
    }
    r = udf_map(v, n->c_part, n->c_lbn + (in >> v->bsh), abs);
    if (r) return r;
    if (v->pm[n->c_part].type == PM_PHYS) *run = n->c_elen - in;
    else *run = v->bs - (in & (v->bs - 1));
    return 0;
}

s32 udf_read(struct udf_vol *v, struct udf_node *n, u32 off, u32 len, u8 FAR *buf)
{
    u32 done = 0;
    if ((u64)off >= n->size) return 0;
    if ((u64)off + len > n->size) len = (u32)(n->size - off);
    if (n->adtype == AD_ICB) {
        u8 FAR *p = udf_bget(v, n->feabs);
        if (!p) return UE_IO;
        if (off + len > n->lad) return UE_CORRUPT;
        udf_fcopy(buf, p + FEHDR(n) + n->lea + off, (u16)len);
        return (s32)len;
    }
    while (done < len) {
        u32 abs, run, k, in = (off + done) & (v->bs - 1);
        int r = udf_bmap(v, n, off + done, &abs, &run);
        if (r == UE_NOTFOUND) {
            abs = 0;            /* beyond the descriptors: a hole */
            run = len - done;
        } else if (r) {
            return done ? (s32)done : r;
        }
        k = len - done;
        if (k > run) k = run;
        if (!abs) {
            u32 z = k;
            while (z) {
                u16 c = z > 0x8000UL ? 0x8000U : (u16)z;
                udf_fset(buf + done + (k - z), 0, c);
                z -= c;
            }
        } else if (in == 0 && k >= v->bs) {
            /* whole blocks straight from the device */
            u32 nb = k >> v->bsh;
            u32 lim = 0x8000UL >> v->bsh;   /* at most 32 KB per device request */
            if (nb > lim) nb = lim;
            k = nb << v->bsh;
            if (udf_devread(v, abs, (u16)nb, buf + done)) return done ? (s32)done : UE_IO;
            {
                /* blocks in the cache may be newer than the device */
                u32 i;
                for (i = 0; i < nb; i++) {
                    struct cslot *s = cfind(v, abs + i);
                    if (s) udf_fcopy(buf + done + (i << v->bsh), s->data, v->bs);
                }
            }
        } else {
            u8 FAR *p = udf_bget(v, abs);
            if (!p) return done ? (s32)done : UE_IO;
            if (k > v->bs - in) k = v->bs - in;
            udf_fcopy(buf + done, p + in, (u16)k);
        }
        done += k;
    }
    return (s32)done;
}

/* ------------------------------------------------------------------ directories */

int udf_readdir(struct udf_vol *v, struct udf_node *dir, u32 *pos, struct udf_fid *f)
{
    u8 h[38];
    u8 nm[256];
    u16 liu, total;
    s32 r;
    if ((u64)*pos + 38 > dir->size) return 0;
    r = udf_read(v, dir, *pos, 38, h);
    if (r != 38) return r < 0 ? (int)r : UE_CORRUPT;
    if (!udf_tagok(h, 257, 0xFFFFFFFFUL, 0)) return UE_CORRUPT;
    f->fchar = h[18];
    f->lfi = h[19];
    f->icb.lbn = G32(h, 24);
    f->icb.part = G16(h, 28);
    liu = G16(h, 36);
    total = (u16)((38 + liu + f->lfi + 3) & ~3);
    if ((u64)*pos + 38 + liu + f->lfi > dir->size) return UE_CORRUPT;
    f->nlen = 0;
    if (f->lfi) {
        r = udf_read(v, dir, *pos + 38 + liu, f->lfi, nm);
        if (r != f->lfi) return r < 0 ? (int)r : UE_CORRUPT;
        udf_cs0(nm, f->lfi, f->name, &f->nlen, UDF_NAMEMAX);
    }
    f->name[f->nlen] = 0;
    f->pos = *pos;
    f->len = total;
    *pos += total;
    return 1;
}

/* 1 when the directory has nothing but the parent entry and deleted entries */
struct udf_fid udf_fidw[2];

int udf_dirempty(struct udf_vol *v, struct udf_node *dir)
{
#define f udf_fidw[1]
    u32 pos = 0;
    int r;
    while ((r = udf_readdir(v, dir, &pos, &f)) > 0)
        if (!(f.fchar & (FC_PARENT | FC_DELETED))) return 0;
    return r < 0 ? r : 1;
}
#undef f

/* the entry of dir named name (compared without case, or with its DOS name): 1 found
   (in f), 0 not found, < 0 error */
int udf_lookup(struct udf_vol *v, struct udf_node *dir, const u16 *name, u16 nlen,
               struct udf_fid *f, const u16 *oemtab)
{
    u32 pos = 0;
    int r;
    char dn[13];
    u16 k;
    int dosok = nlen <= 12;
    while ((r = udf_readdir(v, dir, &pos, f)) > 0) {
        if (f->fchar & (FC_PARENT | FC_DELETED)) continue;
        if (f->nlen == nlen) {
            for (k = 0; k < nlen && udf_upper(f->name[k]) == udf_upper(name[k]); k++)
                ;
            if (k == nlen) return 1;
        }
        if (dosok && (u16)udf_dosname(f->name, f->nlen, dn, oemtab) == nlen) {
            for (k = 0; k < nlen; k++) {
                u8 b = (u8)dn[k];
                u16 c = b < 0x80 || !oemtab ? b : oemtab[b - 0x80];
                if (udf_upper(c) != udf_upper(name[k])) break;
            }
            if (k == nlen) return 1;
        }
    }
    return r;
}

/* ------------------------------------------------------------------ mounting */

/* volume recognition sequence: an NSR descriptor (NSR02 or NSR03) */
static int vrs_ok(struct udf_vol *v)
{
    u16 step = v->bs > 2048 ? v->bs : 2048, i;
    int bea = 0;
    for (i = 0; i < 64; i++) {
        u32 byte = 32768UL + (u32)i * step;
        u8 FAR *p = udf_bget(v, byte >> v->bsh);
        if (!p) return 0;
        p += (u16)(byte & (v->bs - 1));
        if (p[1] == 'B' && p[2] == 'E' && p[3] == 'A' && p[4] == '0' && p[5] == '1') bea = 1;
        else if (p[1] == 'N' && p[2] == 'S' && p[3] == 'R' && p[4] == '0' && (p[5] == '2' || p[5] == '3'))
            return 1;
        else if (p[1] == 'T' && p[2] == 'E' && p[3] == 'A' && p[4] == '0' && p[5] == '1') return 0;
        else if (p[1] == 'C' && p[2] == 'D' && p[3] == '0' && p[4] == '0' && p[5] == '1') continue;
        else if (p[1] == 'B' && p[2] == 'O' && p[3] == 'O' && p[4] == 'T' && p[5] == '2') continue;
        else if (p[1] == 'C' && p[2] == 'D' && p[3] == 'W' && p[4] == '0' && p[5] == '2') continue;
        else if (!bea) return 0;
        else return 0;
    }
    return 0;
}

static int setbs(struct udf_vol *v, u16 bs)
{
    u8 sh = 0;
    u16 b = bs;
    if (bs < v->dev->ssize || bs > UDF_MAXBS || bs > slotsize) return -1;
    while (b > 1) {
        b >>= 1;
        sh++;
    }
    v->bs = bs;
    v->bsh = sh;
    v->ssh = 0;
    for (b = v->dev->ssize; b < bs; b <<= 1) v->ssh++;
    udf_cache_drop(v);
    return 0;
}

/* the anchor volume descriptor pointer: absolute block, 0 none */
static u32 find_avdp(struct udf_vol *v)
{
    u32 c[3];
    int i;
    c[0] = 256;
    c[1] = v->nblocks ? v->nblocks - 1 : 0;
    c[2] = v->nblocks > 257 ? v->nblocks - 257 : 0;
    for (i = 0; i < 3; i++) {
        u8 FAR *p;
        if (!c[i] || (v->nblocks && c[i] >= v->nblocks)) continue;
        p = udf_bget(v, c[i]);
        if (p && udf_tagok(p, 2, c[i], v->bs)) return c[i];
    }
    return 0;
}

struct vdsinfo {
    u32 lvd, pvd;
    u32 lvdn, pvdn;
    u32 pd[UDF_MAXPM];
    u32 pdn[UDF_MAXPM];
    u16 pdnum[UDF_MAXPM];
    u8 npd;
};

static int vds_scan(struct udf_vol *v, u32 loc, u32 len, struct vdsinfo *vi)
{
    u32 i, n = len >> v->bsh;
    int hops = 0;
    for (i = 0; i < n && i < 4096; i++) {
        u8 FAR *p = udf_bget(v, loc + i);
        u32 seq;
        u16 id;
        if (!p) return UE_IO;
        if (!udf_tagok(p, 0xFFFF, loc + i, v->bs)) break;
        id = G16(p, 0);
        seq = G32(p, 16);
        if (id == 8) break;
        if (id == 3) {
            if (++hops > 16) break;
            len = G32(p, 16);
            loc = G32(p, 20);
            n = len >> v->bsh;
            i = (u32)-1;
            continue;
        }
        if (id == 1 && (!vi->pvd || seq >= vi->pvdn)) {
            vi->pvd = loc + i;
            vi->pvdn = seq;
        } else if (id == 6 && (!vi->lvd || seq >= vi->lvdn)) {
            vi->lvd = loc + i;
            vi->lvdn = seq;
        } else if (id == 5) {
            u16 num = G16(p, 22), k;
            for (k = 0; k < vi->npd && vi->pdnum[k] != num; k++)
                ;
            if (k == vi->npd) {
                if (k == UDF_MAXPM) continue;
                vi->npd++;
                vi->pdnum[k] = num;
                vi->pd[k] = 0;
            }
            if (!vi->pd[k] || seq >= vi->pdn[k]) {
                vi->pd[k] = loc + i;
                vi->pdn[k] = seq;
            }
        }
    }
    return (vi->lvd && vi->npd) ? 0 : UE_CORRUPT;
}

static int regid_is(const u8 FAR *p, const char *s)
{
    int i;
    for (i = 0; s[i]; i++)
        if (p[1 + i] != (u8)s[i]) return 0;
    return 1;
}

static void label_set(struct udf_vol *v, const u8 FAR *d, u16 max)
{
    u16 n, i, len = d[max - 1];
#ifdef UDF_DOS16
    u16 ul[33];
#else
    u16 *ul = v->ulabel;
#endif
    if (len > max - 1) len = (u16)(max - 1);
    udf_cs0(d, len, ul, &n, 32);
    ul[n] = 0;
    for (i = 0; i < n; i++) v->label[i] = (char)(ul[i] < 0x80 && ul[i] >= 0x20 ? ul[i] : '_');
    v->label[n] = 0;
}

static int find_vat(struct udf_vol *v, struct udf_pmap *pm)
{
    struct udf_pmap *ph = &v->pm[pm->phys];
    u32 last, c;
    int i;
    if (!v->nblocks) return UE_CORRUPT;
    last = v->nblocks - 1;
    for (i = 0; i < 160 && (u32)i <= last; i++) {
        u8 FAR *p;
        struct udf_loc l;
        struct udf_node *n = &v->sys[0];
        u16 id;
        c = last - i;
        if (c < ph->start || c - ph->start >= ph->len + 1024) continue;
        p = udf_bget(v, c);
        if (!p) continue;
        id = G16(p, 0);
        if ((id != 261 && id != 266) || !udf_tagok(p, id, 0xFFFFFFFFUL, v->bs)) continue;
        if (p[27] != FT_VAT && p[27] != 0) continue;
        l.lbn = c - ph->start;
        l.part = pm->phys;
        n->loc = l;
        n->feabs = c;
        n->efe = (u8)(id == 266);
        udf_node_parse(v, n, p);
        if ((u32)FEHDR(n) + n->lea + n->lad > v->bs) continue;
        if (n->ftype == FT_VAT) {
            u8 h[152];
            if (udf_read(v, n, 0, 152, h) != 152) continue;
            pm->vat_off = G16(h, 0);
            if (pm->vat_off < 152 || (u64)pm->vat_off > n->size) continue;
            pm->vat_n = (u32)((n->size - pm->vat_off) / 4);
            v->nfiles = G32(h, 136);
            v->ndirs = G32(h, 140);
            v->minrd = G16(h, 144);
            v->minwr = G16(h, 146);
            v->maxwr = G16(h, 148);
            label_set(v, h + 4, 128);
        } else {
            u8 t[36];
            if (n->size < 36) continue;
            if (udf_read(v, n, (u32)n->size - 36, 36, t) != 36) continue;
            if (!regid_is(t, "*UDF Virtual Alloc Tbl")) continue;
            pm->vat_off = 0;
            pm->vat_n = (u32)((n->size - 36) / 4);
        }
        pm->vat_abs = c;
        n->ftype = FT_VAT;
        return 0;
    }
    return UE_CORRUPT;
}

static int load_sparing(struct udf_vol *v, struct udf_pmap *pm, const u32 *locs)
{
    u8 i, n = 0;
    for (i = 0; i < pm->nst && i < 4; i++) {
        u32 abs = locs[i];
        u8 FAR *p = udf_bget(v, abs);
        if (!p || !udf_tagok(p, 0, 0xFFFFFFFFUL, 0) || !regid_is(p + 16, "*UDF Sparing Table")) continue;
        if (!n) pm->st_len = G16(p, 48);
        pm->st_abs[n++] = abs;
    }
    pm->nst = n;
    if (!n) pm->st_len = 0;
    return 0;
}

static int load_meta(struct udf_vol *v, struct udf_pmap *pm)
{
    struct udf_loc l;
    int r, ok = 0, k;
    l.part = pm->phys;
    for (k = 0; k < 3; k++) {
        v->sys[k].ftype = 0;
        if (pm->meta_lbn[k] == 0xFFFFFFFFUL) continue;
        l.lbn = pm->meta_lbn[k];
        r = udf_iget(v, l, &v->sys[k]);
        if (r) {
            v->sys[k].ftype = 0;
            continue;
        }
        if (v->sys[k].ftype != FT_META + k) v->sys[k].ftype = 0;
        else if (k < 2) ok = 1;
    }
    if (v->sys[0].ftype != FT_META) {
        if (v->sys[1].ftype != FT_MMIRROR) return UE_CORRUPT;
        pm->badmeta = 1;
        v->sys[0].size = 0;
    }
    return ok ? 0 : UE_CORRUPT;
}

static int read_lvid(struct udf_vol *v, u32 loc, u32 len)
{
    u32 i, n = len >> v->bsh, last = 0;
    int hops = 0;
    for (i = 0; i < n; i++) {
        u8 FAR *p = udf_bget(v, loc + i);
        if (!p || !udf_tagok(p, 0xFFFF, loc + i, v->bs)) break;
        if (G16(p, 0) == 8) break;
        if (G16(p, 0) != 9) break;
        last = loc + i;
        if (G32(p, 32) && hops < 16) {
            hops++;
            n = G32(p, 32) >> v->bsh;
            loc = G32(p, 36);
            i = (u32)-1;
        }
    }
    v->lvid_abs = last;
    if (last) {
        u8 FAR *p = udf_bget(v, last);
        u32 np, liu, k;
        if (!p) return UE_IO;
        if (G32(p, 28) != 1) v->flags |= VF_WASOPEN;
        v->uniq = G64(p, 40);
        np = G32(p, 72);
        liu = G32(p, 76);
        if (80 + np * 8 + 46 <= v->bs) {
            for (k = 0; k < np && k < v->npm; k++) {
                v->pm[k].free = G32(p, (u16)(80 + k * 4));
                v->pm[k].size = G32(p, (u16)(80 + np * 4 + k * 4));
            }
            if (liu >= 46) {
                u16 o = (u16)(80 + np * 8);
                v->nfiles = G32(p, o + 32);
                v->ndirs = G32(p, o + 36);
                v->minrd = G16(p, o + 40);
                v->minwr = G16(p, o + 42);
                v->maxwr = G16(p, o + 44);
            }
        }
    }
    return 0;
}

/* logical block size of a UDF volume on the device at base: 0 when there is none */
int udf_probe_bs(struct udf_dev *d, u64 base, u32 nsect)
{
    static struct udf_vol pv;
    u16 bs;
    for (bs = d->ssize; bs <= UDF_MAXBS && bs <= slotsize; bs <<= 1) {
        pv.dev = d;
        pv.base = base;
        if (setbs(&pv, bs)) continue;
        pv.nblocks = nsect >> pv.ssh;
        if (vrs_ok(&pv) && find_avdp(&pv)) {
            udf_cache_drop(&pv);
            return bs;
        }
        udf_cache_drop(&pv);
        if (d->optical) break;
    }
    return 0;
}

/* mount the UDF volume of device d at sector base (nsect sectors, 0 unknown) */
int udf_mount(struct udf_vol *v, struct udf_dev *d, u64 base, u32 nsect)
{
    struct vdsinfo vi;
    u32 avdp, abs;
    u8 FAR *p;
    int r, bs, i, k;
    u16 nmaps, q;
    u8 dflags;

    {
        u8 *z = (u8 *)v;
        u16 n = sizeof(*v);
        while (n--) *z++ = 0;
    }
    v->dev = d;
    v->base = base;
    v->gen = d->gen;
    v->metapm = 0xFF;
    v->sroot.lbn = 0xFFFFFFFFUL;
    bs = udf_probe_bs(d, base, nsect);
    if (!bs) return UE_NOTUDF;
    setbs(v, (u16)bs);
    v->nblocks = nsect >> v->ssh;
    avdp = find_avdp(v);
    if (!avdp) return UE_NOTUDF;

    for (k = 0; k < 2; k++) {
        u32 len, loc;
        p = udf_bget(v, avdp);
        if (!p) return UE_IO;
        len = G32(p, 16 + k * 8);
        loc = G32(p, 20 + k * 8);
        {
            u8 *z = (u8 *)&vi;
            u16 n = sizeof(vi);
            while (n--) *z++ = 0;
        }
        if (vds_scan(v, loc, len, &vi) == 0) break;
    }
    if (k == 2) return UE_CORRUPT;

    /* logical volume descriptor */
    p = udf_bget(v, vi.lvd);
    if (!p) return UE_IO;
    if (G32(p, 212) != (u32)v->bs) {
        if (setbs(v, (u16)G32(p, 212))) return UE_UNSUPP;
        v->nblocks = nsect >> v->ssh;
        return UE_UNSUPP;
    }
    if (!regid_is(p + 216, "*OSTA UDF Compliant")) return UE_NOTUDF;
    v->rev = G16(p, 240);
    dflags = p[242];
    if (v->rev > UDF_MAXREV) return UE_UNSUPP;
    if (dflags & 3) v->flags |= VF_RDONLY;
    label_set(v, p + 84, 128);
    {
        struct udf_loc fsd;
        u32 iseq_len, iseq_loc;
        fsd.lbn = G32(p, 252);
        fsd.part = G16(p, 256);
        iseq_len = G32(p, 432);
        iseq_loc = G32(p, 436);
        nmaps = (u16)G32(p, 268);
        if (nmaps > UDF_MAXPM || nmaps == 0) return UE_UNSUPP;
        v->npm = (u8)nmaps;
        q = 440;
        for (i = 0; i < (int)nmaps; i++) {
            struct udf_pmap *pm = &v->pm[i];
            u8 type, mlen;
            p = udf_bget(v, vi.lvd);
            if (!p) return UE_IO;
            if (q + 2 > v->bs) return UE_CORRUPT;
            type = p[q];
            mlen = p[q + 1];
            if (q + mlen > v->bs || mlen < 6) return UE_CORRUPT;
            pm->phys = (u8)i;
            pm->ust_lbn = 0xFFFFFFFFUL;
            if (type == 1) {
                pm->type = PM_PHYS;
                pm->num = G16(p, q + 4);
            } else if (type == 2 && mlen >= 64) {
                pm->num = G16(p, q + 38);
                if (regid_is(p + q + 4, "*UDF Virtual Partition")) {
                    pm->type = PM_VIRT;
                } else if (regid_is(p + q + 4, "*UDF Sparable Partition")) {
                    pm->type = PM_SPAR;
                    pm->packet = G16(p, q + 40);
                    pm->nst = p[q + 42];
                    for (k = 0; k < 4; k++) pm->st_abs[k] = G32(p, q + 48 + k * 4);
                    if (!pm->packet || (pm->packet & (pm->packet - 1))) return UE_CORRUPT;
                } else if (regid_is(p + q + 4, "*UDF Metadata Partition")) {
                    pm->type = PM_META;
                    for (k = 0; k < 3; k++) pm->meta_lbn[k] = G32(p, q + 40 + k * 4);
                    pm->aunit = G32(p, q + 52);
                    pm->align = G16(p, q + 56);
                    pm->dup = (u8)(p[q + 58] & 1);
                } else {
                    return UE_UNSUPP;
                }
            } else {
                return UE_UNSUPP;
            }
            q = (u16)(q + mlen);
        }
        /* partition descriptors */
        for (i = 0; i < (int)nmaps; i++) {
            struct udf_pmap *pm = &v->pm[i];
            for (k = 0; k < vi.npd && vi.pdnum[k] != pm->num; k++)
                ;
            if (k == vi.npd) return UE_CORRUPT;
            p = udf_bget(v, vi.pd[k]);
            if (!p) return UE_IO;
            if (!regid_is(p + 24, "+NSR02") && !regid_is(p + 24, "+NSR03")) return UE_CORRUPT;
            pm->start = G32(p, 188);
            pm->len = G32(p, 192);
            pm->access = (u8)G32(p, 184);
            if (G32(p, 56)) pm->ust_lbn = G32(p, 60);
            if (G32(p, 64)) {
                pm->ubm_len = G32(p, 64) & 0x3FFFFFFFUL;
                pm->ubm_lbn = G32(p, 68);
            }
        }
        /* maps on top of a physical one */
        for (i = 0; i < (int)nmaps; i++) {
            struct udf_pmap *pm = &v->pm[i];
            if (pm->type == PM_VIRT || pm->type == PM_META) {
                for (k = 0; k < (int)nmaps; k++)
                    if (k != i && v->pm[k].num == pm->num && (v->pm[k].type == PM_PHYS || v->pm[k].type == PM_SPAR))
                        break;
                if (k == (int)nmaps) return UE_CORRUPT;
                pm->phys = (u8)k;
            }
        }
        for (i = 0; i < (int)nmaps; i++) {
            struct udf_pmap *pm = &v->pm[i];
            if (pm->type == PM_SPAR) {
                u32 locs[4];
                for (k = 0; k < 4; k++) locs[k] = pm->st_abs[k];
                load_sparing(v, pm, locs);
            }
        }
        for (i = 0; i < (int)nmaps; i++) {
            if (v->pm[i].type == PM_VIRT) {
                r = find_vat(v, &v->pm[i]);
                if (r) return r;
                v->flags |= VF_VAT | VF_RDONLY;
            }
        }
        for (i = 0; i < (int)nmaps; i++) {
            if (v->pm[i].type == PM_META) {
                if (v->metapm != 0xFF) return UE_UNSUPP;
                v->metapm = (u8)i;
                r = load_meta(v, &v->pm[i]);
                if (r) return r;
            }
        }
        if (iseq_len && !(v->flags & VF_VAT)) read_lvid(v, iseq_loc, iseq_len);
        else if (iseq_len) {
            u32 nf = v->nfiles, nd = v->ndirs;
            u16 a = v->minrd, b = v->minwr, c = v->maxwr;
            read_lvid(v, iseq_loc, iseq_len);
            if (v->rev >= 0x200) {
                v->nfiles = nf;
                v->ndirs = nd;
                v->minrd = a;
                v->minwr = b;
                v->maxwr = c;
            }
        }
        if (v->minrd > UDF_MAXREV) return UE_UNSUPP;

        /* file set descriptor */
        r = udf_map(v, fsd.part, fsd.lbn, &abs);
        if (r) return r;
        p = udf_bget(v, abs);
        if (!p) return UE_IO;
        if (!udf_tagok(p, 256, 0xFFFFFFFFUL, v->bs)) return UE_CORRUPT;
        v->root.lbn = G32(p, 404);
        v->root.part = G16(p, 408);
        if (v->rev >= 0x200 && (G32(p, 464) & 0x3FFFFFFFUL)) {
            v->sroot.lbn = G32(p, 468);
            v->sroot.part = G16(p, 472);
        }
        if (v->label[0] == 0) label_set(v, p + 112, 128);
        v->serial = ((u32)G16(p, 8) << 16) | udf_crc(p + 400, 16, (u16)G16(p, 8));
    }

    /* where new file entries and data go */
    v->icbpm = v->datapm = 0;
    for (i = 0; i < v->npm; i++)
        if (v->pm[i].type == PM_PHYS || v->pm[i].type == PM_SPAR) {
            v->datapm = v->icbpm = (u8)i;
            break;
        }
    if (v->metapm != 0xFF) {
        v->icbpm = v->metapm;
        v->datapm = v->pm[v->metapm].phys;
        v->flags |= VF_ICBLONG;
    }
    if (v->rev >= 0x250) v->flags |= VF_EFE;
    if (!d->write || d->ro || v->maxwr > UDF_MAXREV || v->minwr > UDF_MAXREV || (v->flags & VF_WASOPEN))
        v->flags |= VF_RDONLY;
    for (i = 0; i < v->npm; i++) {
        struct udf_pmap *pm = &v->pm[i];
        if (pm->type == PM_VIRT) continue;
        if (pm->access == 1 || pm->access == 2) v->flags |= VF_RDONLY;
    }
    {
        struct udf_pmap *pm = &v->pm[v->datapm];
        if (!pm->ubm_len) v->flags |= VF_RDONLY;
    }
    if (v->metapm != 0xFF && v->sys[2].ftype != FT_MBITMAP) v->flags |= VF_RDONLY;
    return 0;
}
