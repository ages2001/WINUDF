/* udf.h - UDF file system core: ECMA-167 / OSTA UDF 1.02, 1.50, 2.00, 2.01, 2.50 and 2.60
   volumes on CD, DVD and BD media and on disks, read and write. The same sources make the
   DOS redirector WINUDF.EXE (16-bit, UDF_DOS16), the Windows 95/98/ME file system driver
   WINUDF.VXD (32-bit) and the test program of the host (UDF_HOST).

   Partitions: physical (type 1), virtual (VAT of UDF 1.50 and 2.00+, write-once media),
   sparable (sparing tables) and metadata (metadata file, mirror and bitmap of UDF 2.50+).
   Files: file entries and extended file entries, ICB strategies 4 and 4096, short, long
   and extended allocation descriptors with allocation extent descriptors, data embedded in
   the ICB, OSTA compressed Unicode names (compression IDs 8, 16, 254, 255). */
#ifndef UDF_H
#define UDF_H

#if defined(UDF_HOST) && !defined(__WATCOMC__)
#include <stdint.h>
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef int32_t s32;
typedef int16_t s16;
typedef uint64_t u64;
#else
typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned long u32;
typedef long s32;
typedef short s16;
typedef unsigned long long u64;
#endif

#ifdef UDF_DOS16
#define FAR __far
#else
#define FAR
#endif

#define UDF_MAXBS       4096    /* largest logical block size */
#ifdef UDF_DOS16
#define UDF_MAXPM       3       /* partition maps of a logical volume */
#else
#define UDF_MAXPM       4
#endif
#define UDF_MAXREV      0x0260  /* newest UDF revision read and written */
#define UDF_NAMEMAX     255     /* Unicode characters of a file name */

/* little-endian fields of on-disc structures */
#define G8(p, o)        (((const u8 FAR *)(p))[o])
#define G16(p, o)       ((u16)(G8(p, o) | ((u16)G8(p, (o) + 1) << 8)))
#define G32(p, o)       ((u32)G16(p, o) | ((u32)G16(p, (o) + 2) << 16))
#define G64(p, o)       ((u64)G32(p, o) | ((u64)G32(p, (o) + 4) << 32))
void udf_p16(u8 FAR *p, u16 v);
void udf_p32(u8 FAR *p, u32 v);
void udf_p64(u8 FAR *p, u64 v);

/* errors (negative return values) */
#define UE_IO           (-1)    /* device error */
#define UE_CORRUPT      (-2)    /* bad or unexpected structure */
#define UE_NOTFOUND     (-3)
#define UE_NOTDIR       (-4)
#define UE_ISDIR        (-5)
#define UE_EXISTS       (-6)
#define UE_NOSPACE      (-7)
#define UE_RDONLY       (-8)    /* read-only volume or file */
#define UE_NOTEMPTY     (-9)
#define UE_NOMEM        (-10)
#define UE_NOTUDF       (-11)   /* no UDF volume */
#define UE_UNSUPP       (-12)   /* newer UDF revision or unknown partition type */
#define UE_ACCESS       (-13)   /* no write permission */
#define UE_BADNAME      (-14)
#define UE_CHANGED      (-15)   /* the medium was changed */

/* device the volume is on, set up by the platform */
struct udf_dev {
    /* n device sectors from sector lba; 0 ok, nonzero error */
    int (*read)(struct udf_dev *d, u64 lba, u16 n, u8 FAR *buf);
    int (*write)(struct udf_dev *d, u64 lba, u16 n, const u8 FAR *buf);   /* NULL: read-only */
    u64 sectors;                /* size of the device or partition, 0 when not known */
    u16 ssize;                  /* bytes per device sector (512, 2048, 4096) */
    u8 optical;                 /* CD, DVD, BD: no partition table, 2048-byte blocks */
    u8 ro;                      /* write protected */
    u32 gen;                    /* platform: medium generation, changes with the medium */
    void *ctx;                  /* platform */
};

/* address of a logical block in a partition (lb_addr) */
struct udf_loc {
    u32 lbn;
    u16 part;                   /* partition reference number (index of the partition map) */
};

