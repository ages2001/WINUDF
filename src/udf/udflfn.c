/* udflfn.c - WINUDF.EXE, resident part: the long file name functions of INT 21h (AX=71xxh)
   for UDF drives in plain DOS, as Windows 95/98/ME offer them in their MS-DOS boxes, and
   the 64-bit file position of Enhanced DR-DOS (AX=7142h) for files on UDF drives (UDF files
   can be larger than 4 GB). DOS never passes these functions to a redirector: WINUDF hooks
   INT 21h and serves them itself (the code that DOSLFN has for FAT drives). Part of
   udfdos.c (it shares its helpers and the node, entry and name buffers).

   Paths may have long names in every component, '/' or '\', "." and "..". Files are opened
   by the DOS function 6Ch with their 8.3 names (DOS then calls the redirector as usual);
   a new file gets its long name first. Search handles and the high dwords of the file
   positions live in small tables. */

#define LFN_HANDLES     8
#define LFN_HBASE       0x5D00      /* search handles: LFN_HBASE + index */
#define LFN_PATH        261

static struct lsearch {
    u8 used, vol, attr, req, state;     /* state: 0 ".", 1 "..", 2 entries */
    struct udf_loc dir;
    u32 pos;
    char pat[LFN_PATH];
} lsr[LFN_HANDLES];

/* high dwords of the file positions beyond 4 GB (SFTs of UDF files) */
#define LFN_HI          8
static struct {
    u8 FAR *sft;
    u32 hi;
} lhi[LFN_HI];

static char lpath[LFN_PATH], lpath2[LFN_PATH], lshort[LFN_PATH];
static char lname[LFN_PATH];

/* an error, also as DOS's extended error (INT 21h AH=59h: COMMAND.COM asks it) */
static void lfail(u16 err)
{
    u8 FAR *sda = di.sda;
    lr.ax = err;
    lr.fl |= 1;
    sda[3] = 2;                                     /* locus: block device */
    *(u16 FAR *)(sda + 4) = err;
    sda[6] = 3;                                     /* action: user intervention */
    sda[7] = (u8)(err == 2 || err == 3 || err == 0x12 ? 8 : err == 5 ? 3 : 0x0D);
}

static void lok(void)
{
    lr.fl &= ~1;
}

static u16 lstrlen(const char *s)
{
    u16 n = 0;
    while (s[n]) n++;
    return n;
}

static char upc(char c)
{
    return (char)(c >= 'a' && c <= 'z' ? c - 32 : c);
}

/* Unicode -> OEM ('_' when the code page has no such character) */
static u16 to_oem(const u16 *u, u16 n, char *out, u16 max)
{
    u16 i, k;
    for (i = 0; i < n && i + 1 < max; i++) {
        u16 c = u[i];
        char o = '_';
        if (c < 0x80) {
            o = (char)c;
        } else {
            for (k = 0; k < 128; k++)
                if (oem[k] == c) {
                    o = (char)(0x80 + k);
                    break;
                }
        }
        out[i] = o;
    }
    out[i] = 0;
    return i;
}

/* ------------------------------------------------------------------ paths */

/* drive of a path: X: or the current drive; *rest after the drive. 0xFF: UNC path */
static u8 lfn_drive(const char FAR *in, const char FAR **rest)
{
    struct rregs r;
    if (in[0] && in[1] == ':') {
        *rest = in + 2;
        return (u8)(upc(in[0]) - 'A');
    }
    *rest = in;
    if ((in[0] == '\\' || in[0] == '/') && (in[1] == '\\' || in[1] == '/')) return 0xFF;
    r.ax = 0x1900;
    intx(0x21, &r);
    return (u8)r.ax;
}

/* current directory of a drive from its CDS: "\DIR\SUB" (8.3 names) or "" */
static const char FAR *cds_dir(u8 drive)
{
    u8 FAR *c = di.cds + (u16)drive * di.cdssize;
    u16 ro = *(u16 FAR *)(c + 0x4F);
    if (ro > 66) ro = 2;
    return (const char FAR *)(c + ro);
}

/* append the components of s (separated by '\' or '/') to the canonical path out
   ("\A\B", "" for the root): ".." goes up, "." stays */
static int lfn_add(char *out, const char FAR *s)
{
    u16 n = (u16)lstrlen(out);
    while (*s) {
        const char FAR *e = s;
        u16 len;
        while (*e && *e != '\\' && *e != '/') e++;
        len = (u16)(e - s);
        if (len == 1 && s[0] == '.') {
            /* the same directory */
        } else if (len == 2 && s[0] == '.' && s[1] == '.') {
            while (n && out[n - 1] != '\\') n--;
            if (n) n--;
            out[n] = 0;
        } else if (len) {
            /* trailing dots and spaces of a name do not count (Windows) */
            while (len && (s[len - 1] == '.' || s[len - 1] == ' ')) len--;
            if (!len) return 3;
            if (n + 1 + len >= LFN_PATH - 3) return 3;
            out[n++] = '\\';
            while (len--) out[n++] = *s++;
            out[n] = 0;
        }
        s = e;
        while (*s == '\\' || *s == '/') s++;
    }
    return 0;
}

