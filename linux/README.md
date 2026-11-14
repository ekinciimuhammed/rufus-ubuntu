Rufus for Linux
===============

This directory contains the native Linux (Ubuntu/Debian) port of Rufus. It is
not a rewrite: the Linux build compiles the same sources as the Windows build
for everything that defines how Rufus behaves, and only replaces the parts that
talk to Windows itself.

Shared with Windows (compiled from `src/`):

* Image scanning and extraction (`iso.c`, libcdio), compressed images (`bled`)
* Partitioning logic: partition layouts, extra partitions (UEFI:NTFS, ESP, MSR,
  persistence, old BIOS fixes), MBR/GPT selection (`drive.c`)
* The whole format process (`format.c`'s `FormatThread`), DD image writing,
  boot records (`ms-sys`), Large FAT32 formatting (`format_fat32.c`)
* Syslinux, GRUB, FreeDOS and MS-DOS installation, UEFI:NTFS, runtime UEFI
  media validation, Secure Boot revocation checks (`hash.c`)
* Windows User Experience options / unattend.xml (`wue.c`)
* Bad blocks check and fake drive detection (`badblocks.c`)
* All the user interface decision logic: allowed file systems, cluster sizes,
  partition scheme and target system, labels, persistence, pre-start checks
  (`rufus.c`'s `ImageScanThread()`, `BootCheckThread()`...)
* Localization (`localization.c`, `res/loc/rufus.loc`, 38 languages)

Linux specific (`linux/`):

* `compat/`: a small Win32 compatibility layer (files, threads, events, errors),
  and an emulation of the Windows disk IOCTLs that reads and writes MBR/GPT
  partition tables, so that `drive.c` runs unmodified.
* `core/`: drives and volumes (`/sys`, mount, udev), formatting through the
  standard mkfs utilities, device enumeration and hotplug, process search
  (`/proc`), downloads (libcurl) and signature validation (OpenSSL).
* `ui/`: the GTK 3 user interface. Every control has the same ID as in the
  Windows dialog and reports the same WM_COMMAND notifications, so that the
  original message handling and localization apply.

Differences with the Windows version
------------------------------------

* Windows To Go is not available (it requires Windows' bcdboot).
* FFU images and ReFS (which require Windows' DISM/ReFS driver) are not available,
  exactly like on Windows versions that don't support them.
* The Windows 11 TPM/Secure Boot/RAM bypass is applied through unattend.xml
  (the method used by the Windows Store version of Rufus), as offline registry
  editing isn't available.
* Downloading Windows ISOs requires PowerShell (`sudo snap install powershell --classic`).
* Dynamic VHD/VHDX images and saving to VHDX require `qemu-utils`.
* The application is updated by the package manager.

Building
--------

    sudo apt install build-essential pkg-config libgtk-3-dev libcurl4-openssl-dev libssl-dev libwim-dev
    make -C linux
    make -C linux deb            # creates linux/build/rufus_<version>_<arch>.deb
    sudo apt install ./linux/build/rufus_*.deb

The data files are looked for in `RUFUS_DATADIR` (`/usr/share/rufus`), or in the
source tree's `res/` directory when running `linux/build/rufus` directly.

Testing
-------

`tests/run-tests.sh` (or `make -C linux check`) runs the real application against
loop devices backed by disk images, in an automation mode where Rufus selects
the options and answers its dialogs by itself, then checks the results with the
standard tools (`sfdisk`, `blkid`, `fsck`, `cmp`, `qemu-img`...). It needs root
and a display, and only ever writes to the loop devices it creates: in automation
mode, Rufus refuses any target that isn't the loop device it was given.

    linux/tests/run-tests.sh                 # all tests
    linux/tests/run-tests.sh freedos dd_xz   # selected tests (-l lists them)
    RUFUS_TEST_ISO=~/Downloads/some.iso RUFUS_TEST_TAG=some linux/tests/run-tests.sh
    RUFUS=/usr/lib/rufus/rufus linux/tests/run-tests.sh   # test the installed package

The tests cover:

* Non bootable drives: MBR/GPT, FAT32, Large FAT32 (> 32 GB), NTFS, exFAT, UDF, ext3
* Boot types: FreeDOS, Syslinux 4 and 6, GRUB 2, Grub4DOS (downloaded and
  signature checked), ReactOS, UEFI:NTFS
* Disk images: raw, `.xz`, `.gz`, fixed VHD, dynamic VHDX
* ISO images (with `RUFUS_TEST_ISO`): ISO mode on NTFS (with UEFI:NTFS) and FAT32
  (with WIM splitting for Windows), Windows User Experience options, ISO → ESP,
  persistence (Ubuntu/Debian), DD mode for ISOHybrid images
* Saving a drive to VHD and VHDX, checksums, zeroing, bad blocks check,
  cancellation, and a drive appearing while Rufus is running (hotplug)

The logs of each test are written to `linux/build/tests/`, with a summary in
`linux/build/tests/summary.txt`.
