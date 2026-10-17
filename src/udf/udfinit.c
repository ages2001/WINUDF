/* udfinit.c - WINUDF.EXE, transient part: options, DOS structures, CD drives of MSCDEX,
   disks of the BIOS and of WINDISK / WINUSB with their partitions (MBR, extended, GPT),
   drive letters, the block cache, INT 2Fh hook and TSR. Unloading (/U). Its code and data
   follow the resident part and are dropped when the program stays resident. */
#include <i86.h>
#include "udf.h"
#include "udfdos.h"

#pragma data_seg("INIT_DATA", "INITDATA")

#include "oemcp.h"

static u8 FAR *tmpbuf;
static u16 nslots = 8;
static u8 opt_nocd, opt_nodisk, opt_quiet, opt_unload, opt_first;
static u16 ver;

/* ------------------------------------------------------------------ output */

static void putch(char c)
{
    struct rregs r;
    r.ax = 0x0200;
    r.dx = (u8)c;
    intx(0x21, &r);
}

static void puts_(const char *s)
{
    while (*s) {
        if (*s == '\n') putch('\r');
        putch(*s++);
    }
}

static void putdec(u32 v)
{
    char b[12];
    int n = 0;
    do {
        b[n++] = (char)('0' + v % 10);
        v /= 10;
    } while (v);
    while (n) putch(b[--n]);
}

static void puthex(u32 v, int digits)
{
    while (digits--) putch("0123456789ABCDEF"[(v >> (digits * 4)) & 15]);
}

static void msg(const char *s)
{
    if (!opt_quiet) puts_(s);
}

/* ------------------------------------------------------------------ memory */

/* paragraphs of DOS memory, upper memory first; 0 when none */
static u16 dosalloc(u16 paras)
{
    struct rregs r;
    u16 strat, link, seg = 0;
    r.ax = 0x5800;
    intx(0x21, &r);
    strat = r.ax;
    r.ax = 0x5802;
    intx(0x21, &r);
    link = (u8)r.ax;
    r.ax = 0x5803;
    r.bx = 1;
    intx(0x21, &r);
    r.ax = 0x5801;
    r.bx = 0x80;                /* high memory first, then low */
    intx(0x21, &r);
    r.ax = 0x4800;
    r.bx = paras;
    intx(0x21, &r);
    if (!(r.fl & 1)) seg = r.ax;
    r.ax = 0x5801;
    r.bx = strat;
    intx(0x21, &r);
    r.ax = 0x5803;
    r.bx = link;
    intx(0x21, &r);
    return seg;
}

static void dosfree(u16 seg)
{
    struct rregs r;
    r.ax = 0x4900;
    r.es = seg;
    intx(0x21, &r);
}

/* ------------------------------------------------------------------ DOS */

static int dos_setup(void)
{
    struct rregs r;
    u8 FAR *lol;
    r.ax = 0x3000;
    intx(0x21, &r);
    di.major = (u8)r.ax;
    di.minor = (u8)(r.ax >> 8);
    ver = (u16)((di.major << 8) | di.minor);
    if (ver < 0x030A) {
        puts_("WINUDF needs DOS 3.10 or later\n");
        return -1;
    }
    r.ax = 0x5D06;
    intx(0x21, &r);
    di.sda = (u8 FAR *)MK_FP(r.ds, r.si);
    r.ax = 0x5200;
    intx(0x21, &r);
    lol = (u8 FAR *)MK_FP(r.es, r.bx);
    di.cds = *(u8 FAR * FAR *)(lol + 0x16);
    di.lastdrive = lol[0x21];
    if (di.major >= 4) {
        di.cdssize = 0x58;
        di.o_fn1 = 0x9E;
        di.o_fn2 = 0x11E;
        di.o_sattr = 0x24D;
        di.o_curcds = 0x282;
        di.o_extact = 0x2DD;
        di.o_extattr = 0x2DF;
        di.o_extmode = 0x2E1;
        di.o_wfp = 0x292;
        di.o_renwfp = 0x294;
    } else {
        di.cdssize = 0x51;
        di.o_fn1 = 0x92;
        di.o_fn2 = 0x112;
        di.o_sattr = 0x23A;
        di.o_curcds = 0x26C;
        di.o_wfp = 0x27C;
        di.o_renwfp = 0x27E;
    }
    return 0;
}