/* the canonical path of in on a UDF drive into out; NULL when the drive is not ours */
static struct dvol *lfn_canon(const char FAR *in, char *out, u8 *drive, u16 *err)
{
    const char FAR *rest;
    struct dvol *dv;
    u8 d = lfn_drive(in, &rest);
    *err = 0;
    if (d == 0xFF || d >= di.lastdrive || !(dv = vol_of(d))) return 0;
    *drive = d;
    out[0] = 0;
    if (*rest != '\\' && *rest != '/') *err = (u16)lfn_add(out, cds_dir(d));
    if (!*err) *err = (u16)lfn_add(out, rest);
    return dv;
}

/* walk a canonical path ("" is the root) */
static int lwalk(struct dvol *dv, const char *path, const char FAR **last)
{
    return walk(dv, *path ? (const char FAR *)path : (const char FAR *)"\\", 1, last);
}

/* the path with the 8.3 names of every component ("\DIR\FILE.TXT"); the last component may
   be missing (name83 of the new file then). 0 or a DOS error */
static u16 lfn_short(struct dvol *dv, const char *path, char *out)
{
    struct udf_node dir;
    const char *s = path;
    u16 n = 0, k;
    out[0] = 0;
    if (udf_iget(&dv->v, dv->v.root, &dir)) return 0x15;
    while (*s == '\\') s++;
    while (*s) {
        const char *e = s;
        int r;
        while (*e && *e != '\\') e++;
        k = to_uni((const char FAR *)s, (u16)(e - s), uname);
        r = udf_lookup(&dv->v, &dir, uname, k, &fid, oem);
        if (r <= 0) return *e ? 3 : 2;
        udf_dosname(fid.name, fid.nlen, dname, oem);
        if (n + 14 >= LFN_PATH) return 3;
        out[n++] = '\\';
        for (k = 0; dname[k]; k++) out[n++] = dname[k];
        out[n] = 0;
        if (*e && (r = udf_iget(&dv->v, fid.icb, &dir)) != 0) return 3;
        s = e;
        while (*s == '\\') s++;
    }
    return 0;
}

/* the path with the long names (an 8.3 path from a CDS) */
static u16 lfn_long(struct dvol *dv, const char *path, char *out)
{
    struct udf_node dir;
    const char *s = path;
    u16 n = 0, k;
    out[0] = 0;
    if (udf_iget(&dv->v, dv->v.root, &dir)) return 0x15;
    while (*s == '\\') s++;
    while (*s) {
        const char *e = s;
        int r;
        while (*e && *e != '\\') e++;
        k = to_uni((const char FAR *)s, (u16)(e - s), uname);
        r = udf_lookup(&dv->v, &dir, uname, k, &fid, oem);
        if (r <= 0) return 3;
        if (n + 2 >= LFN_PATH) return 3;
        out[n++] = '\\';
        n = (u16)(n + to_oem(fid.name, fid.nlen, out + n, (u16)(LFN_PATH - n)));
        if (*e && udf_iget(&dv->v, fid.icb, &dir)) return 3;
        s = e;
        while (*s == '\\') s++;
    }
    return 0;
}

/* split a canonical path: the last component (the pattern or name) */
static const char *lfn_last(const char *path)
{
    const char *p = path + lstrlen(path);
    while (p > path && p[-1] != '\\') p--;
    return p;
}

/* ------------------------------------------------------------------ wildcards */

/* Windows matching of long names: '*' any characters, '?' one character (or none at the
   end); "*.*" and "*" match everything, a name without a dot matches "name.*" */
static int lmatch(const char *p, const char *n)
{
    while (*p) {
        if (*p == '*') {
            while (*p == '*') p++;
            if (!*p) return 1;
            if (p[0] == '.' && p[1] == '*' && !p[2]) return 1;
            for (; *n; n++)
                if (lmatch(p, n)) return 1;
            return lmatch(p, n);
        }
        if (*p == '?') {
            if (*n && *n != '.') n++;
            else if (*n == '.' && p[1] != '.' && p[1] != 0) n++;
            p++;
            continue;
        }
        if (*p == '.' && !*n) {
            p++;
            continue;
        }
        if (upc(*p) != upc(*n)) return 0;
        p++;
        n++;
    }
    return !*n;
}

static int lwild(const char *s)
{
    for (; *s; s++)
        if (*s == '*' || *s == '?') return 1;
    return 0;
}

/* ------------------------------------------------------------------ times */

