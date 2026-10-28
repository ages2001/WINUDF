/* udfvxd.c - WINUDF.VXD: the UDF file system driver (FSD) of the IFS Manager of Windows
   95/98/ME. It mounts UDF volumes on the drives of the I/O Supervisor (CD, DVD and BD drives,
   removable and fixed disks), reads and writes them with I/O requests to the drive, and
   answers the volume and file handle functions of the IFS Manager: long and 8.3 names
   (the UDF method for the short names, as WINUDF.EXE in DOS), find, open, create, read,
   write, delete, rename, directories, attributes and times. The file system work is the
   core of udfcore.c and udfwr.c; a semaphore lets one request at a time into it. */
#include "udf.h"

/* ------------------------------------------------------------------ IFS Manager */

#pragma pack(push, 4)
struct ioreq {
    u32 length;                 /* ir_length, ir_attr */
    u8 flags;
    u8 user;
    u16 sfn;
    u32 pid;
    struct ppath *ppath;
    u32 aux1;                   /* ir_vfunc, ir_hfunc, ir_ppath2, ir_volh */
    u8 *data;
    u16 options;
    s16 error;
    void *rh;
    void *fh;
    u32 pos;                    /* ir_pos, ir_size, ir_attr2 */
    u32 aux2;                   /* ir_mntdrv, ir_dostime, ir_numfree, ir_uFName, ir_cregptr */
    u32 aux3;
    void *pev;
    u32 fsd[16];
};
#pragma pack(pop)

struct ppath {                  /* ParsedPath: elements of pe_length bytes (with the length) */
    u16 total;
    u16 prefix;                 /* offset of the last element */
    u16 el[1];
};

typedef int (__cdecl *fsfn)(struct ioreq *);
struct hfunc {
    fsfn read, write;
    const void *misc;
};

#pragma pack(push, 1)
struct volfunc {
    u16 version;
    u8 revision, size;
    fsfn fn[15];
};
struct hndlmisc {
    u16 version;
    u8 revision, size;
    fsfn fn[8];
};
#pragma pack(pop)

#define IFS_VERSION     0x030A
#define IFS_REVISION    0x10
#define IFSMGRVERSION   0x22

#define IR_FSD_MOUNT    0
#define IR_FSD_VERIFY   2
#define IR_FSD_UNLOAD   3

#define ACCESS_MODE     0x0007
#define SHARE_MODE      0x0070
#define ACT_EXISTS_OPEN 0x01
#define ACT_TRUNCATE    0x02
#define ACT_CREATE      0x10

#define FA_RDONLY       0x01
#define FA_HIDDEN       0x02
#define FA_SYSTEM       0x04
#define FA_LABEL        0x08
#define FA_DIR          0x10
#define FA_ARCH         0x20
#define FF_WILDCARDS    0x80000000UL
#define FF_KEEP_CASE    0x10000000UL

#define UFLG_META       0x01
#define UFLG_NT         0x02
#define UFLG_NT_DOS     0x04
#define BCS_OEM         1

/* DOS errors */
#define E_INVFUNC       1
#define E_NOTFOUND      2
#define E_NOPATH        3
#define E_ACCESS        5
#define E_NOMEM         8
#define E_BADFORMAT     11
#define E_WRPROT        19
#define E_NOTREADY      21
#define E_GENFAIL       31
#define E_NOTSAME       17
#define E_NOMORE        18
#define E_SHARE         32
#define E_NOTSUPP       50
#define E_EXISTS        80
#define E_PARAM         87
#define E_DISKFULL      112
#define E_VOLEXISTS     284

/* ------------------------------------------------------------------ I/O Supervisor */

#pragma pack(push, 1)
struct ilb {
    u32 service_rtn, dprintf_rtn, wait_10th_sec, internal_request, io_criteria_rtn;
    u32 int_io_criteria_rtn, dvt, ios_mem_virt, enqueue_iop, dequeue_iop, rsv1, rsv2;
    u16 flags;
    u8 driver_numb, rsv3;
};
struct drp {
    char eyecatch[8];
    u32 lgn, aer, ilb;
    char name[16];
    u8 revision;
    u32 feature_code;
    u16 if_requirements;
    u8 bus_type;
    u16 reg_result;
    u32 reference_data;
    u8 rsv1[2];
    u32 rsv2;
};
struct isp_iop {                /* ISP_CREATE_IOP */
    u16 func, result;
    u16 size;
    u32 delta;
    u32 iop;
    u8 flags, pad;
};
struct isp_free {               /* ISP_DEALLOC_MEM */
    u16 func, result;
    u32 ptr;
};
struct isp_dcb {                /* ISP_GET_DCB */
    u16 func, result;
    u32 dcb;
    u32 drive;
};
struct vrp {
    u32 demand_flags, event_flags;
    u16 max_sgd, max_req_size;
    u32 delta_to_ior, block_size, fsd_hvol, fsd_entry, device_handle, partition_offset;
    u32 next_vrp, logical_handle, rsv;
};
struct ior {
    u32 next;
    u16 func, status;
    u32 flags, callback, start[2], xfer_count, buffer, private_client, private_ios;
    u32 private_port;
    u32 usage[5];               /* requestor: the IOCTL fields */
    u32 req_req_handle, req_vol_handle, sgd_lin_phys;
    u8 num_sgds, vol_designtr;
    u16 ios_private_1;
    u32 rsv[2];
};
struct aep {
    u16 func, result;
};
#pragma pack(pop)

#define DRP_FSD         0x00000004UL
#define ISP_CREATE_IOP  2
#define ISP_DEALLOC_MEM 4
#define ISP_GET_DCB     7
#define ISP_MUST_SUCCEED 0x02
#define IOR_READ        0
#define IOR_WRITE       1
#define IOR_GEN_IOCTL   0x0D
#define IORF_HIGH_PRIORITY      0x0001
#define IORF_BYPASS_VOLTRK      0x0008
#define IORF_16BIT_IOCTL        0x0010
#define IORF_DOUBLE_BUFFER      0x0040
#define IORF_VERSION_002        0x0400
#define IORF_LOGICAL_START_SECTOR 0x80000UL
#define IORS_ERROR      0x10
#define IORS_UNFORMATTED 0x1C
#define IORS_NO_MEDIA   0x18
#define IORS_UNCERTAIN  0x19
#define IORS_WRITE_PROTECT 0x1F
#define IORS_NOT_READY  0x20
#define DCB_TYPE_CDROM  5
#define DCB_DEV_WRITEABLE 0x02
#define DCB_DEV_REMOVABLE 0x04
#define IOR_PENDING(r)  (*(volatile u32 *)((u8 *)(r) + 0x58))   /* after the IOR, in the IOP */

typedef long long s64;

/* ------------------------------------------------------------------ services (udfvxd.asm) */

void * __cdecl vx_heap_alloc(u32 size, u32 flags);
void __cdecl vx_heap_free(void *p, u32 flags);
void __cdecl vx_block(u32 id, u32 flags);
void __cdecl vx_signal(u32 id);
int __cdecl vx_register_mount(fsfn fn, u32 version, u32 def);
int __cdecl vx_register_fsd(u32 type, fsfn fn, u32 order, u32 z, u32 cls, u32 version);
u32 __cdecl vx_dostime(void);
s32 __cdecl vx_tzbias(void);
int __cdecl vx_metamatch(const u16 *pat, const u16 *name, u32 flags);
u32 __cdecl vx_unitobcs(u8 *out, const u16 *in, u32 bytes, u32 max, u32 set);
u32 __cdecl vx_bcstouni(u16 *out, const u8 *in, u32 bytes, u32 set);
void __cdecl vx_ios_register(struct drp *d);
u32 __cdecl vx_vmm_version(void);
u32 __cdecl vx_ifs_version(void);
u32 __cdecl vx_sem_create(void);
void __cdecl vx_sem_wait(u32 sem);
void __cdecl vx_sem_signal(u32 sem);
u32 __cdecl vx_thread(void);
u32 __cdecl vx_time(void);
u32 __cdecl vx_map_flat(void *cregs, u32 segofs);
void __cdecl vx_ios_send(struct ior *r, u32 dcb);
u32 __cdecl vx_call1(u32 fn, void *arg);
void __cdecl vx_iorcb(void);

void vx_cli(void);
#pragma aux vx_cli = "cli";
void vx_sti(void);
#pragma aux vx_sti = "sti";
u32 vx_flags(void);
#pragma aux vx_flags = "pushfd" "pop eax" value [eax];
void vx_outb(u16 port, u8 v);
#pragma aux vx_outb = "out dx, al" parm [dx] [al];

/* ------------------------------------------------------------------ debug output */

#ifdef UDF_DEBUG
/* port E9h (the debug console of QEMU and Bochs) */
static void dputs(const char *s)
{
    while (*s) vx_outb(0xE9, (u8)*s++);
}
static void dhex(u32 v, int n)
{
    static const char hx[] = "0123456789ABCDEF";
    while (n--) vx_outb(0xE9, (u8)hx[(v >> (n * 4)) & 15]);
}
void udf_log(const char *fmt, ...)
{
    const u32 *a = (const u32 *)(&fmt + 1);
    for (; *fmt; fmt++) {
        if (*fmt != '%') {
            vx_outb(0xE9, (u8)*fmt);
            continue;
        }
        fmt++;
        if (*fmt == 'x') dhex(*a++, 8);
        else if (*fmt == 'w') dhex(*a++, 4);
        else if (*fmt == 'b') dhex(*a++, 2);
        else if (*fmt == 's') dputs((const char *)*a++);
        else if (*fmt == 'u') {                 /* Unicode string, NUL-terminated */
            const u16 *u = (const u16 *)*a++;
            while (u && *u) vx_outb(0xE9, (u8)(*u < 0x80 ? *u : '?')), u++;
        } else vx_outb(0xE9, (u8)*fmt);
    }
}
#define DLOG(x) udf_log x
#else
#define DLOG(x)
#endif

/* ------------------------------------------------------------------ helpers */

static void mcopy(void *d, const void *s, u32 n)
{
    u8 *a = (u8 *)d;
    const u8 *b = (const u8 *)s;
    while (n--) *a++ = *b++;
}

static void mzero(void *d, u32 n)
{
    u8 *a = (u8 *)d;
    while (n--) *a++ = 0;
}

static void *zalloc(u32 n)
{
    void *p = vx_heap_alloc(n, 0);
    if (p) mzero(p, n);
    return p;
}

static u16 ulen(const u16 *s)
{
    u16 n = 0;
    while (s[n]) n++;
    return n;
}

/* ------------------------------------------------------------------ state */

struct wvol;

struct wobj {                   /* an open file: shared by the handles of one ICB */
    struct wobj *next;
    struct wvol *w;
    struct udf_node n;
    struct udf_loc dir;         /* parent directory */
    u32 fidpos;                 /* its entry in the parent */
    u8 fchar;
    u16 nwr;                    /* handles open for writing */
    struct whandle *handles;
};

struct whandle {
    struct whandle *next;
    struct wobj *o;
    u8 mode;                    /* access and sharing mode */
};

struct wfind {                  /* FindFirstFile handle */
    struct wvol *w;
    struct udf_loc dir;
    u32 pos;
    u8 state;                   /* 0 ".", 1 "..", 2 entries, 3 done */
    u8 label;                   /* volume label search */
    u32 attr;
    u16 pat[UDF_NAMEMAX + 1];
};

