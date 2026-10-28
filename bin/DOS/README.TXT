# WINUDF 1.1 - UDF file system for DOS, Windows 1.0x-3.11 and Windows 95 / 98 / ME

WINUDF reads and writes **UDF** (versions 1.02 to 2.60), the file system of DVDs, Blu-ray
discs, packet-written CDs and UDF-formatted USB sticks and disks: `WINUDF.EXE` gives DOS and
Windows 1.0x-3.11 drive letters for them, `WINUDF.VXD` is a file system driver for Windows
95 / 98 / ME with long names.

WINUDF is one of the four WINDRV packages, each in its own repository and usable alone or
together with the others:

| Package | What it drives |
|---------|----------------|
| **WINDISK**  | IDE, SATA AHCI and NVMe disks, IDE/AHCI CD/DVD drives, SD card readers and eMMC |
| **WINUSB**   | USB host controllers and devices, PS/2 mouse and keyboard, touchpads, USB4 / Thunderbolt |
| **WINAUDIO** | HD Audio, AC'97 and the Intel audio DSP (SST / SOF), General MIDI synthesizer, Sound Blaster 16 emulation for DOS programs |
| **WINUDF**   | the UDF file system (DVD, Blu-ray, packet-written CDs, UDF disks), read and write |

---

## Supported systems

| System | WINUDF |
|--------|------|
| MS-DOS 3.10 - 8.0, FreeDOS | UDF volumes (`WINUDF.EXE`) |
| Windows 1.0x, 2.x (real mode) | UDF volumes (`WINUDF.EXE`) |
| Windows/386 2.x | not tried |
| Windows 3.0, 3.1, WfW 3.11 | UDF volumes as network drives (`WINUDF.EXE`) |
| Windows 95 / 98 / ME | UDF with long names (`WINUDF.VXD`, IFS Manager file system driver) (section 3) |

## Installation

From DOS (or an MS-DOS box of Windows 95 / 98 / ME), in `bin\DOS` of this package:

```
INSTUDF                      default directory, AUTOEXEC.BAT / SYSTEM.INI updated
INSTUDF C:\DRV /O /L:R       another directory; options for WINUDF.EXE after /O (letters from R:)
```

DOS / Windows 3.x: `WINUDF.EXE` goes to AUTOEXEC.BAT after MSCDEX, `LASTDRIVE=Z` to
CONFIG.SYS when needed. Windows 95 / 98 / ME: `WINUDF.VXD` goes to `WINDOWS\SYSTEM` and
`device=WINUDF.VXD` to `[386Enh]` of SYSTEM.INI. The originals are saved as `*.WNF`.
Uninstall: `UNINUDF`.

### By hand

* DOS and Windows 1.0x-3.11: `LASTDRIVE=Z` in CONFIG.SYS, and in AUTOEXEC.BAT after the
  MSCDEX line: `C:\WINDRV\WINUDF.EXE`
* Windows 95 / 98 / ME: copy `bin\WIN9X\WINUDF.VXD` to `C:\WINDOWS\SYSTEM` and add
  `device=WINUDF.VXD` to the `[386Enh]` section of SYSTEM.INI.

## Repository layout

```
bin/DOS     DOS: the driver, its 32-bit core, tools, installer and uninstaller (also all
            Windows 1.0x / 2.x need)
bin/WIN3X   Windows 3.0, 3.1 and Windows for Workgroups 3.11
bin/WIN9X   Windows 95, 98 and ME (Plug and Play drivers and their INF files: the folder
            to give to "Have Disk" / "Update Driver")
src         the source code (see "Building")
```

Files used by several systems are in each of their folders. The installer is run from
`bin\DOS`; it takes the files of the other systems from the folders next to it, so the
`bin` tree has to stay together (copy all of it to a floppy or a directory).

| Folder | Files |
|--------|-------|
| `bin/DOS` | `INSTUDF.EXE`, `MANUAL.TXT`, `README.TXT`, `UNINUDF.EXE`, `WINUDF.EXE` |
| `bin/WIN3X` | `WINGRP.EXE`, `WINUDF.TXT` |
| `bin/WIN9X` | `WINGRP.EXE`, `WINUDF.TXT`, `WINUDF.VXD` |

## More

**MANUAL.md** (`MANUAL.TXT` in `bin\DOS`, `WINUDF.TXT` for Windows) describes everything
else: all options, how the driver works, hardware and chipset notes, the programming
interface, building from source, tests and known limitations.

## Version history

**1.1 (26 October 2026)**

Changelog:

- Added long file names in DOS: WINUDF.EXE answers the long file name API of MS-DOS 7 (INT 21h AX=71xxh: find first / next, open / create, attributes, rename, delete, directories, short / long name, file times), so DIR, COPY and other programs of MS-DOS 7.x show and use the long names of UDF disks; free space (7303h) and 64-bit file positions (7142h) for files of 4 GB and more.
- WINUDF.EXE is a regular DOS program that can be loaded high (`LH`), about 70 KB resident; its interrupt handlers follow the IBM interrupt sharing protocol, so it unloads (`/U`) after other TSRs that hooked the same interrupts.
- Works next to SHSUCDX (UDF DVDs of SHSUCDX's drives) and MSCDEX.
- Fixed unloading and the "already loaded" check.
- Fixed Windows 98 freezing on UDF disks in partitions Windows 9x calls unformatted (they are read through the BIOS path now).
- Fixed the "disk structure is corrupted" error when copying folders to UDF disks in Windows 98.
- Fixed WritePrivateProfileString (INI files) and Explorer writing desktop.ini hanging on UDF drives in Windows 95/98/ME: WINUDF.VXD answers the drive IOCTLs (removable, remote, drive map) itself.
- Added writing to Windows 10 UDF 2.60 disks (metadata bitmap embedded in its file entry).
- The volume is written and closed when the disk is removed safely.
- Added MS-DOS 7.0, 7.1, 8.0 and FreeDOS support; the installer puts WINUDF.EXE into AUTOEXEC.BAT there too (only Windows 95/98/ME use DOSSTART.BAT).
- The documentation is split into README.md (overview, installation) and MANUAL.md (all details).

**1.0 (12 October 2026)** - first release: UDF 1.02-2.60 read / write, `WINUDF.EXE` for DOS
and Windows 1.0x-3.11, `WINUDF.VXD` for Windows 95 / 98 / ME.

**1.0 update (14 October 2026):** the package is split into `bin/DOS`, `bin/WIN3X` and
`bin/WIN9X`; English documentation.
