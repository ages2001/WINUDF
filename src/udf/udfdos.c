/* udfdos.c - WINUDF.EXE, resident part: the DOS network redirector (INT 2Fh AH=11h) for
   UDF volumes on CD/DVD/BD drives (through MSCDEX) and on disks (BIOS INT 13h extensions,
   WINDISK / WINUSB disks through their INT 2Fh API). DOS 3.1 and later, under Windows
   1.0x-3.11 in every mode as well. udfinit.c sets it up and finds the volumes. */
#include <i86.h>
#include "udf.h"
#include "udfdos.h"

struct rregs __cdecl rr;
struct dosinfo di;
struct dvol vols[MAXVOL];
u8 nvols;
u16 oem[128];
u8 udf_rdonly_all;

static struct udf_fid fid;              /* directory entries (one at a time) */
static struct udf_node nd, pd;          /* node, parent directory */
static struct dvol *nd_vol;             /* volume of nd */
static u16 rootoff;                     /* path characters before the root backslash */
static u16 uname[UDF_NAMEMAX + 1];
static char dname[13];

/* ------------------------------------------------------------------ helpers */

#ifdef DOS_DEBUG
#include <conio.h>
static void dbgs(const char FAR *s)
{
    while (*s) outp(0xE9, *s++);
}
static void dbgx(u16 v)
{
    int i;
    for (i = 12; i >= 0; i -= 4) outp(0xE9, "0123456789ABCDEF"[(v >> i) & 15]);
}
#define DBG(x) dbgs(x)
u16 getds(void);
#pragma aux getds = "mov ax, ds" value [ax];
u16 getss(void);
#pragma aux getss = "mov ax, ss" value [ax];
#else
#define DBG(x)
#endif


static u8 FAR *sdap(u16 off)
{
    return di.sda + off;
}

/* path argument of the request (WFP_START, REN_WFP) */
static const char FAR *wfp(int second)
{
    u16 off = *(u16 FAR *)(di.sda + (second ? di.o_renwfp : di.o_wfp));
    return (const char FAR *)MK_FP(FP_SEG(di.sda), off);
}

static u32 ticks(void)
{
    return *(u32 FAR *)MK_FP(0x40, 0x6C);
}

static u16 fstrlen(const char FAR *s)
{
    u16 n = 0;
    while (s[n]) n++;
    return n;
}

static void fail(u16 err)
{
    rr.ax = err;
    rr.fl |= 1;
}

static void ok(void)
{
    rr.fl &= ~1;
}

static u16 dos_err(int r)
{
    switch (r) {
    case UE_NOTFOUND: return 2;
    case UE_NOTDIR: return 3;
    case UE_ISDIR:
    case UE_RDONLY:
    case UE_ACCESS:
    case UE_NOTEMPTY: return 5;
    case UE_EXISTS: return 0x50;
    case UE_NOSPACE: return 0x27;
    case UE_BADNAME: return 0x7B;
    case UE_CHANGED: return 0x15;
    }
    return 0x1F;
}

/* ------------------------------------------------------------------ devices */

/* a request to the CD-ROM device driver of the drive (strategy, then interrupt) */
static u16 cd_request(struct dvol *dv, u8 FAR *rq)
{
    struct rregs r;
    u16 FAR *h = (u16 FAR *)dv->devhdr;
#ifdef DOS_DEBUG
    dbgs("[cdrq dv=");
    dbgx((u16)(void *)dv);
    dbgs(" ds=");
    dbgx(getds());
    dbgs(" ss=");
    dbgx(getss());
    dbgs(" dev=");
    dbgx(FP_SEG(dv->devhdr));
    dbgs(":");
    dbgx(FP_OFF(dv->devhdr));
    dbgs(" s=");
    dbgx(h[3]);
    dbgs(" i=");
    dbgx(h[4]);
    dbgs(" me=");
    dbgx(FP_SEG((void FAR *)&rr));
    dbgs(" ");
#endif
    rq[1] = dv->subunit;
    *(u16 FAR *)(rq + 3) = 0;
    r.es = FP_SEG(rq);
    r.bx = FP_OFF(rq);
    farcall(MK_FP(FP_SEG(dv->devhdr), h[3]), &r);
    r.es = FP_SEG(rq);
    r.bx = FP_OFF(rq);
    farcall(MK_FP(FP_SEG(dv->devhdr), h[4]), &r);
    DBG("]");
    return *(u16 FAR *)(rq + 3);
}

static u8 cdrq[32];

static int cd_read(struct udf_dev *d, u64 lba, u16 n, u8 FAR *buf)
{
    struct dvol *dv = (struct dvol *)d->ctx;
    u8 FAR *b = buf;
#ifdef DOS_DEBUG
    dbgs("{rd d=");
    dbgx((u16)(void *)d);
    dbgs(" ctx=");
    dbgx((u16)(void *)d->ctx);
    dbgs(" lba=");
    dbgx((u16)lba);
    dbgs("}");
#endif
    while (n) {
        u16 k = n > 16 ? 16 : n, st, tries;
        for (tries = 0; tries < 3; tries++) {
            u8 i;
            for (i = 0; i < 27; i++) cdrq[i] = 0;
            cdrq[0] = 27;
            cdrq[2] = 128;              /* READ LONG, HSG addressing, cooked */
            *(u16 *)(cdrq + 14) = FP_OFF(b);
            *(u16 *)(cdrq + 16) = FP_SEG(b);
            *(u16 *)(cdrq + 18) = k;
            *(u32 *)(cdrq + 20) = (u32)lba;
            st = cd_request(dv, (u8 FAR *)cdrq);
            if (!(st & 0x8000)) break;
            if ((st & 0xFF) == 0x0F) d->gen++;      /* invalid disk change */
        }
        if (tries == 3) return 1;
        n = (u16)(n - k);
        lba += k;
        b = (u8 FAR *)MK_FP(FP_SEG(b) + (k << 7), FP_OFF(b));
    }
    return 0;
}

struct dap {
    u8 size, rsv;
    u16 count;
    u16 off, seg;
    u32 lba_lo, lba_hi;
};