static void codepage(void)
{
    struct rregs r;
    u16 cp = 437, i, k;
    if (ver >= 0x0303) {
        r.ax = 0x6601;
        intx(0x21, &r);
        if (!(r.fl & 1)) cp = r.bx;
    }
    for (k = 0; k < sizeof(oemcps) / sizeof(oemcps[0]); k++)
        if (oemcps[k].cp == cp) break;
    if (k == sizeof(oemcps) / sizeof(oemcps[0])) k = 0;
    for (i = 0; i < 128; i++) oem[i] = oemcps[k].map[i];
}

/* the CDS entry of drive d */
static u8 FAR *cds_of(u8 d)
{
    return di.cds + (u16)d * di.cdssize;
}

static int drive_free(u8 d)
{
    u8 FAR *c = cds_of(d);
    return !(*(u16 FAR *)(c + 0x43) & 0xC000);
}

static int next_letter(u8 *d)
{
    u8 i, start = 2;
    if (opt_first) start = opt_first;
    for (i = start; i < di.lastdrive; i++) {
        u8 k, taken = 0;
        if (!drive_free(i)) continue;
        for (k = 0; k < nvols; k++)
            if (vols[k].drive == i) taken = 1;
        if (taken) continue;
        *d = i;
        return 0;
    }
    return -1;
}

static void cds_net(u8 d)
{
    u8 FAR *c = cds_of(d);
    u16 i;
    for (i = 0; i < di.cdssize; i++) c[i] = 0;
    c[0] = (u8)('A' + d);
    c[1] = ':';
    c[2] = '\\';
    *(u16 FAR *)(c + 0x43) = 0xC000;
    *(u16 FAR *)(c + 0x4F) = 2;
}

/* ------------------------------------------------------------------ cache */

static int cache_setup(u16 slot)
{
    u16 paras = (u16)(((u32)(nslots + 1) * slot) >> 4), seg, i;
    seg = dosalloc(paras);
    if (!seg) return -1;
    udf_cache_init(slot);
    for (i = 0; i < nslots; i++) udf_cache_slot((u8 FAR *)MK_FP(seg + i * (slot >> 4), 0));
    udf_tmp = (u8 FAR *)MK_FP(seg + nslots * (slot >> 4), 0);
    return 0;
}

/* ------------------------------------------------------------------ disks */

static const char *kindname(struct dvol *dv)
{
    if (dv->kind == DV_CD) return "CD/DVD/BD drive";
    if (dv->kind == DV_BIOS) return "BIOS disk ";
    return dv->mplex == 0xD4 ? "USB disk " : "disk ";
}

static void report(struct dvol *dv)
{
    struct udf_vol *v = &dv->v;
    if (opt_quiet) return;
    putch(' ');
    putch(' ');
    putch((char)('A' + dv->drive));
    puts_(": UDF ");
    putch((char)('0' + (v->rev >> 8)));
    putch('.');
    puthex(v->rev & 0xFF, 2);
    puts_(" '");
    puts_(v->label);
    puts_("', ");
    puts_(kindname(dv));
    if (dv->kind == DV_BIOS) puthex(dv->unit, 2);
    else if (dv->kind == DV_WIN) putdec(dv->unit);
    if (dv->part) {
        puts_(" partition ");
        putdec(dv->part);
    }
    puts_((v->flags & VF_RDONLY) ? ", read-only" : ", read/write");
    if (v->flags & VF_WASOPEN) puts_(" (not closed: read-only)");
    puts_("\n");
}

/* the same volume through another way to the disk? */
static int duplicate(struct dvol *dv)
{
    u8 i;
    for (i = 0; i < nvols; i++) {
        struct udf_vol *a = &vols[i].v, *b = &dv->v;
        u16 k;
        if (&vols[i] == dv || vols[i].kind == DV_CD || !vols[i].mounted) continue;
        if (a->serial != b->serial || a->nblocks != b->nblocks || a->bs != b->bs) continue;
        for (k = 0; k < 32 && a->label[k] == b->label[k] && a->label[k]; k++)
            ;
        if (a->label[k] == b->label[k]) return 1;
    }
    return 0;
}