/* days from 1601-01-01 to y-m-d */
static u32 days1601(u16 y, u8 m, u8 d)
{
    static const u16 cum[12] = { 0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334 };
    u32 yy = (u32)y - 1601;
    u32 days = yy * 365 + yy / 4 - yy / 100 + yy / 400;
    if (m < 1 || m > 12) m = 1;
    days += cum[m - 1] + d - 1;
    if (m > 2 && ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0)) days++;
    return days;
}

/* FILETIME (100 ns since 1601) of a UDF time, as local time like the DOS times */
static void filetime(const struct udf_time *t, u8 FAR *out)
{
    u64 v;
    u16 y = t->year < 1601 ? 1601 : t->year;
    v = (u64)days1601(y, t->month, t->day) * 86400UL + (u32)t->hour * 3600UL + (u32)t->min * 60 + t->sec;
    v = v * 10000000UL + (u32)t->cs * 100000UL;
    udf_p64(out, v);
}

static void dostime_ft(u16 date, u16 time, u8 cs, u8 FAR *out)
{
    struct udf_time t;
    from_dos(date, time, &t);
    t.cs = cs;
    filetime(&t, out);
}

/* FILETIME -> DOS date / time and hundredths */
static void ft_dos(const u8 FAR *ft, u16 *date, u16 *time, u8 *cs)
{
    u64 v = G64(ft, 0);
    u32 secs, days, rem;
    u16 y = 1601;
    u8 m = 1;
    static const u8 ml[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    *cs = (u8)((v / 100000UL) % 100);
    v /= 10000000UL;
    days = (u32)(v / 86400UL);
    secs = (u32)(v % 86400UL);
    for (;;) {
        u16 yl = (u16)(((y % 4 == 0 && y % 100 != 0) || y % 400 == 0) ? 366 : 365);
        if (days < yl) break;
        days -= yl;
        y++;
    }
    for (;;) {
        u8 l = ml[m - 1];
        if (m == 2 && ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0)) l++;
        if (days < l) break;
        days -= l;
        m++;
    }
    rem = secs;
    if (y < 1980) {
        *date = (1 << 5) | 1;
        *time = 0;
        return;
    }
    *date = (u16)(((y - 1980) << 9) | (m << 5) | (days + 1));
    *time = (u16)(((rem / 3600) << 11) | (((rem / 60) % 60) << 5) | ((rem % 60) >> 1));
    if (rem & 1) *cs = (u8)(*cs + 100);
}

/* ------------------------------------------------------------------ find */

static u8 nd_attr(struct dvol *dv)
{
    u8 a = udf_dosattr(&nd, fid.fchar);
    if ((dv->v.flags & VF_RDONLY) && !(a & 0x10)) a |= 0x01;
    return a;
}

/* WIN32_FIND_DATA of nd / fid (or "." / ".." of pd) */
static void put_find(u8 FAR *b, struct dvol *dv, int dot, u16 fmt)
{
    struct udf_node *n = dot ? &pd : &nd;
    u8 a = dot ? 0x10 : nd_attr(dv);
    u16 i;
    for (i = 0; i < 318; i++) b[i] = 0;
    b[0] = a;
    if (fmt == 1) {
        *(u16 FAR *)(b + 0x04) = dos_time(&n->ctime);
        *(u16 FAR *)(b + 0x06) = dos_date(&n->ctime);
        *(u16 FAR *)(b + 0x0C) = dos_time(&n->atime);
        *(u16 FAR *)(b + 0x0E) = dos_date(&n->atime);
        *(u16 FAR *)(b + 0x14) = dos_time(&n->mtime);
        *(u16 FAR *)(b + 0x16) = dos_date(&n->mtime);
    } else {
        filetime(&n->ctime, b + 0x04);
        filetime(&n->atime, b + 0x0C);
        filetime(&n->mtime, b + 0x14);
    }
    if (!(a & 0x10)) {
        *(u32 FAR *)(b + 0x1C) = (u32)(n->size >> 32);
        *(u32 FAR *)(b + 0x20) = (u32)n->size;
    }
    if (dot) {
        b[0x2C] = '.';
        if (dot == 2) b[0x2D] = '.';
        return;
    }
    to_oem(fid.name, fid.nlen, lname, LFN_PATH);
    for (i = 0; lname[i]; i++) b[0x2C + i] = lname[i];
    udf_dosname(fid.name, fid.nlen, dname, oem);
    /* the 8.3 name when the long name is not one */
    for (i = 0; dname[i] && lname[i] == dname[i]; i++)
        ;
    if (dname[i] || lname[i])
        for (i = 0; dname[i]; i++) b[0x130 + i] = dname[i];
}