struct wvol {
    struct wvol *next;
    struct udf_vol v;
    struct udf_dev dev;
    struct vrp *vrp;
    struct ior *ior;            /* request of this volume (in its IOP) */
    u32 dcb;
    u8 drive;                   /* 0 = A: */
    u8 cdrom;
    u8 dead;                    /* dismounted, waiting for its handles */
    u8 probe;                   /* being mounted or verified */
    u8 bunit;                   /* BIOS drive (80h ..) the sectors are read through, 0 none */
    u16 laststat;               /* status of the last request */
    u32 pstart;                 /* BIOS path: first sector of the partition */
    u16 refs;                   /* open files and find handles */
    struct wobj *objs;
};

static u32 sem, owner, depth;
static struct wvol *vols;
static struct ilb ilb;
static struct drp drp = {
    { 'X', 'X', 'X', 'X', 'X', 'X', 'X', 'X' }, DRP_FSD, 0, 0,
    { 'W', 'I', 'N', 'U', 'D', 'F', ' ', 'F', 'S', 'D', ' ', ' ', ' ', ' ', ' ', ' ' },
    1, 0, 0xFF, 0, 0, 0, { 0, 0 }, 0
};
static u8 *bounce;              /* device transfers (locked memory) */
#define BOUNCE          65536UL
static u16 oem[128];            /* Unicode of the OEM characters 80h-FFh */
static s32 tzbias;              /* minutes: UTC = local + bias */
static struct udf_fid fid;
static struct udf_node nd, pd;
static u16 uname[UDF_NAMEMAX + 1];

/* one request at a time; the thread inside may come back (requests of the I/O Supervisor) */
static void enter(void)
{
    u32 t = vx_thread();
    if (depth && owner == t) {
        depth++;
        return;
    }
    vx_sem_wait(sem);
    owner = t;
    depth = 1;
    tzbias = vx_tzbias();               /* set up by the IFS Manager after the start */
}

static void leave(void)
{
    if (--depth == 0) {
        owner = 0;
        vx_sem_signal(sem);
    }
}

static int ret(struct ioreq *ir, int e)
{
    ir->error = (s16)e;
    return e;
}

/* ------------------------------------------------------------------ device I/O */

static void __cdecl aer(struct aep *a)
{
    a->result = 0;
}

static int ios_io(struct wvol *w, u16 func, u64 lba, u32 count, u8 *buf)
{
    struct ior *r = w->ior;
    r->next = 0;
    r->func = func;
    r->status = 0;
    r->flags = IORF_VERSION_002 | IORF_HIGH_PRIORITY | IORF_LOGICAL_START_SECTOR;
    /* mounting and verifying: the volume is not the one of volume tracking yet */
    if (w->probe) r->flags |= IORF_BYPASS_VOLTRK;
    r->callback = (u32)vx_iorcb;
    r->start[0] = (u32)lba;
    r->start[1] = (u32)(lba >> 32);
    r->xfer_count = count;
    r->buffer = (u32)buf;
    r->req_req_handle = (u32)r;
    r->req_vol_handle = (u32)w->vrp;
    r->sgd_lin_phys = (u32)r + 0x60;
    r->num_sgds = 0;
    r->vol_designtr = w->drive;
    if (ilb.io_criteria_rtn && vx_call1(ilb.io_criteria_rtn, r)) r->flags |= IORF_DOUBLE_BUFFER;
    IOR_PENDING(r) = 1;
    vx_ios_send(r, w->dcb);
    vx_cli();
    while (IOR_PENDING(r)) vx_block((u32)r, 1);
    vx_sti();
    w->laststat = r->status;
    if (r->status >= IORS_ERROR) {
        DLOG(("WINUDF: I/O %w lba %x n %x status %w\r\n", func, (u32)lba, count, r->status));
        return r->status;
    }
    return 0;
}

/* ---- the BIOS path: a partition the disk driver of Windows took for an unformatted FAT
   volume (Windows 10 and later format UDF disks inside a partition of type 06h or 07h; the
   I/O Supervisor then fails every request to the drive): the sectors through INT 13h
   extensions in V86 mode, the BIOS drive and the partition start from the DOS drive table */
struct v86r { u32 eax, ebx, ecx, edx, esi, edi; u16 ds, es; u32 flags; };
int __cdecl vx_intr(u32 n, struct v86r *r);
extern u32 vx_v86buf;
#define V86SEC  16                  /* sectors per BIOS call */

static int bios_rw(struct wvol *w, u64 lba, u32 n, u8 *buf, int wr)
{
    u8 *dap = (u8 *)vx_v86buf, *data = dap + 16;
    while (n) {
        u32 k = n > V86SEC ? V86SEC : n;
        struct v86r r;
        int tries;
        u64 a = lba + w->pstart;
        for (tries = 0; tries < 3; tries++) {
            mzero(dap, 16);
            dap[0] = 16;
            dap[2] = (u8)k;
            *(u16 *)(dap + 4) = (u16)(((u32)data) & 15);
            *(u16 *)(dap + 6) = (u16)(((u32)data) >> 4);
            *(u32 *)(dap + 8) = (u32)a;
            *(u32 *)(dap + 12) = (u32)(a >> 32);
            if (wr) mcopy(data, buf, k * 512);
            mzero(&r, sizeof(r));
            r.eax = wr ? 0x4300 : 0x4200;
            r.edx = w->bunit;
            r.ds = (u16)(((u32)dap) >> 4);
            r.esi = ((u32)dap) & 15;
            vx_intr(0x13, &r);
            if (!(r.flags & 1) && !(r.eax & 0xFF00)) break;
        }
        if (tries == 3) return -1;
        if (!wr) mcopy(buf, data, k * 512);
        buf += k * 512;
        lba += k;
        n -= k;
    }
    return 0;
}

/* the BIOS drive and partition of DOS drive w->drive (IO.SYS block devices, INT 2Fh 0803h)
   when they hold a UDF volume: 0 */
static int bios_find(struct wvol *w)
{
    struct v86r r;
    u32 seg, off, guard;
    if (w->dev.ssize != 512) return -1;
    mzero(&r, sizeof(r));
    r.eax = 0x0803;
    vx_intr(0x2F, &r);
    seg = r.ds;
    off = r.edi & 0xFFFF;
    for (guard = 0; guard < 32 && off != 0xFFFF && (seg || off); guard++) {
        u8 *bds = (u8 *)(seg * 16 + off);
        if (bds[5] == w->drive && bds[4] >= 0x80) {
            u32 s;
            mzero(&r, sizeof(r));
            r.eax = 0x4100;
            r.ebx = 0x55AA;
            r.edx = bds[4];
            vx_intr(0x13, &r);
            if ((r.flags & 1) || (r.ebx & 0xFFFF) != 0xAA55 || !(r.ecx & 1)) return -1;
            w->bunit = bds[4];
            w->pstart = *(u32 *)(bds + 6 + 17);         /* BPB hidden sectors */
            for (s = 64; s < 64 + 16 * 4; s += 4) {     /* the volume recognition sequence */
                if (bios_rw(w, s, 1, bounce, 0)) break;
                if (bounce[1] == 'N' && bounce[2] == 'S' && bounce[3] == 'R' && bounce[4] == '0') return 0;
                if (bounce[1] == 'T' && bounce[2] == 'E' && bounce[3] == 'A') break;
            }
            w->bunit = 0;
            return -1;
        }
        off = *(u16 *)bds;
        seg = *(u16 *)(bds + 2);
    }
    return -1;
}

static int dev_rw(struct udf_dev *d, u64 lba, u16 n, u8 *buf, int wr)
{
    struct wvol *w = (struct wvol *)d->ctx;
    u32 max = BOUNCE / d->ssize;
    if (w->dead) return -1;
    if (w->bunit) return bios_rw(w, lba, n, buf, wr);
    while (n) {
        u32 k = n > max ? max : n, bytes = k * d->ssize;
        if (wr) mcopy(bounce, buf, bytes);
        if (ios_io(w, (u16)(wr ? IOR_WRITE : IOR_READ), lba, k, bounce)) return -1;
        if (!wr) mcopy(buf, bounce, bytes);
        buf += bytes;
        lba += k;
        n = (u16)(n - k);
    }
    return 0;
}

static int dev_read(struct udf_dev *d, u64 lba, u16 n, u8 *buf)
{
    return dev_rw(d, lba, n, buf, 0);
}

static int dev_write(struct udf_dev *d, u64 lba, u16 n, const u8 *buf)
{
    return dev_rw(d, lba, n, (u8 *)buf, 1);
}

/* ------------------------------------------------------------------ times */

static const u16 mdays[12] = { 0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334 };

static int leap(u32 y)
{
    return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
}

/* UDF time to Win32 FILETIME (100 ns units since 1601, UTC) */
static u64 t2ft(const struct udf_time *t)
{
    u32 y = t->year - 1601, days;
    s32 tz = t->tz == UDF_NOTZ ? -tzbias : t->tz;
    u64 s;
    days = y * 365 + y / 4 - y / 100 + y / 400 + mdays[t->month - 1] + t->day - 1;
    if (t->month > 2 && leap(t->year)) days++;
    s = (u64)days * 86400UL + (u32)t->hour * 3600UL + (u32)t->min * 60 + t->sec;
    s = (u64)((s64)s - (s64)tz * 60);
    return s * 10000000UL + (u32)t->cs * 100000UL;
}

/* FILETIME to the local time of this machine */
static void ft2t(u64 ft, struct udf_time *t)
{
    u64 s = ft / 10000000UL;
    u32 days, sec, y = 1601, m;
    t->cs = (u8)((ft / 100000UL) % 100);
    s = (u64)((s64)s - (s64)tzbias * 60);
    days = (u32)(s / 86400UL);
    sec = (u32)(s % 86400UL);
    for (;;) {
        u32 n = leap(y) ? 366 : 365;
        if (days < n) break;
        days -= n;
        y++;
    }
    for (m = 11; m > 0; m--) {
        u32 st = mdays[m] + (m >= 2 && leap(y) ? 1 : 0);
        if (days >= st) break;
    }
    days -= mdays[m] + (m >= 2 && leap(y) ? 1 : 0);
    t->year = (u16)y;
    t->month = (u8)(m + 1);
    t->day = (u8)(days + 1);
    t->hour = (u8)(sec / 3600);
    t->min = (u8)(sec / 60 % 60);
    t->sec = (u8)(sec % 60);
    t->tz = (s16)-tzbias;
}

/* DOS date << 16 | time (local) of a UDF time */
static u32 t2dos(const struct udf_time *t)
{
    struct udf_time l;
    ft2t(t2ft(t), &l);
    if (l.year < 1980) return (0x21UL << 16);
    return ((u32)(((l.year - 1980) << 9) | (l.month << 5) | l.day) << 16) |
           (u16)((l.hour << 11) | (l.min << 5) | (l.sec >> 1));
}

static void dos2t(u32 dt, u8 cs, struct udf_time *t)
{
    u16 date = (u16)(dt >> 16), time = (u16)dt;
    t->year = (u16)(1980 + (date >> 9));
    t->month = (u8)((date >> 5) & 15);
    t->day = (u8)(date & 31);
    t->hour = (u8)(time >> 11);
    t->min = (u8)((time >> 5) & 63);
    t->sec = (u8)((time & 31) * 2);
    if (cs >= 100) {
        t->sec++;
        cs = (u8)(cs - 100);
    }
    t->cs = cs;
    t->tz = (s16)-tzbias;
    if (t->month < 1 || t->month > 12 || t->day < 1) {
        t->month = 1;
        t->day = 1;
    }
}

