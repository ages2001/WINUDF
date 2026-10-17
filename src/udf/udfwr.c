/* udfwr.c - UDF file system core, writing: space bitmaps of physical, sparable and metadata
   partitions, file entries, allocation descriptors (with allocation extent descriptors),
   data embedded in the ICB, directories, link counts and unique IDs, and the logical volume
   integrity descriptor (open while changes are made, closed when nothing is open). Volumes
   on write-once media (VAT) and volumes that were not closed are read-only. */
#include "udf.h"

u8 udf_osclass = 1;

#ifdef UDF_DOS16
#define MAXEXT          96      /* extents of a file whose descriptors can be rewritten */
#else
#define MAXEXT          160
#endif
#define MAXAED          24

struct uext {
    u32 len;                    /* bytes */
    u32 lbn;
    u16 part;
    u8 type;                    /* 0 recorded, 1 allocated, 2 not allocated */
};

static struct uext ext[MAXEXT];
static u16 next_n;
static struct udf_loc aed[MAXAED];
static u16 naed;
static u8 fidbuf[520];
static u8 fidtmp[520];
#define sf1 udf_fidw[0]           /* directory entries of the operations (not on the stack) */
#define sf2 udf_fidw[1]

#define BS(v)           ((u32)(v)->bs)
#define BLKS(v, n)      (((n) + BS(v) - 1) >> (v)->bsh)

static u16 descver(struct udf_vol *v)
{
    return (u16)(v->rev >= 0x200 ? 3 : 2);
}

int udf_writable(struct udf_vol *v)
{
    return !(v->flags & VF_RDONLY) && !v->dev->ro && v->dev->write;
}

/* ------------------------------------------------------------------ partition blocks */

static u8 FAR *pget(struct udf_vol *v, u16 part, u32 lbn, u32 *abs)
{
    if (udf_map(v, part, lbn, abs)) return 0;
    return udf_bget(v, *abs);
}

/* write a block of partition part; the metadata mirror gets it too */
static int pput(struct udf_vol *v, u16 part, u32 lbn, u32 abs, u8 FAR *data)
{
    int r;
    u32 a2 = 0, run;
    if (part == v->metapm && v->pm[part].dup && v->sys[1].ftype == FT_MMIRROR) {
        udf_pin(data);
        if (udf_bmap(v, &v->sys[1], lbn << v->bsh, &a2, &run)) a2 = 0;
        udf_unpin();
    }
    r = udf_bput(v, abs, data);
    if (!r && a2 && a2 != abs) r = udf_bput(v, a2, data);
    return r;
}

static void regid_set(u8 FAR *p, const char *id, u8 b0, u8 b1)
{
    int i;
    udf_fset(p, 0, 32);
    for (i = 0; id[i] && i < 23; i++) p[1 + i] = (u8)id[i];
    p[24] = b0;
    p[25] = b1;
}

static void implid(u8 FAR *p)
{
    regid_set(p, "*WINDRV UDF", udf_osclass, 0);
}

/* ------------------------------------------------------------------ space bitmaps */

/* block of byte off of the space bitmap descriptor of map pmi */
static u8 FAR *bm_get(struct udf_vol *v, u8 pmi, u32 off, u32 *abs, u32 *lbn, u16 *part)
{
    struct udf_pmap *pm = &v->pm[pmi];
    if (pm->type == PM_META) {
        struct udf_node *b = &v->sys[2];
        u32 run;
        if (b->ftype != FT_MBITMAP || udf_bmap(v, b, off, abs, &run) || !*abs) return 0;
        *part = b->c_part;
        *lbn = b->c_lbn + ((off - b->c_off) >> v->bsh);
        return udf_bget(v, *abs);
    }
    if (!pm->ubm_len) return 0;
    *part = pmi;
    *lbn = pm->ubm_lbn + (off >> v->bsh);
    return pget(v, pmi, *lbn, abs);
}

static u32 bm_bits(struct udf_vol *v, u8 pmi)
{
    struct udf_pmap *pm = &v->pm[pmi];
    if (!pm->ubm_bits) {
        u32 abs, lbn;
        u16 part;
        u8 FAR *p = bm_get(v, pmi, 0, &abs, &lbn, &part);
        if (p && udf_tagok(p, 264, 0xFFFFFFFFUL, 0)) pm->ubm_bits = G32(p, 16);
    }
    return pm->ubm_bits;
}

/* free (val 1) or take (val 0) n blocks from lbn in map pmi */
static int bm_set(struct udf_vol *v, u8 pmi, u32 lbn, u32 n, int val)
{
    struct udf_pmap *pm = &v->pm[pmi];
    u32 nbits = bm_bits(v, pmi);
    while (n) {
        u32 off = 24 + (lbn >> 3), abs, blbn;
        u16 part, o;
        u8 FAR *p = bm_get(v, pmi, off, &abs, &blbn, &part);
        int r;
        if (!p || lbn + n > nbits) return UE_CORRUPT;
        o = (u16)(off & (BS(v) - 1));
        while (n && o < v->bs) {
            u8 m = (u8)(1 << (lbn & 7));
            if (val) {
                if (!(p[o] & m) && pm->free != 0xFFFFFFFFUL) pm->free++;
                p[o] |= m;
            } else {
                if ((p[o] & m) && pm->free != 0xFFFFFFFFUL && pm->free) pm->free--;
                p[o] &= (u8)~m;
            }
            lbn++;
            n--;
            if (!(lbn & 7)) o++;
        }
        r = pput(v, part, blbn, abs, p);
        if (r) return r;
    }
    return 0;
}