/* try a UDF volume at sector base (n sectors) of the disk set up in vols[nvols] */
static void try_volume(u64 base, u64 n, u8 part)
{
    struct dvol *dv = &vols[nvols];
    u8 d;
    if (nvols >= MAXVOL) return;
    if (n > 0xFFFFFFFFUL) n = 0xFFFFFFFFUL;
    if (udf_mount(&dv->v, &dv->dev, base, (u32)n)) return;
    if (duplicate(dv)) {
        udf_cache_drop(&dv->v);
        return;
    }
    if (next_letter(&d)) {
        msg("  no free drive letter for a UDF volume (LASTDRIVE)\n");
        udf_cache_drop(&dv->v);
        return;
    }
    dv->drive = d;
    dv->part = part;
    dv->mounted = 1;
    cds_net(d);
    report(dv);
    nvols++;
    if (nvols < MAXVOL) {
        vols[nvols] = *dv;
        vols[nvols].mounted = 0;
        dvol_setdev(&vols[nvols]);
    }
}

static int rd(u64 lba, u8 FAR *buf)
{
    struct dvol *dv = &vols[nvols];
    return dv->dev.read(&dv->dev, lba, 1, buf);
}

static void scan_gpt(u64 total)
{
    struct dvol *dv = &vols[nvols];
    u8 FAR *b = tmpbuf;
    u64 elba;
    u32 cnt, esz, i, per;
    u8 part = 0;
    if (rd(1, b)) return;
    if (b[0] != 'E' || b[1] != 'F' || b[2] != 'I' || b[3] != ' ' || b[4] != 'P') return;
    elba = G64(b, 72);
    cnt = G32(b, 80);
    esz = G32(b, 84);
    if (esz < 128 || esz > dv->dev.ssize || cnt > 256) return;
    per = dv->dev.ssize / esz;
    for (i = 0; i < cnt; i++) {
        u8 FAR *e;
        u16 k;
        int used = 0;
        if (i % per == 0 && rd(elba + i / per, b)) return;
        e = b + (u16)((i % per) * esz);
        for (k = 0; k < 16; k++)
            if (e[k]) used = 1;
        if (!used) continue;
        part++;
        {
            u64 first = G64(e, 32), last = G64(e, 40);
            if (last >= first && last < total) try_volume(first, last - first + 1, part);
        }
        /* the entries sector was overwritten by the mount: read it again */
        if (i + 1 < cnt && (i + 1) % per != 0 && rd(elba + i / per, b)) return;
    }
}

static void scan_mbr(u64 total)
{
    u8 FAR *b = tmpbuf;
    u32 st[4], ln[4], ext = 0, ebr;
    u8 ty[4], i, part = 0;
    int hops = 0;
    if (rd(0, b) || b[510] != 0x55 || b[511] != 0xAA) return;
    for (i = 0; i < 4; i++) {
        u8 FAR *e = b + 446 + i * 16;
        ty[i] = e[4];
        st[i] = G32(e, 8);
        ln[i] = G32(e, 12);
    }
    for (i = 0; i < 4; i++)
        if (ty[i] == 0xEE) {
            scan_gpt(total);
            return;
        }
    for (i = 0; i < 4; i++) {
        if (!ty[i] || !ln[i]) continue;
        part++;
        if (ty[i] == 0x05 || ty[i] == 0x0F || ty[i] == 0x85) {
            if (!ext) ext = st[i];
            continue;
        }
        if (st[i] && (u64)st[i] + ln[i] <= total) try_volume(st[i], ln[i], part);
    }
    ebr = ext;
    while (ebr && hops++ < 64) {
        u32 s, l, nx;
        u8 t;
        if (rd(ebr, b) || b[510] != 0x55 || b[511] != 0xAA) return;
        t = b[446 + 4];
        s = G32(b, 446 + 8);
        l = G32(b, 446 + 12);
        nx = G32(b, 446 + 16 + 8);
        if (b[446 + 16 + 4] == 0) nx = 0;
        part++;
        if (t && l) try_volume((u64)ebr + s, l, part);
        ebr = nx ? ext + nx : 0;
    }
}