void udf_now(struct udf_time *t)
{
    dos2t(vx_dostime(), 0, t);
}

/* ------------------------------------------------------------------ names */

/* short name of an entry as Unicode, NUL-terminated (length returned) */
static u16 shortname(const u16 *name, u16 len, u16 *out)
{
    char d[13];
    int n = udf_dosname(name, len, d, oem), i;
    for (i = 0; i < n; i++) {
        u8 b = (u8)d[i];
        out[i] = b < 0x80 ? b : oem[b - 0x80];
    }
    out[n] = 0;
    return (u16)n;
}

/* the 8.3 name is the long name itself (no alias needed) */
static int same83(const u16 *name, u16 len, const u16 *s)
{
    u16 i;
    for (i = 0; i < len; i++)
        if (name[i] != s[i]) return 0;
    return s[len] == 0;
}

/* ------------------------------------------------------------------ paths */

#define WALK_NOPATH     (-100)

/* walk a parsed path from the root: nd (and fid) the node of the last element, pd its
   parent. Returns 0, UE_NOTFOUND (the last element is missing: pd valid), WALK_NOPATH
   or another error. The root has fid.pos 0xFFFFFFFF. */
static int walk(struct wvol *w, const struct ppath *pp, const u16 **lastname, u16 *lastlen)
{
    const u8 *p = (const u8 *)pp->el, *end = (const u8 *)pp + pp->total;
    int r = udf_iget(&w->v, w->v.root, &nd);
    if (r) return r;
    fid.pos = 0xFFFFFFFFUL;
    fid.fchar = FC_DIR;
    pd = nd;
    if (lastname) {
        *lastname = 0;
        *lastlen = 0;
    }
    while (p < end) {
        u16 el = *(const u16 *)p, n = (u16)((el - 2) / 2);
        const u16 *name = (const u16 *)(p + 2);
        if (el < 2) break;
        p += el;
        if (nd.ftype != FT_DIR) return WALK_NOPATH;
        pd = nd;
        if (lastname) {
            *lastname = name;
            *lastlen = n;
        }
        if (n == 0 || n > UDF_NAMEMAX) return p >= end ? UE_NOTFOUND : WALK_NOPATH;
        r = udf_lookup(&w->v, &pd, name, n, &fid, oem);
        if (r < 0) return r;
        if (r == 0) return p >= end ? UE_NOTFOUND : WALK_NOPATH;
        r = udf_iget(&w->v, fid.icb, &nd);
        if (r) return r;
    }
    return 0;
}

static int is_root(const struct ppath *pp)
{
    return pp->total <= 4;
}

static int err(int r)
{
    switch (r) {
    case 0: return 0;
    case UE_NOTFOUND: return E_NOTFOUND;
    case WALK_NOPATH:
    case UE_NOTDIR: return E_NOPATH;
    case UE_ISDIR:
    case UE_ACCESS:
    case UE_NOTEMPTY: return E_ACCESS;
    case UE_EXISTS: return E_EXISTS;
    case UE_NOSPACE: return E_DISKFULL;
    case UE_RDONLY: return E_WRPROT;
    case UE_NOMEM: return E_NOMEM;
    case UE_BADNAME: return E_ACCESS;
    case UE_CHANGED: return E_NOTREADY;
    case UE_IO: return E_NOTREADY;
    default: return E_GENFAIL;
    }
}

/* name for a new entry: the case-preserved name of the IFS Manager when it gives one */
static int newname(struct ioreq *ir, u32 attr, const u16 *last, u16 n, const u16 **name, u16 *len)
{
    *name = last;
    *len = n;
    if ((attr & FF_KEEP_CASE) && ir->aux2) {
        const u16 *u = (const u16 *)ir->aux2;
        u16 k = ulen(u);
        if (k == n) *name = u;
    }
    if (*len == 0 || *len > UDF_NAMEMAX) return E_ACCESS;
    return 0;
}

static u8 attr_of(struct wvol *w, struct udf_node *n, u8 fchar)
{
    u8 a = udf_dosattr(n, fchar);
    if (!udf_writable(&w->v) && !(a & FA_DIR)) a |= FA_RDONLY;
    return a;
}

/* the open file of an ICB, if any */
static struct wobj *obj_find(struct wvol *w, struct udf_loc l)
{
    struct wobj *o;
    for (o = w->objs; o; o = o->next)
        if (o->n.loc.lbn == l.lbn && o->n.loc.part == l.part) return o;
    return 0;
}

/* node to change: the one of the open file when the ICB is open */
static struct udf_node *live(struct wvol *w, struct udf_node *n)
{
    struct wobj *o = obj_find(w, n->loc);
    return o ? &o->n : n;
}

/* ------------------------------------------------------------------ volumes */

static struct wvol *vol_of(struct ioreq *ir)
{
    return (struct wvol *)ir->rh;
}

static void vol_free(struct wvol *w)
{
    struct wvol **pp;
    for (pp = &vols; *pp; pp = &(*pp)->next)
        if (*pp == w) {
            *pp = w->next;
            break;
        }
    udf_cache_drop(&w->v);
    if (w->vrp && w->vrp->fsd_hvol == (u32)w) w->vrp->fsd_hvol = 0;
    if (w->ior) {
        struct isp_free f;
        f.func = ISP_DEALLOC_MEM;
        f.result = 0;
        f.ptr = (u32)w->ior - w->vrp->delta_to_ior;
        vx_call1(ilb.service_rtn, &f);
    }
    vx_heap_free(w, 0);
}

static void vol_unref(struct wvol *w)
{
    if (w->refs) w->refs--;
    if (w->dead && !w->refs) vol_free(w);
}

/* physical DCB of a drive (0 = A:), 0 when there is none */
static u32 drive_dcb(u8 drive)
{
    struct isp_dcb g;
    u32 dcb;
    g.func = ISP_GET_DCB;
    g.result = 0;
    g.dcb = 0;
    g.drive = drive;
    vx_call1(ilb.service_rtn, &g);
    if (g.result || !g.dcb) return 0;
    dcb = *(u32 *)g.dcb;                /* DCB_physical_dcb */
    return dcb ? dcb : g.dcb;
}

static struct wvol *vol_new(struct vrp *vrp, u8 drive)
{
    struct wvol *w;
    struct isp_iop c;
    u16 sgd = vrp->max_sgd < 8 ? 8 : vrp->max_sgd;
    u32 dcb = drive_dcb(drive);
    if (vrp->block_size != 512 && vrp->block_size != 1024 && vrp->block_size != 2048 &&
        vrp->block_size != 4096) return 0;
    w = (struct wvol *)zalloc(sizeof(*w));
    if (!w) return 0;
    w->vrp = vrp;
    w->dcb = vrp->device_handle;
    w->drive = drive;
    c.func = ISP_CREATE_IOP;
    c.result = 0;
    c.size = (u16)(vrp->max_req_size + sgd * 8 + 0x18);
    c.delta = vrp->delta_to_ior;
    c.iop = 0;
    c.flags = ISP_MUST_SUCCEED;
    c.pad = 0;
    vx_call1(ilb.service_rtn, &c);
    if (!c.iop) {
        vx_heap_free(w, 0);
        return 0;
    }
    w->ior = (struct ior *)(c.iop + c.delta);
    w->ior->private_client = c.delta;
    w->cdrom = (u8)(dcb && *(u8 *)(dcb + 0x40) == DCB_TYPE_CDROM);
    w->dev.read = dev_read;
    w->dev.write = 0;
    if (!w->cdrom || (dcb && (*(u32 *)(dcb + 0x20) & DCB_DEV_WRITEABLE))) w->dev.write = dev_write;
    w->dev.ssize = (u16)vrp->block_size;
    w->dev.optical = w->cdrom;
    w->dev.ctx = w;
    return w;
}

/* the last readable sector of a CD, DVD or BD (the end of the recorded area) */
static u32 cd_last(struct wvol *w)
{
    u32 lo = 16, hi = 1UL << 24;            /* lo readable, hi not */
    if (ios_io(w, IOR_READ, lo, 1, bounce)) return 0;
    while (hi - lo > 1) {
        u32 m = lo + (hi - lo) / 2;
        if (ios_io(w, IOR_READ, m, 1, bounce)) hi = m;
        else lo = m;
    }
    return lo;
}

/* an NSR descriptor in the volume recognition sequence of a disc */
static int nsr(struct wvol *w)
{
    u32 s;
    for (s = 16; s < 32; s++) {
        if (ios_io(w, IOR_READ, s, 1, bounce)) return 0;
        if (bounce[1] == 'N' && bounce[2] == 'S' && bounce[3] == 'R' && bounce[4] == '0') return 1;
        if (bounce[1] == 'T' && bounce[2] == 'E' && bounce[3] == 'A') return 0;
    }
    return 0;
}

/* mount: the size of an optical disc is found when the volume needs it (virtual partition,
   anchor at the end only); the sector counts of the drive's DCB are geometry values */
static int mount(struct wvol *w)
{
    int r;
    w->probe = 1;
    w->laststat = 0;
    r = udf_mount(&w->v, &w->dev, 0, 0);
    if (r && !w->cdrom && w->laststat == IORS_UNFORMATTED && vx_v86buf && bios_find(w) == 0) {
        udf_cache_drop(&w->v);
        r = udf_mount(&w->v, &w->dev, 0, 0);
        DLOG(("WINUDF: drive %b through the BIOS: unit %b partition at %x -> %x\r\n", w->drive, w->bunit,
              w->pstart, (u32)r));
        if (r) w->bunit = 0;
    }
    if (r && r != UE_IO && w->cdrom && nsr(w)) {
        u32 n = cd_last(w);
        if (n) {
            udf_cache_drop(&w->v);
            r = udf_mount(&w->v, &w->dev, 0, n + 1);
        }
    }
    w->probe = 0;
    return r;
}

static int vol_same(struct udf_vol *a, struct udf_vol *b)
{
    u16 i;
    if (a->serial != b->serial || a->root.lbn != b->root.lbn || a->root.part != b->root.part ||
        a->lvid_abs != b->lvid_abs || a->nblocks != b->nblocks || a->rev != b->rev) return 0;
    for (i = 0; i < 33; i++)
        if (a->label[i] != b->label[i]) return 0;
    return 1;
}

static const struct volfunc volfunc;
static int __cdecl fs_mount(struct ioreq *ir);

static int do_mount(struct ioreq *ir)
{
    struct vrp *vrp = (struct vrp *)ir->aux1;
    u8 drive = (u8)ir->aux2;
    struct wvol *w = vol_new(vrp, drive), *o;
    int r;
    if (!w) return E_NOMEM;
    r = mount(w);
    DLOG(("WINUDF: mount %b: vrp %x bs %x dcb %x type %b sectors %x -> %x\r\n", drive, (u32)vrp,
          vrp->block_size, drive_dcb(drive), w->cdrom, (u32)w->dev.sectors, (u32)r));
    if (r) {
        vol_free(w);
        return E_BADFORMAT;
    }
    /* the same volume again (still in use from before): the IFS Manager takes that one */
    for (o = vols; o; o = o->next)
        if (o->vrp == vrp && vol_same(&o->v, &w->v)) {
            vol_free(w);
            o->dead = 0;
            ir->rh = o;
            return E_VOLEXISTS;
        }
    w->next = vols;
    vols = w;
    ir->rh = w;
    ir->aux1 = (u32)&volfunc;
    /* volume tracking calls the file system of the volume through the VRP */
    vrp->fsd_hvol = (u32)w;
    vrp->fsd_entry = (u32)fs_mount;
    return 0;
}