/* up to want free blocks in a row in map pmi, near goal: *lbn, *got (at least 1) */
static int balloc(struct udf_vol *v, u8 pmi, u32 goal, u32 want, u32 *lbn, u32 *got)
{
    struct udf_pmap *pm = &v->pm[pmi];
    u32 nbits = bm_bits(v, pmi), i, s, run;
    int pass;
    if (!nbits) return UE_RDONLY;
    if (goal >= nbits) goal = pm->hint < nbits ? pm->hint : 0;
    for (pass = 0; pass < 2; pass++) {
        u32 end = pass ? goal : nbits;
        i = pass ? 0 : goal;
        while (i < end) {
            u32 off = 24 + (i >> 3), abs, blbn;
            u16 part, o;
            u8 FAR *p = bm_get(v, pmi, off, &abs, &blbn, &part);
            if (!p) return UE_CORRUPT;
            o = (u16)(off & (BS(v) - 1));
            while (o < v->bs && i < end) {
                u8 b = (u8)(p[o] >> (i & 7));
                if (!b) {
                    i = (i | 7) + 1;
                    o++;
                    continue;
                }
                if (b & 1) goto found;
                i++;
                if (!(i & 7)) o++;
            }
        }
    }
    return UE_NOSPACE;
found:
    s = i;
    run = 1;
    while (run < want && s + run < nbits) {
        u32 off = 24 + ((s + run) >> 3), abs, blbn;
        u16 part;
        u8 FAR *p = bm_get(v, pmi, off, &abs, &blbn, &part);
        if (!p) break;
        if (!((p[off & (BS(v) - 1)] >> ((s + run) & 7)) & 1)) break;
        run++;
    }
    if (bm_set(v, pmi, s, run, 0)) return UE_IO;
    pm->hint = s + run;
    *lbn = s;
    *got = run;
    return 0;
}

static int bfree(struct udf_vol *v, u16 part, u32 lbn, u32 n)
{
    if (part >= v->npm || !n) return 0;
    if (v->pm[part].type == PM_VIRT) return UE_RDONLY;
    return bm_set(v, (u8)part, lbn, n, 1);
}

/* one block for a file entry or a directory in map pmi */
static int balloc1(struct udf_vol *v, u8 pmi, u32 goal, u32 *lbn)
{
    u32 got;
    return balloc(v, pmi, goal, 1, lbn, &got);
}

u32 udf_freeblocks(struct udf_vol *v)
{
    u32 f = v->pm[v->datapm].free;
    if (f == 0xFFFFFFFFUL || (v->flags & VF_RDONLY)) return 0;
    return f;
}

u32 udf_totalblocks(struct udf_vol *v)
{
    struct udf_pmap *pm = &v->pm[v->datapm];
    if (v->flags & VF_VAT) return v->nblocks;
    return pm->len;
}

/* ------------------------------------------------------------------ integrity */

static int lvid_write(struct udf_vol *v, int closed)
{
    u8 FAR *p;
    u32 np, liu, k;
    u16 o;
    struct udf_time t;
    if (!v->lvid_abs) return 0;
    p = udf_bget(v, v->lvid_abs);
    if (!p) return UE_IO;
    np = G32(p, 72);
    liu = G32(p, 76);
    if (80 + np * 8 + liu > v->bs) return UE_CORRUPT;
    udf_now(&t);
    udf_timeset(p + 16, &t);
    udf_p32(p + 28, closed ? 1 : 0);
    udf_p64(p + 40, v->uniq);
    for (k = 0; k < np && k < v->npm; k++) {
        if (v->pm[k].type == PM_VIRT) continue;
        udf_p32(p + 80 + k * 4, v->pm[k].free);
    }
    if (liu >= 46) {
        o = (u16)(80 + np * 8);
        implid(p + o);
        udf_p32(p + o + 32, v->nfiles);
        udf_p32(p + o + 36, v->ndirs);
        if (v->maxwr < v->rev) v->maxwr = v->rev;
        udf_p16(p + o + 44, v->maxwr);
    }
    udf_tagset(p, 9, G16(p, 2), v->lvid_abs, (u16)(80 + np * 8 + liu));
    return udf_bput(v, v->lvid_abs, p);
}

int udf_begin(struct udf_vol *v)
{
    int r;
    if (!udf_writable(v)) return UE_RDONLY;
    if (v->dev->gen != v->gen) return UE_CHANGED;
    if (v->flags & VF_DIRTY) return 0;
    r = lvid_write(v, 0);
    if (!r) v->flags |= VF_DIRTY;
    return r;
}

int udf_sync(struct udf_vol *v)
{
    int r;
    if (!(v->flags & VF_DIRTY) || v->nopen) return 0;
    r = lvid_write(v, 1);
    if (!r) v->flags &= ~VF_DIRTY;
    return r;
}

static u64 newuniq(struct udf_vol *v)
{
    u64 u = v->uniq;
    if ((u32)u < 16) u = (u & ~(u64)0xFFFFFFFFUL) | 16;
    v->uniq = u + 1;
    if ((u32)v->uniq < 16) v->uniq |= 16;
    return u;
}

/* ------------------------------------------------------------------ file entries */

/* write the file entry from the node: link count, permissions, ICB flags, sizes, times,
   descriptor area length. ads != NULL: new descriptors (or embedded data) of n->lad bytes */
static int fe_update(struct udf_vol *v, struct udf_node *n, const u8 FAR *ads)
{
    u8 FAR *p = udf_bget(v, n->feabs);
    u16 hdr = FEHDR(n), id;
    int r;
    if (!p) return UE_IO;
    id = G16(p, 0);
    if ((u32)hdr + n->lea + n->lad > v->bs) return UE_CORRUPT;
    udf_p16(p + 34, (u16)((n->icbflags & ~7) | n->adtype));
    udf_p32(p + 44, n->perm);
    udf_p16(p + 48, n->links);
    udf_p64(p + 56, n->size);
    if (n->efe) {
        udf_p64(p + 64, n->size);
        udf_p64(p + 72, n->blocks);
        if (n->tset) {
            udf_timeset(p + 80, &n->atime);
            udf_timeset(p + 92, &n->mtime);
        }
        udf_p32(p + 212, n->lad);
    } else {
        udf_p64(p + 64, n->blocks);
        if (n->tset) {
            udf_timeset(p + 72, &n->atime);
            udf_timeset(p + 84, &n->mtime);
        }
        udf_p32(p + 172, n->lad);
    }
    if (ads) udf_fcopy(p + hdr + n->lea, ads, (u16)n->lad);
    implid(p + (n->efe ? 168 : 128));
    udf_tagset(p, id, G16(p, 2), n->loc.lbn, (u16)(hdr + n->lea + n->lad));
    r = pput(v, n->loc.part, n->loc.lbn, n->feabs, p);
    n->tset = 0;
    n->c_ok = 0;
    return r;
}

/* ------------------------------------------------------------------ extents */