static void scan_disk(u64 total)
{
    struct dvol *dv = &vols[nvols];
    u8 before = nvols;
    dv->dev.sectors = total;
    try_volume(0, total, 0);
    if (nvols != before) return;
    scan_mbr(total);
}

static void scan_bios(void)
{
    struct rregs r;
    u8 ndisk, u;
    static struct {
        u16 size, flags;
        u32 cyl, heads, spt;
        u32 sect_lo, sect_hi;
        u16 bps;
    } dp;
    r.ax = 0x0800;
    r.dx = 0x80;
    r.es = 0;
    r.di = 0;
    intx(0x13, &r);
    if (r.fl & 1) return;
    ndisk = (u8)r.dx;
    for (u = 0; u < ndisk && u < 16 && nvols < MAXVOL; u++) {
        struct dvol *dv = &vols[nvols];
        u64 total;
        r.ax = 0x4100;
        r.bx = 0x55AA;
        r.dx = (u16)(0x80 + u);
        intx(0x13, &r);
        if ((r.fl & 1) || r.bx != 0xAA55 || !(r.cx & 1)) continue;
        dp.size = 0x1A;
        r.ax = 0x4800;
        r.dx = (u16)(0x80 + u);
        r.ds = FP_SEG((void FAR *)&dp);
        r.si = FP_OFF((void FAR *)&dp);
        intx(0x13, &r);
        if (r.fl & 1) continue;
        total = ((u64)dp.sect_hi << 32) | dp.sect_lo;
        if (!total || (dp.bps != 512 && dp.bps != 2048 && dp.bps != 4096)) continue;
        dv->kind = DV_BIOS;
        dv->unit = (u16)(0x80 + u);
        dv->dev.ssize = dp.bps;
        dv->dev.optical = 0;
        dv->dev.ro = 0;
        dvol_setdev(dv);
        scan_disk(total);
    }
}

struct winreq {
    u16 fn, status;
    u32 arg0, arg1, arg2, buflen, retlen, buf, rsv;
};

static void scan_win(u8 mplex)
{
    struct rregs r;
    static struct winreq q;
    static u8 info[64];
    u8 FAR *disks = tmpbuf;
    u8 n, i;
    u16 rec;
    r.ax = (u16)(mplex << 8);
    r.bx = 0;
    intx(0x2F, &r);
    if ((u8)r.ax != 0xFF || r.bx != 0x3357) return;
    q.fn = 0x0001;
    q.buflen = sizeof(info);
    q.buf = ((u32)FP_SEG((void FAR *)info) << 16) | FP_OFF((void FAR *)info);
    if (win_call(mplex, &q) != 0) return;
    n = info[8];
    if (!n || n > 16) return;
    q.fn = 0x0020;
    q.buflen = 4096;
    q.buf = ((u32)FP_SEG(disks) << 16) | FP_OFF(disks);
    q.retlen = 0;
    if (win_call(mplex, &q) != 0 || !q.retlen) return;
    rec = (u16)(q.retlen / n);
    if (rec < 104) return;
    for (i = 0; i < n && nvols < MAXVOL; i++) {
        u8 FAR *e = disks + (u16)i * rec;
        struct dvol *dv = &vols[nvols];
        u64 total = ((u64)G32(e, 8) << 32) | G32(e, 4);
        u32 ss = G32(e, 12);
        if (!e[2] || (ss != 512 && ss != 4096 && ss != 2048)) continue;
        dv->kind = DV_WIN;
        dv->mplex = mplex;
        dv->unit = e[0];
        dv->dev.ssize = (u16)ss;
        dv->dev.optical = 0;
        dv->dev.ro = 0;
        dvol_setdev(dv);
        scan_disk(total);
    }
}