/* the medium in the drive still has the volume: 0, else E_NOTSAME with its label */
static int do_verify(struct ioreq *ir)
{
    struct wvol *w = vol_of(ir), *t;
    int r, same = 0;
    if (!w) return E_NOTSAME;
    t = vol_new(w->vrp, w->drive);
    if (!t) return E_NOMEM;
    r = mount(t);
    if (r == 0 && vol_same(&t->v, &w->v)) same = 1;
    vol_free(t);
    DLOG(("WINUDF: verify %b -> %x\r\n", w->drive, (u32)same));
    if (same) {
        udf_cache_drop(&w->v);
        return 0;
    }
    if (ir->data) {
        u16 i;
        for (i = 0; i < 11 && w->v.label[i]; i++) ir->data[i] = (u8)w->v.label[i];
        ir->data[i] = 0;
    }
    ir->aux2 = w->v.serial;
    return E_NOTSAME;
}

static int __cdecl fs_mount(struct ioreq *ir)
{
    int r;
    enter();
    switch (ir->flags) {
    case IR_FSD_MOUNT:
        r = do_mount(ir);
        break;
    case IR_FSD_VERIFY:
        r = do_verify(ir);
        break;
    case IR_FSD_UNLOAD:
        r = 0;
        if (vol_of(ir)) {
            struct wvol *w = vol_of(ir);
            /* safe removal: the volume is closed (integrity descriptor) while the disk is
               still there; open files keep their space, the volume stays consistent */
            if (!w->dead) {
                w->v.nopen = 0;
                udf_sync(&w->v);
            }
            w->dead = 1;
            if (!w->refs) vol_free(w);
        }
        break;
    default:
        r = E_NOTSUPP;
    }
    leave();
    return ret(ir, r);
}

/* ------------------------------------------------------------------ open files */

static int share_ok(struct wobj *o, u8 mode)
{
    struct whandle *h;
    u8 acc = (u8)(mode & ACCESS_MODE), sh = (u8)(mode & SHARE_MODE);
    int rd = acc != 1, wr = acc == 1 || acc == 2;
    for (h = o->handles; h; h = h->next) {
        u8 hacc = (u8)(h->mode & ACCESS_MODE), hsh = (u8)(h->mode & SHARE_MODE);
        int hrd = hacc != 1, hwr = hacc == 1 || hacc == 2;
        /* sharing of the open handle against the new access, and the other way round */
        if (hsh == 0x10 || sh == 0x10) return 0;
        if (hsh == 0x20 && wr) return 0;
        if (hsh == 0x30 && rd) return 0;
        if (sh == 0x20 && hwr) return 0;
        if (sh == 0x30 && hrd) return 0;
    }
    return 1;
}

static int __cdecl fs_read(struct ioreq *ir);
static int __cdecl fs_write(struct ioreq *ir);
static const struct hndlmisc hmisc;

/* ------------------------------------------------------------------ volume functions */

static int __cdecl fs_open(struct ioreq *ir)
{
    struct wvol *w = vol_of(ir);
    u8 act = (u8)ir->options, acc = (u8)(ir->flags & ACCESS_MODE);
    u32 attr = ir->length;
    const u16 *last;
    u16 llen;
    int r, created = 0, action = 1, wr = acc == 1 || acc == 2;
    struct wobj *o;
    struct whandle *h;
    u8 fchar;
    enter();
    if (attr & FF_WILDCARDS) {
        r = E_NOTFOUND;
        goto out;
    }
    r = walk(w, ir->ppath, &last, &llen);
    DLOG(("WINUDF: open %u act %b fl %b attr %x uf %u -> %x\r\n", last ? last : (const u16 *)"\0",
          act, ir->flags, attr, (attr & FF_KEEP_CASE) ? (const u16 *)ir->aux2 : (const u16 *)"\0", (u32)r));
    if (r && r != UE_NOTFOUND) {
        r = r == WALK_NOPATH ? E_NOPATH : err(r);
        goto out;
    }
    if (r == 0) {
        if (fid.pos == 0xFFFFFFFFUL || nd.ftype == FT_DIR) {
            r = E_ACCESS;
            goto out;
        }
        if (!(act & ACT_EXISTS_OPEN) && !(act & ACT_TRUNCATE)) {
            r = E_EXISTS;
            goto out;
        }
        if (wr || (act & ACT_TRUNCATE)) {
            if (!udf_writable(&w->v)) {
                r = E_WRPROT;
                goto out;
            }
            if (attr_of(w, live(w, &nd), fid.fchar) & FA_RDONLY) {
                r = E_ACCESS;
                goto out;
            }
        }
        fchar = fid.fchar;
    } else {
        const u16 *name;
        u16 nlen;
        struct udf_time t;
        if (!(act & ACT_CREATE)) {
            r = E_NOTFOUND;
            goto out;
        }
        if (!udf_writable(&w->v)) {
            r = E_WRPROT;
            goto out;
        }
        r = newname(ir, attr, last, llen, &name, &nlen);
        if (r) goto out;
        udf_now(&t);
        r = udf_create(&w->v, &pd, name, nlen, 0, (u8)(attr & 0x27), &t, &nd, &fid.pos);
        if (r) {
            udf_sync(&w->v);
            r = err(r);
            goto out;
        }
        fchar = (u8)((attr & FA_HIDDEN) ? FC_HIDDEN : 0);
        created = 1;
        action = 2;
    }
    o = obj_find(w, nd.loc);
    if (o && !share_ok(o, ir->flags)) {
        r = E_SHARE;
        goto out;
    }
    if (!created && (act & ACT_TRUNCATE)) {
        /* replace an existing file */
        r = udf_truncate(&w->v, live(w, &nd), 0);
        if (r) {
            udf_sync(&w->v);
            r = err(r);
            goto out;
        }
        action = 3;
    }
    if (!o) {
        o = (struct wobj *)zalloc(sizeof(*o));
        if (!o) {
            r = E_NOMEM;
            goto out;
        }
        o->w = w;
        o->n = nd;
        o->dir = pd.loc;
        o->fidpos = fid.pos;
        o->fchar = fchar;
        o->next = w->objs;
        w->objs = o;
        w->refs++;
    }
    h = (struct whandle *)zalloc(sizeof(*h));
    if (!h) {
        r = E_NOMEM;
        goto out;
    }
    h->o = o;
    h->mode = ir->flags;
    h->next = o->handles;
    o->handles = h;
    if (wr) {
        o->nwr++;
        w->v.nopen++;
    }
    /* created or replaced: the integrity descriptor is closed again unless written to */
    if (action != 1) udf_sync(&w->v);
    ir->fh = h;
    {
        struct hfunc *hf = (struct hfunc *)ir->aux1;
        hf->read = fs_read;
        hf->write = fs_write;
        hf->misc = &hmisc;
    }
    ir->pos = (u32)o->n.size;
    ir->aux2 = t2dos(&o->n.mtime);
    ir->length = attr_of(w, &o->n, o->fchar);
    ir->options = (u16)action;
    r = 0;
out:
    leave();
    return ret(ir, r);
}

/* delete files: a name or a pattern with wildcards; ir_attr: search attributes in bits 0-7,
   the ones that must match in 8-15 */
static int del_one(struct wvol *w, struct udf_node *dir, u32 pos, struct udf_loc icb)
{
    if (obj_find(w, icb)) return E_SHARE;
    return err(udf_unlink(&w->v, dir, pos));
}

static int __cdecl fs_delete(struct ioreq *ir)
{
    struct wvol *w = vol_of(ir);
    u32 attr = ir->length;
    const u16 *last;
    u16 llen;
    int r;
    enter();
    DLOG(("WINUDF: delete fl %b opt %w len %x pos %x\r\n", ir->flags, ir->options, ir->length, ir->pos));
    if (!udf_writable(&w->v)) {
        r = E_WRPROT;
        goto out;
    }
    r = walk(w, ir->ppath, &last, &llen);
    if (!(attr & FF_WILDCARDS)) {
        if (r) {
            r = r == UE_NOTFOUND ? E_NOTFOUND : r == WALK_NOPATH ? E_NOPATH : err(r);
            goto out;
        }
        if (fid.pos == 0xFFFFFFFFUL || nd.ftype == FT_DIR ||
            (attr_of(w, &nd, fid.fchar) & FA_RDONLY)) {
            r = E_ACCESS;
            goto out;
        }
        r = del_one(w, &pd, fid.pos, nd.loc);
    } else {
        struct udf_node dir;
        u32 pos = 0;
        u16 pat[UDF_NAMEMAX + 1], sn[13], k;
        int n = 0, e = 0;
        if (r != 0 && r != UE_NOTFOUND) {
            r = r == WALK_NOPATH ? E_NOPATH : err(r);
            goto out;
        }
        for (k = 0; k < llen; k++) pat[k] = last[k];
        pat[k] = 0;
        dir = pd;
        while ((r = udf_readdir(&w->v, &dir, &pos, &fid)) > 0) {
            u8 a;
            if (fid.fchar & (FC_PARENT | FC_DELETED | FC_DIR)) continue;
            fid.name[fid.nlen] = 0;
            shortname(fid.name, fid.nlen, sn);
            if (!vx_metamatch(pat, fid.name, UFLG_META | UFLG_NT) &&
                !vx_metamatch(pat, sn, UFLG_META | UFLG_NT_DOS)) continue;
            if (udf_iget(&w->v, fid.icb, &nd)) continue;
            a = attr_of(w, &nd, fid.fchar);
            if (a & ~attr & (FA_HIDDEN | FA_SYSTEM)) continue;
            if (((attr >> 8) & 0x3F) & ~a) continue;
            if (a & FA_RDONLY) {
                e = E_ACCESS;
                continue;
            }
            e = del_one(w, &dir, fid.pos, nd.loc);
            if (!e) n++;
        }
        r = n ? 0 : e ? e : E_NOTFOUND;
    }
    udf_sync(&w->v);
out:
    leave();
    return ret(ir, r);
}