static int bios_rw(struct udf_dev *d, u64 lba, u16 n, u8 FAR *buf, int wr)
{
    struct dvol *dv = (struct dvol *)d->ctx;
    static struct dap p;
    struct rregs r;
    while (n) {
        u16 k = n > 64 ? 64 : n, i;
        for (i = 0; i < 3; i++) {
            p.size = 16;
            p.rsv = 0;
            p.count = k;
            p.off = FP_OFF(buf);
            p.seg = FP_SEG(buf);
            p.lba_lo = (u32)lba;
            p.lba_hi = (u32)(lba >> 32);
            r.ax = wr ? 0x4300 : 0x4200;
            r.dx = dv->unit;
            r.ds = FP_SEG((void FAR *)&p);
            r.si = (u16)(u32)(void *)&p;
            r.si = FP_OFF((void FAR *)&p);
            intx(0x13, &r);
            if (!(r.fl & 1)) break;
            r.ax = 0;
            r.dx = dv->unit;
            intx(0x13, &r);
        }
        if (i == 3) return 1;
        n = (u16)(n - k);
        lba += k;
        buf = (u8 FAR *)MK_FP(FP_SEG(buf) + (u16)(((u32)k * d->ssize) >> 4), FP_OFF(buf));
    }
    return 0;
}

static int bios_read(struct udf_dev *d, u64 lba, u16 n, u8 FAR *buf)
{
    return bios_rw(d, lba, n, buf, 0);
}

static int bios_write(struct udf_dev *d, u64 lba, u16 n, const u8 FAR *buf)
{
    return bios_rw(d, lba, n, (u8 FAR *)buf, 1);
}

/* request of the WINDISK / WINUSB API (32 bytes) */
struct winreq {
    u16 fn, status;
    u32 arg0, arg1, arg2, buflen, retlen, buf, rsv;
};

int win_call(u8 mplex, struct winreq *q)
{
    struct rregs r;
    r.ax = (u16)((mplex << 8) | 1);
    r.es = FP_SEG((void FAR *)q);
    r.bx = FP_OFF((void FAR *)q);
    q->status = 0xFFFF;
    intx(0x2F, &r);
    return q->status;
}

static int win_rw(struct udf_dev *d, u64 lba, u16 n, u8 FAR *buf, int wr)
{
    struct dvol *dv = (struct dvol *)d->ctx;
    static struct winreq q;
    while (n) {
        u16 k = n;
        u32 bytes;
        if ((u32)k * d->ssize > 0x8000UL) k = (u16)(0x8000UL / d->ssize);
        bytes = (u32)k * d->ssize;
        q.fn = 0x0024;
        q.arg0 = dv->unit | (wr ? 0x80000000UL : 0);
        q.arg1 = (u32)lba;
        q.arg2 = (u32)(lba >> 32);
        q.buflen = bytes;
        q.retlen = 0;
        q.buf = ((u32)FP_SEG(buf) << 16) | FP_OFF(buf);
        if (win_call(dv->mplex, &q) != 0 || q.retlen == 0) return 1;
        k = (u16)(q.retlen / d->ssize);
        n = (u16)(n - k);
        lba += k;
        buf = (u8 FAR *)MK_FP(FP_SEG(buf) + (u16)(q.retlen >> 4), FP_OFF(buf));
    }
    return 0;
}

static int win_read(struct udf_dev *d, u64 lba, u16 n, u8 FAR *buf)
{
    return win_rw(d, lba, n, buf, 0);
}

static int win_write(struct udf_dev *d, u64 lba, u16 n, const u8 FAR *buf)
{
    return win_rw(d, lba, n, (u8 FAR *)buf, 1);
}

void dvol_setdev(struct dvol *dv)
{
    dv->dev.ctx = dv;
    if (dv->kind == DV_CD) {
        dv->dev.read = cd_read;
        dv->dev.write = 0;
        dv->dev.ro = 1;
        dv->dev.optical = 1;
        dv->dev.ssize = 2048;
    } else if (dv->kind == DV_BIOS) {
        dv->dev.read = bios_read;
        dv->dev.write = udf_rdonly_all ? 0 : bios_write;
    } else {
        dv->dev.read = win_read;
        dv->dev.write = udf_rdonly_all ? 0 : win_write;
    }
}

/* ------------------------------------------------------------------ volumes */

/* checksum of the anchor and the first descriptors of the volume (a CD changed?) */
static u16 cd_sum(struct dvol *dv)
{
    u8 FAR *b = udf_tmp;
    u16 s = 0;
    if (cd_read(&dv->dev, 16, 1, b)) return 0;
    s = udf_crc(b, 64, 0x1234);
    if (cd_read(&dv->dev, 256, 1, b)) return s;
    s = udf_crc(b, 64, s);
    return s ? s : 1;
}

/* volume size of a CD (IOCTL input 8), 0 when not known */
static u32 cd_size(struct dvol *dv)
{
    static u8 cb[5];
    u8 i;
    for (i = 0; i < 26; i++) cdrq[i] = 0;
    cdrq[0] = 26;
    cdrq[2] = 3;
    *(u16 *)(cdrq + 14) = FP_OFF((void FAR *)cb);
    *(u16 *)(cdrq + 16) = FP_SEG((void FAR *)cb);
    *(u16 *)(cdrq + 18) = 5;
    cb[0] = 8;
    if (cd_request(dv, (u8 FAR *)cdrq) & 0x8000) return 0;
    return *(u32 *)(cb + 1);
}

int cd_mount(struct dvol *dv)
{
    u32 n = cd_size(dv);
    int r;
    dv->mounted = 0;
    udf_cache_drop(&dv->v);
    dv->dev.sectors = n;
    dv->dev.gen++;
    r = udf_mount(&dv->v, &dv->dev, 0, n);
    if (r) return r;
    dv->sum = cd_sum(dv);
    dv->mounted = 1;
    dv->checked = ticks();
    return 0;
}

/* the mounted volume of a drive (0 = A), NULL when the drive is not ours */
static struct dvol *vol_of(u8 drive)
{
    u8 i;
    for (i = 0; i < nvols; i++) {
        struct dvol *dv = &vols[i];
        if (dv->drive != drive) continue;
        if (dv->kind != DV_CD) return dv->mounted ? dv : 0;
        /* CD drive of MSCDEX: the disc may have changed */
        if (ticks() - dv->checked >= 36 || ticks() < dv->checked) {
            u16 s = cd_sum(dv);
            dv->checked = ticks();
            if (!s) {
                dv->mounted = 0;
                udf_cache_drop(&dv->v);
                dv->tried = 0;
            } else if (!dv->mounted || s != dv->sum) {
                if (!dv->mounted && dv->tried == s) return 0;
                dv->tried = s;
                cd_mount(dv);
            }
        }
        return dv->mounted ? dv : 0;
    }
    return 0;
}