static int ext_load(struct udf_vol *v, struct udf_node *n)
{
    u32 blk = n->feabs;
    u16 pos = (u16)(FEHDR(n) + n->lea), end = (u16)(pos + n->lad), sz = udf_adsize(n->adtype);
    next_n = naed = 0;
    if (n->adtype == AD_ICB) return 0;
    for (;;) {
        u8 FAR *p;
        u32 len, lbn;
        u16 part;
        u8 type;
        if ((u32)pos + sz > end) return 0;
        p = udf_bget(v, blk);
        if (!p) return UE_IO;
        p += pos;
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
            u8 FAR *q;
            if (naed == MAXAED) return UE_UNSUPP;
            aed[naed].lbn = lbn;
            aed[naed].part = part;
            naed++;
            q = pget(v, part, lbn, &blk);
            if (!q) return UE_IO;
            if (!udf_tagok(q, 258, 0xFFFFFFFFUL, v->bs) || G32(q, 20) + 24 > v->bs) return UE_CORRUPT;
            pos = 24;
            end = (u16)(24 + G32(q, 20));
            continue;
        }
        if (next_n == MAXEXT) return UE_UNSUPP;
        ext[next_n].len = len;
        ext[next_n].lbn = lbn;
        ext[next_n].part = part;
        ext[next_n].type = type;
        next_n++;
        pos = (u16)(pos + sz);
    }
}

static void ad_put(u8 FAR *p, u8 adtype, u32 len, u8 type, u32 lbn, u16 part)
{
    udf_p32(p, len | ((u32)type << 30));
    udf_p32(p + 4, lbn);
    if (adtype == AD_LONG) {
        udf_p16(p + 8, part);
        udf_fset(p + 10, 0, 6);
    }
}

/* write the extent list back: descriptors in the file entry, the rest in allocation
   extent descriptors (reused, more allocated, the ones left over freed) */
static int ext_store(struct udf_vol *v, struct udf_node *n)
{
    u8 t = n->adtype == AD_EXT ? AD_LONG : n->adtype;
    u16 sz = udf_adsize(t), capfe, capaed, i, k, need, inaed;
    u16 hdr = FEHDR(n);
    u64 blocks = 0;
    int r;
    u8 FAR *p;
    u32 abs;
    n->adtype = t;
    for (i = 0; i < next_n; i++)
        if (ext[i].type != 2) blocks += BLKS(v, ext[i].len);
    n->blocks = blocks;
    capfe = (u16)((v->bs - hdr - n->lea) / sz);
    capaed = (u16)((v->bs - 24) / sz);
    if (capfe < 2 || capaed < 3) return UE_UNSUPP;
    /* descriptor blocks: the file entry holds capfe (one of them a pointer when more
       follow), each extent descriptor capaed */
    need = 0;
    if (next_n > capfe) {
        u16 left = (u16)(next_n - (capfe - 1));
        while (left > capaed) {
            need++;
            left = (u16)(left - (capaed - 1));
        }
        need++;
    }
    while (naed < need) {
        u32 lbn;
        if (naed == MAXAED) return UE_UNSUPP;
        r = balloc1(v, (u8)n->loc.part, n->loc.lbn, &lbn);
        if (r) return r;
        aed[naed].lbn = lbn;
        aed[naed].part = n->loc.part;
        naed++;
    }
    while (naed > need) {
        naed--;
        bfree(v, aed[naed].part, aed[naed].lbn, 1);
    }
    /* extent descriptors, last first */
    k = need ? (u16)(capfe - 1) : next_n;
    for (i = 0; i < need; i++) {
        u16 first = (u16)(capfe - 1 + i * (capaed - 1));
        u16 cnt = (u16)(next_n - first), j, len;
        u32 prev = i ? aed[i - 1].lbn : 0;
        if (i + 1 < need) cnt = (u16)(capaed - 1);
        if (udf_map(v, aed[i].part, aed[i].lbn, &abs)) return UE_CORRUPT;
        p = udf_bnew(v, abs);
        udf_fset(p, 0, v->bs);
        for (j = 0; j < cnt; j++)
            ad_put(p + 24 + j * sz, t, ext[first + j].len, ext[first + j].type, ext[first + j].lbn, ext[first + j].part);
        len = (u16)(cnt * sz);
        if (i + 1 < need) {
            ad_put(p + 24 + len, t, BS(v), 3, aed[i + 1].lbn, aed[i + 1].part);
            len = (u16)(len + sz);
        }
        udf_p32(p + 16, prev);
        udf_p32(p + 20, len);
        udf_tagset(p, 258, descver(v), aed[i].lbn, (u16)(v->rev >= 0x201 ? 24 + len : 24));
        r = pput(v, aed[i].part, aed[i].lbn, abs, p);
        if (r) return r;
    }
    inaed = 0;
    (void)inaed;
    /* the file entry */
    p = udf_bget(v, n->feabs);
    if (!p) return UE_IO;
    udf_pin(p);
    {
        u8 FAR *a = p + hdr + n->lea;
        u16 len = 0;
        udf_fset(a, 0, (u16)(v->bs - hdr - n->lea));
        for (i = 0; i < k; i++) {
            ad_put(a + len, t, ext[i].len, ext[i].type, ext[i].lbn, ext[i].part);
            len = (u16)(len + sz);
        }
        if (need) {
            ad_put(a + len, t, BS(v), 3, aed[0].lbn, aed[0].part);
            len = (u16)(len + sz);
        }
        n->lad = len;
    }
    udf_unpin();
    return fe_update(v, n, 0);
}

static u8 data_pm(struct udf_vol *v, struct udf_node *n)
{
    if (n->adtype == AD_SHORT) return (u8)n->loc.part;
    return (n->ftype == FT_DIR || n->ftype == FT_STREAMDIR) ? v->icbpm : v->datapm;
}

/* zeros to blocks lbn.. of map part */
static int zero_blocks(struct udf_vol *v, u16 part, u32 lbn, u32 n)
{
    while (n--) {
        u32 abs;
        u8 FAR *p;
        int r;
        if (udf_map(v, part, lbn, &abs)) return UE_CORRUPT;
        p = udf_bnew(v, abs);
        udf_fset(p, 0, v->bs);
        r = pput(v, part, lbn, abs, p);
        if (r) return r;
        lbn++;
    }
    return 0;
}

/* allocate nb blocks at the end of the extent list (merged with the last extent when they
   follow it) */