#define PM_PHYS         1
#define PM_VIRT         2
#define PM_SPAR         3
#define PM_META         4

struct udf_node;

struct udf_pmap {
    u8 type;                    /* PM_* */
    u8 phys;                    /* index of the physical partition map underneath */
    u8 access;                  /* partition access type (0-4) of the partition descriptor */
    u8 nst;                     /* sparable: sparing tables */
    u16 num;                    /* partition number */
    u16 packet;                 /* sparable: blocks per packet */
    u32 start;                  /* physical: first absolute block */
    u32 len;                    /* physical: blocks */
    u32 ubm_lbn, ubm_len;       /* unallocated space bitmap (lbn in this partition, bytes) */
    u32 ust_lbn;                /* unallocated space table: lbn, 0xFFFFFFFF none */
    u32 st_abs[4];              /* sparable: absolute block of each sparing table */
    u32 st_len;                 /* sparable: entries in the table */
    u32 vat_abs;                /* virtual: absolute block of the VAT file entry */
    u32 vat_off, vat_n;         /* virtual: offset of the first entry, number of entries */
    u32 meta_lbn[3];            /* metadata: file, mirror, bitmap (lbn in the physical one) */
    u32 aunit;                  /* metadata: allocation unit size */
    u16 align;                  /* metadata: alignment unit size */
    u8 dup;                     /* metadata: the mirror duplicates the file */
    u8 badmeta;                 /* metadata: the main file is unreadable, the mirror is used */
    u32 free;                   /* free blocks (logical volume integrity descriptor) */
    u32 size;                   /* blocks (logical volume integrity descriptor) */
    u32 hint;                   /* allocation: next block to look at */
    u32 ubm_bits;               /* bits of the space bitmap, 0 not read yet */
};

/* ICB file types */
#define FT_DIR          4
#define FT_FILE         5
#define FT_SYMLINK      12
#define FT_STREAMDIR    13
#define FT_VAT          248
#define FT_META         250
#define FT_MMIRROR      251
#define FT_MBITMAP      252

/* allocation descriptor types (ICB tag flags bits 0-2) */
#define AD_SHORT        0
#define AD_LONG         1
#define AD_EXT          2
#define AD_ICB          3

/* file identifier characteristics */
#define FC_HIDDEN       0x01
#define FC_DIR          0x02
#define FC_DELETED      0x04
#define FC_PARENT       0x08
#define FC_META         0x10

/* ICB tag flags */
#define ICBF_ARCHIVE    0x0020
#define ICBF_SYSTEM     0x0400
#define ICBF_STREAM     0x2000

/* permissions: owner write and delete */
#define PERM_OWRITE     0x0800
#define PERM_WRITE      (0x0002 | 0x0040 | 0x0800)
#define PERM_DELETE     (0x0010 | 0x0200 | 0x4000)

/* timestamp converted from the 12-byte UDF form */
struct udf_time {
    u16 year;
    u8 month, day, hour, min, sec;
    u8 cs;                      /* hundredths */
    s16 tz;                     /* minutes east of UTC, UDF_NOTZ: not specified (local) */
};
#define UDF_NOTZ        (-2047)

/* an ICB (file entry) read in */
struct udf_node {
    struct udf_loc loc;         /* address of the ICB */
    u32 feabs;                  /* absolute block of the file entry in use (strategy 4096) */
    u8 ftype;                   /* FT_* */
    u8 efe;                     /* extended file entry */
    u8 adtype;                  /* AD_* */
    u8 big;                     /* file larger than 4 GB - 1 */
    u16 icbflags;
    u16 links;
    u32 perm;
    u32 lea, lad;               /* lengths of the extended attributes and allocation descriptors */
    u64 size;                   /* information length */
    u64 blocks;                 /* logical blocks recorded */
    u64 uniq;                   /* unique ID */
    struct udf_time mtime, atime, ctime;
    struct udf_loc stream;      /* named streams directory (extended file entry), lbn ~0 none */
    /* allocation descriptor cursor of the last mapping (sequential access) */
    u32 c_off;                  /* file offset where the cursor extent begins */
    u32 c_blk;                  /* absolute block the descriptor at c_pos is in */
    u16 c_pos, c_end;           /* byte offset of the descriptor in that block, end of the list */
    u32 c_elen;                 /* extent at the cursor: length in bytes, */
    u32 c_lbn;                  /*  lbn, */
    u16 c_part;                 /*  partition, */
    u8 c_type;                  /*  extent type (0 recorded, 1 allocated, 2 sparse), */
    u8 c_ok;                    /*  cursor valid */
    u32 c_ablk;                 /* block and offset of the descriptor of the cursor extent */
    u16 c_apos;
    u8 tset;                    /* mtime / atime changed, written with the file entry */
};