/* ------------------------------------------------------------------ names */

/* OEM name -> Unicode */
static u16 to_uni(const char FAR *s, u16 n, u16 *out)
{
    u16 i;
    for (i = 0; i < n && i < UDF_NAMEMAX; i++) {
        u8 c = (u8)s[i];
        out[i] = c < 0x80 ? c : (oem[c - 0x80] ? oem[c - 0x80] : '_');
    }
    return i;
}

/* "NAME.EXT" -> 11-character FCB form */
static void to_fcb(const char *n, char *fcb)
{
    int i = 0, j;
    for (j = 0; j < 11; j++) fcb[j] = ' ';
    for (j = 0; n[i] && n[i] != '.' && j < 8; i++, j++) fcb[j] = n[i];
    while (n[i] && n[i] != '.') i++;
    if (n[i] == '.') i++;
    for (j = 8; n[i] && j < 11; i++, j++) fcb[j] = n[i];
}

/* last path component (FAR) -> FCB pattern; '*' fills with '?' */
static void pat_fcb(const char FAR *s, char *fcb)
{
    int i = 0, j;
    for (j = 0; j < 11; j++) fcb[j] = ' ';
    for (j = 0; s[i] && s[i] != '.' && j < 8; i++) {
        if (s[i] == '*') {
            while (j < 8) fcb[j++] = '?';
        } else {
            fcb[j++] = s[i];
        }
    }
    while (s[i] && s[i] != '.') i++;
    if (s[i] == '.') i++;
    for (j = 8; s[i] && j < 11; i++) {
        if (s[i] == '*') {
            while (j < 11) fcb[j++] = '?';
        } else {
            fcb[j++] = s[i];
        }
    }
}

static int haswild(const char FAR *s)
{
    for (; *s; s++)
        if (*s == '?' || *s == '*') return 1;
    return 0;
}

/* ------------------------------------------------------------------ paths */

/* walk path p (after "X:", starting with '\') of volume dv: the node in nd; with parent,
   the parent directory in pd, the entry in fid. Returns 0, UE_NOTFOUND (only the last
   component is missing: pd and *last valid) or another error (UE_NOTDIR: path) */
static int walk(struct dvol *dv, const char FAR *p, int parent, const char FAR **last)
{
    int r;
    nd_vol = 0;
    r = udf_iget(&dv->v, dv->v.root, &nd);
    if (r) return r;
    nd_vol = dv;
    pd = nd;
    fid.pos = 0xFFFFFFFFUL;
    fid.fchar = FC_DIR;
    if (last) *last = p;
    while (*p == '\\') p++;
    while (*p) {
        const char FAR *e = p;
        u16 n;
        while (*e && *e != '\\') e++;
        if (nd.ftype != FT_DIR) return UE_NOTDIR;
        if (last) *last = p;
        n = to_uni(p, (u16)(e - p), uname);
        pd = nd;
        r = udf_lookup(&dv->v, &pd, uname, n, &fid, oem);
        if (r < 0) return r;
        if (r == 0) return *e ? UE_NOTDIR : UE_NOTFOUND;
        nd_vol = 0;
        r = udf_iget(&dv->v, fid.icb, &nd);
        if (r) return r;
        nd_vol = dv;
        p = e;
        while (*p == '\\') p++;
    }
    (void)parent;
    return 0;
}

static int path_drive(const char FAR *fn, u8 *drive)
{
    if (fn[1] != ':' || fn[0] < 'A' || fn[0] > 'Z') return 0;
    *drive = (u8)(fn[0] - 'A');
    return 1;
}

/* ------------------------------------------------------------------ time */

static u16 dos_time(const struct udf_time *t)
{
    return (u16)((t->hour << 11) | (t->min << 5) | (t->sec >> 1));
}

static u16 dos_date(const struct udf_time *t)
{
    if (t->year < 1980) return (1 << 5) | 1;
    return (u16)(((t->year - 1980) << 9) | (t->month << 5) | t->day);
}

static void from_dos(u16 date, u16 time, struct udf_time *t)
{
    t->year = (u16)(1980 + (date >> 9));
    t->month = (u8)((date >> 5) & 15);
    t->day = (u8)(date & 31);
    t->hour = (u8)(time >> 11);
    t->min = (u8)((time >> 5) & 63);
    t->sec = (u8)((time & 31) * 2);
    t->cs = 0;
    t->tz = UDF_NOTZ;
}

static u8 bcd(u8 v)
{
    return (u8)((v >> 4) * 10 + (v & 15));
}

/* the time from the real-time clock of the BIOS: DOS cannot be called inside a DOS call */
void udf_now(struct udf_time *t)
{
    struct rregs r;
    r.ax = 0x0400;
    r.cx = r.dx = 0;
    intx(0x1A, &r);
    t->year = (u16)(bcd((u8)(r.cx >> 8)) * 100 + bcd((u8)r.cx));
    t->month = bcd((u8)(r.dx >> 8));
    t->day = bcd((u8)r.dx);
    if (t->year < 1980 || !t->month || t->month > 12 || !t->day) {
        t->year = 1980;
        t->month = 1;
        t->day = 1;
    }
    r.ax = 0x0200;
    r.cx = r.dx = 0;
    intx(0x1A, &r);
    t->hour = bcd((u8)(r.cx >> 8));
    t->min = bcd((u8)r.cx);
    t->sec = bcd((u8)(r.dx >> 8));
    t->cs = 0;
    t->tz = UDF_NOTZ;
    if (t->hour > 23 || t->min > 59 || t->sec > 59) t->hour = t->min = t->sec = 0;
}

/* ------------------------------------------------------------------ SFT */

#define SFT_SIG         0xEE