static int ext_grow(struct udf_vol *v, struct udf_node *n, u32 nb)
{
    u8 pmi = data_pm(v, n);
    while (nb) {
        u32 goal = 0, lbn, got;
        struct uext *l = next_n ? &ext[next_n - 1] : 0;
        int r;
        if (l && l->part == pmi && l->type == 0) goal = l->lbn + BLKS(v, l->len);
        else if (!l && n->loc.part == pmi) goal = n->loc.lbn + 1;
        r = balloc(v, pmi, goal, nb, &lbn, &got);
        if (r) return r;
        while (got) {
            u32 k = got;
            l = next_n ? &ext[next_n - 1] : 0;
            if (l && l->type == 0 && l->part == pmi && l->lbn + BLKS(v, l->len) == lbn &&
                !(l->len & (BS(v) - 1)) && l->len + (k << v->bsh) <= 0x3FFFFFFFUL - BS(v)) {
                l->len += k << v->bsh;
            } else {
                u32 maxb = (0x3FFFFFFFUL - BS(v)) >> v->bsh;
                if (next_n == MAXEXT) {
                    bfree(v, pmi, lbn, got);
                    return UE_UNSUPP;
                }
                if (k > maxb) k = maxb;
                ext[next_n].len = k << v->bsh;
                ext[next_n].lbn = lbn;
                ext[next_n].part = pmi;
                ext[next_n].type = 0;
                next_n++;
            }
            lbn += k;
            got -= k;
            nb -= k;
        }
    }
    return 0;
}

/* bytes the extents describe, rounded up to whole blocks */
static u32 ext_alloc_end(struct udf_vol *v)
{
    u32 e = 0;
    u16 i;
    for (i = 0; i < next_n; i++) e += i + 1 < next_n ? ext[i].len : BLKS(v, ext[i].len) << v->bsh;
    return e;
}

/* the extent list covers exactly size bytes (more blocks allocated as needed) */
static int ext_setsize(struct udf_vol *v, struct udf_node *n, u32 size)
{
    u32 have;
    int r;
    /* the last extent first takes the rest of its last block */
    if (next_n) ext[next_n - 1].len = BLKS(v, ext[next_n - 1].len) << v->bsh;
    have = ext_alloc_end(v);
    if (size > have) {
        r = ext_grow(v, n, BLKS(v, size - have));
        if (r) return r;
    }
    /* the last extent ends at size */
    {
        u32 e = 0;
        u16 i;
        for (i = 0; i < next_n; i++) {
            if (e + ext[i].len >= size || i + 1 == next_n) {
                ext[i].len = size - e;
                if (!ext[i].len) i--;
                next_n = (u16)(i + 1);
                break;
            }
            e += ext[i].len;
        }
    }
    return 0;
}

/* extents that are not recorded (holes) in [off, end) get blocks and zeros */
static int ext_fill(struct udf_vol *v, struct udf_node *n, u32 off, u32 end)
{
    u32 e = 0;
    u16 i;
    int r;
    for (i = 0; i < next_n; i++) {
        struct uext *x = &ext[i];
        if (e < end && e + x->len > off && x->type != 0) {
            if (x->type == 2) {
                /* not allocated: blocks for the whole extent, as separate extents */
                u32 nb = BLKS(v, x->len), len = x->len;
                u16 tail = (u16)(next_n - i - 1), j;
                static struct uext save[8];
                u8 pmi = data_pm(v, n);
                if (tail > 8) return UE_UNSUPP;
                for (j = 0; j < tail; j++) save[j] = ext[i + 1 + j];
                next_n = i;
                while (nb) {
                    u32 lbn, got, gl;
                    r = balloc(v, pmi, 0, nb, &lbn, &got);
                    if (r) return r;
                    r = zero_blocks(v, pmi, lbn, got);
                    if (r) return r;
                    if (next_n == MAXEXT) return UE_UNSUPP;
                    gl = got << v->bsh;
                    if (gl > len) gl = len;
                    ext[next_n].len = gl;
                    ext[next_n].lbn = lbn;
                    ext[next_n].part = pmi;
                    ext[next_n].type = 0;
                    next_n++;
                    len -= gl;
                    nb -= got;
                }
                if (next_n + tail > MAXEXT) return UE_UNSUPP;
                for (j = 0; j < tail; j++) ext[next_n++] = save[j];
                return ext_fill(v, n, off, end);
            }
            r = zero_blocks(v, x->part, x->lbn, BLKS(v, x->len));
            if (r) return r;
            x->type = 0;
        }
        e += x->len;
    }
    return 0;
}

/* blocks of the extents from index i on are freed and dropped */
static int ext_free_from(struct udf_vol *v, u16 i)
{
    u16 j;
    for (j = i; j < next_n; j++)
        if (ext[j].type != 2) bfree(v, ext[j].part, ext[j].lbn, BLKS(v, ext[j].len));
    next_n = i;
    return 0;
}

/* ------------------------------------------------------------------ file data */

static int bmap_lbn(struct udf_vol *v, struct udf_node *n, u32 off, u32 *abs, u32 *lbn)
{
    u32 run;
    int r;
    if (n->adtype == AD_ICB) {
        *abs = n->feabs;
        *lbn = n->loc.lbn;
        return 0;
    }
    r = udf_bmap(v, n, off, abs, &run);
    if (r) return r;
    if (!*abs) return UE_CORRUPT;
    *lbn = n->c_lbn + ((off - n->c_off) >> v->bsh);
    return 0;
}

/* embedded data -> blocks */
static int icb_convert(struct udf_vol *v, struct udf_node *n)
{
    u32 size = (u32)n->size, abs;
    u8 FAR *p;
    int r;
    u8 to = (n->ftype == FT_DIR || n->ftype == FT_STREAMDIR || !(v->flags & VF_ICBLONG)) ? AD_SHORT : AD_LONG;
    if (to == AD_SHORT && n->loc.part != ((n->ftype == FT_DIR || n->ftype == FT_STREAMDIR) ? v->icbpm : v->datapm))
        to = AD_LONG;
    p = udf_bget(v, n->feabs);
    if (!p) return UE_IO;
    udf_fcopy(udf_tmp, p + FEHDR(n) + n->lea, (u16)n->lad);
    n->adtype = to;
    next_n = naed = 0;
    if (size) {
        r = ext_setsize(v, n, size);
        if (r) return r;
        /* the first extent holds the data (size <= one block) */
        if (udf_map(v, ext[0].part, ext[0].lbn, &abs)) return UE_CORRUPT;
        p = udf_bnew(v, abs);
        udf_fset(p, 0, v->bs);
        udf_fcopy(p, udf_tmp, (u16)size);
        r = pput(v, ext[0].part, ext[0].lbn, abs, p);
        if (r) return r;
    }
    n->lad = 0;
    r = ext_store(v, n);
    if (r) return r;
    if (n->ftype == FT_DIR || n->ftype == FT_STREAMDIR) {
        /* the tag locations of the file identifiers change with the move */
        u32 pos = 0;
        u8 h[38];
        while ((u64)pos + 38 <= n->size) {
            u16 tot, clen;
            u32 l2;
            if (udf_read(v, n, pos, 38, h) != 38) return UE_IO;
            tot = (u16)((38 + G16(h, 36) + h[19] + 3) & ~3);
            clen = (u16)(38 + G16(h, 36) + h[19]);
            (void)clen;
            if (udf_read(v, n, pos, tot, fidtmp) != tot) return UE_IO;
            if (bmap_lbn(v, n, pos, &abs, &l2)) return UE_CORRUPT;
            udf_tagset(fidtmp, 257, G16(fidtmp, 2), l2, tot);
            r = (int)udf_write(v, n, pos, tot, fidtmp);
            if (r != tot) return r < 0 ? r : UE_IO;
            pos += tot;
        }
    }
    return 0;
}