/* the next entry of search s into the buffer; 0 or a DOS error (12h: no more) */
static u16 lfind_next(struct lsearch *s, u8 FAR *b, u16 fmt)
{
    struct dvol *dv = &vols[s->vol];
    int r;
    if (s->vol >= nvols || !dv->mounted) return 0x12;
    if (udf_iget(&dv->v, s->dir, &pd)) return 0x12;
    while (s->state < 2) {
        u8 st = s->state++;
        if (!(s->attr & 0x10) || !lmatch(s->pat, st ? ".." : ".")) continue;
        put_find(b, dv, st + 1, fmt);
        return 0;
    }
    for (;;) {
        u8 a;
        r = udf_readdir(&dv->v, &pd, &s->pos, &fid);
        if (r <= 0) return 0x12;
        if (fid.fchar & (FC_PARENT | FC_DELETED)) continue;
        to_oem(fid.name, fid.nlen, lname, LFN_PATH);
        udf_dosname(fid.name, fid.nlen, dname, oem);
        if (!lmatch(s->pat, lname) && !lmatch(s->pat, dname)) continue;
        if (udf_iget(&dv->v, fid.icb, &nd)) continue;
        a = nd_attr(dv);
        if ((a & 0x16) & ~s->attr) continue;
        if ((s->req & 0x3F) & ~a) continue;
        put_find(b, dv, 0, fmt);
        return 0;
    }
}

static void l_findfirst(struct dvol *dv)
{
    u8 FAR *b = (u8 FAR *)MK_FP(lr.es, lr.di);
    const char *pat;
    struct lsearch *s = 0;
    u16 i, e;
    int r;
    for (i = 0; i < LFN_HANDLES; i++)
        if (!lsr[i].used) {
            s = &lsr[i];
            break;
        }
    if (!s) {
        lfail(4);
        return;
    }
    pat = lfn_last(lpath);
    if (!*pat) {
        lfail(2);           /* no name: "X:\" alone */
        return;
    }
    {
        /* the directory: everything before the last component */
        char save = *(char *)(pat - 1);
        *(char *)(pat - 1) = 0;
        r = lwalk(dv, lpath, 0);
        *(char *)(pat - 1) = save;
    }
    if (r || nd.ftype != FT_DIR) {
        lfail(3);
        return;
    }
    s->vol = (u8)(dv - vols);
    s->dir = nd.loc;
    s->pos = 0;
    s->attr = (u8)lr.cx;
    s->req = (u8)(lr.cx >> 8);
    s->state = (u8)((nd.loc.lbn == dv->v.root.lbn && nd.loc.part == dv->v.root.part) ? 2 : 0);
    for (i = 0; pat[i] && i < LFN_PATH - 1; i++) s->pat[i] = pat[i];
    s->pat[i] = 0;
    if (s->attr == 0x08) {
        lfail(0x12);        /* the volume label alone: none on UDF drives here */
        return;
    }
    e = lfind_next(s, b, lr.si);
    if (e) {
        lfail(e == 0x12 ? 0x12 : e);
        return;
    }
    s->used = 1;
    lr.ax = (u16)(LFN_HBASE + (s - lsr));
    lr.cx = 0;
    lok();
}

/* ------------------------------------------------------------------ handles */

/* SFT of a handle of the running program */
static u8 FAR *sft_of(u16 h)
{
    struct rregs r;
    u8 FAR *j;
    r.ax = 0x1220;
    r.bx = h;
    intx(0x2F, &r);
    if (r.fl & 1) return 0;
    j = (u8 FAR *)MK_FP(r.es, r.di);
    if (*j == 0xFF) return 0;
    r.ax = 0x1216;
    r.bx = *j;
    intx(0x2F, &r);
    if (r.fl & 1) return 0;
    return (u8 FAR *)MK_FP(r.es, r.di);
}

static int same_sft(u8 FAR *a, u8 FAR *b)
{
    return ((u32)FP_SEG(a) << 4) + FP_OFF(a) == ((u32)FP_SEG(b) << 4) + FP_OFF(b);
}

/* the high dword of the position of a file (0 when below 4 GB) */
u32 lfn_hi(u8 FAR *sft)
{
    u16 i;
    for (i = 0; i < LFN_HI; i++)
        if (lhi[i].sft && same_sft(lhi[i].sft, sft)) return lhi[i].hi;
    return 0;
}

/* set it; 0 or -1 when the table is full */
int lfn_sethi(u8 FAR *sft, u32 hi)
{
    u16 i, f = LFN_HI;
    for (i = 0; i < LFN_HI; i++) {
        if (lhi[i].sft && same_sft(lhi[i].sft, sft)) {
            if (hi) lhi[i].hi = hi;
            else lhi[i].sft = 0;
            return 0;
        }
        if (!lhi[i].sft && f == LFN_HI) f = i;
    }
    if (!hi) return 0;
    if (f == LFN_HI) return -1;
    lhi[f].sft = sft;
    lhi[f].hi = hi;
    return 0;
}

/* ------------------------------------------------------------------ functions */

