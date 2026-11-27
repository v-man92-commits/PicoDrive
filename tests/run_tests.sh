#!/usr/bin/env bash
# Host-side tests for the image mounter (Linux / WSL: needs g++ and python3).
set -euo pipefail
T="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SRC="$(dirname "$T")"
W="$(mktemp -d)"
trap 'rm -rf "$W"' EXIT
cd "$W"
FLAGS="-Wno-sign-compare -std=gnu++2a -O1 -g -fsanitize=address,undefined -Wall -Wno-unused-result -Wno-unused-variable -Wno-format"

echo "== building test FAT images"
python3 "$T/mkfat.py" fat32 fat32 && python3 "$T/mkfat.py" fat16 fat16
python3 "$T/json2txt.py" fat32.json fat32.txt && python3 "$T/json2txt.py" fat16.json fat16.txt

echo "== fatmap_test"
g++ $FLAGS -I"$T/stub" -I"$SRC/common" -I"$SRC/arm9/source" -include "$T/shim.h" -c "$SRC/arm9/source/FatMap.cpp" -o fatmap.o
g++ $FLAGS -I"$T/stub" -I"$SRC/common" -I"$SRC/arm9/source" -c "$SRC/arm9/source/FatVolume.cpp" -o fatvolume.o
g++ $FLAGS -I"$T/stub" -I"$SRC/common" -I"$SRC/arm9/source" "$T/fatmap_test.cpp" fatmap.o fatvolume.o -o fatmap_test
./fatmap_test fat32.img fat32.txt fat16.img fat16.txt

echo "== cardsize_test (whole SD card sharing)"
g++ $FLAGS -I"$T/stub" -I"$SRC/common" -I"$SRC/arm9/source" "$SRC/arm9/source/FatVolume.cpp" "$T/cardsize_test.cpp" -o cardsize_test
./cardsize_test

echo "== imagedisk_test"
python3 "$T/mkfat.py" fat32 fat32 >/dev/null
mkdir -p a7 && cp "$SRC/arm7/source/ImageDisk.cpp" "$SRC/arm7/source/ImageDisk.h" "$SRC/arm7/source/OpticalDrive.cpp" "$SRC/arm7/source/OpticalDrive.h" a7/
g++ $FLAGS '-DIMAGEDISK_READ_KEYXY()=0xFFFF' -I"$T/stub7" -I"$SRC/common" -Ia7 a7/ImageDisk.cpp a7/OpticalDrive.cpp "$T/imagedisk_test.cpp" -o imagedisk_test
./imagedisk_test

echo "== msc_stream_test (TinyUSB's real MSC code + CD drive, as a USB host)"
TU="$(cd "$SRC/libs/tinyusb/src" && pwd)"
gcc -O1 -g -fsanitize=address,undefined -I"$T/mschost" -I"$TU" -c "$TU/class/msc/msc_device.c" -o msc_device.o
g++ $FLAGS '-DIMAGEDISK_READ_KEYXY()=0xFFFF' -I"$T/stub7" -I"$SRC/common" -Ia7 a7/ImageDisk.cpp a7/OpticalDrive.cpp "$T/msc_stream_test.cpp" msc_device.o -o msc_stream_test
./msc_stream_test

echo "== exfat_test (exFAT build: FatFs on exFAT and FAT32 volumes + independent checkers)"
FF="$SRC/arm9/source/fatfs"
gcc -O1 -g -fsanitize=address,undefined -DFFCONF_USE_MKFS=1 -I"$FF" -c "$FF/ff.c" -o ff.o
gcc -O1 -g -DFFCONF_USE_MKFS=1 -I"$FF" -c "$FF/ffunicode.c" -o ffunicode.o
EXF="$FLAGS -DIMAGE_MOUNTER_EXFAT -DFFCONF_USE_MKFS=1 -I$T/stubff -I$T/stub -I$SRC/common -I$SRC/arm9/source -I$FF"
g++ $EXF -DIMAGE_MOUNTER_FATFS_IO -include "$T/shim.h" -c "$SRC/arm9/source/FatMap.cpp" -o fatmap_ex.o
g++ $EXF "$FF/FatMapFatFs.cpp" "$FF/QuickAllocFatFs.cpp" "$FF/sdDiscFatFs.cpp" "$T/exfat_test.cpp" fatmap_ex.o ff.o ffunicode.o -o exfat_test
./exfat_test exfat exfat.img 8192 32768 exfat.txt
python3 "$T/exfat_check.py" exfat exfat.img exfat.txt
./exfat_test fat32 fat32q.img 256 2048 fat32q.txt
python3 "$T/exfat_check.py" fat32 fat32q.img fat32q.txt