static s32 data_write(struct udf_vol *v, struct udf_node *n, u32 off, u32 len, const u8 FAR *buf)
{
    u32 done = 0;
    while (done < len) {
        u32 abs, run, k, in = (off + done) & (BS(v) - 1);
        int r = udf_bmap(v, n, off + done, &abs, &run);
        if (r) return r;
        if (!abs) return UE_CORRUPT;
        k = len - done;
        if (k > run) k = run;
        if (in == 0 && k >= BS(v) && n->c_part < v->npm && v->pm[n->c_part].type == PM_PHYS) {
            u32 nb = k >> v->bsh, lim = 0x8000UL >> v->bsh;
            if (nb > lim) nb = lim;
            k = nb << v->bsh;
            if (buf) {
                r = udf_devwrite(v, abs, (u16)nb, buf + done);
                if (r) return r;
                udf_cache_update(v, abs, (u16)nb, buf + done);
            } else {
                u32 i;
                for (i = 0; i < nb; i++) {
                    u8 FAR *p = udf_bnew(v, abs + i);
                    udf_fset(p, 0, v->bs);
                    r = udf_bput(v, abs + i, p);
                    if (r) return r;
                }
            }
        } else {
            u8 FAR *p;
            u32 lbn = n->c_lbn + ((off + done - n->c_off) >> v->bsh);
            u16 part = n->c_part;
            if (k > BS(v) - in) k = BS(v) - in;
            p = (in == 0 && k == BS(v)) ? udf_bnew(v, abs) : udf_bget(v, abs);
            if (!p) return UE_IO;
            if (buf) udf_fcopy(p + in, buf + done, (u16)k);
            else udf_fset(p + in, 0, (u16)k);
            r = pput(v, part, lbn, abs, p);
            if (r) return r;
        }
        done += k;
    }
    return (s32)done;
}

s32 udf_write(struct udf_vol *v, struct udf_node *n, u32 off, u32 len, const u8 FAR *buf)
{
    u32 end = off + len, oldsize;
    int r;
    s32 w;
    if (end < off) return UE_NOSPACE;
    if (n->big) return UE_UNSUPP;
    r = udf_begin(v);
    if (r) return r;
    oldsize = (u32)n->size;
    if (n->adtype == AD_ICB) {
        u32 cap = BS(v) - FEHDR(n) - n->lea;
        if (end <= cap) {
            u8 FAR *p = udf_bget(v, n->feabs);
            u16 at = (u16)(FEHDR(n) + n->lea);
            if (!p) return UE_IO;
            if (off > n->lad) udf_fset(p + at + n->lad, 0, (u16)(off - n->lad));
            if (buf) udf_fcopy(p + at + off, buf, (u16)len);
            else udf_fset(p + at + off, 0, (u16)len);
            if (end > n->lad) n->lad = end;
            n->size = n->lad;
            if (n->ftype == FT_FILE) {
                udf_now(&n->mtime);
                n->atime = n->mtime;
                n->tset = 1;
            }
            r = fe_update(v, n, 0);
            return r ? r : (s32)len;
        }
        r = icb_convert(v, n);
        if (r) return r;
    }
    if (end > oldsize || off > oldsize) {
        r = ext_load(v, n);
        if (r) return r;
        r = ext_fill(v, n, off < oldsize ? off : oldsize, oldsize);
        if (r) return r;
        r = ext_setsize(v, n, end);
        if (r) return r;
        n->size = end;
        r = ext_store(v, n);
        if (r) return r;
        if (off > oldsize) {
            w = data_write(v, n, oldsize, off - oldsize, 0);
            if (w < 0) return w;
        }
    } else {
        /* holes in the range? */
        u32 o = off;
        while (o < end) {
            u32 abs, run;
            r = udf_bmap(v, n, o, &abs, &run);
            if (r) return r;
            if (!abs) {
                r = ext_load(v, n);
                if (r) return r;
                r = ext_fill(v, n, off, end);
                if (r) return r;
                r = ext_store(v, n);
                if (r) return r;
                break;
            }
            o += run;
        }
    }
    w = data_write(v, n, off, len, buf);
    if (w < 0) return w;
    if (n->ftype == FT_FILE) {
        udf_now(&n->mtime);
        n->atime = n->mtime;
        n->tset = 1;
        r = fe_update(v, n, 0);
        if (r) return r;
    }
    return w;
}

int udf_truncate(struct udf_vol *v, struct udf_node *n, u32 size)
{
    int r;
    if (n->big) return UE_UNSUPP;
    if (size > n->size) {
        s32 w = udf_write(v, n, size, 0, 0);
        return w < 0 ? (int)w : 0;
    }
    r = udf_begin(v);
    if (r) return r;
    if (n->adtype == AD_ICB) {
        n->lad = size;
        n->size = size;
    } else {
        r = ext_load(v, n);
        if (r) return r;
        {
            u32 e = 0;
            u16 i;
            for (i = 0; i < next_n; i++) {
                u32 keep;
                if (e + ext[i].len < size) {
                    e += ext[i].len;
                    continue;
                }
                keep = size - e;
                if (ext[i].type != 2 && BLKS(v, ext[i].len) > BLKS(v, keep))
                    bfree(v, ext[i].part, ext[i].lbn + BLKS(v, keep), BLKS(v, ext[i].len) - BLKS(v, keep));
                ext[i].len = keep;
                ext_free_from(v, (u16)(keep ? i + 1 : i));
                break;
            }
        }
        n->size = size;
        r = ext_store(v, n);
        if (r) return r;
    }
    udf_now(&n->mtime);
    n->atime = n->mtime;
    n->tset = 1;
    return fe_update(v, n, 0);
}