static struct dvol *sft_vol(u8 FAR *sft)
{
    u16 flags = *(u16 FAR *)(sft + 5);
    u8 i;
    if (!(flags & 0x8000) || sft[0x1F] != SFT_SIG) return 0;
    i = sft[0x1A];
    if (i >= nvols || vols[i].drive != (flags & 0x3F)) return 0;
    return &vols[i];
}

static int sft_node(struct dvol *dv, u8 FAR *sft)
{
    struct udf_loc l;
    if (!dv->mounted || dv->v.gen != dv->dev.gen) return UE_CHANGED;
    l.lbn = *(u32 FAR *)(sft + 0x1B);
    l.part = sft[0x19];
    if (nd_vol == dv && nd.loc.lbn == l.lbn && nd.loc.part == l.part) return 0;
    nd_vol = 0;
    if (udf_iget(&dv->v, l, &nd)) return UE_CHANGED;
    nd_vol = dv;
    return 0;
}

static void sft_fill(u8 FAR *sft, struct dvol *dv, u8 attr, const char *name83)
{
    char fcb[11];
    u16 i;
    sft[4] = attr;
    *(u16 FAR *)(sft + 5) = (u16)(0x8040 | dv->drive);
    *(u32 FAR *)(sft + 7) = 0;
    *(u16 FAR *)(sft + 0x0B) = 0;
    *(u16 FAR *)(sft + 0x0D) = dos_time(&nd.mtime);
    *(u16 FAR *)(sft + 0x0F) = dos_date(&nd.mtime);
    *(u32 FAR *)(sft + 0x11) = nd.big ? 0xFFFFFFFFUL : (u32)nd.size;
    *(u32 FAR *)(sft + 0x15) = 0;
    sft[0x19] = (u8)nd.loc.part;
    sft[0x1A] = (u8)(dv - vols);
    *(u32 FAR *)(sft + 0x1B) = nd.loc.lbn;
    sft[0x1F] = SFT_SIG;
    to_fcb(name83, fcb);
    for (i = 0; i < 11; i++) sft[0x20 + i] = fcb[i];
}

/* the last component of FN1 in 8.3 form for the SFT */
static void last83(const char FAR *last, char *out)
{
    u16 i = 0;
    while (*last && *last != '\\' && i < 12) out[i++] = *last++;
    out[i] = 0;
}

/* ------------------------------------------------------------------ search */

#define MAXDS   16
static struct {
    u8 vol;
    u8 gen;
    struct udf_loc loc;
} dslots[MAXDS];
static u8 dsnext, dsgen;

static u8 dslot(u8 vol, struct udf_loc loc)
{
    u8 i;
    for (i = 0; i < MAXDS; i++)
        if (dslots[i].gen && dslots[i].vol == vol && dslots[i].loc.lbn == loc.lbn &&
            dslots[i].loc.part == loc.part) return i;
    i = dsnext;
    dsnext = (u8)((dsnext + 1) % MAXDS);
    if (++dsgen == 0) dsgen = 1;
    dslots[i].vol = vol;
    dslots[i].gen = dsgen;
    dslots[i].loc = loc;
    return i;
}

/* SDB (21 bytes): 0 drive | 80h, 1-11 pattern, 12 attributes, 13 directory slot,
   14 slot generation, 15-18 position, 19 state (0 ".", 1 "..", 2 entries), 20 volume */
static void put_entry(u8 FAR *e, const char *fcb, u8 attr, u16 time, u16 date, u32 size)
{
    u16 i;
    for (i = 0; i < 11; i++) e[i] = fcb[i];
    e[11] = attr;
    for (i = 12; i < 22; i++) e[i] = 0;
    *(u16 FAR *)(e + 22) = time;
    *(u16 FAR *)(e + 24) = date;
    *(u16 FAR *)(e + 26) = 0;
    *(u32 FAR *)(e + 28) = size;
}

static int fcbmatch(const char *f, const u8 FAR *pat)
{
    u16 i;
    for (i = 0; i < 11; i++)
        if (pat[i] != '?' && (u8)pat[i] != (u8)f[i]) return 0;
    return 1;
}

static int find_next(u8 FAR *dta)
{
    struct dvol *dv;
    u8 slot = dta[13], vi = dta[20], sattr = dta[12];
    u32 pos = *(u32 FAR *)(dta + 15);
    char fcb[11];
    int r;
    if (vi >= nvols || slot >= MAXDS || dslots[slot].gen != dta[14] || dslots[slot].vol != vi) return 0x12;
    dv = &vols[vi];
    if (!dv->mounted) return 0x12;
    if (udf_iget(&dv->v, dslots[slot].loc, &pd)) return 0x12;
    while (dta[19] < 2) {
        u8 st = dta[19];
        dta[19] = (u8)(st + 1);
        for (r = 0; r < 11; r++) fcb[r] = ' ';
        fcb[0] = '.';
        if (st) fcb[1] = '.';
        if (!(sattr & 0x10) || !fcbmatch(fcb, dta + 1)) continue;
        put_entry(dta + 21, fcb, 0x10, dos_time(&pd.mtime), dos_date(&pd.mtime), 0);
        return 0;
    }
    for (;;) {
        u8 a;
        r = udf_readdir(&dv->v, &pd, &pos, &fid);
        if (r <= 0) break;
        if (fid.fchar & (FC_PARENT | FC_DELETED)) continue;
        udf_dosname(fid.name, fid.nlen, dname, oem);
        to_fcb(dname, fcb);
        if (!fcbmatch(fcb, dta + 1)) continue;
        if (udf_iget(&dv->v, fid.icb, &nd)) continue;
        a = udf_dosattr(&nd, fid.fchar);
        if ((dv->v.flags & VF_RDONLY) && !(a & 0x10)) a |= 0x01;
        if ((a & 0x16) & ~sattr) continue;
        *(u32 FAR *)(dta + 15) = pos;
        put_entry(dta + 21, fcb, a, dos_time(&nd.mtime), dos_date(&nd.mtime),
                  nd.big ? 0xFFFFFFFFUL : (u32)nd.size);
        return 0;
    }
    *(u32 FAR *)(dta + 15) = pos;
    return 0x12;
}