/* file identifier descriptor (directory entry) read by udf_readdir */
struct udf_fid {
    u32 pos;                    /* offset in the directory */
    u16 len;                    /* bytes of the descriptor */
    u8 fchar;                   /* FC_* */
    u8 lfi;                     /* length of the name field */
    struct udf_loc icb;
    u16 nlen;                   /* name: Unicode characters in name[] */
    u16 name[UDF_NAMEMAX + 1];
};

/* volume flags */
#define VF_RDONLY       0x0001  /* mounted read-only */
#define VF_DIRTY        0x0002  /* the integrity descriptor says "open" (changes not closed) */
#define VF_WASOPEN      0x0004  /* the volume was not closed when mounted */
#define VF_ICBLONG      0x0008  /* file data needs long_ad (metadata partition) */
#define VF_EFE          0x0010  /* new files get extended file entries */
#define VF_VAT          0x0020

struct udf_vol {
    struct udf_dev *dev;
    u64 base;                   /* first device sector of the volume */
    u32 nblocks;                /* logical blocks of the device from base (0 unknown) */
    u16 bs;                     /* logical block size */
    u8 bsh;                     /* log2(bs) */
    u8 ssh;                     /* log2(bs / sector size) */
    u16 rev;                    /* UDF revision of the domain identifier */
    u16 flags;                  /* VF_* */
    u8 npm;
    u8 metapm;                  /* index of the metadata partition map, 0xFF none */
    u8 datapm;                  /* map that file data goes to */
    u8 icbpm;                   /* map that file entries and directories go to */
    struct udf_pmap pm[UDF_MAXPM];
    struct udf_loc root;        /* root directory ICB */
    struct udf_loc sroot;       /* system stream directory ICB, lbn 0xFFFFFFFF none */
    u32 lvid_abs;               /* absolute block of the current logical volume integrity desc. */
    u32 nfiles, ndirs;          /* logical volume integrity descriptor */
    u16 minrd, minwr, maxwr;    /* UDF revisions of the integrity descriptor */
    u64 uniq;                   /* next unique ID */
    u32 serial;                 /* volume serial number (DOS, Windows) */
    u32 gen;                    /* dev->gen when mounted */
    char label[33];             /* logical volume identifier, OEM characters */
#ifndef UDF_DOS16
    u16 ulabel[33];             /* logical volume identifier, Unicode */
#endif
    u16 nopen;                  /* platform: files open for writing */
    struct udf_node sys[3];     /* metadata file, mirror and bitmap; VAT (sys[0]) */
};

/* ---- udfcore.c ---- */
extern u8 FAR *udf_tmp;         /* scratch block (UDF_MAXBS bytes), set up by the platform */
void udf_cache_init(u16 slotsize);
int udf_cache_slot(u8 FAR *buf);         /* one more cache buffer of slotsize bytes */
void udf_cache_drop(struct udf_vol *v);     /* forget the blocks of a volume */
u8 FAR *udf_bget(struct udf_vol *v, u32 abs);
int udf_bput(struct udf_vol *v, u32 abs, u8 FAR *data);
u8 FAR *udf_bnew(struct udf_vol *v, u32 abs);
u16 udf_crc(const u8 FAR *p, u16 len, u16 crc);
int udf_tagok(const u8 FAR *p, u16 ident, u32 loc, u16 maxlen);
void udf_tagset(u8 FAR *p, u16 ident, u16 ver, u32 loc, u16 len);