static void l_mkdir(struct dvol *dv)
{
    const char FAR *last;
    struct udf_time t;
    u16 n;
    int r = lwalk(dv, lpath, &last);
    if (r == 0) {
        lfail(5);
        return;
    }
    if (r != UE_NOTFOUND || lwild(lfn_last(lpath)) || !*lfn_last(lpath)) {
        lfail(3);
        return;
    }
    n = to_uni(last, (u16)lstrlen(lfn_last(lpath)), uname);
    udf_now(&t);
    r = udf_create(&dv->v, &pd, uname, n, 1, 0, &t, &nd, 0);
    udf_sync(&dv->v);
    if (r) lfail(r == UE_NOSPACE ? 0x52 : dos_err(r));
    else lok();
}

static void l_rmdir(struct dvol *dv)
{
    int r = lwalk(dv, lpath, 0);
    if (r) {
        lfail(3);
        return;
    }
    if (nd.ftype != FT_DIR || fid.pos == 0xFFFFFFFFUL) {
        lfail(5);
        return;
    }
    if (!udf_dirempty(&dv->v, &nd)) {
        lfail(5);
        return;
    }
    r = udf_unlink(&dv->v, &pd, fid.pos);
    udf_sync(&dv->v);
    if (r) lfail(dos_err(r));
    else lok();
}

static void l_delete(struct dvol *dv)
{
    const char *pat = lfn_last(lpath);
    struct udf_node dir;
    u32 pos = 0;
    u8 sattr = (u8)lr.cx, req = (u8)(lr.cx >> 8);
    int r, n = 0;
    if (!lwild(pat) || !lr.si) {
        r = lwalk(dv, lpath, 0);
        if (r) {
            lfail(r == UE_NOTFOUND ? 2 : 3);
            return;
        }
        if (nd.ftype == FT_DIR || (nd_attr(dv) & 0x07)) {
            lfail(5);
            return;
        }
        r = udf_unlink(&dv->v, &pd, fid.pos);
        udf_sync(&dv->v);
        if (r) lfail(dos_err(r));
        else lok();
        return;
    }
    {
        char save = *(char *)(pat - 1);
        *(char *)(pat - 1) = 0;
        r = lwalk(dv, lpath, 0);
        *(char *)(pat - 1) = save;
    }
    if (r || nd.ftype != FT_DIR) {
        lfail(3);
        return;
    }
    dir = nd;
    while ((r = udf_readdir(&dv->v, &dir, &pos, &fid)) > 0) {
        u8 a;
        if (fid.fchar & (FC_PARENT | FC_DELETED | FC_DIR)) continue;
        to_oem(fid.name, fid.nlen, lname, LFN_PATH);
        udf_dosname(fid.name, fid.nlen, dname, oem);
        if (!lmatch(pat, lname) && !lmatch(pat, dname)) continue;
        if (udf_iget(&dv->v, fid.icb, &nd)) continue;
        a = nd_attr(dv);
        if ((a & 0x07) || ((a & 0x16) & ~sattr) || ((req & 0x3F) & ~a)) continue;
        if (udf_unlink(&dv->v, &dir, fid.pos) == 0) n++;
    }
    udf_sync(&dv->v);
    if (n) lok();
    else lfail(2);
}

static void l_attr(struct dvol *dv)
{
    u8 op = (u8)lr.bx;
    int r = lwalk(dv, lpath, 0);
    struct udf_time t;
    if (r) {
        lfail(r == UE_NOTFOUND ? 2 : 3);
        return;
    }
    switch (op) {
    case 0:
        lr.cx = nd_attr(dv);
        lr.ax = lr.cx;
        lok();
        return;
    case 1:
        if (fid.pos == 0xFFFFFFFFUL || (dv->v.flags & VF_RDONLY)) {
            lfail(5);
            return;
        }
        r = udf_setattr(&dv->v, &pd, fid.pos, &nd, (u8)lr.cx);
        udf_sync(&dv->v);
        if (r) lfail(dos_err(r));
        else lok();
        return;
    case 2:
        lr.ax = (u16)nd.size;
        lr.dx = (u16)(nd.size >> 16);
        lok();
        return;
    case 3:
    case 5:
    case 7:
        if (dv->v.flags & VF_RDONLY) {
            lfail(5);
            return;
        }
        if (op == 7) {
            lok();          /* the creation time stays */
            return;
        }
        if (op == 3) from_dos(lr.cx, lr.di, &t);   /* CX date, DI time */
        else from_dos(lr.di, 0, &t);                /* DI date */
        r = op == 3 ? udf_settime(&dv->v, &nd, &t, 0) : udf_settime(&dv->v, &nd, 0, &t);
        udf_sync(&dv->v);
        if (r) lfail(dos_err(r));
        else lok();
        return;
    case 4:
        lr.cx = dos_time(&nd.mtime);
        lr.di = dos_date(&nd.mtime);
        lok();
        return;
    case 6:
        lr.di = dos_date(&nd.atime);
        lr.cx = dos_time(&nd.atime);
        lok();
        return;
    case 8:
        lr.cx = dos_time(&nd.ctime);
        lr.di = dos_date(&nd.ctime);
        lr.si = (u16)(nd.ctime.cs + (nd.ctime.sec & 1 ? 100 : 0));
        lok();
        return;
    }
    lfail(1);
}