/* path of the volume in long or 8.3 names (ir_ppath2), from the names along the way */
static int query_path(struct wvol *w, struct ioreq *ir, int shrt)
{
    const struct ppath *pp = ir->ppath;
    struct ppath *out = (struct ppath *)ir->aux1;
    const u8 *p = (const u8 *)pp->el, *end = (const u8 *)pp + pp->total;
    u8 *o = (u8 *)out->el;
    u16 lastoff = 4;
    int r = udf_iget(&w->v, w->v.root, &nd);
    if (r) return err(r);
    while (p < end) {
        u16 el = *(const u16 *)p, n = (u16)((el - 2) / 2), k, len;
        if (el < 2) break;
        if (nd.ftype != FT_DIR) return E_NOPATH;
        pd = nd;
        r = udf_lookup(&w->v, &pd, (const u16 *)(p + 2), n, &fid, oem);
        if (r <= 0) return r < 0 ? err(r) : E_NOPATH;
        if (udf_iget(&w->v, fid.icb, &nd)) return E_NOPATH;
        p += el;
        if (shrt) len = shortname(fid.name, fid.nlen, uname);
        else {
            for (k = 0; k < fid.nlen; k++) uname[k] = fid.name[k];
            len = fid.nlen;
        }
        if ((u32)(o - (u8 *)out) + 2 + len * 2 + 2 > 520) return E_ACCESS;
        lastoff = (u16)(o - (u8 *)out);
        *(u16 *)o = (u16)(2 + len * 2);
        for (k = 0; k < len; k++) ((u16 *)(o + 2))[k] = uname[k];
        o += 2 + len * 2;
    }
    out->total = (u16)(o - (u8 *)out);
    out->prefix = lastoff;
    *(u16 *)o = 0;
    return 0;
}

static int __cdecl fs_dir(struct ioreq *ir)
{
    struct wvol *w = vol_of(ir);
    u32 attr = ir->length;
    const u16 *last;
    u16 llen;
    int r;
    enter();
    DLOG(("WINUDF: dir fl %b opt %w len %x pos %x\r\n", ir->flags, ir->options, ir->length, ir->pos));
    switch (ir->flags) {
    case 0:                                         /* CREATE_DIR */
        if (!udf_writable(&w->v)) {
            r = E_WRPROT;
            break;
        }
        r = walk(w, ir->ppath, &last, &llen);
        if (r == 0) r = E_ACCESS;
        else if (r != UE_NOTFOUND) r = r == WALK_NOPATH ? E_NOPATH : err(r);
        else {
            const u16 *name;
            u16 nlen;
            struct udf_time t;
            r = newname(ir, attr, last, llen, &name, &nlen);
            if (r) break;
            udf_now(&t);
            r = err(udf_create(&w->v, &pd, name, nlen, 1, 0, &t, &nd, 0));
            udf_sync(&w->v);
        }
        break;
    case 1:                                         /* DELETE_DIR */
        if (!udf_writable(&w->v)) {
            r = E_WRPROT;
            break;
        }
        r = walk(w, ir->ppath, &last, &llen);
        if (r) r = r == UE_NOTFOUND || r == WALK_NOPATH ? E_NOPATH : err(r);
        else if (fid.pos == 0xFFFFFFFFUL) r = E_ACCESS;
        else if (nd.ftype != FT_DIR) r = E_NOPATH;
        else if (obj_find(w, nd.loc)) r = E_SHARE;
        else {
            r = err(udf_unlink(&w->v, &pd, fid.pos));
            udf_sync(&w->v);
        }
        break;
    case 2:                                         /* CHECK_DIR */
        if (is_root(ir->ppath)) {
            r = 0;
            break;
        }
        r = walk(w, ir->ppath, 0, 0);
        r = r == 0 && nd.ftype == FT_DIR ? 0 : E_NOPATH;
        break;
    case 3:                                         /* QUERY83_DIR */
    case 4:                                         /* QUERYLONG_DIR */
        r = query_path(w, ir, ir->flags == 3);
        break;
    default:
        r = E_INVFUNC;
    }
    leave();
    return ret(ir, r);
}

static int __cdecl fs_attrib(struct ioreq *ir)
{
    struct wvol *w = vol_of(ir);
    int r;
    struct udf_node *n;
    struct udf_time t;
    enter();
    DLOG(("WINUDF: attrib fl %b opt %w len %x pos %x\r\n", ir->flags, ir->options, ir->length, ir->pos));
    r = walk(w, ir->ppath, 0, 0);
    if (r) {
        r = r == UE_NOTFOUND ? E_NOTFOUND : r == WALK_NOPATH ? E_NOPATH : err(r);
        goto out;
    }
    n = live(w, &nd);
    switch (ir->flags) {
    case 0:                                         /* GET_ATTRIBUTES */
        ir->length = attr_of(w, n, fid.fchar);
        ir->pos = (u32)n->size;
        break;
    case 1:                                         /* SET_ATTRIBUTES */
        if (!udf_writable(&w->v)) r = E_WRPROT;
        else if (fid.pos == 0xFFFFFFFFUL) r = E_ACCESS;
        else {
            u8 a = (u8)(ir->length & 0x27);
            if (n->ftype == FT_DIR) a &= (u8)~FA_RDONLY;
            r = err(udf_setattr(&w->v, &pd, fid.pos, n, a));
            if (!r) {
                struct wobj *o = obj_find(w, n->loc);
                if (o) o->fchar = (u8)((o->fchar & ~FC_HIDDEN) | ((a & FA_HIDDEN) ? FC_HIDDEN : 0));
            }
            udf_sync(&w->v);
        }
        break;
    case 2:                                         /* GET_ATTRIB_COMP_FILESIZE */
        ir->pos = (u32)((n->size + w->v.bs - 1) & ~(u64)(w->v.bs - 1));
        break;
    case 3:                                         /* SET_ATTRIB_MODIFY_DATETIME */
    case 5:                                         /* SET_ATTRIB_LAST_ACCESS_DATETIME */
        if (!udf_writable(&w->v)) r = E_WRPROT;
        else {
            if (ir->flags == 5 && !(ir->aux2 & 0xFFFF)) {
                /* a date alone: keep the time of day */
                dos2t((ir->aux2 & 0xFFFF0000UL) | (t2dos(&n->atime) & 0xFFFF), 0, &t);
            } else dos2t(ir->aux2, 0, &t);
            r = err(udf_settime(&w->v, n, ir->flags == 3 ? &t : 0, ir->flags == 5 ? &t : 0));
            udf_sync(&w->v);
        }
        break;
    case 4:                                         /* GET_ATTRIB_MODIFY_DATETIME */
        ir->aux2 = t2dos(&n->mtime);
        break;
    case 6:                                         /* GET_ATTRIB_LAST_ACCESS_DATETIME */
        ir->aux2 = t2dos(&n->atime);
        break;
    case 7:                                         /* SET_ATTRIB_CREATION_DATETIME */
        if (!udf_writable(&w->v)) r = E_WRPROT;
        break;
    case 8:                                         /* GET_ATTRIB_CREATION_DATETIME */
        ir->aux2 = t2dos(&n->ctime);
        ir->options = (u16)(n->ctime.cs + ((n->ctime.sec & 1) ? 100 : 0));
        break;
    default:
        r = E_NOTSUPP;
    }
out:
    leave();
    return ret(ir, r);
}

static int __cdecl fs_flush(struct ioreq *ir)
{
    struct wvol *w = vol_of(ir);
    enter();
    DLOG(("WINUDF: flush fl %b opt %w len %x pos %x\r\n", ir->flags, ir->options, ir->length, ir->pos));
    if (w && !w->dead) udf_sync(&w->v);
    leave();
    return ret(ir, 0);
}

static int __cdecl fs_diskinfo(struct ioreq *ir)
{
    struct wvol *w = vol_of(ir);
    u32 total, fr, spc = 1;
    enter();
    DLOG(("WINUDF: diskinfo fl %b opt %w len %x pos %x\r\n", ir->flags, ir->options, ir->length, ir->pos));
    total = udf_totalblocks(&w->v);
    fr = udf_writable(&w->v) ? udf_freeblocks(&w->v) : 0;
    if (ir->options & 1) {
        /* extended free space (INT 21h 7303h) */
        u32 *x = (u32 *)ir->data;
        if (!x || ir->length < 0x2C) {
            leave();
            return ret(ir, E_ACCESS);
        }
        x[0] = 0x2C;
        x[1] = 1;
        x[2] = w->v.bs;
        x[3] = fr;
        x[4] = total;
        x[5] = fr << w->v.ssh;
        x[6] = total << w->v.ssh;
        x[7] = fr;
        x[8] = total;
        x[9] = x[10] = 0;
        ir->options = 0xFFFF;
    } else {
        /* clusters in 16 bits */
        while (total / spc > 0xFFF0UL && spc < 128) spc <<= 1;
        ir->length = w->v.bs;
        ir->options = (u16)spc;
        ir->pos = total / spc > 0xFFF0UL ? 0xFFF0UL : total / spc;
        ir->aux2 = fr / spc > 0xFFF0UL ? 0xFFF0UL : fr / spc;
        ir->flags = (u8)(w->cdrom ? 0xF8 : 0xF8);
    }
    leave();
    return ret(ir, 0);
}

/* rename: ir_ppath to ir_ppath2 on this volume; wildcards in the names (DOS REN rules) */
static void ren_name(const u16 *src, u16 slen, const u16 *pat, u16 plen, u16 *out, u16 *olen)
{
    /* the characters of the source where the pattern has '?' and '*' (one part at a time) */
    u16 si = 0, pi = 0, o = 0;
    while (pi < plen && o < UDF_NAMEMAX) {
        u16 c = pat[pi];
        if (c == '*') {
            while (si < slen && src[si] != '.' && o < UDF_NAMEMAX) out[o++] = src[si++];
            pi++;
        } else if (c == '?') {
            if (si < slen && src[si] != '.') out[o++] = src[si++];
            pi++;
        } else if (c == '.') {
            while (si < slen && src[si] != '.') si++;
            if (si < slen) si++;
            out[o++] = '.';
            pi++;
        } else {
            out[o++] = c;
            if (si < slen && src[si] != '.') si++;
            pi++;
        }
    }
    while (o && out[o - 1] == '.') o--;
    *olen = o;
}