int udf_mount(struct udf_vol *v, struct udf_dev *d, u64 base, u32 nblocks);
int udf_probe_bs(struct udf_dev *d, u64 base, u32 nsect);
int udf_map(struct udf_vol *v, u16 part, u32 lbn, u32 *abs);
int udf_iget(struct udf_vol *v, struct udf_loc loc, struct udf_node *n);
s32 udf_read(struct udf_vol *v, struct udf_node *n, u32 off, u32 len, u8 FAR *buf);
int udf_bmap(struct udf_vol *v, struct udf_node *n, u32 off, u32 *abs, u32 *run);
int udf_readdir(struct udf_vol *v, struct udf_node *dir, u32 *pos, struct udf_fid *f);
int udf_dirempty(struct udf_vol *v, struct udf_node *dir);   /* uses udf_fidw[1] */
extern struct udf_fid udf_fidw[2];          /* entries of the write operations */
int udf_lookup(struct udf_vol *v, struct udf_node *dir, const u16 *name, u16 nlen,
               struct udf_fid *f, const u16 *oemtab);
void udf_cs0(const u8 FAR *p, u16 len, u16 *out, u16 *nout, u16 max);
void udf_timeget(const u8 FAR *p, struct udf_time *t);
void udf_timeset(u8 FAR *p, const struct udf_time *t);
int udf_dosname(const u16 *name, u16 len, char *out, const u16 *oemtab);
int udf_fcbmatch(const char *fcb, const char *pat);
u8 udf_dosattr(struct udf_node *n, u8 fchar);
u16 udf_upper(u16 c);

/* core internals shared with udfwr.c */
void udf_fcopy(u8 FAR *d, const u8 FAR *s, u16 n);
void udf_fset(u8 FAR *d, u8 c, u16 n);
int udf_devread(struct udf_vol *v, u32 abs, u16 n, u8 FAR *buf);
int udf_devwrite(struct udf_vol *v, u32 abs, u16 n, const u8 FAR *buf);
void udf_cache_update(struct udf_vol *v, u32 abs, u16 n, const u8 FAR *data);
void udf_pin(u8 FAR *p);                    /* keep a cache buffer while others are read */
void udf_unpin(void);                       /* the last pinned one */
void udf_ad_rewind(struct udf_vol *v, struct udf_node *n);
int udf_ad_next(struct udf_vol *v, struct udf_node *n);
u16 udf_adsize(u8 t);
void udf_node_parse(struct udf_vol *v, struct udf_node *n, const u8 FAR *p);
#define FEHDR(n)        ((n)->efe ? 216 : 176)

/* ---- udfwr.c ---- */
int udf_begin(struct udf_vol *v);           /* before a change: integrity "open" */
int udf_sync(struct udf_vol *v);            /* integrity "close" when nothing is open */
int udf_create(struct udf_vol *v, struct udf_node *dir, const u16 *name, u16 nlen,
               int isdir, u8 dosattr, const struct udf_time *t, struct udf_node *out,
               u32 *fidpos);
s32 udf_write(struct udf_vol *v, struct udf_node *n, u32 off, u32 len, const u8 FAR *buf);
int udf_truncate(struct udf_vol *v, struct udf_node *n, u32 size);
int udf_unlink(struct udf_vol *v, struct udf_node *dir, u32 fidpos);
int udf_rename(struct udf_vol *v, struct udf_node *sdir, u32 spos, struct udf_node *ddir,
               const u16 *name, u16 nlen, u32 dpos_replace);
int udf_setattr(struct udf_vol *v, struct udf_node *dir, u32 fidpos, struct udf_node *n,
                u8 dosattr);
int udf_settime(struct udf_vol *v, struct udf_node *n, const struct udf_time *mt,
                const struct udf_time *at);
int udf_writable(struct udf_vol *v);
u32 udf_freeblocks(struct udf_vol *v);
u32 udf_totalblocks(struct udf_vol *v);

/* platform services used by the core */
extern u8 udf_osclass;          /* OS class of the implementation identifier (1 DOS, 5 Win9x) */
void udf_now(struct udf_time *t);
#ifdef UDF_DEBUG
void udf_log(const char *fmt, ...);
#define ULOG(x) udf_log x
#else
#define ULOG(x)
#endif

#endif