static int find_first(struct dvol *dv)
{
    const char FAR *fn = wfp(0);
    const char FAR *last;
    u8 FAR *dta = *(u8 FAR * FAR *)sdap(0x0C);
    u8 sattr = *sdap(di.o_sattr);
    char pat[11];
    int r, root;
    u16 i;
    /* the directory: everything before the last component */
    r = walk(dv, fn + rootoff, 1, &last);
    if (r && r != UE_NOTFOUND) return 3;
    pat_fcb(last, pat);
    root = (pd.loc.lbn == dv->v.root.lbn && pd.loc.part == dv->v.root.part);
    if (r == 0 && last == fn + rootoff) {
        /* "X:\" alone */
        root = 1;
    }
    dta[0] = (u8)(0x80 | dv->drive);
    for (i = 0; i < 11; i++) dta[1 + i] = pat[i];
    dta[12] = sattr;
    dta[20] = (u8)(dv - vols);
    {
        u8 s = dslot((u8)(dv - vols), pd.loc);
        dta[13] = s;
        dta[14] = dslots[s].gen;
    }
    *(u32 FAR *)(dta + 15) = 0;
    dta[19] = (u8)(root ? 2 : 0);
    if ((sattr & 0x08) && (sattr == 0x08 || root)) {
        /* volume label: alone (attribute 08h) or first in the root directory */
        char lab[11];
        for (i = 0; i < 11; i++) lab[i] = ' ';
        for (i = 0; i < 11 && dv->v.label[i]; i++) {
            char c = dv->v.label[i];
            lab[i] = (char)(c >= 'a' && c <= 'z' ? c - 32 : c);
        }
        if (sattr == 0x08) dta[13] = 0xFF;
        if (dv->v.label[0] && (sattr == 0x08 || fcbmatch(lab, (const u8 FAR *)pat))) {
            put_entry(dta + 21, lab, 0x08, 0, (1 << 5) | 1, 0);
            return 0;
        }
        if (sattr == 0x08) return 0x12;
    }
    r = find_next(dta);
    return r == 0x12 ? 0x12 : r;
}

/* ------------------------------------------------------------------ requests */

static void sft_close(struct dvol *dv, u8 FAR *sft)
{
    u16 FAR *cnt = (u16 FAR *)sft;
    if (*cnt) (*cnt)--;
    if (*cnt == 0 || *cnt == 0xFFFF) {
        u16 mode = *(u16 FAR *)(sft + 2);
        u16 flags = *(u16 FAR *)(sft + 5);
        if ((mode & 3) && dv->mounted && sft_node(dv, sft) == 0) {
            if (flags & 0x4000) {
                /* time set with INT 21h 5701h */
                struct udf_time t;
                from_dos(*(u16 FAR *)(sft + 0x0F), *(u16 FAR *)(sft + 0x0D), &t);
                udf_settime(&dv->v, &nd, &t, 0);
            }
        }
        if ((mode & 3) && dv->v.nopen) dv->v.nopen--;
        if (dv->mounted) udf_sync(&dv->v);
    }
}

static void do_read(struct dvol *dv, u8 FAR *sft)
{
    u32 pos = *(u32 FAR *)(sft + 0x15), size;
    u8 FAR *buf = *(u8 FAR * FAR *)sdap(0x0C);
    u16 cnt = rr.cx;
    s32 r;
    if (sft_node(dv, sft)) {
        fail(0x1F);
        return;
    }
    size = nd.big ? 0xFFFFFFFFUL : (u32)nd.size;
    if (pos >= size) cnt = 0;
    else if ((u32)cnt > size - pos) cnt = (u16)(size - pos);
    buf = (u8 FAR *)MK_FP(FP_SEG(buf) + (FP_OFF(buf) >> 4), FP_OFF(buf) & 15);
    r = cnt ? udf_read(&dv->v, &nd, pos, cnt, buf) : 0;
    if (r < 0) {
        fail(dos_err((int)r));
        return;
    }
    *(u32 FAR *)(sft + 0x15) = pos + (u32)r;
    rr.cx = (u16)r;
    ok();
}

static void do_write(struct dvol *dv, u8 FAR *sft)
{
    u32 pos = *(u32 FAR *)(sft + 0x15);
    u8 FAR *buf = *(u8 FAR * FAR *)sdap(0x0C);
    u16 cnt = rr.cx;
    s32 r;
    if (!(*(u16 FAR *)(sft + 2) & 3)) {
        fail(5);
        return;
    }
    if (sft_node(dv, sft)) {
        fail(0x1F);
        return;
    }
    buf = (u8 FAR *)MK_FP(FP_SEG(buf) + (FP_OFF(buf) >> 4), FP_OFF(buf) & 15);
    if (cnt == 0) {
        r = udf_truncate(&dv->v, &nd, pos);
        if (r) {
            fail(dos_err((int)r));
            return;
        }
    } else {
        r = udf_write(&dv->v, &nd, pos, cnt, buf);
        if (r < 0) {
            if (r == UE_NOSPACE) {
                rr.cx = 0;
                ok();
                return;
            }
            fail(dos_err((int)r));
            return;
        }
        pos += (u32)r;
        cnt = (u16)r;
    }
    *(u32 FAR *)(sft + 0x15) = pos;
    *(u32 FAR *)(sft + 0x11) = (u32)nd.size;
    *(u16 FAR *)(sft + 5) &= ~0x40;
    *(u16 FAR *)(sft + 0x0D) = dos_time(&nd.mtime);
    *(u16 FAR *)(sft + 0x0F) = dos_date(&nd.mtime);
    rr.cx = cnt;
    ok();
}

