// Host test for the BlocksDS file syscalls (arm9/source/fatfs/fsSyscalls.cpp +
// fsDirent.cpp): real FatFs on a disk image, driven through the same POSIX
// entry points picolibc's stdio uses on the DS. The syscalls are compiled
// with fs_rename.h so they become fsx_open, fsx_read... and don't replace the
// host's own libc.
// Usage: fs_syscalls_test exfat|fat32 disk.img sizeMiB
#define FS_RENAME_WITH_DIRENT
#include "fs_rename.h"
#include "fs_hostdirent.h"
#include "fsSyscalls.h"

static int gPass, gFail;
#define CHECK(c, ...) do { if (c) gPass++; else { gFail++; printf("  FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

extern "C" {
int host_setup(const char* path, unsigned long long mib, int exfat);
int host_type_exfat(void);
int host_type_fat32(void);
void host_teardown(void);
int host_make_sized(const char* path, unsigned long long bytes);

// prototypes of the renamed syscalls (normally from the system headers)
int fsx_open(const char*, int, ...);
int fsx_close(int);
ssize_t fsx_read(int, void*, size_t);
ssize_t fsx_write(int, const void*, size_t);
off_t fsx_lseek(int, off_t, int);
int fsx_fstat(int, struct stat*);
int fsx_stat(const char*, struct stat*);
int fsx_access(const char*, int);
int fsx_unlink(const char*);
int fsx_rmdir(const char*);
int fsx_rename(const char*, const char*);
int fsx_mkdir(const char*, mode_t);
int fsx_statvfs(const char*, struct statvfs*);
int fsx_ftruncate(int, off_t);
int fsx_truncate(const char*, off_t);
int fsx_fsync(int);
int fsx_isatty(int);
int fsx_fcntl(int, int, ...);
DIR* fsx_opendir(const char*);
struct dirent* fsx_readdir(DIR*);
int fsx_closedir(DIR*);
void fsx_rewinddir(DIR*);
long fsx_telldir(DIR*);
void fsx_seekdir(DIR*, long);
// console hooks normally in libnds
static std::string gStdout, gStderr;
ssize_t write_stdout_libnds(int, const void* p, size_t n) { gStdout.append((const char*)p, n); return n; }
ssize_t write_stderr_libnds(int, const void* p, size_t n) { gStderr.append((const char*)p, n); return n; }

// socket hooks (DSWiFi fills these in on the DS)
extern ssize_t (*fsx_socket_fn_write)(int, const void*, size_t);
extern ssize_t (*fsx_socket_fn_read)(int, void*, size_t);
extern int (*fsx_socket_fn_close)(int);
extern int (*fsx_socket_fn_fcntl)(int, int, int);
}

static int gSockFd = -1;
static std::string gSockWritten;
static ssize_t sockWrite(int fd, const void* p, size_t n) { gSockFd = fd; gSockWritten.append((const char*)p, n); return n; }
static ssize_t sockRead(int fd, void* p, size_t n) { gSockFd = fd; memset(p, 'S', n); return n; }
static int sockClose(int fd) { gSockFd = fd; return 0; }
static int sockFcntl(int fd, int cmd, int arg) { gSockFd = fd; return cmd * 1000 + arg; }

static std::string readAll(const char* path)
{
    int fd = fsx_open(path, O_RDONLY);
    if (fd < 0) return "<open failed>";
    std::string s;
    char buf[777];
    ssize_t n;
    while ((n = fsx_read(fd, buf, sizeof(buf))) > 0) s.append(buf, n);
    fsx_close(fd);
    return s;
}

