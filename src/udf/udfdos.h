/* udfdos.h - WINUDF.EXE (DOS redirector): shared definitions of udfdos.c and udfinit.c */
#ifndef UDFDOS_H
#define UDFDOS_H

struct rregs {
    u16 ax, bx, cx, dx, si, di, bp, ds, es, fl, ss, sp;
};

/* DOS data and the offsets of the swappable data area for this DOS version */
struct dosinfo {
    u8 FAR *sda;
    u8 FAR *cds;                /* current directory structures */
    u16 cdssize;                /* bytes per CDS entry */
    u8 lastdrive;
    u8 major, minor;
    u16 o_fn1, o_fn2, o_sattr, o_curcds;
    u16 o_extact, o_extattr, o_extmode;
    u16 o_wfp, o_renwfp;        /* offsets (in the DOS data segment) of the path arguments */
};

#define DV_CD           1       /* CD/DVD/BD drive of MSCDEX (INT 2Fh 1508h) */
#define DV_BIOS         2       /* BIOS disk (INT 13h extensions) */
#define DV_WIN          3       /* WINDISK / WINUSB disk (INT 2Fh API) */

#define MAXVOL          6

struct dvol {
    u8 mounted;
    u8 drive;                   /* 0 = A */
    u8 kind;                    /* DV_* */
    u8 mplex;                   /* DV_WIN: multiplex number of the driver */
    u16 unit;                   /* CD: drive number; BIOS: 80h..; WIN: disk index */
    u16 sum, tried;             /* CD: checksum of the disc's first descriptors */
    u32 checked;                /* CD: BIOS ticks of the last check */
    u8 part;                    /* partition number (0 whole disk) */
    u8 subunit;                 /* CD: unit of the device driver */
    void __far *devhdr;         /* CD: device driver header (called directly) */
    struct udf_dev dev;
    struct udf_vol v;
};

extern struct rregs __cdecl rr;
extern struct dosinfo di;
extern struct dvol vols[MAXVOL];
extern u8 nvols;
extern u16 oem[128];
extern u8 udf_rdonly_all;

int __cdecl intx(int n, struct rregs *r);
void __cdecl farcall(void __far *fn, struct rregs *r);
void __cdecl hook2f(void);
void __cdecl unhook2f(void);
extern void __far * __cdecl old2f;
extern u8 __cdecl resend[];
extern u8 __cdecl imgend[];
extern u8 __cdecl exitcode;

void dvol_setdev(struct dvol *dv);
int cd_mount(struct dvol *dv);
struct winreq;
int win_call(u8 mplex, struct winreq *q);
int __cdecl redir(void);
int __cdecl api(void);
int __cdecl dos_init(void);

#endif