static void l_rename(struct dvol *dv, u8 drive)
{
    static struct udf_node src;
    const char *nn;
    struct dvol *dv2;
    u8 d2;
    u16 err, n;
    int r;
    u32 spos;
    dv2 = lfn_canon((const char FAR *)MK_FP(lr.es, lr.di), lpath2, &d2, &err);
    if (!dv2 || d2 != drive) {
        lfail(0x11);
        return;
    }
    if (err || lwild(lfn_last(lpath)) || lwild(lfn_last(lpath2))) {
        lfail(err ? err : 3);
        return;
    }
    r = lwalk(dv, lpath, 0);
    if (r) {
        lfail(r == UE_NOTFOUND ? 2 : 3);
        return;
    }
    if (fid.pos == 0xFFFFFFFFUL) {
        lfail(5);
        return;
    }
    src = pd;
    spos = fid.pos;
    r = lwalk(dv, lpath2, 0);
    if (r == 0) {
        lfail(5);           /* the new name exists */
        return;
    }
    if (r != UE_NOTFOUND) {
        lfail(3);
        return;
    }
    nn = lfn_last(lpath2);
    n = to_uni((const char FAR *)nn, (u16)lstrlen(nn), uname);
    r = udf_rename(&dv->v, &src, spos, &pd, uname, n, 0xFFFFFFFFUL);
    udf_sync(&dv->v);
    if (r) lfail(dos_err(r));
    else lok();
}

static void l_chdir(struct dvol *dv, u8 drive)
{
    struct rregs q;
    u16 e;
    int r = lwalk(dv, lpath, 0);
    if (r || nd.ftype != FT_DIR) {
        lfail(3);
        return;
    }
    e = lfn_short(dv, lpath, lshort + 2);
    if (e) {
        lfail(3);
        return;
    }
    lshort[0] = (char)('A' + drive);
    lshort[1] = ':';
    if (!lshort[2]) {
        lshort[2] = '\\';
        lshort[3] = 0;
    }
    q.ax = 0x3B00;
    q.ds = FP_SEG((void FAR *)lshort);
    q.dx = FP_OFF((void FAR *)lshort);
    intx(0x21, &q);
    if (q.fl & 1) lfail(q.ax);
    else lok();
}

static void l_getcwd(struct dvol *dv, u8 d)
{
    char FAR *out = (char FAR *)MK_FP(lr.ds, lr.si);
    u16 i, e;
    lpath[0] = 0;
    lfn_add(lpath, cds_dir(d));
    e = lfn_long(dv, lpath, lname);
    if (e) {
        lfail(0x0F);
        return;
    }
    for (i = 0; lname[i + (lname[0] == '\\')]; i++) out[i] = lname[i + (lname[0] == '\\')];
    out[i] = 0;
    lok();
}

static void l_truename(struct dvol *dv, u8 drive)
{
    char FAR *out = (char FAR *)MK_FP(lr.es, lr.di);
    const char *p = lpath;
    u16 i = 0, e = 0;
    u8 how = (u8)lr.cx;
    if (how == 1) {
        e = lfn_short(dv, lpath, lshort);
        p = lshort;
    } else if (how == 2) {
        e = lfn_long(dv, lpath, lshort);
        if (e == 3 && lwalk(dv, lpath, 0) == UE_NOTFOUND) e = 0, p = lpath;
        else p = lshort;
    }
    if (e && how) {
        lfail(e);
        return;
    }
    out[i++] = (char)('A' + drive);
    out[i++] = ':';
    if (!*p) out[i++] = '\\';
    for (; *p && i < LFN_PATH - 1; p++) out[i++] = how == 0 ? upc(*p) : *p;
    out[i] = 0;
    lok();
}