static int __cdecl fs_rename(struct ioreq *ir)
{
    struct wvol *w = vol_of(ir);
    u32 attr = ir->length, attr2 = ir->pos;
    const u16 *last, *dlast;
    u16 llen, dlen, k;
    static struct udf_node src, dst;
    static u16 spat[UDF_NAMEMAX + 1], dpat[UDF_NAMEMAX + 1], nn[UDF_NAMEMAX + 1];
    u16 sn[13];
    int r, n = 0;
    u32 dpos = 0xFFFFFFFFUL;
    enter();
    DLOG(("WINUDF: rename fl %b opt %w len %x pos %x\r\n", ir->flags, ir->options, ir->length, ir->pos));
    if (!udf_writable(&w->v)) {
        r = E_WRPROT;
        goto out;
    }
    /* destination directory, and an existing destination */
    r = walk(w, (const struct ppath *)ir->aux1, &dlast, &dlen);
    if (r != 0 && r != UE_NOTFOUND) {
        r = r == WALK_NOPATH ? E_NOPATH : err(r);
        goto out;
    }
    if (!dlast) {
        r = E_ACCESS;
        goto out;
    }
    for (k = 0; k < dlen; k++) dpat[k] = dlast[k];
    dpat[dlen] = 0;
    if (r == 0 && !(attr2 & FF_WILDCARDS)) {
        dpos = fid.pos;
        dst = pd;
    } else dst = pd;
    r = walk(w, ir->ppath, &last, &llen);
    if (!(attr & FF_WILDCARDS)) {
        const u16 *name = dpat;
        u16 nlen = dlen;
        if (r) {
            r = r == UE_NOTFOUND ? E_NOTFOUND : r == WALK_NOPATH ? E_NOPATH : err(r);
            goto out;
        }
        if (fid.pos == 0xFFFFFFFFUL) {
            r = E_ACCESS;
            goto out;
        }
        src = pd;
        if (dpos != 0xFFFFFFFFUL) {
            /* the destination exists: only the same entry (a change of case) */
            if (dst.loc.lbn != src.loc.lbn || dst.loc.part != src.loc.part || dpos != fid.pos) {
                r = E_EXISTS;
                goto out;
            }
            dpos = 0xFFFFFFFFUL;
        }
        if ((attr2 & FF_KEEP_CASE) && ir->aux2) {
            const u16 *u = (const u16 *)ir->aux2;
            if (ulen(u) == dlen) name = u;
        }
        if (attr2 & FF_WILDCARDS) {
            ren_name(fid.name, fid.nlen, dpat, dlen, nn, &nlen);
            name = nn;
        }
        if (obj_find(w, fid.icb)) {
            r = E_SHARE;
            goto out;
        }
        r = err(udf_rename(&w->v, &src, fid.pos, &dst, name, nlen, 0xFFFFFFFFUL));
    } else {
        u32 pos = 0;
        int e = 0;
        if (r != 0 && r != UE_NOTFOUND) {
            r = r == WALK_NOPATH ? E_NOPATH : err(r);
            goto out;
        }
        for (k = 0; k < llen; k++) spat[k] = last[k];
        spat[llen] = 0;
        src = pd;
        while ((r = udf_readdir(&w->v, &src, &pos, &fid)) > 0) {
            u16 nlen;
            u8 a;
            if (fid.fchar & (FC_PARENT | FC_DELETED)) continue;
            fid.name[fid.nlen] = 0;
            shortname(fid.name, fid.nlen, sn);
            if (!vx_metamatch(spat, fid.name, UFLG_META | UFLG_NT) &&
                !vx_metamatch(spat, sn, UFLG_META | UFLG_NT_DOS)) continue;
            if (udf_iget(&w->v, fid.icb, &nd)) continue;
            a = attr_of(w, &nd, fid.fchar);
            if (a & ~attr & (FA_HIDDEN | FA_SYSTEM | FA_DIR)) continue;
            ren_name(fid.name, fid.nlen, dpat, dlen, nn, &nlen);
            if (!nlen || obj_find(w, fid.icb)) continue;
            e = err(udf_rename(&w->v, &src, fid.pos, &dst, nn, nlen, 0xFFFFFFFFUL));
            if (e) break;
            n++;
            if (src.loc.lbn == dst.loc.lbn && src.loc.part == dst.loc.part) src = dst;
        }
        r = e ? e : n ? 0 : E_NOTFOUND;
    }
    udf_sync(&w->v);
out:
    leave();
    return ret(ir, r);
}

/* ---- DOS find first / next (INT 21h 4Eh / 4Fh, FCB 11h / 12h): 8.3 names */

#pragma pack(push, 1)
struct srch_entry {
    u8 drive;
    char pattern[11];
    u8 sattr;
    u8 local[4];                /* slot, generation */
    u8 net[2];
    u8 ifs[2];
    u8 attr;
    u16 time, date;
    u32 size;
    char name[13];
};
#pragma pack(pop)

#define NSRCH 32
static struct srch {
    struct wvol *w;
    struct udf_loc dir;
    u32 pos, gen, used;
    u8 state, attr, must, label;
    char pat[11];
} srch[NSRCH];
static u32 srchgen, srchclock;

static void fcb_pat(const u16 *u, u16 n, char *fcb)
{
    u8 b[UDF_NAMEMAX + 1];
    u16 i = 0, k = 0, m;
    m = (u16)vx_unitobcs(b, u, (u32)n * 2, UDF_NAMEMAX, BCS_OEM);
    b[m] = 0;
    for (k = 0; k < 11; k++) fcb[k] = ' ';
    k = 0;
    if (m == 1 && b[0] == '.') {
        fcb[0] = '.';
        return;
    }
    if (m == 2 && b[0] == '.' && b[1] == '.') {
        fcb[0] = fcb[1] = '.';
        return;
    }
    while (i < m && b[i] != '.' && k < 8) {
        if (b[i] == '*') {
            while (k < 8) fcb[k++] = '?';
        } else fcb[k++] = (char)(b[i] >= 'a' && b[i] <= 'z' ? b[i] - 32 : b[i]);
        i++;
    }
    while (i < m && b[i] != '.') {
        if (b[i] == '*') while (k < 8) fcb[k++] = '?';
        i++;
    }
    if (i < m && b[i] == '.') i++;
    k = 8;
    while (i < m && k < 11) {
        if (b[i] == '*') {
            while (k < 11) fcb[k++] = '?';
        } else if (b[i] != '.') fcb[k++] = (char)(b[i] >= 'a' && b[i] <= 'z' ? b[i] - 32 : b[i]);
        i++;
    }
}

static int fcb_match(const char *n83, const char *pat)
{
    char f[11];
    u16 i = 0, k = 0;
    for (k = 0; k < 11; k++) f[k] = ' ';
    if (n83[0] == '.') {
        f[0] = '.';
        if (n83[1] == '.') f[1] = '.';
    } else {
        k = 0;
        while (n83[i] && n83[i] != '.' && k < 8) f[k++] = n83[i++];
        if (n83[i] == '.') i++;
        k = 8;
        while (n83[i] && k < 11) f[k++] = n83[i++];
    }
    for (k = 0; k < 11; k++)
        if (pat[k] != '?' && pat[k] != f[k]) return 0;
    return 1;
}

static void put_srch(struct ioreq *ir, struct srch *s, u16 slot, u8 attr, u32 dostime, u32 size,
                     const char *name)
{
    struct srch_entry e;
    u16 i;
    mzero(&e, sizeof(e));
    e.drive = (u8)(s->w->drive + 1);
    for (i = 0; i < 11; i++) e.pattern[i] = s->pat[i];
    e.sattr = s->attr;
    e.local[0] = (u8)slot;
    e.local[1] = (u8)s->gen;
    e.local[2] = (u8)(s->gen >> 8);
    e.local[3] = (u8)(s->gen >> 16);
    e.attr = attr;
    e.time = (u16)dostime;
    e.date = (u16)(dostime >> 16);
    e.size = size;
    for (i = 0; i < 12 && name[i]; i++) e.name[i] = name[i];
    mcopy(ir->data, &e, sizeof(e));
}

/* next match of a DOS search: 0 found (entry written), E_NOMORE */
static int srch_next(struct ioreq *ir, struct srch *s, u16 slot)
{
    struct wvol *w = s->w;
    char n83[13];
    struct udf_node dir;
    int r;
    if (s->label) {
        s->state = 3;
        if (!w->v.label[0]) return E_NOMORE;
        {
            char l[13];
            u16 i, k = 0;
            for (i = 0; i < 11 && w->v.label[i]; i++) {
                if (i == 8) l[k++] = '.';
                l[k++] = w->v.label[i];
            }
            l[k] = 0;
            put_srch(ir, s, slot, FA_LABEL, vx_dostime(), 0, l);
        }
        return 0;
    }
    if (s->state < 2) {
        /* "." and ".." of a subdirectory */
        int st = s->state++;
        if (s->dir.lbn != w->v.root.lbn || s->dir.part != w->v.root.part) {
            if ((FA_DIR & ~s->attr & 0x1E) == 0 && fcb_match(st ? ".." : ".", s->pat) &&
                !(s->must & ~FA_DIR) && udf_iget(&w->v, s->dir, &dir) == 0) {
                put_srch(ir, s, slot, FA_DIR, t2dos(&dir.mtime), 0, st ? ".." : ".");
                return 0;
            }
        }
        if (s->state < 2) return srch_next(ir, s, slot);
    }
    if (s->state > 2) return E_NOMORE;
    r = udf_iget(&w->v, s->dir, &dir);
    if (r) return err(r);
    while ((r = udf_readdir(&w->v, &dir, &s->pos, &fid)) > 0) {
        u8 a;
        if (fid.fchar & (FC_PARENT | FC_DELETED)) continue;
        udf_dosname(fid.name, fid.nlen, n83, oem);
        if (!fcb_match(n83, s->pat)) continue;
        if (udf_iget(&w->v, fid.icb, &nd)) continue;
        a = attr_of(w, live(w, &nd), fid.fchar);
        if (a & ~s->attr & 0x1E) continue;
        if (s->must & ~a & 0x3F) continue;
        put_srch(ir, s, slot, a, t2dos(&nd.mtime), a & FA_DIR ? 0 : nd.big ? 0xFFFFFFFFUL : (u32)nd.size, n83);
        return 0;
    }
    s->state = 3;
    return r < 0 ? err(r) : E_NOMORE;
}

static int __cdecl fs_search(struct ioreq *ir)
{
    struct wvol *w = vol_of(ir);
    int r;
    u16 slot;
    struct srch *s;
    enter();
    DLOG(("WINUDF: search fl %b opt %w len %x pos %x\r\n", ir->flags, ir->options, ir->length, ir->pos));
    if (ir->flags == 0) {                           /* SEARCH_FIRST */
        const u16 *last;
        u16 llen, i, best = 0;
        u32 attr = ir->length;
        /* a free slot, else the one used longest ago */
        for (i = 0; i < NSRCH && srch[i].w; i++)
            if (srch[i].used < srch[best].used) best = i;
        slot = i < NSRCH ? i : best;
        s = &srch[slot];
        mzero(s, sizeof(*s));
        s->w = w;
        s->gen = ++srchgen & 0xFFFFFF;
        s->used = ++srchclock;
        s->attr = (u8)attr;
        s->must = (u8)(attr >> 8);
        r = walk(w, ir->ppath, &last, &llen);
        if (r && r != UE_NOTFOUND) {
            s->w = 0;
            r = r == WALK_NOPATH ? E_NOPATH : err(r);
            goto out;
        }
        if (last) fcb_pat(last, llen, s->pat);
        else for (i = 0; i < 11; i++) s->pat[i] = '?';
        s->dir = pd.loc;
        if ((s->attr & 0x3F) == FA_LABEL) {
            if (pd.loc.lbn != w->v.root.lbn || pd.loc.part != w->v.root.part) {
                s->w = 0;
                r = E_NOMORE;
                goto out;
            }
            s->label = 1;
        }
        r = srch_next(ir, s, slot);
        if (r) s->w = 0;
    } else {                                        /* SEARCH_NEXT */
        struct srch_entry *e = (struct srch_entry *)ir->data;
        u32 gen = e->local[1] | ((u32)e->local[2] << 8) | ((u32)e->local[3] << 16);
        slot = e->local[0];
        if (slot >= NSRCH || srch[slot].w != w || srch[slot].gen != gen) {
            r = E_NOMORE;
            goto out;
        }
        s = &srch[slot];
        s->used = ++srchclock;
        r = srch_next(ir, s, slot);
        if (r) s->w = 0;
    }
out:
    leave();
    return ret(ir, r);
}

/* forget the DOS searches of a volume */
static void srch_drop(struct wvol *w)
{
    u16 i;
    for (i = 0; i < NSRCH; i++)
        if (srch[i].w == w) srch[i].w = 0;
}

/* ---- volume information (GetVolumeInformation) */

