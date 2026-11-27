// The image mounter's web UI: one page plus a small JSON API.
//
// Security: the DS shows a 4-digit PIN while WiFi is on. The page asks for it
// once; the DS then hands the browser a random session cookie. Wrong PINs
// are rate-limited. Everything that changes something is a POST, so a link
// preview or prefetch can't mount or delete anything.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>
#include <nds.h>
#include "httputil.h"
#include "webServer.h"
#include "webBridge.h"
#include "webApp.h"
#include "webPage.h"

#ifndef WEBAPP_PORT
#define WEBAPP_PORT 80
#endif

// Extra unpredictability for the PIN and session token: the display line
// counter and timer 0 change too quickly to guess from outside.
#ifndef WEBAPP_HW_ENTROPY
#define WEBAPP_HW_ENTROPY() ((u32)(*(vu16*)0x04000006) | ((u32)(*(vu16*)0x04000100) << 16))
#endif

namespace
{
    char sPin[5];
    char sSession[17];
    int sBadPins;
    u32 sLockedUntil;
    u32 sNow;
    char sLastAction[64];

    char sOut[16 * 1024];
    size_t sOutLen;

    void outReset()
    {
        sOutLen = 0;
        sOut[0] = 0;
    }

    void outf(const char* fmt, ...)
    {
        if (sOutLen >= sizeof(sOut) - 1)
            return;
        va_list ap;
        va_start(ap, fmt);
        int n = vsnprintf(sOut + sOutLen, sizeof(sOut) - sOutLen, fmt, ap);
        va_end(ap);
        if (n > 0)
            sOutLen += (size_t)n;
        if (sOutLen > sizeof(sOut) - 1)
            sOutLen = sizeof(sOut) - 1;
    }

    // Appends s as a JSON string (with quotes).
    void outJsonString(const char* s)
    {
        outf("\"");
        for (; s && *s; s++)
        {
            unsigned char c = (unsigned char)*s;
            if (c == '"' || c == '\\')
                outf("\\%c", c);
            else if (c < 0x20)
                outf("\\u%04x", c);
            else
                outf("%c", c);
        }
        outf("\"");
    }

    u32 randomWord()
    {
        static u32 state;
        state ^= (u32)time(nullptr) ^ (sNow * 2654435761u) ^ WEBAPP_HW_ENTROPY() ^ ((u32)rand() << 8);
        state = state * 1664525u + 1013904223u;
        return state ^ (state >> 15);
    }

    bool authorised(const WebRequest& req)
    {
        if (!sSession[0])
            return false;
        char cookies[256], value[32];
        if (!http_header_get(req.raw, req.rawLength, "Cookie", cookies, sizeof(cookies)))
            return false;
        return http_cookie_get(cookies, "imsess", value, sizeof(value)) && strcmp(value, sSession) == 0;
    }

    void sendJson(const WebResponder& out, const char* status = "200 OK", const char* extra = nullptr)
    {
        webServer_send(out, status, "application/json", sOut, sOutLen, extra);
    }

    void sendResult(const WebResponder& out, bool ok, const char* msg)
    {
        snprintf(sLastAction, sizeof(sLastAction), "%s", msg);
        outReset();
        outf("{\"ok\":%s,\"msg\":", ok ? "true" : "false");
        outJsonString(msg);
        outf("}");
        sendJson(out);
    }

    void handleLogin(const WebRequest& req, const WebResponder& out)
    {
        if ((s32)(sLockedUntil - sNow) > 0)
        {
            sendResult(out, false, "Too many wrong PINs. Wait a minute.");
            return;
        }
        char pin[8];
        if (!http_query_get(req.line.query, "pin", pin, sizeof(pin)) || strcmp(pin, sPin) != 0)
        {
            if (++sBadPins >= 5)
            {
                sBadPins = 0;
                sLockedUntil = sNow + 60 * 60;
            }
            sendResult(out, false, "Wrong PIN. It is shown on the DS top screen.");
            return;
        }
        sBadPins = 0;
        if (!sSession[0])
            snprintf(sSession, sizeof(sSession), "%08lx%08lx", (unsigned long)randomWord(), (unsigned long)randomWord());
        char cookie[96];
        snprintf(cookie, sizeof(cookie), "Set-Cookie: imsess=%s; Path=/; HttpOnly; SameSite=Strict\r\n", sSession);
        outReset();
        outf("{\"ok\":true,\"msg\":\"Signed in\"}");
        sendJson(out, "200 OK", cookie);
    }

    void handleStatus(const WebResponder& out)
    {
        WebStatus s;
        webBridge_status(s);
        outReset();
        outf("{\"mounted\":%s,\"sdShared\":%s,\"readOnly\":%s,\"cd\":%s,\"hostEjected\":%s,\"safe\":%s,\"busy\":%s,",
             s.mounted ? "true" : "false", s.sdShared ? "true" : "false", s.readOnly ? "true" : "false",
             s.cd ? "true" : "false", s.hostEjected ? "true" : "false", s.safeToUnmount ? "true" : "false",
             s.busy ? "true" : "false");
        outf("\"name\":");
        outJsonString(s.mountedName);
        outf(",\"usb\":");
        outJsonString(s.usbState);
        outf(",\"readKBs\":%lu,\"fs\":", (unsigned long)s.readKBs);
        outJsonString(s.fsType);
        outf(",\"maxMiB\":%lu,\"fsMaxMiB\":%lu,\"last\":", (unsigned long)s.maxImageMiB, (unsigned long)s.fsLimitMiB);
        outJsonString(sLastAction);
        outf("}");
        sendJson(out);
    }

