# DSPicoDrive v0.1

> [!WARNING]
> **Beta software: use at your own risk.** DSPicoDrive is early, experimental
> homebrew. It writes to disk images and SD cards over USB, and bugs could
> corrupt data or images. Keep backups of anything important, and don't use
> it on the only copy of your data.

A DSpico app for the Nintendo DSi (and DS) that turns the console into a USB
drive. It mounts disk-image files from the DSpico's SD card, and you can
control it from a phone over WiFi, even with the lid closed.

- Mounts `.img` disk images read/write or read-only, including Xbox (FATX)
  images. Can also create new images.
- Mounts `.iso` and `.bin`/`.cue` images as a virtual DVD/CD-ROM drive.
- Can share the whole SD card.
- Works with FAT12/16/32 and exFAT SD cards, and images over 4 GB.
- Phone/web control: in DSi mode it joins WPA2 WiFi using the console's saved
  connections or `sd:/wifi.txt`, then serves a small PIN-protected page for
  mounting and unmounting.
- Can return to the loader menu.
- Only offers image sizes the SD card's filesystem and free space can hold.

Images go in `sd:/images/`. Output: `dspicodrive.nds` (DSi-enhanced; WPA2 needs
DSi mode).

## Building

Needs [BlocksDS](https://blocksds.skylyrac.net/) (tested with 1.25.0 and the
Wonderful toolchain).

    git clone --recursive <this repo>
    cd DSPicoDrive
    make

`make` applies `patches/tinyusb-msc-cdrom.patch` to the TinyUSB submodule
once (MSC changes for CD/DVD-ROM emulation). It also builds libtwl for BlocksDS
with `patches/libtwl-Makefile.blocksds` into `libs/libtwl-blocks`.

## How the pieces fit

- **ARM7:** libtwl (threads, IPC), plus DSWiFi's ARM7 code.
  - `arm7/source/shim/ndsIrqShim7.c` and `ndsIrqNest7.s` give DSWiFi the
    libnds interrupt API on top of libtwl. Its handlers run nested, as libnds
    would.
  - `arm7/source/wifi7.cpp` runs DSWiFi in its own thread.
- **ARM9:** libnds owns interrupts, and DSWiFi's network stack runs in a libnds
  cothread. `arm9/source/shim/twlRtosShim9.c` gives the app libtwl's rtos API
  on top of libnds.
- **Both CPUs:** `common/shim/ndsFifoShim.c` carries libnds FIFO messages over
  one libtwl IPC channel. Messages sent from interrupts are queued, never
  waited on.
- **Files:** `arm9/source/fatfs/` routes POSIX file calls to FatFs.
- **Web UI:** `arm9/source/web/` (connection manager, HTTP server, page, API).
  Its actions go through `webBridge_*` in `main.cpp`, the same code the
  buttons use.

## Tests

    bash tests/run_tests.sh

Host-side tests (Linux, gcc, address/UB sanitizers) cover the file layer,
FATX/ISO/CUE parsing, the FIFO protocol, the HTTP helpers and the web server.

## Contributors

- [v-man92-commits](https://github.com/v-man92-commits): author and hardware testing
- Claude (Anthropic): co-developer

## Credits

Based on [dspico-usb-examples](https://github.com/LNH-team/dspico-usb-examples)
by the LNH team (USB platform code in `platform/`). Built with:
- [libtwl](https://github.com/Gericom/libtwl) (Gericom);
- [TinyUSB](https://github.com/hathach/tinyusb);
- BlocksDS (libnds, DSWiFi);
- FatFs (ChaN).

Licence: zlib, see `LICENSE.txt` (third-party parts keep their own licences).