static int __cdecl fs_query(struct ioreq *ir)
{
    struct wvol *w = vol_of(ir);
    int r = 0;
    enter();
    DLOG(("WINUDF: query fl %b opt %w len %x pos %x\r\n", ir->flags, ir->options, ir->length, ir->pos));
    if (ir->options == 2) {
        if (ir->data && ir->length >= 4) {
            ir->data[0] = 'U';
            ir->data[1] = 'D';
            ir->data[2] = 'F';
            ir->data[3] = 0;
        }
        ir->options = (u16)(0x4000 | 0x0004 | (udf_writable(&w->v) ? 0x0002 : 0));
        ir->length = 0x010400FFUL;
        ir->pos = w->v.bs;
    } else if (ir->options != 0) r = E_INVFUNC;
    leave();
    return ret(ir, r);
}

static int __cdecl fs_disconnect(struct ioreq *ir)
{
    struct wvol *w = vol_of(ir);
    enter();
    DLOG(("WINUDF: disconnect fl %b opt %w len %x pos %x\r\n", ir->flags, ir->options, ir->length, ir->pos));
    DLOG(("WINUDF: disconnect %b refs %w\r\n", w ? w->drive : 0xFF, w ? w->refs : 0));
    if (w) {
        srch_drop(w);
        udf_sync(&w->v);
        w->dead = 1;
        if (!w->refs) vol_free(w);
    }
    leave();
    return ret(ir, 0);
}

static int __cdecl fs_unsupported(struct ioreq *ir)
{
    DLOG(("WINUDF: unsupported fl %b opt %w len %x pos %x\r\n", ir->flags, ir->options, ir->length, ir->pos));
    return ret(ir, E_INVFUNC);
}

static int __cdecl fs_denied(struct ioreq *ir)
{
    DLOG(("WINUDF: denied fl %b opt %w len %x pos %x\r\n", ir->flags, ir->options, ir->length, ir->pos));
    return ret(ir, E_ACCESS);
}

/* drive IOCTLs (INT 21h 44xxh): the media ID here, the others to the drive */
#pragma pack(push, 1)
struct mid {
    u16 level;
    u32 serial;
    char label[11];
    char fstype[8];
};
#pragma pack(pop)

static int __cdecl fs_ioctl(struct ioreq *ir)
{
    struct wvol *w = vol_of(ir);
    u8 *c = (u8 *)ir->aux2;     /* client registers */
    u16 ax = *(u16 *)(c + 0x1C), cx = *(u16 *)(c + 0x18), bx = *(u16 *)(c + 0x10);
    int r = 0;
    enter();
    DLOG(("WINUDF: ioctl fl %b opt %w len %x pos %x\r\n", ir->flags, ir->options, ir->length, ir->pos));
    if (ax == 0x440D && (cx == 0x0866 || cx == 0x4866)) {
        struct mid *m = (struct mid *)vx_map_flat(c, 0x3C14);
        u16 i;
        m->level = 0;
        m->serial = w->v.serial;
        for (i = 0; i < 11; i++) m->label[i] = w->v.label[i] && i < 32 ? w->v.label[i] : ' ';
        for (i = 0; i < 11 && w->v.label[i]; i++)
            ;
        for (; i < 11; i++) m->label[i] = ' ';
        mcopy(m->fstype, "UDF     ", 8);
    } else if (ax == 0x440D && (cx == 0x0846 || cx == 0x4846)) {
        r = E_ACCESS;                               /* set media ID */
    } else if (ax == 0x4408) {
        /* removable media? Answered here: KERNEL's profile code (WritePrivateProfileString,
           Explorer's desktop.ini) asks it while it writes a file, and the I/O Supervisor would
           take it to the real-mode DOS of the system VM, which waits for this very drive */
        u32 fl = w->dcb ? *(u32 *)(w->dcb + 0x20) : 0;
        *(u16 *)(c + 0x1C) = (u16)((w->cdrom || (fl & DCB_DEV_REMOVABLE)) ? 0 : 1);
    } else if (ax == 0x4409) {
        *(u16 *)(c + 0x14) = 0;                     /* a local drive */
    } else if (ax == 0x440D && (cx == 0x086F || cx == 0x486F)) {
        u8 *m = (u8 *)vx_map_flat(c, 0x3C14);       /* drive map information */
        u32 map = 1UL << (w->drive & 31);
        m[1] = 16;
        m[2] = 0x01;                                /* protected mode only */
        m[3] = w->bunit ? w->bunit : 0xFF;
        mcopy(m + 4, &map, 4);
        mcopy(m + 8, &w->pstart, 4);
        mzero(m + 12, 4);
    } else {
        struct ior *q = w->ior;
        mzero(q, sizeof(*q));
        q->func = IOR_GEN_IOCTL;
        q->flags = IORF_VERSION_002 | IORF_HIGH_PRIORITY | IORF_16BIT_IOCTL;
        q->callback = (u32)vx_iorcb;
        q->private_client = w->vrp->delta_to_ior;
        q->sgd_lin_phys = (u32)q + 0x60;
        q->vol_designtr = (u8)((u8)bx ? (u8)bx - 1 : w->drive);
        *(u16 *)((u8 *)q + 0x2C) = (u16)((u8)bx ? (u8)bx : w->drive + 1);    /* drive */
        *(u16 *)((u8 *)q + 0x2E) = ax;                                       /* function */
        *(u32 *)((u8 *)q + 0x30) = cx;                                       /* parameter */
        *(u32 *)((u8 *)q + 0x34) = vx_map_flat(c, 0x3C14);                  /* buffer */
        *(u32 *)((u8 *)q + 0x38) = (u32)c;                                   /* client */
        *(u32 *)((u8 *)q + 0x3C) = 1;                                        /* return */
        q->req_req_handle = (u32)q;
        q->req_vol_handle = 0;
        IOR_PENDING(q) = 1;
        vx_ios_send(q, w->dcb);
        vx_cli();
        while (IOR_PENDING(q)) vx_block((u32)q, 1);
        vx_sti();
        r = *(u16 *)((u8 *)q + 0x3C);
        *(u16 *)(c + 0x1C) = (u16)r;
        if (q->status < IORS_ERROR) r = 0;
        else if (!r) r = E_INVFUNC;
    }
    leave();
    return ret(ir, r);
}

/* ---- Win32 find (INT 21h 714Eh, FindFirstFile) */

static int __cdecl fs_findnext(struct ioreq *ir);
static int __cdecl fs_findclose(struct ioreq *ir);
static int __cdecl fs_findnone(struct ioreq *ir);
static const struct hndlmisc fmisc;

static void put_find(u8 *out, u8 attr, struct udf_node *n, const u16 *name, u16 len, int big)
{
    u64 ft;
    u16 i, sl;
    u16 *fn = (u16 *)(out + 0x2C), *an = (u16 *)(out + 0x234);
    *(u32 *)out = attr;
    ft = t2ft(&n->ctime);
    mcopy(out + 4, &ft, 8);
    ft = t2ft(&n->atime);
    mcopy(out + 0x0C, &ft, 8);
    ft = t2ft(&n->mtime);
    mcopy(out + 0x14, &ft, 8);
    *(u32 *)(out + 0x1C) = (attr & FA_DIR) ? 0 : (u32)(n->size >> 32);
    *(u32 *)(out + 0x20) = (attr & FA_DIR) ? 0 : (u32)n->size;
    *(u32 *)(out + 0x24) = 0;
    *(u32 *)(out + 0x28) = 0;
    for (i = 0; i < len && i < 259; i++) fn[i] = name[i];
    fn[i] = 0;
    an[0] = 0;
    if (big) {
        sl = shortname(name, len, uname);
        if (!same83(name, len, uname))
            for (i = 0; i <= sl && i < 14; i++) an[i] = uname[i];
    }
}

/* next match of a Win32 search: 0 (written to ir_data), E_NOMORE */
static int find_next(struct ioreq *ir, struct wfind *f)
{
    struct wvol *w = f->w;
    struct udf_node dir;
    int r;
    if (f->label) {
        if (f->state > 2 || !w->v.label[0]) return E_NOMORE;
        f->state = 3;
        if (udf_iget(&w->v, w->v.root, &dir)) return E_NOMORE;
        {
            u16 i;
            for (i = 0; i < 32 && w->v.ulabel[i]; i++) uname[i] = w->v.ulabel[i];
            uname[i] = 0;
            mcopy(fid.name, uname, (u32)(i + 1) * 2);
            put_find(ir->data, FA_LABEL, &dir, fid.name, i, 0);
        }
        return 0;
    }
    while (f->state < 2) {
        static const u16 dot[] = { '.', 0 }, dotdot[] = { '.', '.', 0 };
        int st = f->state++;
        if (f->dir.lbn == w->v.root.lbn && f->dir.part == w->v.root.part) continue;
        if (FA_DIR & ~f->attr & 0x1E) continue;
        if ((f->attr >> 8) & ~FA_DIR & 0x3F) continue;
        if (!vx_metamatch(f->pat, st ? dotdot : dot, UFLG_META | UFLG_NT)) continue;
        if (udf_iget(&w->v, f->dir, &dir)) continue;
        put_find(ir->data, FA_DIR, &dir, st ? dotdot : dot, (u16)(st + 1), 0);
        return 0;
    }
    if (f->state > 2) return E_NOMORE;
    r = udf_iget(&w->v, f->dir, &dir);
    if (r) return err(r);
    while ((r = udf_readdir(&w->v, &dir, &f->pos, &fid)) > 0) {
        u8 a;
        u16 sn[13];
        if (fid.fchar & (FC_PARENT | FC_DELETED)) continue;
        fid.name[fid.nlen] = 0;
        if (!vx_metamatch(f->pat, fid.name, UFLG_META | UFLG_NT)) {
            shortname(fid.name, fid.nlen, sn);
            if (!vx_metamatch(f->pat, sn, UFLG_META | UFLG_NT_DOS)) continue;
        }
        if (udf_iget(&w->v, fid.icb, &nd)) continue;
        a = attr_of(w, live(w, &nd), fid.fchar);
        if (a & ~f->attr & 0x1E) continue;
        if ((f->attr >> 8) & ~a & 0x3F) continue;
        put_find(ir->data, a, live(w, &nd), fid.name, fid.nlen, 1);
        return 0;
    }
    f->state = 3;
    return r < 0 ? err(r) : E_NOMORE;
}

static int __cdecl fs_findopen(struct ioreq *ir)
{
    struct wvol *w = vol_of(ir);
    const u16 *last;
    u16 llen, k;
    int r;
    struct wfind *f;
    enter();
    DLOG(("WINUDF: findopen fl %b opt %w len %x pos %x\r\n", ir->flags, ir->options, ir->length, ir->pos));
    r = walk(w, ir->ppath, &last, &llen);
    if (r && r != UE_NOTFOUND) {
        r = r == WALK_NOPATH ? E_NOPATH : err(r);
        goto out;
    }
    f = (struct wfind *)zalloc(sizeof(*f));
    if (!f) {
        r = E_NOMEM;
        goto out;
    }
    f->w = w;
    f->dir = pd.loc;
    f->attr = ir->length;
    if (last) {
        for (k = 0; k < llen; k++) f->pat[k] = last[k];
        f->pat[llen] = 0;
    } else {
        f->pat[0] = '*';
        f->pat[1] = 0;
    }
    if ((f->attr & 0x3F) == FA_LABEL) f->label = 1;
    r = find_next(ir, f);
    DLOG(("WINUDF: findopen %u attr %x -> %x\r\n", f->pat, f->attr, (u32)r));
    if (r) {
        vx_heap_free(f, 0);
        if (r == E_NOMORE) r = E_NOTFOUND;
        goto out;
    }
    w->refs++;
    ir->fh = f;
    {
        struct hfunc *hf = (struct hfunc *)ir->aux1;
        hf->read = f->label ? fs_findnone : fs_findnext;
        hf->write = fs_denied;
        hf->misc = &fmisc;
    }
out:
    leave();
    return ret(ir, r);
}

