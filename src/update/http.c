#include "update/http.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <winhttp.h>

#include "core/neturl.h"
#include "core/str.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void nc_http_response_free(NcHttpResponse *response) {
    if (!response) {
        return;
    }
    free(response->body);
    response->body = NULL;
    response->body_size = 0;
    response->status = 0;
}

static void close_handles(HINTERNET session, HINTERNET connect, HINTERNET request) {
    if (request) {
        WinHttpCloseHandle(request);
    }
    if (connect) {
        WinHttpCloseHandle(connect);
    }
    if (session) {
        WinHttpCloseHandle(session);
    }
}

static bool query_status(HINTERNET request, int *status) {
    DWORD value = 0;
    DWORD size = sizeof(value);
    if (!WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                             WINHTTP_HEADER_NAME_BY_INDEX, &value, &size, WINHTTP_NO_HEADER_INDEX)) {
        return false;
    }
    *status = (int)value;
    return true;
}

static char *query_location(HINTERNET request) {
    DWORD size = 0;
    wchar_t *wide;
    char *utf8;
    WinHttpQueryHeaders(request, WINHTTP_QUERY_CUSTOM, L"Location", NULL, &size,
                        WINHTTP_NO_HEADER_INDEX);
    if (size == 0) {
        return NULL;
    }
    wide = (wchar_t *)malloc((size_t)size + sizeof(wchar_t));
    if (!wide) {
        return NULL;
    }
    if (!WinHttpQueryHeaders(request, WINHTTP_QUERY_CUSTOM, L"Location", wide, &size,
                             WINHTTP_NO_HEADER_INDEX)) {
        free(wide);
        return NULL;
    }
    wide[size / sizeof(wchar_t)] = 0;
    utf8 = nc_utf8_dup(wide);
    free(wide);
    return utf8;
}

static bool read_body(HINTERNET request, size_t max_body, NcHttpResponse *out, NcErr *err) {
    size_t capacity = 8192;
    size_t size = 0;
    unsigned char *body = (unsigned char *)malloc(capacity);
    if (!body) {
        return nc_fail(err, "内存不足。");
    }
    for (;;) {
        DWORD available = 0;
        DWORD read = 0;
        if (!WinHttpQueryDataAvailable(request, &available)) {
            free(body);
            return nc_fail(err, "读取更新响应失败（Win32 错误 %lu）。", (unsigned long)GetLastError());
        }
        if (available == 0) {
            break;
        }
        if (size + available > max_body) {
            free(body);
            return nc_fail(err, "响应内容过大。");
        }
        if (size + available > capacity) {
            size_t next = capacity;
            unsigned char *grown;
            while (next < size + available) {
                next *= 2;
            }
            grown = (unsigned char *)realloc(body, next);
            if (!grown) {
                free(body);
                return nc_fail(err, "内存不足。");
            }
            body = grown;
            capacity = next;
        }
        if (!WinHttpReadData(request, body + size, available, &read)) {
            free(body);
            return nc_fail(err, "读取更新响应失败（Win32 错误 %lu）。", (unsigned long)GetLastError());
        }
        if (read == 0) {
            break;
        }
        size += read;
    }
    out->body = body;
    out->body_size = size;
    return true;
}