static void scan_cd(void)
{
    struct rregs r;
    static u8 letters[26];
    static u8 devlist[26 * 5];
    u16 n, i;
    r.ax = 0x1500;
    r.bx = 0;
    intx(0x2F, &r);
    n = r.bx;
    if (!n) {
        msg("  MSCDEX is not loaded: no CD/DVD drives\n");
        return;
    }
    r.ax = 0x150D;
    r.es = FP_SEG((void FAR *)letters);
    r.bx = FP_OFF((void FAR *)letters);
    intx(0x2F, &r);
    /* device driver and unit of each drive: the driver is called directly */
    r.ax = 0x1501;
    r.es = FP_SEG((void FAR *)devlist);
    r.bx = FP_OFF((void FAR *)devlist);
    intx(0x2F, &r);
    for (i = 0; i < n && i < 26 && nvols < MAXVOL; i++) {
        struct dvol *dv = &vols[nvols];
        dv->subunit = devlist[i * 5];
        dv->devhdr = *(void FAR * FAR *)(devlist + i * 5 + 1);
        dv->kind = DV_CD;
        dv->unit = letters[i];
        dv->drive = letters[i];
        dvol_setdev(dv);
        nvols++;
        if (cd_mount(dv) == 0) {
            report(dv);
        } else if (!opt_quiet) {
            puts_("  ");
            putch((char)('A' + dv->drive));
            puts_(": CD/DVD/BD drive (UDF discs)\n");
        }
    }
}

/* ------------------------------------------------------------------ main */

static void usage(void)
{
    puts_("Usage: WINUDF [/U] [/R] [/C:n] [/L:x] [/NOCD] [/NODISK] [/Q]\n"
          "  /U       unload\n"
          "  /R       all volumes read-only\n"
          "  /C:n     n blocks of cache (4-32, default 8)\n"
          "  /L:x     drive letters of UDF disks from x on\n"
          "  /NOCD    no CD/DVD/BD drives    /NODISK  no disks\n"
          "  /Q       quiet\n");
}

static int args(void)
{
    char FAR *p = (char FAR *)MK_FP(FP_SEG((void FAR *)&di), 0x81);
    u8 len = *(u8 FAR *)MK_FP(FP_SEG((void FAR *)&di), 0x80);
    char FAR *end = p + len;
    while (p < end) {
        char c;
        if (*p == ' ' || *p == '\t') {
            p++;
            continue;
        }
        if (*p != '/' && *p != '-') return -1;
        p++;
        c = (char)(*p >= 'a' && *p <= 'z' ? *p - 32 : *p);
        if (c == 'U') opt_unload = 1;
        else if (c == 'R') udf_rdonly_all = 1;
        else if (c == 'Q') opt_quiet = 1;
        else if (c == 'C' && p[1] == ':') {
            u16 n = 0;
            p += 2;
            while (p < end && *p >= '0' && *p <= '9') n = (u16)(n * 10 + (*p++ - '0'));
            if (n < 4 || n > 32) return -1;
            nslots = n;
            continue;
        } else if (c == 'L' && p[1] == ':') {
            char l = p[2];
            if (l >= 'a' && l <= 'z') l = (char)(l - 32);
            if (l < 'C' || l > 'Z') return -1;
            opt_first = (u8)(l - 'A');
            p += 3;
            continue;
        } else if (c == 'N' && (p[1] == 'O' || p[1] == 'o')) {
            char x = p[2];
            if (x == 'C' || x == 'c') opt_nocd = 1;
            else if (x == 'D' || x == 'd') opt_nodisk = 1;
            else return -1;
        } else if (c == '?') {
            return -1;
        } else {
            return -1;
        }
        while (p < end && *p != ' ' && *p != '/' && *p != '\t') p++;
    }
    return 0;
}

static int unload(void)
{
    struct rregs r;
    u16 seg;
    r.ax = 0xD600;
    r.bx = 0;
    intx(0x2F, &r);
    if ((u8)r.ax != 0xFF || r.bx != 0x5544) {
        puts_("WINUDF is not loaded\n");
        return 1;
    }
    r.ax = 0xD601;
    intx(0x2F, &r);
    if (r.ax != 0) {
        puts_("WINUDF cannot be unloaded: a program loaded after it uses INT 2Fh\n");
        return 1;
    }
    seg = r.es;
    /* the cache block and the program (its PSP is 10h paragraphs below the code) */
    {
        u8 FAR *mcb = (u8 FAR *)MK_FP(0, 0);
        u16 owner = (u16)(seg - 0x10), m;
        struct rregs q;
        q.ax = 0x5200;
        intx(0x21, &q);
        m = *(u16 FAR *)MK_FP(q.es, q.bx - 2);
        while (1) {
            u16 next;
            mcb = (u8 FAR *)MK_FP(m, 0);
            next = (u16)(m + 1 + *(u16 FAR *)(mcb + 3));
            if (*(u16 FAR *)(mcb + 1) == owner && (u16)(m + 1) != owner) dosfree((u16)(m + 1));
            if (mcb[0] == 'Z') break;
            m = next;
        }
        dosfree(owner);
    }
    puts_("WINUDF unloaded\n");
    return 0;
}