/* ------------------------------------------------------------------ freeing files */

/* blocks of the file body and its descriptor blocks */
static int free_body(struct udf_vol *v, struct udf_node *n)
{
    u32 blk = n->feabs;
    u16 pos = (u16)(FEHDR(n) + n->lea), end = (u16)(pos + n->lad), sz = udf_adsize(n->adtype);
    int hops = 0;
    if (n->adtype == AD_ICB) return 0;
    for (;;) {
        u8 FAR *p;
        u32 len, lbn;
        u16 part;
        u8 type;
        if ((u32)pos + sz > end) return 0;
        p = udf_bget(v, blk);
        if (!p) return UE_IO;
        p += pos;
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
        pos = (u16)(pos + sz);
        if (type == 3) {
            u8 FAR *q;
            if (++hops > 1000) return UE_CORRUPT;
            q = pget(v, part, lbn, &blk);
            if (!q || !udf_tagok(q, 258, 0xFFFFFFFFUL, v->bs)) return UE_CORRUPT;
            pos = 24;
            end = (u16)(24 + G32(q, 20));
            bfree(v, part, lbn, 1);
            continue;
        }
        if (type != 2) bfree(v, part, lbn, BLKS(v, len));
    }
}

static int free_node(struct udf_vol *v, struct udf_node *n, int depth)
{
    int r;
    u8 FAR *p;
    /* extended attributes in an ICB of their own */
    p = udf_bget(v, n->feabs);
    if (!p) return UE_IO;
    if (G32(p, n->efe ? 136 : 112) & 0x3FFFFFFFUL) {
        struct udf_loc l;
        struct udf_node ea;
        l.lbn = G32(p, n->efe ? 140 : 116);
        l.part = G16(p, n->efe ? 144 : 120);
        if (udf_iget(v, l, &ea) == 0) {
            free_body(v, &ea);
            bfree(v, l.part, l.lbn, 1);
        }
    }
    /* named streams */
    if (n->stream.lbn != 0xFFFFFFFFUL && depth == 0) {
        struct udf_node sd, s;
        u32 pos = 0;
        if (udf_iget(v, n->stream, &sd) == 0) {
            while (udf_readdir(v, &sd, &pos, &sf2) > 0) {
                if (sf2.fchar & (FC_PARENT | FC_DELETED)) continue;
                if (udf_iget(v, sf2.icb, &s) == 0) free_node(v, &s, 1);
            }
            free_node(v, &sd, 1);
        }
    }
    r = free_body(v, n);
    if (r) return r;
    return bfree(v, n->loc.part, n->loc.lbn, 1);
}

/* ------------------------------------------------------------------ directories */

static u16 fid_build(struct udf_vol *v, u8 fchar, const u16 *name, u16 nlen, struct udf_loc icb, u64 uniq)
{
    u16 lfi = 0, i, clen;
    int wide = 0;
    udf_fset(fidbuf, 0, sizeof(fidbuf));
    for (i = 0; i < nlen; i++)
        if (name[i] > 0xFF) wide = 1;
    if (nlen) {
        lfi = (u16)(1 + (wide ? 2 * nlen : nlen));
        if (lfi > 255) return 0;
        fidbuf[38] = (u8)(wide ? 16 : 8);
        for (i = 0; i < nlen; i++) {
            if (wide) {
                fidbuf[39 + 2 * i] = (u8)(name[i] >> 8);
                fidbuf[40 + 2 * i] = (u8)name[i];
            } else {
                fidbuf[39 + i] = (u8)name[i];
            }
        }
    }
    udf_p16(fidbuf + 16, 1);
    fidbuf[18] = fchar;
    fidbuf[19] = (u8)lfi;
    udf_p32(fidbuf + 20, BS(v));
    udf_p32(fidbuf + 24, icb.lbn);
    udf_p16(fidbuf + 28, icb.part);
    if (v->rev >= 0x200) udf_p32(fidbuf + 32, (u32)uniq);
    clen = (u16)(38 + lfi);
    udf_p16(fidbuf + 2, descver(v));
    return clen;
}

/* write the identifier in fidbuf (clen bytes) at pos of dir, tag location filled in */
static int fid_put(struct udf_vol *v, struct udf_node *dir, u32 pos, u16 clen)
{
    u32 abs, lbn;
    s32 w;
    u16 tot = (u16)((clen + 3) & ~3);
    int r;
    /* first written to make the room, then with the tag of its block; the CRC covers
       the padding too */
    udf_tagset(fidbuf, 257, descver(v), 0, tot);
    w = udf_write(v, dir, pos, tot, fidbuf);
    if (w != tot) return w < 0 ? (int)w : UE_IO;
    r = bmap_lbn(v, dir, pos, &abs, &lbn);
    if (r) return r;
    udf_tagset(fidbuf, 257, descver(v), lbn, tot);
    w = udf_write(v, dir, pos, tot, fidbuf);
    if (w != tot) return w < 0 ? (int)w : UE_IO;
    return 0;
}

static int dir_touch(struct udf_vol *v, struct udf_node *dir)
{
    udf_now(&dir->mtime);
    dir->atime = dir->mtime;
    dir->tset = 1;
    return fe_update(v, dir, 0);
}

/* a new identifier in dir: a deleted one of the same size is reused */
static int dir_add(struct udf_vol *v, struct udf_node *dir, u8 fchar, const u16 *name, u16 nlen,
                   struct udf_loc icb, u64 uniq, u32 *fidpos)
{
    u32 pos = 0, at = 0xFFFFFFFFUL;
    u16 clen, tot;
    int r;
    clen = fid_build(v, 0, name, nlen, icb, 0);
    if (!clen) return UE_BADNAME;
    tot = (u16)((clen + 3) & ~3);
    while ((r = udf_readdir(v, dir, &pos, &sf2)) > 0)
        if ((sf2.fchar & FC_DELETED) && sf2.len == tot && at == 0xFFFFFFFFUL) at = sf2.pos;
    if (r < 0) return r;
    if (at == 0xFFFFFFFFUL) at = (u32)dir->size;
    clen = fid_build(v, fchar, name, nlen, icb, uniq);
    r = fid_put(v, dir, at, clen);
    if (r) return r;
    if (fidpos) *fidpos = at;
    return dir_touch(v, dir);
}