/* open (1116h), create (1117h), extended open (112Eh) */
static void do_open(struct dvol *dv, int how, u16 mode, u8 attr)
{
    const char FAR *fn = wfp(0);
    const char FAR *last;
    u8 FAR *sft = (u8 FAR *)MK_FP(rr.es, rr.di);
    char n83[13];
    int r, exists, created = 0;
    u16 action = 1;
    /* how: 0 open existing, 1 create/truncate, 2 create new, 3 extended (action in mode hi) */
    r = walk(dv, fn + rootoff, 1, &last);
    if (r && r != UE_NOTFOUND) {
        fail(3);
        return;
    }
    if (!*last || haswild(last)) {
        fail(r ? 3 : 2);
        return;
    }
    last83(last, n83);
    exists = r == 0;
    if (exists && nd.ftype == FT_DIR) {
        fail(5);
        return;
    }
    if (how == 3) {
        u16 act = rr.cx;                    /* set by the caller from the SDA */
        if (exists) {
            if ((act & 0x0F) == 0) {
                fail(0x50);
                return;
            }
            how = (act & 0x0F) == 2 ? 1 : 0;
            action = (u16)(how == 1 ? 3 : 1);
        } else {
            if (!(act & 0xF0)) {
                fail(2);
                return;
            }
            how = 2;
            action = 2;
        }
    }
    if (how == 0) {
        if (!exists) {
            fail(2);
            return;
        }
        if ((mode & 3) && ((dv->v.flags & VF_RDONLY) || !(nd.perm & PERM_OWRITE))) {
            fail(5);
            return;
        }
    } else {
        if (exists && how == 2) {
            fail(0x50);
            return;
        }
        if (dv->v.flags & VF_RDONLY) {
            fail(5);
            return;
        }
        if (exists) {
            if (!(nd.perm & PERM_OWRITE)) {
                fail(5);
                return;
            }
            r = udf_truncate(&dv->v, &nd, 0);
        } else {
            struct udf_time t;
            u16 n = to_uni(last, (u16)fstrlen(n83), uname);
            udf_now(&t);
            r = udf_create(&dv->v, &pd, uname, n, 0, attr, &t, &nd, 0);
            created = 1;
        }
        if (r) {
            udf_sync(&dv->v);
            fail(r == UE_NOSPACE ? 0x52 : dos_err(r));
            return;
        }
        mode = (u16)((mode & ~3) | 2);
    }
    nd_vol = dv;
    sft_fill(sft, dv, udf_dosattr(&nd, created ? 0 : fid.fchar), n83);
    *(u16 FAR *)(sft + 2) = (u16)((*(u16 FAR *)(sft + 2) & 0xFF00) | (mode & 0x7F));
    if (mode & 3) dv->v.nopen++;
    else udf_sync(&dv->v);
    rr.cx = action;
    ok();
}

static void do_getattr(struct dvol *dv)
{
    const char FAR *fn = wfp(0);
    const char FAR *last;
    int r = walk(dv, fn + rootoff, 1, &last);
    u8 a;
    if (r) {
        fail(r == UE_NOTFOUND ? 2 : 3);
        return;
    }
    a = udf_dosattr(&nd, fid.fchar);
    if ((dv->v.flags & VF_RDONLY) && !(a & 0x10)) a |= 1;
    rr.ax = a;
    rr.bx = nd.big ? 0xFFFF : (u16)(nd.size >> 16);
    rr.di = nd.big ? 0xFFFF : (u16)nd.size;
    rr.cx = dos_time(&nd.mtime);
    rr.dx = dos_date(&nd.mtime);
    ok();
}

static void do_setattr(struct dvol *dv, u8 attr)
{
    const char FAR *fn = wfp(0);
    const char FAR *last;
    int r = walk(dv, fn + rootoff, 1, &last);
    if (r) {
        fail(r == UE_NOTFOUND ? 2 : 3);
        return;
    }
    if (fid.pos == 0xFFFFFFFFUL) {
        fail(5);
        return;
    }
    r = udf_setattr(&dv->v, &pd, fid.pos, &nd, attr);
    udf_sync(&dv->v);
    if (r) fail(dos_err(r));
    else ok();
}

static void do_mkdir(struct dvol *dv)
{
    const char FAR *fn = wfp(0);
    const char FAR *last;
    struct udf_time t;
    char n83[13];
    int r = walk(dv, fn + rootoff, 1, &last);
    u16 n;
    if (r == 0) {
        fail(5);
        return;
    }
    if (r != UE_NOTFOUND || haswild(last)) {
        fail(3);
        return;
    }
    last83(last, n83);
    n = to_uni(last, (u16)fstrlen(n83), uname);
    udf_now(&t);
    r = udf_create(&dv->v, &pd, uname, n, 1, 0, &t, &nd, 0);
    udf_sync(&dv->v);
    if (r) fail(r == UE_NOSPACE ? 0x52 : dos_err(r));
    else ok();
}

static void do_rmdir(struct dvol *dv)
{
    const char FAR *fn = wfp(0);
    const char FAR *last;
    int r = walk(dv, fn + rootoff, 1, &last);
    if (r) {
        fail(3);
        return;
    }
    if (nd.ftype != FT_DIR || fid.pos == 0xFFFFFFFFUL) {
        fail(r ? 3 : 5);
        return;
    }
    r = udf_unlink(&dv->v, &pd, fid.pos);
    udf_sync(&dv->v);
    if (r) fail(dos_err(r));
    else ok();
}

static void do_chdir(struct dvol *dv)
{
    const char FAR *fn = wfp(0);
    int r;
    /* DOS checks the current directory of a network drive with a CHDIR that leaves the
       path of the last request in place: then the current directory of the CDS counts
       (a real CHDIR never has wildcards) */
    if (haswild(fn)) fn = (const char FAR *)*(u8 FAR * FAR *)sdap(di.o_curcds);
    r = walk(dv, fn + rootoff, 0, 0);
    if (r || nd.ftype != FT_DIR) fail(3);
    else ok();
}

/* delete: wildcards allowed, only normal files */
static void do_delete(struct dvol *dv)
{
    const char FAR *fn = wfp(0);
    const char FAR *last;
    char pat[11], fcb[11];
    int r = walk(dv, fn + rootoff, 1, &last), n = 0;
    struct udf_node dir;
    u32 pos = 0;
    if (r && r != UE_NOTFOUND) {
        fail(3);
        return;
    }
    if (!haswild(last)) {
        if (r) {
            fail(2);
            return;
        }
        if (nd.ftype == FT_DIR || (udf_dosattr(&nd, fid.fchar) & 0x07)) {
            fail(5);
            return;
        }
        r = udf_unlink(&dv->v, &pd, fid.pos);
        udf_sync(&dv->v);
        if (r) fail(dos_err(r));
        else ok();
        return;
    }
    pat_fcb(last, pat);
    dir = pd;
    while ((r = udf_readdir(&dv->v, &dir, &pos, &fid)) > 0) {
        u8 a;
        if (fid.fchar & (FC_PARENT | FC_DELETED | FC_DIR)) continue;
        udf_dosname(fid.name, fid.nlen, dname, oem);
        to_fcb(dname, fcb);
        if (!fcbmatch(fcb, (u8 FAR *)pat)) continue;
        if (udf_iget(&dv->v, fid.icb, &nd)) continue;
        a = udf_dosattr(&nd, fid.fchar);
        if (a & 0x07) continue;
        if (udf_unlink(&dv->v, &dir, fid.pos) == 0) n++;
    }
    udf_sync(&dv->v);
    if (n) ok();
    else fail(2);
}