echo "== fs_syscalls_test (BlocksDS build: POSIX file syscalls over the app's FatFs)"
gcc -O1 -g -fsanitize=address,undefined -DFFCONF_USE_MKFS=1 -I"$FF" -c "$T/fs_host_io.c" -o fs_host_io.o
g++ -O1 -g -fsanitize=address,undefined -std=gnu++20 -DFFCONF_USE_MKFS=1 -I"$T" -include "$T/fs_rename.h" -I"$FF" -c "$FF/fsSyscalls.cpp" -o fsx_sys.o
g++ -O1 -g -fsanitize=address,undefined -std=gnu++20 -DFS_RENAME_WITH_DIRENT -I"$T" -include "$T/fs_rename.h" -include "$T/fs_hostdirent.h" -I"$FF" -c "$FF/fsDirent.cpp" -o fsx_dir.o
g++ -O1 -g -fsanitize=address,undefined -std=gnu++20 -I"$T" -I"$FF" "$T/fs_syscalls_test.cpp" fsx_sys.o fsx_dir.o ff.o ffunicode.o fs_host_io.o -o fs_syscalls_test
./fs_syscalls_test exfat fsx_exfat.img 8192
./fs_syscalls_test fat32 fsx_fat32.img 512

echo "== WiFi build: libnds FIFO over libtwl IPC (wire format + shim loopback)"
gcc -Wall -fsanitize=address,undefined -g -I"$SRC/common/shim" "$T/fifoproto_test.c" "$SRC/common/shim/ndsFifoProto.c" -o fifoproto_test
./fifoproto_test
gcc -Wall -fsanitize=address,undefined -g -Wno-int-to-pointer-cast -Wno-pointer-to-int-cast -I"$T/stubshim" -I"$SRC/common/shim" -I"$SRC/common" -DLIBTWL_ARM9 "$T/fifoshim_test.c" "$SRC/common/shim/ndsFifoShim.c" "$SRC/common/shim/ndsFifoProto.c" -o fifoshim_test
./fifoshim_test

echo "== WiFi build: web UI (HTTP helpers, connection manager, server + API end to end)"
WEB="$SRC/arm9/source/web"
gcc -Wall -fsanitize=address,undefined -g -I"$WEB" "$T/web/httputil_test.c" "$WEB/httputil.c" -o httputil_test
./httputil_test
g++ -std=gnu++20 -Wall -fsanitize=address,undefined -g -I"$T/web/stubwifi" -I"$WEB" "$T/web/wifinet_test.cpp" "$WEB/wifiNet.cpp" -x c "$WEB/wfcslots.c" "$WEB/wificonfig.c" -x none -o wifinet_test
./wifinet_test
g++ -std=gnu++20 -Wall -fsanitize=address,undefined -g -DWEBAPP_PORT=18090 '-DWEBAPP_HW_ENTROPY()=((u32)getpid())' -I"$T/web/stub" -I"$WEB" "$T/web/web_harness.cpp" "$WEB/webApp.cpp" "$WEB/webServer.cpp" "$WEB/webPage.cpp" -x c "$WEB/httputil.c" -x none -o web_harness
python3 "$T/web/web_test.py"

echo "== exit_test (return to DSpico menu: Pico Loader header)"
g++ $FLAGS -DEXIT_TO_MENU_HOST_TEST -I"$T/stub" -I"$SRC/arm9/source" "$SRC/arm9/source/exitToMenu.cpp" "$T/exit_test.cpp" -o exit_test
./exit_test

echo "== exit7_test (ARM7 bootstub handover vs libnds resetARM7, two threads)"
mkdir -p e7 && cp "$SRC/arm7/source/exitToMenu7.cpp" "$SRC/arm7/source/exitToMenu7.h" e7/
g++ -std=gnu++2a -O1 -g -pthread -fsanitize=thread -DEXIT7_HOST_TEST -I"$T/stubexit" -I"$T/stub7" -Ie7 -include "$T/stubexit/exit7_sim.h" e7/exitToMenu7.cpp "$T/exit7_test.cpp" -o exit7_test
./exit7_test

echo "== cue_test (BIN/CUE: cue sheets and disc layout)"
g++ $FLAGS -I"$T/stub" -I"$SRC/common" -I"$SRC/arm9/source" "$SRC/arm9/source/CueSheet.cpp" "$T/cue_test.cpp" -o cue_test
./cue_test "$T/data"

echo "== cueimage_test (BIN/CUE files on the card)"
g++ $FLAGS -I"$T/stub" -I"$SRC/common" -I"$SRC/arm9/source" "$SRC/arm9/source/cueImage.cpp" "$SRC/arm9/source/CueSheet.cpp" "$T/cueimage_test.cpp" -o cueimage_test
./cueimage_test "$T/data" cuedir

echo "== cdda_test (BIN/CUE over USB: audio, mixed-mode, CD-Text, TinyUSB's real MSC code)"
g++ $FLAGS '-DIMAGEDISK_READ_KEYXY()=0xFFFF' -I"$T/stub7" -I"$SRC/common" -Ia7 -I"$SRC/arm9/source" a7/ImageDisk.cpp a7/OpticalDrive.cpp "$SRC/arm9/source/CueSheet.cpp" "$T/cdda_test.cpp" msc_device.o -o cdda_test
./cdda_test "$T/data"