int main(int argc, char** argv)
{
    if (argc < 4) { printf("usage\n"); return 2; }
    bool exfat = !strcmp(argv[1], "exfat");
    unsigned long long mib = strtoull(argv[3], nullptr, 10);
    int type = host_setup(argv[2], mib, exfat);
    CHECK(type == (exfat ? host_type_exfat() : host_type_fat32()), "format/mount type %d", type);

    // --- mkdir / stat / root -------------------------------------------------
    CHECK(fsx_mkdir("sd:/images", 0777) == 0, "mkdir");
    CHECK(fsx_mkdir("sd:/images", 0777) == -1 && errno == EEXIST, "mkdir twice errno %d", errno);
    struct stat st;
    CHECK(fsx_stat("sd:/", &st) == 0 && S_ISDIR(st.st_mode), "stat root");
    CHECK(fsx_stat("sd:", &st) == 0 && S_ISDIR(st.st_mode), "stat bare device");
    CHECK(fsx_stat("sd:/images", &st) == 0 && S_ISDIR(st.st_mode), "stat dir");
    CHECK(fsx_stat("sd:/nope", &st) == -1 && errno == ENOENT, "stat missing errno %d", errno);

    // --- create / write / read / sizes ---------------------------------------
    int fd = fsx_open("sd:/images/a.img", O_WRONLY | O_CREAT | O_TRUNC, 0666);
    CHECK((fd & FS_FD_KIND_MASK) == FS_FD_FILE, "file fd kind 0x%x", fd);
    std::vector<char> data(300000);
    for (size_t i = 0; i < data.size(); i++) data[i] = (char)(i * 7 + i / 513);
    CHECK(fsx_write(fd, data.data(), data.size()) == (ssize_t)data.size(), "write");
    CHECK(fsx_fstat(fd, &st) == 0 && st.st_size == (off_t)data.size() && S_ISREG(st.st_mode), "fstat size");
    CHECK(fsx_close(fd) == 0, "close");
    CHECK(fsx_close(fd) == -1 && errno == EBADF, "double close");
    CHECK(readAll("sd:/images/a.img") == std::string(data.begin(), data.end()), "read back");
    CHECK(readAll("fat:/images/a.img") == std::string(data.begin(), data.end()), "fat: alias");
    CHECK(fsx_stat("sd:/images/a.img", &st) == 0 && st.st_size == (off_t)data.size(), "stat size");

    // --- lseek variants, read at offsets, seek past end extends ---------------
    fd = fsx_open("sd:/images/a.img", O_RDWR);
    CHECK(fsx_lseek(fd, 1000, SEEK_SET) == 1000, "seek set");
    char b4[4];
    CHECK(fsx_read(fd, b4, 4) == 4 && !memcmp(b4, &data[1000], 4), "read at 1000");
    CHECK(fsx_lseek(fd, -4, SEEK_CUR) == 1000, "seek cur");
    CHECK(fsx_lseek(fd, -10, SEEK_END) == (off_t)data.size() - 10, "seek end");
    CHECK(fsx_lseek(fd, -1, SEEK_SET) == -1 && errno == EINVAL, "negative seek");
    CHECK(fsx_lseek(fd, 0, 99) == -1 && errno == EINVAL, "bad whence");
    CHECK(fsx_lseek(fd, 400000, SEEK_SET) == 400000, "seek past end");
    CHECK(fsx_write(fd, "END", 3) == 3, "write past end");
    CHECK(fsx_fstat(fd, &st) == 0 && st.st_size == 400003, "extended size %lld", (long long)st.st_size);
    // ftruncate shrink keeps position sane
    CHECK(fsx_ftruncate(fd, 5000) == 0, "ftruncate");
    CHECK(fsx_fstat(fd, &st) == 0 && st.st_size == 5000, "truncated size");
    CHECK(fsx_lseek(fd, 0, SEEK_CUR) == 5000, "pos clamped %lld", (long long)fsx_lseek(fd, 0, SEEK_CUR));
    CHECK(fsx_ftruncate(fd, -1) == -1 && errno == EINVAL, "negative truncate");
    CHECK(fsx_fsync(fd) == 0, "fsync");
    fsx_close(fd);
    CHECK(fsx_truncate("sd:/images/a.img", 100) == 0 && fsx_stat("sd:/images/a.img", &st) == 0 && st.st_size == 100, "truncate path");

    // --- open flags ---------------------------------------------------------
    CHECK(fsx_open("sd:/images/a.img", O_WRONLY | O_CREAT | O_EXCL, 0666) == -1 && errno == EEXIST, "O_EXCL errno %d", errno);
    CHECK(fsx_open("sd:/images/missing", O_RDONLY) == -1 && errno == ENOENT, "missing errno %d", errno);
    CHECK(fsx_open("sd:/images/a.img", 3 /* bad accmode */) == -1 && errno == EINVAL, "bad accmode");
    fd = fsx_open("sd:/images/a.img", O_WRONLY | O_TRUNC);
    CHECK(fd >= 0 && fsx_fstat(fd, &st) == 0 && st.st_size == 0, "O_TRUNC without O_CREAT");
    fsx_write(fd, "abc", 3);
    fsx_close(fd);
    fd = fsx_open("sd:/images/a.img", O_WRONLY | O_APPEND);
    fsx_lseek(fd, 0, SEEK_SET);
    fsx_write(fd, "def", 3);
    fsx_close(fd);
    CHECK(readAll("sd:/images/a.img") == "abcdef", "append got '%s'", readAll("sd:/images/a.img").c_str());
    // read-only fd refuses writes
    fd = fsx_open("sd:/images/a.img", O_RDONLY);
    CHECK(fsx_write(fd, "x", 1) == -1, "write on O_RDONLY");
    fsx_close(fd);

    // --- descriptor limit and reuse ------------------------------------------
    std::vector<int> fds;
    for (int i = 0; i < FS_MAX_FILES + 2; i++) {
        char p[64];
        snprintf(p, sizeof p, "sd:/f%d.bin", i);
        int f = fsx_open(p, O_WRONLY | O_CREAT, 0666);
        if (f >= 0) fds.push_back(f);
        else CHECK(errno == EMFILE && i >= FS_MAX_FILES, "EMFILE at %d errno %d", i, errno);
    }
    CHECK(fds.size() == FS_MAX_FILES, "opened %zu", fds.size());
    std::set<int> uniq(fds.begin(), fds.end());
    CHECK(uniq.size() == fds.size(), "unique fds");
    for (int f : fds) fsx_close(f);
    fd = fsx_open("sd:/f0.bin", O_RDONLY);
    CHECK(fd >= 0, "slot reused after close");
    fsx_close(fd);

    // --- rename / unlink / rmdir ---------------------------------------------
    CHECK(fsx_rename("sd:/f1.bin", "sd:/images/renamed.bin") == 0, "rename");
    CHECK(fsx_access("sd:/images/renamed.bin", R_OK) == 0, "access");
    CHECK(fsx_access("sd:/f1.bin", F_OK) == -1, "old name gone");
    CHECK(fsx_unlink("sd:/images/renamed.bin") == 0, "unlink");
    CHECK(fsx_unlink("sd:/images/renamed.bin") == -1 && errno == ENOENT, "unlink twice");
    CHECK(fsx_mkdir("sd:/tmpdir", 0) == 0 && fsx_rmdir("sd:/tmpdir") == 0, "rmdir");

    // --- directories ---------------------------------------------------------
    for (int i = 0; i < 40; i++) {
        char p[96];
        snprintf(p, sizeof p, "sd:/images/Long file name number %02d with spaces.img", i);
        int f = fsx_open(p, O_WRONLY | O_CREAT, 0666);
        fsx_close(f);
    }
    fsx_mkdir("sd:/images/sub", 0);
    DIR* d = fsx_opendir("sd:/images");
    CHECK(d != nullptr, "opendir");
    int files = 0, dirs = 0;
    bool sawLong = false;
    std::vector<std::string> first3;
    while (struct dirent* e = fsx_readdir(d)) {
        if (e->d_type == DT_DIR) dirs++; else files++;
        if (!strcmp(e->d_name, "Long file name number 39 with spaces.img")) sawLong = true;
        if (first3.size() < 3) first3.push_back(e->d_name);
    }
    CHECK(files == 41 && dirs == 1, "files %d dirs %d", files, dirs);
    CHECK(sawLong, "long name");
    long here = fsx_telldir(d);
    CHECK(here == 42, "telldir %ld", here);
    fsx_rewinddir(d);
    struct dirent* e = fsx_readdir(d);
    CHECK(e && first3[0] == e->d_name, "rewind");
    fsx_seekdir(d, 2);
    e = fsx_readdir(d);
    CHECK(e && first3.size() == 3 && first3[2] == e->d_name, "seekdir");
    CHECK(fsx_closedir(d) == 0, "closedir");
    CHECK(fsx_opendir("sd:/nope") == nullptr && errno == ENOENT, "opendir missing");
    // several directories open at once, then the limit
    std::vector<DIR*> ds;
    for (int i = 0; i < FS_MAX_DIRS + 1; i++) { DIR* x = fsx_opendir("sd:/"); if (x) ds.push_back(x); }
    CHECK(ds.size() == FS_MAX_DIRS && errno == EMFILE, "dir limit %zu", ds.size());
    for (DIR* x : ds) fsx_closedir(x);

    // --- files over 2 GiB: sizes come from fs_fileSize64 (st_size is 32-bit on the DS) ---
    if (exfat)
    {
        unsigned long long sz = 0; int isDir = -1;
        CHECK(host_make_sized("/images/big.img", 2147483648ull) == 0, "make 2 GiB file");
        CHECK(fs_fileSize64("sd:/images/big.img", &sz, &isDir) == 0 && sz == 2147483648ull && isDir == 0, "2 GiB size %llu", sz);
        CHECK(host_make_sized("/images/huge.img", 5ull << 30) == 0, "make 5 GiB file");
        CHECK(fs_fileSize64("sd:/images/huge.img", &sz, &isDir) == 0 && sz == (5ull << 30), "5 GiB size %llu", sz);
        CHECK(fs_fileSize64("sd:/images", &sz, &isDir) == 0 && isDir == 1, "directory");
        CHECK(fs_fileSize64("sd:/", &sz, &isDir) == 0 && isDir == 1, "root");
        CHECK(fs_fileSize64("sd:/images/none", &sz, &isDir) == -1 && errno == ENOENT, "missing");
        fsx_unlink("sd:/images/big.img");
        fsx_unlink("sd:/images/huge.img");
    }

    // --- statvfs ---------------------------------------------------------------
    struct statvfs vfs;
    CHECK(fsx_statvfs("sd:/", &vfs) == 0 && vfs.f_bfree > 0 && vfs.f_bfree <= vfs.f_blocks, "statvfs");
    CHECK(vfs.f_bsize == (unsigned long)(exfat ? 32768 : 4096), "cluster size %lu", vfs.f_bsize);

    // --- stdio fds -------------------------------------------------------------
    CHECK(fsx_write(1, "hello", 5) == 5 && gStdout == "hello", "stdout");
    CHECK(fsx_write(2, "err", 3) == 3 && gStderr == "err", "stderr");
    CHECK(fsx_isatty(1) == 1 && fsx_isatty(fd) == 0, "isatty");
    CHECK(fsx_lseek(1, 0, SEEK_SET) == -1 && errno == ESPIPE, "seek on stdout");
    CHECK(fsx_close(1) == 0, "close stdout is harmless");

    // --- sockets: small fds go to DSWiFi's hooks ---------------------------------
    CHECK(fsx_write(5, "x", 1) == -1 && errno == EBADF, "socket write without wifi");
    fsx_socket_fn_write = sockWrite;
    fsx_socket_fn_read = sockRead;
    fsx_socket_fn_close = sockClose;
    fsx_socket_fn_fcntl = sockFcntl;
    CHECK(fsx_write(5, "GET", 3) == 3 && gSockFd == 5 && gSockWritten == "GET", "socket write");
    char sb[3];
    CHECK(fsx_read(6, sb, 3) == 3 && gSockFd == 6 && sb[0] == 'S', "socket read");
    CHECK(fsx_fcntl(7, F_SETFL, O_NONBLOCK) == F_SETFL * 1000 + O_NONBLOCK && gSockFd == 7, "socket fcntl");
    CHECK(fsx_close(8) == 0 && gSockFd == 8, "socket close");
    CHECK(fsx_fstat(9, &st) == 0 && S_ISSOCK(st.st_mode), "socket fstat");
    // files are never mistaken for sockets
    fd = fsx_open("sd:/f0.bin", O_RDWR);
    gSockFd = -1;
    CHECK(fsx_write(fd, "file", 4) == 4 && gSockFd == -1, "file write not routed to socket");
    fsx_close(fd);
    CHECK(fsx_read(FS_FD_FILE | 7, sb, 1) == -1 && errno == EBADF, "closed file slot");

    host_teardown();
    printf("fs_syscalls (%s): %d checks passed, %d failed\n", argv[1], gPass, gFail);
    return gFail ? 1 : 0;
}