static void l_open(struct dvol *dv, u8 drive)
{
    struct rregs q;
    const char FAR *last;
    u16 act = lr.dx, mode = lr.bx, done;
    int r = lwalk(dv, lpath, &last), created = 0;
    const char *nm = lfn_last(lpath);
    if (r && r != UE_NOTFOUND) {
        lfail(3);
        return;
    }
    if (!*nm || lwild(nm)) {
        lfail(r ? 3 : 2);
        return;
    }
    if (r == 0) {
        if (nd.ftype == FT_DIR) {
            lfail(5);
            return;
        }
        if (!(act & 0x03)) {
            lfail(0x50);
            return;
        }
        done = (act & 0x03) == 2 ? 3 : 1;
    } else {
        struct udf_time t;
        u16 n;
        if (!(act & 0x10)) {
            lfail(2);
            return;
        }
        if (dv->v.flags & VF_RDONLY) {
            lfail(5);
            return;
        }
        n = to_uni(last, (u16)lstrlen(nm), uname);
        udf_now(&t);
        r = udf_create(&dv->v, &pd, uname, n, 0, (u8)(lr.cx & 0x27), &t, &nd, 0);
        udf_sync(&dv->v);
        if (r) {
            lfail(r == UE_NOSPACE ? 0x52 : dos_err(r));
            return;
        }
        created = 1;
        done = 2;
        act = 1;
    }
    if (lfn_short(dv, lpath, lshort + 2)) {
        lfail(3);
        return;
    }
    lshort[0] = (char)('A' + drive);
    lshort[1] = ':';
    q.ax = 0x6C00;
    q.bx = (u16)(mode & 0x60FF);
    q.cx = (u16)(lr.cx & 0x27);
    q.dx = (u16)(act & 0x03);
    q.ds = FP_SEG((void FAR *)lshort);
    q.si = FP_OFF((void FAR *)lshort);
    intx(0x21, &q);
    if (q.fl & 1) {
        lfail(q.ax);
        return;
    }
    lr.ax = q.ax;
    lr.cx = created ? 2 : done;
    lok();
}

static void l_volinfo(void)
{
    char FAR *b = (char FAR *)MK_FP(lr.es, lr.di);
    if (lr.cx >= 4) {
        b[0] = 'U';
        b[1] = 'D';
        b[2] = 'F';
        b[3] = 0;
    }
    lr.bx = 0x4006;         /* case preserved, Unicode on the disk, LFN functions */
    lr.cx = 255;
    lr.dx = 260;
    lok();
}

/* file information by handle (71A6h) */
static void l_fileinfo(struct dvol *dv, u8 FAR *sft)
{
    u8 FAR *b = (u8 FAR *)MK_FP(lr.ds, lr.dx);
    u16 i;
    if (sft_node(dv, sft)) {
        lfail(6);
        return;
    }
    for (i = 0; i < 52; i++) b[i] = 0;
    b[0] = sft[4];
    filetime(&nd.ctime, b + 0x04);
    filetime(&nd.atime, b + 0x0C);
    filetime(&nd.mtime, b + 0x14);
    *(u32 FAR *)(b + 0x1C) = dv->v.serial;
    *(u32 FAR *)(b + 0x20) = (u32)(nd.size >> 32);
    *(u32 FAR *)(b + 0x24) = (u32)nd.size;
    *(u32 FAR *)(b + 0x28) = nd.links ? nd.links : 1;
    *(u32 FAR *)(b + 0x2C) = (u32)(nd.uniq >> 32);
    *(u32 FAR *)(b + 0x30) = (u32)nd.uniq;
    lok();
}

/* 64-bit seek (7142h) */
static void l_seek(struct dvol *dv, u8 FAR *sft)
{
    u8 FAR *q = (u8 FAR *)MK_FP(lr.ds, lr.dx);
    u64 cur = ((u64)lfn_hi(sft) << 32) | *(u32 FAR *)(sft + 0x15), base = 0, np;
    u64 uoff = G64(q, 0);
    int neg = (q[7] & 0x80) != 0;
    u8 how = (u8)lr.cx;
    if (sft_node(dv, sft)) {
        lfail(6);
        return;
    }
    if (how == 1) base = cur;
    else if (how == 2) base = nd.size;
    else if (how != 0) {
        lfail(1);
        return;
    }
    if (neg && (0 - uoff) > base) {
        lfail(0x83);        /* negative position */
        return;
    }
    np = base + uoff;
    if (lfn_sethi(sft, (u32)(np >> 32))) {
        lfail(4);
        return;
    }
    *(u32 FAR *)(sft + 0x15) = (u32)np;
    udf_p64(q, np);
    lok();
}

/* 71A7h: FILETIME <-> DOS date and time */
static void l_timeconv(void)
{
    if ((u8)lr.bx == 0) {
        u16 d, t;
        u8 cs;
        ft_dos((const u8 FAR *)MK_FP(lr.ds, lr.si), &d, &t, &cs);
        lr.cx = t;
        lr.dx = d;
        lr.bx = (u16)((lr.bx & 0x00FF) | ((u16)cs << 8));
        lok();
    } else if ((u8)lr.bx == 1) {
        dostime_ft(lr.dx, lr.cx, (u8)(lr.bx >> 8), (u8 FAR *)MK_FP(lr.es, lr.di));
        lok();
    } else {
        lfail(1);
    }
}

