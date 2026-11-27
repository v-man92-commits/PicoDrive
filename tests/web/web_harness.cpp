// Runs the real web UI code (webApp + webServer + page) on the host with a
// fake image mounter behind webBridge, on 127.0.0.1:18090.
#include <stdio.h>
#include <string.h>
#include <string>
#include <vector>
#include <signal.h>
#include <unistd.h>
#include "webBridge.h"
#include "webApp.h"

struct Img { std::string name; u64 size; std::string kind; bool cue, disc; };
static std::vector<Img> gImgs = {
    { "IMAGE_01.img", 512ull << 20, "FATX (Xbox memory unit)", false, false },
    { "UBISCRABBLE.iso", 481ull << 20, "ISO 9660", false, true },
    { "Disney \"<b>\" & co.cue", 600ull << 20, "BIN/CUE", true, true },
};
static std::string gMounted; static bool gSd, gRo, gCd, gBusy;

int webBridge_imageCount() { return gSd ? 0 : (int)gImgs.size(); }
bool webBridge_image(int i, WebImageInfo& o)
{
    if (i < 0 || i >= (int)gImgs.size()) return false;
    o.name = gImgs[i].name.c_str(); o.size = gImgs[i].size; o.kind = gImgs[i].kind.c_str();
    o.mounted = gImgs[i].name == gMounted; o.isCue = gImgs[i].cue; o.isDisc = gImgs[i].disc;
    return true;
}
void webBridge_status(WebStatus& s)
{
    s.mounted = !gMounted.empty() || gSd; s.sdShared = gSd; s.readOnly = gRo; s.cd = gCd;
    s.hostEjected = false; s.safeToUnmount = !gBusy; s.busy = gBusy;
    s.mountedName = gSd ? "Whole SD card" : gMounted.c_str(); s.usbState = s.mounted ? "Host connected" : "";
    s.readKBs = gBusy ? 900 : 0; s.fsType = "exFAT"; s.maxImageMiB = 2097151;
}
static int find(const char* n) { for (size_t i = 0; i < gImgs.size(); i++) if (gImgs[i].name == n) return (int)i; return -1; }
bool webBridge_mount(const char* name, WebMountMode m, char* msg, size_t sz)
{
    if (!gMounted.empty() || gSd) { snprintf(msg, sz, "Unmount the current image first."); return false; }
    int i = find(name); if (i < 0) { snprintf(msg, sz, "No image called %s.", name); return false; }
    gMounted = name; gCd = m == WebMountMode::Disc || (gImgs[i].disc && m != WebMountMode::ReadOnly); gRo = gCd || m == WebMountMode::ReadOnly;
    snprintf(msg, sz, "Mounted %s", name); return true;
}
bool webBridge_unmount(bool force, char* msg, size_t sz)
{
    if (gMounted.empty() && !gSd) { snprintf(msg, sz, "Nothing is mounted."); return false; }
    if (gBusy && !force) { snprintf(msg, sz, "The PC is using the drive."); return false; }
    gMounted.clear(); gSd = false; snprintf(msg, sz, "Unmounted"); return true;
}
bool webBridge_reinsert(char* msg, size_t sz) { snprintf(msg, sz, "Nothing to reinsert."); return false; }
bool webBridge_delete(const char* name, char* msg, size_t sz)
{
    int i = find(name); if (i < 0) { snprintf(msg, sz, "No image called %s.", name); return false; }
    gImgs.erase(gImgs.begin() + i); snprintf(msg, sz, "Deleted %s", name); return true;
}
bool webBridge_create(const char* name, u32 mib, char* msg, size_t sz)
{
    gImgs.push_back({ std::string(name) + ".img", (u64)mib << 20, "", false, false });
    snprintf(msg, sz, "Created %s.img", name); return true;
}
bool webBridge_rescan(char* msg, size_t sz) { snprintf(msg, sz, "%zu images", gImgs.size()); return true; }
bool webBridge_shareSd(char* msg, size_t sz) { gSd = true; snprintf(msg, sz, "Whole SD card shared over USB"); return true; }

int main()
{
    signal(SIGPIPE, SIG_IGN);
    u32 frame = 0;
    if (!webApp_start(frame)) { perror("start"); return 1; }
    FILE* f = fopen("pin.txt", "w"); fprintf(f, "%s", webApp_pin()); fclose(f);
    while (access("stop", F_OK) != 0) {
        webApp_poll(frame++);
        if (access("busy", F_OK) == 0) gBusy = true; else gBusy = false;
        usleep(16667);
    }
    webApp_stop();
    return 0;
}