/* rename: FN1 -> FN2 (same drive), wildcards in the last component (DOS REN rules) */
static void do_rename(struct dvol *dv)
{
    const char FAR *fn = wfp(0);
    const char FAR *fn2 = wfp(1);
    const char FAR *last;
    const char FAR *last2;
    static struct udf_node src, dst;
    char pat[11], dpat[11], fcb[11], nn[13];
    int r, n = 0;
    u32 pos = 0;
    {
        u16 i;
        for (i = 0; i < rootoff && fn[i] == fn2[i]; i++)
            ;
        if (i < rootoff) {
            fail(0x11);
            return;
        }
    }
    r = walk(dv, fn2 + rootoff, 1, &last2);
    if (r == 0 && !haswild(last2)) {
        fail(5);
        return;
    }
    if (r != UE_NOTFOUND && r != 0) {
        fail(3);
        return;
    }
    dst = pd;
    pat_fcb(last2, dpat);
    r = walk(dv, fn + rootoff, 1, &last);
    if (r && r != UE_NOTFOUND) {
        fail(3);
        return;
    }
    src = pd;
    pat_fcb(last, pat);
    while ((r = udf_readdir(&dv->v, &src, &pos, &fid)) > 0) {
        u16 i, k = 0, ln;
        if (fid.fchar & (FC_PARENT | FC_DELETED)) continue;
        udf_dosname(fid.name, fid.nlen, dname, oem);
        to_fcb(dname, fcb);
        if (!fcbmatch(fcb, (u8 FAR *)pat)) continue;
        /* the new name from the destination pattern */
        for (i = 0; i < 8; i++) {
            char c = dpat[i] == '?' ? fcb[i] : dpat[i];
            if (c != ' ') nn[k++] = c;
        }
        if (dpat[8] != ' ' || fcb[8] != ' ') {
            u16 dot = k;
            nn[k++] = '.';
            for (i = 8; i < 11; i++) {
                char c = dpat[i] == '?' ? fcb[i] : dpat[i];
                if (c != ' ') nn[k++] = c;
            }
            if (k == dot + 1) k = dot;
        }
        nn[k] = 0;
        ln = to_uni((const char FAR *)nn, k, uname);
        r = udf_rename(&dv->v, &src, fid.pos, &dst, uname, ln, 0xFFFFFFFFUL);
        if (r) {
            udf_sync(&dv->v);
            fail(dos_err(r));
            return;
        }
        n++;
        if (src.loc.lbn == dst.loc.lbn && src.loc.part == dst.loc.part) src = dst;
        if (!haswild(last)) break;
    }
    udf_sync(&dv->v);
    if (n) ok();
    else fail(2);
}

static void do_diskinfo(struct dvol *dv)
{
    u32 total = udf_totalblocks(&dv->v), fr = udf_freeblocks(&dv->v);
    u16 bs = dv->v.bs, spc = 1;
    /* clusters must fit in 16 bits */
    while ((total / spc) > 0xFFF0UL && spc < 128) spc <<= 1;
    while ((u32)spc * bs > 32768UL && spc > 1) spc >>= 1;
    rr.ax = (u16)(spc | 0xF800);
    rr.bx = (u16)((total / spc) > 0xFFF0UL ? 0xFFF0UL : total / spc);
    rr.cx = bs;
    rr.dx = (u16)((fr / spc) > 0xFFF0UL ? 0xFFF0UL : fr / spc);
    ok();
}

/* the drive of a path request: the current CDS (paths of MSCDEX drives look like
   "\\R.\A.\DIR"); rootoff: characters of the path before the root backslash */

/* drive of a CDS entry, 0xFF when it is not one of the CDS array */
static u8 cds_drive(u8 FAR *cds)
{
    u32 a = ((u32)FP_SEG(cds) << 4) + FP_OFF(cds), b = ((u32)FP_SEG(di.cds) << 4) + FP_OFF(di.cds);
    if (a >= b && (a - b) % di.cdssize == 0 && (a - b) / di.cdssize < di.lastdrive)
        return (u8)((a - b) / di.cdssize);
    return 0xFF;
}

static struct dvol *path_vol(void)
{
    u8 FAR *cds = *(u8 FAR * FAR *)sdap(di.o_curcds);
    u8 drive = cds_drive(cds);
    if (drive != 0xFF) {
        rootoff = *(u16 FAR *)(cds + 0x4F);
        if (rootoff > 66) rootoff = 2;
    } else {
        if (!path_drive(wfp(0), &drive)) return 0;
        rootoff = 2;
    }
    return vol_of(drive);
}

