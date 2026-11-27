// Tiny non-blocking HTTP/1.0 server (lwIP sockets through DSWiFi).
//
// Browsers open spare connections that may never send anything, so each
// connection gets its own slot and nothing blocks while waiting for data
// (the fix proven on hardware with wifi-probe). One request per connection;
// the response is sent synchronously.
#include <stdio.h>
#include <string.h>
#include <nds.h>
#include <dswifi9.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include "httputil.h"
#include "webServer.h"

namespace
{
    constexpr int MAX_CLIENTS = 4;
    constexpr u32 CLIENT_TIMEOUT_FRAMES = 5 * 60;

    struct Client
    {
        int fd;
        size_t len;
        u32 since;
        u32 peer;
        char buf[WEB_REQUEST_MAX];
    };

    int sListen = -1;
    Client sClients[MAX_CLIENTS];
    WebHandler sHandler;
    u32 sRequests;

    void closeClient(Client& c)
    {
        if (c.fd >= 0)
        {
            shutdown(c.fd, SHUT_RDWR);
            closesocket(c.fd);
        }
        c.fd = -1;
        c.len = 0;
    }

    void acceptClient(u32 now)
    {
        struct sockaddr_in peer;
        socklen_t plen = sizeof(peer);
        int fd = accept(sListen, (struct sockaddr*)&peer, &plen);
        if (fd < 0)
            return;
        Client* slot = nullptr;
        for (Client& c : sClients)
            if (c.fd < 0)
            {
                slot = &c;
                break;
            }
        if (!slot)
        {
            // all busy: drop the connection that has waited longest
            slot = &sClients[0];
            for (Client& c : sClients)
                if ((s32)(c.since - slot->since) < 0)
                    slot = &c;
            closeClient(*slot);
        }
        slot->fd = fd;
        slot->len = 0;
        slot->since = now;
        slot->peer = peer.sin_addr.s_addr;
    }
}

bool webServer_start(int port, WebHandler handler)
{
    webServer_stop();
    sHandler = handler;
    for (Client& c : sClients)
        c.fd = -1;
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0)
        return false;
    int one = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = INADDR_ANY;
    if (bind(s, (struct sockaddr*)&addr, sizeof(addr)) != 0 || listen(s, 4) != 0)
    {
        closesocket(s);
        return false;
    }
    sListen = s;
    return true;
}

void webServer_stop()
{
    for (Client& c : sClients)
        closeClient(c);
    if (sListen >= 0)
        closesocket(sListen);
    sListen = -1;
}

bool webServer_running()
{
    return sListen >= 0;
}

u32 webServer_requests()
{
    return sRequests;
}

void webServer_poll(u32 now)
{
    if (sListen < 0)
        return;
    fd_set rd;
    FD_ZERO(&rd);
    FD_SET(sListen, &rd);
    int maxfd = sListen;
    for (Client& c : sClients)
        if (c.fd >= 0)
        {
            FD_SET(c.fd, &rd);
            if (c.fd > maxfd)
                maxfd = c.fd;
        }
    struct timeval tv = { 0, 0 };
    if (select(maxfd + 1, &rd, nullptr, nullptr, &tv) > 0)
    {
        for (Client& c : sClients)
        {
            if (c.fd < 0 || !FD_ISSET(c.fd, &rd))
                continue;
            int n = recv(c.fd, c.buf + c.len, sizeof(c.buf) - 1 - c.len, 0);
            if (n <= 0)
            {
                closeClient(c);
                continue;
            }
            c.len += (size_t)n;
            c.buf[c.len] = 0;
            bool full = c.len >= sizeof(c.buf) - 1;
            if (http_header_complete(c.buf, c.len) || full)
            {
                WebRequest req;
                req.raw = c.buf;
                req.rawLength = c.len;
                req.peer = c.peer;
                req.valid = !full || http_header_complete(c.buf, c.len);
                if (req.valid && http_parse_request_line(c.buf, c.len, &req.line) != 0)
                    req.valid = false;
                sRequests++;
                WebResponder out = { c.fd };
                if (!req.valid)
                    webServer_sendText(out, "400 Bad Request", "text/plain", "Bad request\n");
                else
                    sHandler(req, out);
                closeClient(c);
            }
        }
        if (FD_ISSET(sListen, &rd))
        {
            // Take every waiting connection now, so a burst of spare
            // connections from a browser doesn't fill the backlog.
            for (int i = 0; i < 8; i++)
            {
                acceptClient(now);
                fd_set ld;
                FD_ZERO(&ld);
                FD_SET(sListen, &ld);
                struct timeval zero = { 0, 0 };
                if (select(sListen + 1, &ld, nullptr, nullptr, &zero) <= 0)
                    break;
            }
        }
    }
    for (Client& c : sClients)
        if (c.fd >= 0 && now - c.since > CLIENT_TIMEOUT_FRAMES)
            closeClient(c);
}

bool webServer_sendAll(const WebResponder& out, const void* data, size_t len)
{
    const char* p = (const char*)data;
    while (len > 0)
    {
        int n = send(out.fd, p, len, 0);
        if (n <= 0)
            return false;
        p += n;
        len -= (size_t)n;
    }
    return true;
}

bool webServer_sendHeaders(const WebResponder& out, const char* status, const char* type, size_t length,
                           const char* extraHeaders)
{
    char hdr[384];
    int n = snprintf(hdr, sizeof(hdr),
                     "HTTP/1.0 %s\r\n"
                     "Content-Type: %s\r\n"
                     "Content-Length: %lu\r\n"
                     "Cache-Control: no-store\r\n"
                     "X-Content-Type-Options: nosniff\r\n"
                     "Connection: close\r\n"
                     "%s\r\n",
                     status, type, (unsigned long)length, extraHeaders ? extraHeaders : "");
    return n > 0 && n < (int)sizeof(hdr) && webServer_sendAll(out, hdr, (size_t)n);
}

bool webServer_send(const WebResponder& out, const char* status, const char* type, const void* body, size_t len,
                    const char* extraHeaders)
{
    return webServer_sendHeaders(out, status, type, len, extraHeaders) && (len == 0 || webServer_sendAll(out, body, len));
}

bool webServer_sendText(const WebResponder& out, const char* status, const char* type, const char* body,
                        const char* extraHeaders)
{
    return webServer_send(out, status, type, body, strlen(body), extraHeaders);
}