/* the identifier at pos of dir: read into fidbuf, its length */
static int fid_get(struct udf_vol *v, struct udf_node *dir, u32 pos, u16 *clen)
{
    s32 r = udf_read(v, dir, pos, 38, fidbuf);
    if (r != 38) return r < 0 ? (int)r : UE_CORRUPT;
    if (!udf_tagok(fidbuf, 257, 0xFFFFFFFFUL, 0)) return UE_CORRUPT;
    *clen = (u16)((38 + G16(fidbuf, 36) + fidbuf[19] + 3) & ~3);
    if (*clen > sizeof(fidbuf)) return UE_CORRUPT;
    r = udf_read(v, dir, pos, *clen, fidbuf);
    if (r != *clen) return r < 0 ? (int)r : UE_CORRUPT;
    return 0;
}

static int fid_rewrite(struct udf_vol *v, struct udf_node *dir, u32 pos, u16 clen)
{
    s32 w;
    udf_tagset(fidbuf, 257, G16(fidbuf, 2), G32(fidbuf, 12), clen);
    w = udf_write(v, dir, pos, clen, fidbuf);
    return w == clen ? 0 : (w < 0 ? (int)w : UE_IO);
}

static int dir_del(struct udf_vol *v, struct udf_node *dir, u32 pos)
{
    u16 clen;
    int r = fid_get(v, dir, pos, &clen);
    if (r) return r;
    fidbuf[18] |= FC_DELETED;
    udf_fset(fidbuf + 20, 0, 16);
    r = fid_rewrite(v, dir, pos, clen);
    if (r) return r;
    return dir_touch(v, dir);
}

/* the parent identifier of directory d points to newparent */
static int dir_reparent(struct udf_vol *v, struct udf_node *d, struct udf_node *np)
{
    u32 pos = 0;
    int r;
    while ((r = udf_readdir(v, d, &pos, &sf2)) > 0) {
        u16 clen;
        if (!(sf2.fchar & FC_PARENT)) continue;
        r = fid_get(v, d, sf2.pos, &clen);
        if (r) return r;
        udf_p32(fidbuf + 24, np->loc.lbn);
        udf_p16(fidbuf + 28, np->loc.part);
        if (v->rev >= 0x200) udf_p32(fidbuf + 32, (u32)np->uniq);
        return fid_rewrite(v, d, sf2.pos, clen);
    }
    return r < 0 ? r : UE_CORRUPT;
}

/* ------------------------------------------------------------------ operations */

static int perm_for(int isdir)
{
    return isdir ? 0x7CA5 : 0x7884;
}

int udf_create(struct udf_vol *v, struct udf_node *dir, const u16 *name, u16 nlen,
               int isdir, u8 dosattr, const struct udf_time *t, struct udf_node *out,
               u32 *fidpos)
{
    u32 lbn, abs;
    u8 FAR *p;
    u16 hdr, lad = 0, id, ver = descver(v);
    u64 uniq;
    int r, efe = (v->flags & VF_EFE) != 0;
    struct udf_time now;
    if (dir->ftype != FT_DIR && dir->ftype != FT_STREAMDIR) return UE_NOTDIR;
    if (!nlen || nlen > UDF_NAMEMAX) return UE_BADNAME;
    r = udf_begin(v);
    if (r) return r;
    r = udf_lookup(v, dir, name, nlen, &sf1, 0);
    if (r < 0) return r;
    if (r > 0) return UE_EXISTS;
    if (!fid_build(v, 0, name, nlen, dir->loc, 0)) return UE_BADNAME;
    if (!t) {
        udf_now(&now);
        t = &now;
    }
    r = balloc1(v, v->icbpm, dir->loc.part == v->icbpm ? dir->loc.lbn : 0, &lbn);
    if (r) return r;
    if (udf_map(v, v->icbpm, lbn, &abs)) return UE_CORRUPT;
    uniq = newuniq(v);
    p = udf_bnew(v, abs);
    udf_pin(p);
    udf_fset(p, 0, v->bs);
    id = (u16)(efe ? 266 : 261);
    hdr = (u16)(efe ? 216 : 176);
    /* ICB tag: strategy 4, one entry, data in the ICB */
    udf_p16(p + 20, 4);
    udf_p16(p + 24, 1);
    p[27] = (u8)(isdir ? FT_DIR : FT_FILE);
    udf_p16(p + 34, (u16)(AD_ICB | (isdir ? 0 : ICBF_ARCHIVE) | ((dosattr & 0x04) ? ICBF_SYSTEM : 0)));
    udf_p32(p + 36, 0xFFFFFFFFUL);
    udf_p32(p + 40, 0xFFFFFFFFUL);
    udf_p32(p + 44, (u32)perm_for(isdir) & ((dosattr & 0x01) ? ~(u32)(PERM_WRITE | PERM_DELETE) : 0xFFFFFFFFUL));
    udf_p16(p + 48, 1);
    if (isdir) {
        /* the parent entry */
        u8 FAR *fd = p + hdr;
        lad = 40;
        udf_p16(fd + 16, 1);
        fd[18] = FC_DIR | FC_PARENT;
        udf_p32(fd + 20, BS(v));
        udf_p32(fd + 24, dir->loc.lbn);
        udf_p16(fd + 28, dir->loc.part);
        if (v->rev >= 0x200) udf_p32(fd + 32, (u32)dir->uniq);
        udf_tagset(fd, 257, ver, lbn, 40);
    }
    udf_p64(p + 56, lad);
    if (efe) {
        udf_p64(p + 64, lad);
        udf_timeset(p + 80, t);
        udf_timeset(p + 92, t);
        udf_timeset(p + 104, t);
        udf_timeset(p + 116, t);
        udf_p32(p + 128, 1);
        implid(p + 168);
        udf_p64(p + 200, uniq);
        udf_p32(p + 212, lad);
    } else {
        udf_timeset(p + 72, t);
        udf_timeset(p + 84, t);
        udf_timeset(p + 96, t);
        udf_p32(p + 108, 1);
        implid(p + 128);
        udf_p64(p + 160, uniq);
        udf_p32(p + 172, lad);
    }
    udf_tagset(p, id, ver, lbn, (u16)(hdr + lad));
    r = pput(v, v->icbpm, lbn, abs, p);
    udf_unpin();
    if (r) return r;
    out->loc.lbn = lbn;
    out->loc.part = v->icbpm;
    out->feabs = abs;
    out->efe = (u8)efe;
    p = udf_bget(v, abs);
    if (!p) return UE_IO;
    udf_node_parse(v, out, p);
    r = dir_add(v, dir, (u8)((isdir ? FC_DIR : 0) | ((dosattr & 0x02) ? FC_HIDDEN : 0)), name, nlen,
                out->loc, uniq, fidpos);
    if (r) {
        bfree(v, out->loc.part, lbn, 1);
        return r;
    }
    if (isdir) {
        dir->links++;
        r = fe_update(v, dir, 0);
        if (r) return r;
        v->ndirs++;
    } else {
        v->nfiles++;
    }
    return 0;
}