static bool perform_request(const NcUrl *url, uint32_t timeout_ms, size_t max_body,
                            NcHttpResponse *out, char **location, NcErr *err) {
    HINTERNET session = NULL;
    HINTERNET connect = NULL;
    HINTERNET request = NULL;
    wchar_t *wide_host = NULL;
    wchar_t *wide_path = NULL;
    DWORD disabled = WINHTTP_DISABLE_REDIRECTS;
    bool ok = false;
    int port;
    session = WinHttpOpen(L"NetConditioner-Updater/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                          WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) {
        return nc_fail(err, "初始化 HTTP 会话失败（Win32 错误 %lu）。", (unsigned long)GetLastError());
    }
    WinHttpSetOption(session, WINHTTP_OPTION_DISABLE_FEATURE, &disabled, sizeof(disabled));
    port = url->port > 0 ? url->port : nc_url_default_port(url);
    wide_host = nc_wide_dup(url->host);
    wide_path = nc_wide_dup(url->path);
    if (!wide_host || !wide_path) {
        nc_fail(err, "内存不足。");
        goto done;
    }
    connect = WinHttpConnect(session, wide_host, (INTERNET_PORT)port, 0);
    if (!connect) {
        nc_fail(err, "连接更新服务器失败（Win32 错误 %lu）。", (unsigned long)GetLastError());
        goto done;
    }
    request = WinHttpOpenRequest(connect, L"GET", wide_path, NULL, WINHTTP_NO_REFERER,
                                 WINHTTP_DEFAULT_ACCEPT_TYPES,
                                 nc_ieq_ascii(url->scheme, "https") ? WINHTTP_FLAG_SECURE : 0);
    if (!request) {
        nc_fail(err, "创建 HTTP 请求失败（Win32 错误 %lu）。", (unsigned long)GetLastError());
        goto done;
    }
    WinHttpSetOption(request, WINHTTP_OPTION_DISABLE_FEATURE, &disabled, sizeof(disabled));
    WinHttpSetTimeouts(request, (int)timeout_ms, (int)timeout_ms, (int)timeout_ms, (int)timeout_ms);
    if (!WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0,
                            0)) {
        nc_fail(err, "网络请求失败（Win32 错误 %lu）。", (unsigned long)GetLastError());
        goto done;
    }
    if (!WinHttpReceiveResponse(request, NULL)) {
        nc_fail(err, "网络请求失败（Win32 错误 %lu）。", (unsigned long)GetLastError());
        goto done;
    }
    if (!query_status(request, &out->status)) {
        nc_fail(err, "读取 HTTP 状态失败（Win32 错误 %lu）。", (unsigned long)GetLastError());
        goto done;
    }
    if (out->status == 301 || out->status == 302 || out->status == 303 || out->status == 307 ||
        out->status == 308) {
        *location = query_location(request);
        if (!*location) {
            nc_fail(err, "重定向缺少地址。");
            goto done;
        }
        ok = true;
        goto done;
    }
    if (!read_body(request, max_body, out, err)) {
        goto done;
    }
    ok = true;
done:
    free(wide_host);
    free(wide_path);
    close_handles(session, connect, request);
    return ok;
}

bool nc_http_get(const char *url_text, bool https_only, uint32_t timeout_ms, size_t max_body,
                 NcHttpResponse *out, NcErr *err) {
    NcUrl current;
    int attempt;
    NcErr local;
    if (!out) {
        return nc_fail(err, "内部错误。");
    }
    memset(out, 0, sizeof(*out));
    if (!nc_url_parse(url_text, "更新地址", &current, &local)) {
        nc_err_set(err, "%s", local.msg);
        return false;
    }
    for (attempt = 0; attempt < 11; attempt++) {
        char *location = NULL;
        bool loopback = nc_url_is_loopback(&current);
        if (https_only && !nc_ieq_ascii(current.scheme, "https")) {
            return nc_fail(err, "更新地址必须使用 https（本机回环地址可用 http 调试）。");
        }
        if (!nc_url_validate_public_target(&current, loopback, &local)) {
            nc_err_set(err, "%s", local.msg);
            return false;
        }
        if (!perform_request(&current, timeout_ms, max_body, out, &location, &local)) {
            nc_err_set(err, "%s", local.msg);
            return false;
        }
        if (!location) {
            return true;
        }
        {
            NcUrl next;
            if (!nc_url_join(&current, location, &next, &local)) {
                free(location);
                nc_err_set(err, "%s", local.msg);
                return false;
            }
            if (!nc_url_validate_redirect(&current, &next, &local)) {
                free(location);
                nc_err_set(err, "%s", local.msg);
                return false;
            }
            current = next;
        }
        free(location);
    }
    return nc_fail(err, "重定向次数过多。");
}