/* 7303h: extended free space (MS-DOS 7.1 DIR asks for it) */
static void l_freespace(struct dvol *dv)
{
    u8 FAR *b = (u8 FAR *)MK_FP(lr.es, lr.di);
    u32 total = udf_totalblocks(&dv->v), fr = udf_freeblocks(&dv->v);
    u16 i;
    if (lr.cx < 44) {
        lfail(0x18);
        return;
    }
    for (i = 0; i < 44; i++) b[i] = 0;
    *(u16 FAR *)(b + 0x00) = 44;
    *(u32 FAR *)(b + 0x04) = 1;             /* a cluster is a logical block */
    *(u32 FAR *)(b + 0x08) = dv->v.bs;
    *(u32 FAR *)(b + 0x0C) = fr;
    *(u32 FAR *)(b + 0x10) = total;
    *(u32 FAR *)(b + 0x14) = fr;
    *(u32 FAR *)(b + 0x18) = total;
    *(u32 FAR *)(b + 0x1C) = fr;
    *(u32 FAR *)(b + 0x20) = total;
    lok();
}

/* INT 21h AX=71xxh (and AH=42h, AX=7303h): 0 when not for a UDF drive (passed on) */
#ifdef DOS_DEBUG
static int lfn2(void);
int __cdecl lfn(void)
{
    int h;
    dbgs("[71");
    dbgx(lr.ax);
    dbgs(" sp=");
    dbgx(lr.sp);
    dbgs(" ss=");
    dbgx(lr.ss);
    h = lfn2();
    dbgs(h ? " ours " : " pass ");
    dbgx(lr.ax);
    dbgs(" fl=");
    dbgx(lr.fl);
    dbgs("]\n");
    return h;
}
static int lfn2(void)
#else
int __cdecl lfn(void)
#endif
{
    u8 fn = (u8)lr.ax, drive;
    struct dvol *dv;
    u16 err;
    const char FAR *in;
    if (lr.ax == 0x7303) {
        in = (const char FAR *)MK_FP(lr.ds, lr.dx);
        if (!in[0] || in[1] != ':' || !(dv = vol_of((u8)(upc(in[0]) - 'A')))) return 0;
        l_freespace(dv);
        return 1;
    }
    if ((lr.ax >> 8) == 0x73) return 0;
    if ((lr.ax >> 8) == 0x42) {
        /* an ordinary seek of a UDF file: back below 4 GB */
        u8 FAR *sft = sft_of(lr.bx);
        if (sft && sft_vol(sft)) lfn_sethi(sft, 0);
        return 0;
    }
    switch (fn) {
    case 0x39: case 0x3A: case 0x3B: case 0x41: case 0x43: case 0x4E: case 0x56:
        in = (const char FAR *)MK_FP(lr.ds, lr.dx);
        break;
    case 0x60: case 0x6C:
        in = (const char FAR *)MK_FP(lr.ds, lr.si);
        break;
    case 0xA0:
        in = (const char FAR *)MK_FP(lr.ds, lr.dx);
        if (!in[0] || in[1] != ':') return 0;
        if (!vol_of((u8)(upc(in[0]) - 'A'))) return 0;
        l_volinfo();
        return 1;
    case 0x47:
        drive = (u8)lr.dx;
        if (!drive) {
            struct rregs r;
            r.ax = 0x1900;
            intx(0x21, &r);
            drive = (u8)r.ax;
        } else {
            drive--;
        }
        if (drive >= di.lastdrive || !(dv = vol_of(drive))) return 0;
        l_getcwd(dv, drive);
        return 1;
    case 0x4F:
    case 0xA1:
    {
        u16 h = (u16)(lr.bx - LFN_HBASE);
        if (lr.bx < LFN_HBASE || h >= LFN_HANDLES || !lsr[h].used) return 0;
        if (fn == 0xA1) {
            lsr[h].used = 0;
            lok();
            return 1;
        }
        err = lfind_next(&lsr[h], (u8 FAR *)MK_FP(lr.es, lr.di), lr.si);
        if (err) lfail(err);
        else lok();
        return 1;
    }
    case 0xA6:
    case 0x42:
    {
        u8 FAR *sft = sft_of(lr.bx);
        if (!sft || !(dv = sft_vol(sft))) return 0;
        if (fn == 0xA6) l_fileinfo(dv, sft);
        else l_seek(dv, sft);
        return 1;
    }
    case 0xA7:
        l_timeconv();
        return 1;
    default:
        return 0;
    }
    dv = lfn_canon(in, lpath, &drive, &err);
    if (!dv) return 0;
    if (err) {
        lfail(err);
        return 1;
    }
    switch (fn) {
    case 0x39: l_mkdir(dv); break;
    case 0x3A: l_rmdir(dv); break;
    case 0x3B: l_chdir(dv, drive); break;
    case 0x41: l_delete(dv); break;
    case 0x43: l_attr(dv); break;
    case 0x4E: l_findfirst(dv); break;
    case 0x56: l_rename(dv, drive); break;
    case 0x60: l_truename(dv, drive); break;
    case 0x6C: l_open(dv, drive); break;
    }
    return 1;
}