int __cdecl dos_init(void)
{
    struct rregs r;
    u16 bsmax = 2048, i;
    puts_("WINUDF 1.0 - UDF file system for DOS and Windows 1.0-3.11 (UDF 1.02-2.60)\n");
    if (args()) {
        usage();
        exitcode = 1;
        return 0;
    }
    if (dos_setup()) {
        exitcode = 1;
        return 0;
    }
    if (opt_unload) {
        exitcode = (u8)unload();
        return 0;
    }
    r.ax = 0x1600;
    intx(0x2F, &r);
    if ((u8)r.ax >= 4 && (u8)r.ax != 0x80 && (u8)r.ax != 0xFF) {
        puts_("Windows 95/98/ME is running: it reads UDF volumes with WINUDF.VXD\n");
        exitcode = 1;
        return 0;
    }
    r.ax = 0xD600;
    r.bx = 0;
    intx(0x2F, &r);
    if ((u8)r.ax == 0xFF && r.bx == 0x5544) {
        puts_("WINUDF is already loaded\n");
        exitcode = 1;
        return 0;
    }
    codepage();
    udf_osclass = 1;
    /* the program block: only the image (DOS gave all memory to the .COM) */
    r.ax = 0x4A00;
    r.es = FP_SEG((void FAR *)&di);
    r.bx = (u16)(((u16)(void *)imgend + 15) >> 4);
    intx(0x21, &r);
    /* a cache for the scan: the largest block size decides the size of the cache blocks */
    if (cache_setup(4096)) {
        puts_("WINUDF: not enough memory\n");
        exitcode = 1;
        return 0;
    }
    tmpbuf = (u8 FAR *)udf_tmp;
    nvols = 0;
    for (i = 0; i < MAXVOL; i++) vols[i].mounted = 0;
    if (!opt_nocd) scan_cd();
    if (!opt_nodisk) {
        dvol_setdev(&vols[nvols]);
        scan_win(0xD3);
        scan_win(0xD4);
        scan_bios();
    }
    for (i = 0; i < nvols; i++)
        if (vols[i].mounted && vols[i].v.bs > bsmax) bsmax = vols[i].v.bs;
    if (bsmax < 4096) {
        /* smaller cache blocks: set up again and mount again */
        u16 seg = FP_SEG(tmpbuf) - nslots * (4096 >> 4);
        dosfree(seg);
        if (cache_setup(bsmax)) {
            puts_("WINUDF: not enough memory\n");
            exitcode = 1;
            return 0;
        }
        for (i = 0; i < nvols; i++) {
            struct dvol *dv = &vols[i];
            if (!dv->mounted) continue;
            if (dv->kind == DV_CD) cd_mount(dv);
            else if (udf_mount(&dv->v, &dv->dev, dv->v.base, dv->v.nblocks << dv->v.ssh)) dv->mounted = 0;
        }
    }
    if (!nvols) {
        puts_("No CD/DVD drives and no UDF volumes: WINUDF not loaded\n");
        exitcode = 1;
        return 0;
    }
    hook2f();
    /* the environment is not needed */
    {
        u16 env = *(u16 FAR *)MK_FP(FP_SEG((void FAR *)&di), 0x2C);
        if (env) {
            dosfree(env);
            *(u16 FAR *)MK_FP(FP_SEG((void FAR *)&di), 0x2C) = 0;
        }
    }
    if (!opt_quiet) {
        puts_("WINUDF resident, ");
        putdec(((u32)(u16)(u32)(void *)resend + 15) >> 4 << 4);
        puts_(" bytes + ");
        putdec((u32)(nslots + 1) * bsmax);
        puts_(" bytes of cache\n");
    }
    return (int)(((u16)(void *)resend + 15) >> 4);
}