static int __cdecl fs_findnext(struct ioreq *ir)
{
    struct wfind *f = (struct wfind *)ir->fh;
    int r;
    enter();
    DLOG(("WINUDF: findnext fl %b opt %w len %x pos %x\r\n", ir->flags, ir->options, ir->length, ir->pos));
    r = f->w->dead ? E_NOMORE : find_next(ir, f);
    leave();
    return ret(ir, r);
}

static int __cdecl fs_findnone(struct ioreq *ir)
{
    return ret(ir, E_NOMORE);
}

static int __cdecl fs_findclose(struct ioreq *ir)
{
    struct wfind *f = (struct wfind *)ir->fh;
    enter();
    DLOG(("WINUDF: findclose fl %b opt %w len %x pos %x\r\n", ir->flags, ir->options, ir->length, ir->pos));
    if (f) {
        struct wvol *w = f->w;
        vx_heap_free(f, 0);
        vol_unref(w);
    }
    leave();
    return ret(ir, 0);
}

/* ------------------------------------------------------------------ handle functions */

static int __cdecl fs_read(struct ioreq *ir)
{
    struct whandle *h = (struct whandle *)ir->fh;
    struct wobj *o = h->o;
    s32 n;
    int r = 0;
    enter();
    DLOG(("WINUDF: read fl %b opt %w len %x pos %x\r\n", ir->flags, ir->options, ir->length, ir->pos));
    if ((h->mode & ACCESS_MODE) == 1) r = E_ACCESS;
    else if (o->w->dead) r = E_NOTREADY;
    else if (ir->length) {
        u32 len = ir->length;
        if ((u64)ir->pos >= o->n.size) len = 0;
        else if ((u64)ir->pos + len > o->n.size) len = (u32)(o->n.size - ir->pos);
        n = len ? udf_read(&o->w->v, &o->n, ir->pos, len, ir->data) : 0;
        if (n < 0) {
            r = err((int)n);
            ir->length = 0;
        } else {
            ir->length = (u32)n;
            ir->pos += (u32)n;
        }
    }
    leave();
    return ret(ir, r);
}

static int __cdecl fs_write(struct ioreq *ir)
{
    struct whandle *h = (struct whandle *)ir->fh;
    struct wobj *o = h->o;
    struct wvol *w = o->w;
    int r = 0;
    enter();
    DLOG(("WINUDF: write fl %b opt %w len %x pos %x\r\n", ir->flags, ir->options, ir->length, ir->pos));
    if ((h->mode & ACCESS_MODE) == 0 || (h->mode & ACCESS_MODE) == 3) r = E_ACCESS;
    else if (w->dead) r = E_NOTREADY;
    else if (ir->length == 0) {
        r = err(udf_truncate(&w->v, &o->n, ir->pos));
    } else {
        s32 n = udf_write(&w->v, &o->n, ir->pos, ir->length, ir->data);
        if (n < 0) {
            r = n == UE_NOSPACE ? E_DISKFULL : err((int)n);
            ir->length = 0;
        } else {
            if (n == 0) r = E_DISKFULL;
            ir->length = (u32)n;
            ir->pos += (u32)n;
        }
    }
    leave();
    return ret(ir, r);
}

static int __cdecl fs_seek(struct ioreq *ir)
{
    struct whandle *h = (struct whandle *)ir->fh;
    int r = 0;
    enter();
    DLOG(("WINUDF: seek fl %b opt %w len %x pos %x\r\n", ir->flags, ir->options, ir->length, ir->pos));
    if (ir->flags == 2) {                           /* FILE_END */
        s32 p = (s32)h->o->n.size + (s32)ir->pos;
        if (p < 0) r = 131;
        else ir->pos = (u32)p;
    } else if (ir->flags != 0) r = E_INVFUNC;
    leave();
    return ret(ir, r);
}

static int __cdecl fs_close(struct ioreq *ir)
{
    struct whandle *h = (struct whandle *)ir->fh, **pp;
    struct wobj *o = h->o, **op;
    struct wvol *w = o->w;
    enter();
    DLOG(("WINUDF: close fl %b opt %w len %x pos %x\r\n", ir->flags, ir->options, ir->length, ir->pos));
    if (ir->flags == 2) {                           /* CLOSE_FINAL */
        u8 acc = (u8)(h->mode & ACCESS_MODE);
        for (pp = &o->handles; *pp; pp = &(*pp)->next)
            if (*pp == h) {
                *pp = h->next;
                break;
            }
        if (acc == 1 || acc == 2) {
            if (o->nwr) o->nwr--;
            if (w->v.nopen) w->v.nopen--;
        }
        vx_heap_free(h, 0);
        if (!o->handles) {
            for (op = &w->objs; *op; op = &(*op)->next)
                if (*op == o) {
                    *op = o->next;
                    break;
                }
            vx_heap_free(o, 0);
            if (!w->dead) udf_sync(&w->v);
            vol_unref(w);
        } else if (!w->dead) udf_sync(&w->v);
    }
    leave();
    return ret(ir, 0);
}

static int __cdecl fs_commit(struct ioreq *ir)
{
    DLOG(("WINUDF: commit fl %b opt %w len %x pos %x\r\n", ir->flags, ir->options, ir->length, ir->pos));
    return ret(ir, 0);
}

static int __cdecl fs_locks(struct ioreq *ir)
{
    DLOG(("WINUDF: locks fl %b opt %w len %x pos %x\r\n", ir->flags, ir->options, ir->length, ir->pos));
    return ret(ir, ir->flags <= 1 ? 0 : E_INVFUNC);
}

static int __cdecl fs_filetimes(struct ioreq *ir)
{
    struct whandle *h = (struct whandle *)ir->fh;
    struct wobj *o = h->o;
    struct udf_time t;
    int r = 0;
    enter();
    DLOG(("WINUDF: filetimes fl %b opt %w len %x pos %x\r\n", ir->flags, ir->options, ir->length, ir->pos));
    switch (ir->flags) {
    case 0:
        ir->aux2 = t2dos(&o->n.mtime);
        break;
    case 4:
        ir->aux2 = t2dos(&o->n.atime);
        break;
    case 6:
        ir->aux2 = t2dos(&o->n.ctime);
        ir->options = (u16)(o->n.ctime.cs + ((o->n.ctime.sec & 1) ? 100 : 0));
        break;
    case 1:
    case 5:
        if (!udf_writable(&o->w->v)) r = E_WRPROT;
        else {
            dos2t(ir->aux2, 0, &t);
            r = err(udf_settime(&o->w->v, &o->n, ir->flags == 1 ? &t : 0, ir->flags == 5 ? &t : 0));
        }
        break;
    case 7:
        break;
    default:
        r = E_INVFUNC;
    }
    leave();
    return ret(ir, r);
}

static int __cdecl fs_enumhandle(struct ioreq *ir)
{
    struct whandle *h = (struct whandle *)ir->fh;
    struct wobj *o = h->o;
    int r = 0;
    enter();
    DLOG(("WINUDF: enumhandle fl %b opt %w len %x pos %x\r\n", ir->flags, ir->options, ir->length, ir->pos));
    if (ir->flags == 0) {                           /* ENUMH_GETFILEINFO */
        u8 *b = ir->data;
        u64 ft;
        *(u32 *)b = attr_of(o->w, &o->n, o->fchar);
        ft = t2ft(&o->n.ctime);
        mcopy(b + 4, &ft, 8);
        ft = t2ft(&o->n.atime);
        mcopy(b + 0x0C, &ft, 8);
        ft = t2ft(&o->n.mtime);
        mcopy(b + 0x14, &ft, 8);
        *(u32 *)(b + 0x1C) = o->w->v.serial;
        *(u32 *)(b + 0x20) = (u32)(o->n.size >> 32);
        *(u32 *)(b + 0x24) = (u32)o->n.size;
        *(u32 *)(b + 0x28) = o->n.links ? o->n.links : 1;
        *(u32 *)(b + 0x2C) = o->n.loc.part;
        *(u32 *)(b + 0x30) = o->n.loc.lbn;
    } else r = E_NOTSUPP;
    leave();
    return ret(ir, r);
}

/* ------------------------------------------------------------------ tables */

static const struct volfunc volfunc = {
    IFS_VERSION, IFS_REVISION, 15,
    {
        fs_delete, fs_dir, fs_attrib, fs_flush, fs_diskinfo, fs_open, fs_rename, fs_search,
        fs_query, fs_disconnect, fs_unsupported, fs_ioctl, fs_denied, fs_findopen,
        fs_unsupported
    }
};

static const struct hndlmisc hmisc = {
    IFS_VERSION, IFS_REVISION, 8,
    {
        fs_seek, fs_close, fs_commit, fs_locks, fs_filetimes, fs_unsupported, fs_unsupported,
        fs_enumhandle
    }
};

static const struct hndlmisc fmisc = {
    IFS_VERSION, IFS_REVISION, 8,
    {
        fs_unsupported, fs_findclose, fs_unsupported, fs_unsupported, fs_unsupported,
        fs_unsupported, fs_unsupported, fs_unsupported
    }
};

/* ------------------------------------------------------------------ init */

void __cdecl udf_vxd_init(void)
{
    u16 i;
    u8 b[128];
    u32 ver;
    if (!vx_ifs_version()) return;
    udf_osclass = 5;                /* Windows 9x */
    sem = vx_sem_create();
    if (!sem) return;
    bounce = (u8 *)vx_heap_alloc(BOUNCE, 0);
    udf_tmp = (u8 *)vx_heap_alloc(UDF_MAXBS, 0);
    if (!bounce || !udf_tmp) return;
    udf_cache_init(UDF_MAXBS);
    for (i = 0; i < 64; i++) {
        u8 *p = (u8 *)vx_heap_alloc(UDF_MAXBS, 0);
        if (!p || udf_cache_slot(p)) break;
    }
    for (i = 0; i < 128; i++) b[i] = (u8)(0x80 + i);
    for (i = 0; i < 128; i++) oem[i] = (u16)(0x80 + i);
    vx_bcstouni(oem, b, 128, BCS_OEM);
    tzbias = vx_tzbias();
    drp.aer = (u32)aer;
    drp.ilb = (u32)&ilb;
    vx_ios_register(&drp);
    ver = vx_vmm_version();
    /* Windows 98 and ME: the mount routine of the CD and DVD file systems' order, before
       CDFS (ISO 9660 / Joliet of the bridge discs) like the UDF driver of Windows */
    if (ver >= 0x040A) vx_register_fsd(0, fs_mount, 0x40, 0, 3, IFSMGRVERSION);
    else vx_register_mount(fs_mount, IFSMGRVERSION, 0);
    DLOG(("WINUDF: init vmm %w ilb %x tz %x\r\n", ver, ilb.service_rtn, (u32)tzbias));
}