#ifdef DOS_DEBUG
static int redir2(void);
int __cdecl redir(void)
{
    u8 fnc = (u8)rr.ax;
    int h;
    dbgs("[v0dev=");
    dbgx(FP_SEG(vols[0].devhdr));
    dbgs(":");
    dbgx(FP_OFF(vols[0].devhdr));
    dbgs(" n=");
    dbgx(nvols);
    dbgs(" ctx=");
    dbgx((u16)(void *)vols[0].dev.ctx);
    dbgs(" v0=");
    dbgx((u16)(void *)&vols[0]);
    dbgs(" rd=");
    dbgx((u16)(void *)vols[0].dev.read);
    dbgs("]");
    dbgs("11");
    dbgx(fnc);
    dbgs(" ");
    if (fnc != 0x08 && fnc != 0x09 && fnc != 0x06 && fnc != 0x1C) dbgs(wfp(0));
    dbgs(" sda=");
    dbgx(FP_OFF(di.sda));
    dbgs(" wfp=");
    dbgx(*(u16 FAR *)(di.sda + di.o_wfp));
    dbgs(" cds=");
    dbgx(FP_OFF(*(u8 FAR * FAR *)sdap(di.o_curcds)));
    dbgs(" ");
    dbgs((const char FAR *)sdap(di.o_fn1));
    if (fnc == 0x1B) {
        dbgs(" sattr=");
        dbgx(*sdap(di.o_sattr));
    }
    h = redir2();
    if (h && (fnc == 0x1B || fnc == 0x1C) && !(rr.fl & 1)) {
        u8 FAR *e = *(u8 FAR * FAR *)sdap(0x0C) + 21;
        char nm[13];
        int i;
        for (i = 0; i < 11; i++) nm[i] = e[i];
        nm[11] = 0;
        dbgs(" [");
        dbgs((const char FAR *)nm);
        dbgs("] a=");
        dbgx(e[11]);
    }
    dbgs(h ? " -> " : " -> pass ");
    if (h) {
        dbgx(rr.fl & 1);
        dbgs(" ax=");
        dbgx(rr.ax);
    }
    dbgs("\n");
    return h;
}
static int redir2(void)
#else
int __cdecl redir(void)
#endif
{
    u8 fnc = (u8)rr.ax;
    struct dvol *dv;
    u8 FAR *sft = (u8 FAR *)MK_FP(rr.es, rr.di);
    switch (fnc) {
    case 0x01:
        if (!(dv = path_vol())) return 0;
        do_rmdir(dv);
        return 1;
    case 0x03:
        if (!(dv = path_vol())) return 0;
        do_mkdir(dv);
        return 1;
    case 0x05:
        if (!(dv = path_vol())) return 0;
        do_chdir(dv);
        return 1;
    case 0x06:
        if (!(dv = sft_vol(sft))) return 0;
        sft_close(dv, sft);
        ok();
        return 1;
    case 0x07:
        if (!(dv = sft_vol(sft))) return 0;
        ok();
        return 1;
    case 0x08:
        if (!(dv = sft_vol(sft))) return 0;
        do_read(dv, sft);
        return 1;
    case 0x09:
        if (!(dv = sft_vol(sft))) return 0;
        do_write(dv, sft);
        return 1;
    case 0x0A:
    case 0x0B:
        if (!(dv = sft_vol(sft))) return 0;
        ok();
        return 1;
    case 0x0C:
    {
        u8 d = cds_drive((u8 FAR *)MK_FP(rr.es, rr.di));
        if (d == 0xFF || !(dv = vol_of(d))) return 0;
        do_diskinfo(dv);
        return 1;
    }
    case 0x0E:
        if (!(dv = path_vol())) return 0;
        do_setattr(dv, *(u8 FAR *)MK_FP(rr.ss, rr.sp + 6));
        return 1;
    case 0x0F:
        if (!(dv = path_vol())) return 0;
        do_getattr(dv);
        return 1;
    case 0x11:
        if (!(dv = path_vol())) return 0;
        do_rename(dv);
        return 1;
    case 0x13:
        if (!(dv = path_vol())) return 0;
        do_delete(dv);
        return 1;
    case 0x16:
        if (!(dv = path_vol())) return 0;
        do_open(dv, 0, *(u16 FAR *)MK_FP(rr.ss, rr.sp + 6), 0);
        return 1;
    case 0x17:
    {
        u16 w = *(u16 FAR *)MK_FP(rr.ss, rr.sp + 6);
        if (!(dv = path_vol())) return 0;
        do_open(dv, (w >> 8) ? 2 : 1, 2, (u8)w);
        return 1;
    }
    case 0x1B:
    {
        int e;
        if (!(dv = path_vol())) return 0;
        e = find_first(dv);
        if (e) fail((u16)e);
        else ok();
        return 1;
    }
    case 0x1C:
    {
        u8 FAR *dta = *(u8 FAR * FAR *)sdap(0x0C);
        int e;
        if (!(dta[0] & 0x80) || dta[20] >= nvols || vols[dta[20]].drive != (dta[0] & 0x1F)) return 0;
        if (dta[13] == 0xFF) {
            fail(0x12);
            return 1;
        }
        e = find_next(dta);
        if (e) fail((u16)e);
        else ok();
        return 1;
    }
    case 0x21:
    {
        u32 size, pos;
        s32 off = (s32)(((u32)rr.cx << 16) | rr.dx);
        if (!(dv = sft_vol(sft))) return 0;
        size = *(u32 FAR *)(sft + 0x11);
        pos = (u32)((s32)size + off);
        if (off < 0 && (u32)(-off) > size) pos = 0;
        *(u32 FAR *)(sft + 0x15) = pos;
        rr.dx = (u16)(pos >> 16);
        rr.ax = (u16)pos;
        ok();
        return 1;
    }
    case 0x2E:
    {
        u16 act = *(u16 FAR *)sdap(di.o_extact), attr = *(u16 FAR *)sdap(di.o_extattr);
        u16 mode = *(u16 FAR *)sdap(di.o_extmode);
        if (di.major < 4 || !(dv = path_vol())) return 0;
        rr.cx = act;
        do_open(dv, 3, mode, (u8)attr);
        return 1;
    }
    }
    return 0;
}

/* WINUDF API (INT 2Fh AH=D6h): AL=01h unload. Returns AX=0 and ES = the resident
   segment, AX=1 when another program hooked INT 2Fh after WINUDF */
int __cdecl api(void)
{
    u8 i;
    if ((u8)rr.ax != 1) {
        rr.ax = 0xFFFF;
        return 1;
    }
    {
        void __far *cur;
        struct rregs r;
        r.ax = 0x352F;
        intx(0x21, &r);
        cur = MK_FP(r.es, r.bx);
        if (FP_SEG(cur) != FP_SEG((void __far *)&rr)) {
            rr.ax = 1;
            return 1;
        }
    }
    for (i = 0; i < nvols; i++) {
        struct dvol *dv = &vols[i];
        if (dv->mounted) udf_sync(&dv->v);
        if (dv->kind != DV_CD) {
            u8 FAR *c = di.cds + (u16)dv->drive * di.cdssize;
            *(u16 FAR *)(c + 0x43) = 0;
        }
        dv->mounted = 0;
    }
    nvols = 0;
    unhook2f();
    rr.ax = 0;
    rr.es = FP_SEG((void __far *)&rr);
    return 1;
}
