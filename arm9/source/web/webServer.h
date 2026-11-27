#pragma once
#include <stddef.h>
#include <nds/ndstypes.h>
#include "httputil.h"

#define WEB_REQUEST_MAX 2048

struct WebRequest
{
    const char* raw;        ///< whole request header (NUL-terminated)
    size_t rawLength;
    u32 peer;               ///< client IPv4 address (network order)
    bool valid;
    http_request_t line;    ///< method, path, query
};

struct WebResponder
{
    int fd;
};

typedef void (*WebHandler)(const WebRequest& req, const WebResponder& out);

bool webServer_start(int port, WebHandler handler);
void webServer_stop();
bool webServer_running();
/// Call once per frame (now = a frame counter).
void webServer_poll(u32 now);
u32 webServer_requests();

bool webServer_sendAll(const WebResponder& out, const void* data, size_t len);
bool webServer_sendHeaders(const WebResponder& out, const char* status, const char* type, size_t length,
                           const char* extraHeaders = nullptr);
bool webServer_send(const WebResponder& out, const char* status, const char* type, const void* body, size_t len,
                    const char* extraHeaders = nullptr);
bool webServer_sendText(const WebResponder& out, const char* status, const char* type, const char* body,
                        const char* extraHeaders = nullptr);