echo "== info_test"
python3 - <<'PY'
import struct, random, os
os.makedirs('info', exist_ok=True); d='info/'; MiB=1024*1024
b=bytearray(8*MiB); b[0:4]=b'FATX'; struct.pack_into('<III',b,4,0x1234,32,1); open(d+'mu.img','wb').write(b)
b=bytearray(MiB); b[0:4]=b'XTAF'; struct.pack_into('>III',b,4,1,32,1); open(d+'x360.img','wb').write(b)
open(d+'blank.img','wb').write(bytearray(2*MiB))
b=bytearray(MiB); b[0:3]=b'\xEB\x58\x90'; struct.pack_into('<HB',b,11,512,8); b[0x47:0x52]=b'MYDRIVE    '; b[0x52:0x5a]=b'FAT32   '; b[510:512]=b'\x55\xaa'; open(d+'sf32.img','wb').write(b)
b=bytearray(4*MiB); b[0x1BE+4]=0x06; struct.pack_into('<II',b,0x1BE+8,63,8000); b[510:512]=b'\x55\xaa'
o=63*512; b[o:o+3]=b'\xEB\x3C\x90'; struct.pack_into('<HBHBHHBH',b,o+11,512,4,4,2,512,8000,0xF8,32); b[o+0x2B:o+0x36]=b'GAMESAVE   '; b[o+0x36:o+0x3E]=b'FAT16   '; b[o+510:o+512]=b'\x55\xaa'; open(d+'mbr16.img','wb').write(b)
b=bytearray(MiB); b[3:11]=b'EXFAT   '; b[510:512]=b'\x55\xaa'; open(d+'exfat.img','wb').write(b)
b=bytearray(MiB); b[0x1BE+4]=0xEE; b[510:512]=b'\x55\xaa'; open(d+'gpt.img','wb').write(b)
b=bytearray(MiB); b[0x8000]=1; b[0x8001:0x8006]=b'CD001'; b[0x8000+40:0x8000+72]=b'MY_DISC'.ljust(32); open(d+'disc.iso','wb').write(b)
b=bytearray(MiB); b[0x10000:0x10014]=b'MICROSOFT*XBOX*MEDIA'; open(d+'game.iso','wb').write(b)
b=bytearray(MiB); b[0x80000:0x80004]=b'FATX'; open(d+'xhdd.img','wb').write(b)
b=bytearray(MiB); b[0x1BE+4]=0x17; struct.pack_into('<II',b,0x1BE+8,0,2048); b[510:512]=b'\x55\xaa'; b[0x8000]=1; b[0x8001:0x8006]=b'CD001'; b[0x8000+40:0x8000+72]=b'UBUNTU'.ljust(32); open(d+'hybrid.iso','wb').write(b)
b=bytearray(MiB); b[0x1BE+4]=0xEE; struct.pack_into('<II',b,0x1BE+8,1,2047); b[510:512]=b'\x55\xaa'; b[0x8000]=1; b[0x8001:0x8006]=b'CD001'; open(d+'hybridgpt.iso','wb').write(b)
b=bytearray(MiB); b[0x8001:0x8006]=b'BEA01'; open(d+'udf.iso','wb').write(b)
r=random.Random(3); open(d+'raw.bin','wb').write(bytes(r.getrandbits(8) for _ in range(65536)))
open(d+'tiny.img','wb').write(b'abc')
PY
g++ $FLAGS -I"$T/stub" -I"$SRC/arm9/source" "$SRC/arm9/source/ImageInfo.cpp" "$T/info_test.cpp" -o info_test
./info_test info/mu.img "FATX (Xbox memory unit)" info/x360.img XTAF info/blank.img Blank info/sf32.img FAT32 \
    info/mbr16.img "MBR: FAT16" info/exfat.img exFAT info/gpt.img GPT info/disc.iso ISO9660 info/game.iso XISO \
    info/xhdd.img "Xbox HDD" info/hybrid.iso "ISO9660 (hybrid)" info/hybridgpt.iso "ISO9660 (hybrid)" info/udf.iso "UDF disc" info/raw.bin Unknown info/tiny.img "Too small"
echo "== quickalloc_test (quick create + power-loss simulation)"
g++ $FLAGS -DQUICKALLOC_NO_LIBFAT -I"$T/stub" -I"$SRC/common" -I"$SRC/arm9/source" "$SRC/arm9/source/QuickAlloc.cpp" "$SRC/arm9/source/FatVolume.cpp" "$T/quickalloc_main.cpp" -o quickalloc_main
python3 "$T/quickalloc_test.py" "$W/quickalloc_main"

echo; echo "All tests passed."