    void handleImages(const WebResponder& out)
    {
        outReset();
        outf("[");
        int n = webBridge_imageCount();
        for (int i = 0; i < n; i++)
        {
            WebImageInfo e;
            if (!webBridge_image(i, e))
                continue;
            if (sOutLen > sizeof(sOut) - 300)
                break;
            outf("%s{\"name\":", i ? "," : "");
            outJsonString(e.name);
            outf(",\"size\":%llu,\"kind\":", (unsigned long long)e.size);
            outJsonString(e.kind);
            outf(",\"mounted\":%s,\"cue\":%s,\"disc\":%s}", e.mounted ? "true" : "false", e.isCue ? "true" : "false",
                 e.isDisc ? "true" : "false");
        }
        outf("]");
        sendJson(out);
    }

    void handleAction(const WebRequest& req, const WebResponder& out)
    {
        const char* path = req.line.path;
        char name[160], arg[16], msg[128];
        msg[0] = 0;
        bool ok = false;
        http_query_get(req.line.query, "name", name, sizeof(name));
        if (strcmp(path, "/api/mount") == 0)
        {
            http_query_get(req.line.query, "mode", arg, sizeof(arg));
            WebMountMode mode = !strcmp(arg, "ro") ? WebMountMode::ReadOnly
                              : !strcmp(arg, "cd") ? WebMountMode::Disc : WebMountMode::Auto;
            ok = webBridge_mount(name, mode, msg, sizeof(msg));
        }
        else if (strcmp(path, "/api/unmount") == 0)
        {
            bool force = http_query_get(req.line.query, "force", arg, sizeof(arg)) && arg[0] == '1';
            ok = webBridge_unmount(force, msg, sizeof(msg));
        }
        else if (strcmp(path, "/api/reinsert") == 0)
            ok = webBridge_reinsert(msg, sizeof(msg));
        else if (strcmp(path, "/api/delete") == 0)
            ok = webBridge_delete(name, msg, sizeof(msg));
        else if (strcmp(path, "/api/create") == 0)
        {
            http_query_get(req.line.query, "mib", arg, sizeof(arg));
            ok = webBridge_create(name, (u32)strtoul(arg, nullptr, 10), msg, sizeof(msg));
        }
        else if (strcmp(path, "/api/rescan") == 0)
            ok = webBridge_rescan(msg, sizeof(msg));
        else if (strcmp(path, "/api/sharesd") == 0)
            ok = webBridge_shareSd(msg, sizeof(msg));
        else
        {
            webServer_sendText(out, "404 Not Found", "text/plain", "Not found\n");
            return;
        }
        sendResult(out, ok, msg[0] ? msg : (ok ? "Done" : "Failed"));
    }

    void handle(const WebRequest& req, const WebResponder& out)
    {
        const char* m = req.line.method;
        const char* path = req.line.path;
        bool isGet = strcmp(m, "GET") == 0 || strcmp(m, "HEAD") == 0;
        bool isPost = strcmp(m, "POST") == 0;

        if (isGet && (strcmp(path, "/") == 0 || strcmp(path, "/index.html") == 0))
        {
            webServer_send(out, "200 OK", "text/html; charset=utf-8", gWebPage, gWebPageSize,
                           "Content-Security-Policy: default-src 'self' 'unsafe-inline'\r\n");
            return;
        }
        if (isGet && strcmp(path, "/favicon.ico") == 0)
        {
            webServer_sendText(out, "404 Not Found", "text/plain", "");
            return;
        }
        if (isPost && strcmp(path, "/api/login") == 0)
        {
            handleLogin(req, out);
            return;
        }
        if (strncmp(path, "/api/", 5) != 0)
        {
            webServer_sendText(out, "404 Not Found", "text/plain", "Not found\n");
            return;
        }
        if (!authorised(req))
        {
            outReset();
            outf("{\"ok\":false,\"login\":true,\"msg\":\"Enter the PIN shown on the DS.\"}");
            sendJson(out, "401 Unauthorized");
            return;
        }
        if (isGet && strcmp(path, "/api/status") == 0)
            handleStatus(out);
        else if (isGet && strcmp(path, "/api/images") == 0)
            handleImages(out);
        else if (isPost)
            handleAction(req, out);
        else
            webServer_sendText(out, "405 Method Not Allowed", "text/plain", "Use POST\n", "Allow: POST\r\n");
    }
}

bool webApp_start(u32 now)
{
    sNow = now;
    // One PIN (and session) per run of the app: WiFi dropping and rejoining
    // restarts the server, but must not change the PIN under the user.
    if (!sPin[0])
    {
        snprintf(sPin, sizeof(sPin), "%04lu", (unsigned long)(randomWord() % 10000));
        sSession[0] = 0;
    }
    sBadPins = 0;
    sLockedUntil = now;
    sLastAction[0] = 0;
    return webServer_start(WEBAPP_PORT, handle);
}

void webApp_stop()
{
    webServer_stop();
    sSession[0] = 0;
}

void webApp_poll(u32 now)
{
    sNow = now;
    webServer_poll(now);
}

const char* webApp_pin()
{
    return sPin;
}

bool webApp_running()
{
    return webServer_running();
}