int udf_unlink(struct udf_vol *v, struct udf_node *dir, u32 fidpos)
{
    struct udf_node n;
    u32 pos = fidpos;
    int r = udf_begin(v);
    if (r) return r;
    r = udf_readdir(v, dir, &pos, &sf1);
    if (r <= 0) return r ? r : UE_NOTFOUND;
    if (sf1.fchar & (FC_PARENT | FC_DELETED)) return UE_NOTFOUND;
    r = udf_iget(v, sf1.icb, &n);
    if (r) return r;
    if (n.ftype == FT_DIR) {
        r = udf_dirempty(v, &n);
        if (r < 0) return r;
        if (!r) return UE_NOTEMPTY;
    }
    r = dir_del(v, dir, fidpos);
    if (r) return r;
    if (n.ftype == FT_DIR) {
        r = free_node(v, &n, 0);
        if (r) return r;
        if (dir->links > 1) dir->links--;
        r = fe_update(v, dir, 0);
        if (v->ndirs) v->ndirs--;
        return r;
    }
    if (n.links > 1) {
        n.links--;
        return fe_update(v, &n, 0);
    }
    if (v->nfiles) v->nfiles--;
    return free_node(v, &n, 0);
}

int udf_rename(struct udf_vol *v, struct udf_node *sdir, u32 spos, struct udf_node *ddir,
               const u16 *name, u16 nlen, u32 dpos_replace)
{
    struct udf_node n;
    u32 pos = spos, np;
    int r, same = sdir->loc.lbn == ddir->loc.lbn && sdir->loc.part == ddir->loc.part;
    if (dpos_replace != 0xFFFFFFFFUL) return UE_EXISTS;
    r = udf_begin(v);
    if (r) return r;
    r = udf_readdir(v, sdir, &pos, &sf1);
    if (r <= 0) return r ? r : UE_NOTFOUND;
    if (sf1.fchar & (FC_PARENT | FC_DELETED)) return UE_NOTFOUND;
    r = udf_iget(v, sf1.icb, &n);
    if (r) return r;
    r = udf_lookup(v, ddir, name, nlen, &sf2, 0);
    if (r < 0) return r;
    if (r > 0) {
        /* the same entry with another case of letters: renamed in place */
        if (!(same && sf2.pos == spos)) return UE_EXISTS;
    }
    if (n.ftype == FT_DIR && !same) {
        /* not into itself or below it */
        struct udf_node c = *ddir;
        int depth;
        for (depth = 0; depth < 256; depth++) {
            u32 pp = 0;
            if (c.loc.lbn == n.loc.lbn && c.loc.part == n.loc.part) return UE_ACCESS;
            if (c.loc.lbn == v->root.lbn && c.loc.part == v->root.part) break;
            while ((r = udf_readdir(v, &c, &pp, &sf2)) > 0 && !(sf2.fchar & FC_PARENT))
                ;
            if (r <= 0) break;
            if (udf_iget(v, sf2.icb, &c)) break;
        }
    }
    r = dir_add(v, ddir, (u8)(sf1.fchar & ~FC_DELETED), name, nlen, sf1.icb, n.uniq, &np);
    if (r) return r;
    if (same) *sdir = *ddir;
    r = dir_del(v, sdir, spos);
    if (r) return r;
    if (same) *ddir = *sdir;
    if (n.ftype == FT_DIR && !same) {
        r = dir_reparent(v, &n, ddir);
        if (r) return r;
        ddir->links++;
        r = fe_update(v, ddir, 0);
        if (r) return r;
        if (sdir->links > 1) sdir->links--;
        r = fe_update(v, sdir, 0);
        if (r) return r;
    }
    return 0;
}

int udf_setattr(struct udf_vol *v, struct udf_node *dir, u32 fidpos, struct udf_node *n, u8 a)
{
    int r = udf_begin(v);
    u16 flags = n->icbflags;
    u32 perm = n->perm;
    if (r) return r;
    if (a & 0x04) flags |= ICBF_SYSTEM;
    else flags &= ~ICBF_SYSTEM;
    if (a & 0x20) flags |= ICBF_ARCHIVE;
    else flags &= ~ICBF_ARCHIVE;
    if (n->ftype != FT_DIR) {
        if (a & 0x01) perm &= ~(u32)(PERM_WRITE | PERM_DELETE);
        else if (!(perm & PERM_OWRITE)) perm |= PERM_OWRITE | 0x4000;
    }
    if (flags != n->icbflags || perm != n->perm) {
        n->icbflags = flags;
        n->perm = perm;
        r = fe_update(v, n, 0);
        if (r) return r;
    }
    if (dir && fidpos != 0xFFFFFFFFUL) {
        u16 clen;
        u8 fc;
        r = fid_get(v, dir, fidpos, &clen);
        if (r) return r;
        fc = (u8)((fidbuf[18] & ~FC_HIDDEN) | ((a & 0x02) ? FC_HIDDEN : 0));
        if (fc != fidbuf[18]) {
            fidbuf[18] = fc;
            r = fid_rewrite(v, dir, fidpos, clen);
        }
    }
    return r;
}

int udf_settime(struct udf_vol *v, struct udf_node *n, const struct udf_time *mt,
                const struct udf_time *at)
{
    int r = udf_begin(v);
    if (r) return r;
    if (mt) n->mtime = *mt;
    if (at) n->atime = *at;
    n->tset = 1;
    return fe_update(v, n, 0);
}
