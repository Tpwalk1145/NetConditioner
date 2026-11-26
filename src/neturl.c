#include "neturl.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

static INIT_ONCE g_wsa_once = INIT_ONCE_STATIC_INIT;

static BOOL CALLBACK wsa_init_once([[maybe_unused]] PINIT_ONCE once,
                                   [[maybe_unused]] PVOID parameter,
                                   [[maybe_unused]] PVOID *context) {
    WSADATA data;
    return WSAStartup(MAKEWORD(2, 2), &data) == 0;
}

static bool ensure_winsock(void) {
    return InitOnceExecuteOnce(&g_wsa_once, wsa_init_once, NULL, NULL) != 0;
}

static void copy_text(char *out, size_t cap, const char *text, size_t length) {
    size_t copy = length < cap - 1 ? length : cap - 1;
    if (!out || cap == 0) {
        return;
    }
    memcpy(out, text, copy);
    out[copy] = 0;
}

bool nc_url_parse(const char *text, const char *label, NcUrl *out, NcErr *err) {
    char buffer[2050];
    size_t length;
    const char *scheme_end;
    const char *authority;
    const char *authority_end;
    const char *host_start;
    const char *host_end;
    const char *path_start;
    const char *fragment;
    size_t authority_length;
    const char *default_label = "更新地址";
    if (!label) {
        label = default_label;
    }
    if (!out) {
        return nc_fail(err, "URL 参数无效。");
    }
    memset(out, 0, sizeof(*out));
    if (!text) {
        text = "";
    }
    while (*text && isspace((unsigned char)*text)) {
        text++;
    }
    length = strlen(text);
    while (length > 0 && isspace((unsigned char)text[length - 1])) {
        length--;
    }
    if (length == 0) {
        return nc_fail(err, "未配置更新地址。");
    }
    if (length > 2048) {
        return nc_fail(err, "更新地址过长。");
    }
    memcpy(buffer, text, length);
    buffer[length] = 0;
    scheme_end = strstr(buffer, "://");
    if (!scheme_end) {
        return nc_fail(err, "%s无效。", label);
    }
    {
        size_t scheme_length = (size_t)(scheme_end - buffer);
        if (scheme_length == 0 || scheme_length >= sizeof(out->scheme)) {
            return nc_fail(err, "%s无效。", label);
        }
        copy_text(out->scheme, sizeof(out->scheme), buffer, scheme_length);
        if (!nc_ieq_ascii(out->scheme, "http") && !nc_ieq_ascii(out->scheme, "https")) {
            return nc_fail(err, "%s必须使用 https（本机回环地址可用 http 调试）。", label);
        }
    }
    authority = scheme_end + 3;
    authority_end = authority;
    while (*authority_end && *authority_end != '/' && *authority_end != '?' && *authority_end != '#') {
        authority_end++;
    }
    authority_length = (size_t)(authority_end - authority);
    if (authority_length == 0) {
        return nc_fail(err, "%s不是有效的绝对 URL。", label);
    }
    if (memchr(authority, '@', authority_length) != NULL) {
        return nc_fail(err, "%s不能包含用户信息。", label);
    }
    host_start = authority;
    host_end = host_start;
    if (host_start < authority_end && *host_start == '[') {
        const char *closing = memchr(host_start, ']', authority_length);
        if (!closing) {
            return nc_fail(err, "%s不是有效的绝对 URL。", label);
        }
        host_start++;
        host_end = closing;
        if (host_end == host_start) {
            return nc_fail(err, "%s不是有效的绝对 URL。", label);
        }
        if (host_end + 1 < authority_end) {
            if (host_end[1] != ':') {
                return nc_fail(err, "%s不是有效的绝对 URL。", label);
            }
            {
                const char *port_text = host_end + 2;
                int port = 0;
                while (port_text < authority_end && *port_text >= '0' && *port_text <= '9') {
                    port = port * 10 + (*port_text - '0');
                    if (port > 65535) {
                        return nc_fail(err, "%s不是有效的绝对 URL。", label);
                    }
                    port_text++;
                }
                if (port_text != authority_end || port <= 0) {
                    return nc_fail(err, "%s不是有效的绝对 URL。", label);
                }
                out->port = port;
            }
        }
    } else {
        const char *colon = memchr(host_start, ':', authority_length);
        host_end = colon ? colon : authority_end;
        if (host_end == host_start) {
            return nc_fail(err, "%s不是有效的绝对 URL。", label);
        }
        if (colon) {
            const char *port_text = colon + 1;
            int port = 0;
            while (port_text < authority_end && *port_text >= '0' && *port_text <= '9') {
                port = port * 10 + (*port_text - '0');
                if (port > 65535) {
                    return nc_fail(err, "%s不是有效的绝对 URL。", label);
                }
                port_text++;
            }
            if (port_text != authority_end || port <= 0) {
                return nc_fail(err, "%s不是有效的绝对 URL。", label);
            }
            out->port = port;
        }
    }
    if ((size_t)(host_end - host_start) >= sizeof(out->host)) {
        return nc_fail(err, "%s无效。", label);
    }
    copy_text(out->host, sizeof(out->host), host_start, (size_t)(host_end - host_start));
    path_start = authority_end;
    fragment = strchr(path_start, '#');
    if (fragment) {
        length = (size_t)(fragment - path_start);
    } else {
        length = strlen(path_start);
    }
    if (length == 0) {
        snprintf(out->path, sizeof(out->path), "/");
    } else if (path_start[0] == '?') {
        if (length + 1 >= sizeof(out->path)) {
            return nc_fail(err, "更新地址过长。");
        }
        out->path[0] = '/';
        copy_text(out->path + 1, sizeof(out->path) - 1, path_start, length);
    } else {
        if (length >= sizeof(out->path)) {
            return nc_fail(err, "更新地址过长。");
        }
        copy_text(out->path, sizeof(out->path), path_start, length);
    }
    return true;
}

int nc_url_default_port(const NcUrl *url) {
    if (!url) {
        return 443;
    }
    return nc_ieq_ascii(url->scheme, "http") ? 80 : 443;
}

bool nc_url_is_loopback(const NcUrl *url) {
    if (!url) {
        return false;
    }
    if (nc_ieq_ascii(url->host, "localhost")) {
        return true;
    }
    return nc_ip_text_is_loopback(url->host);
}

static bool ipv4_bytes_global(const unsigned char *bytes) {
    unsigned a = bytes[0];
    unsigned b = bytes[1];
    if (a == 0 || a == 10 || a == 127) {
        return false;
    }
    if (a == 169 && b == 254) {
        return false;
    }
    if (a == 172 && b >= 16 && b <= 31) {
        return false;
    }
    if (a == 192 && b == 168) {
        return false;
    }
    if (a == 255 && bytes[1] == 255 && bytes[2] == 255 && bytes[3] == 255) {
        return false;
    }
    if (a >= 224 && a <= 239) {
        return false;
    }
    if (a == 192 && b == 0 && bytes[2] == 2) {
        return false;
    }
    if (a == 198 && b == 51 && bytes[2] == 100) {
        return false;
    }
    if (a == 203 && b == 0 && bytes[2] == 113) {
        return false;
    }
    return true;
}

static bool ipv6_bytes_global(const unsigned char *bytes) {
    static const unsigned char loopback[16] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1};
    static const unsigned char unspecified[16] = {};
    if (memcmp(bytes, loopback, 16) == 0 || memcmp(bytes, unspecified, 16) == 0) {
        return false;
    }
    if ((bytes[0] & 0xFE) == 0xFC) {
        return false;
    }
    if (bytes[0] == 0xFE && (bytes[1] & 0xC0) == 0x80) {
        return false;
    }
    if (bytes[0] == 0xFF) {
        return false;
    }
    return true;
}

static bool address_global(const struct sockaddr *address) {
    if (!address) {
        return false;
    }
    if (address->sa_family == AF_INET) {
        const struct sockaddr_in *v4 = (const struct sockaddr_in *)address;
        const unsigned char *bytes = (const unsigned char *)&v4->sin_addr;
        if (bytes[0] == 0 && bytes[1] == 0 && bytes[2] == 0 && bytes[3] == 0) {
            return false;
        }
        return ipv4_bytes_global(bytes);
    }
    if (address->sa_family == AF_INET6) {
        const struct sockaddr_in6 *v6 = (const struct sockaddr_in6 *)address;
        const unsigned char *bytes = (const unsigned char *)&v6->sin6_addr;
        static const unsigned char mapped_prefix[12] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xFF};
        if (memcmp(bytes, mapped_prefix, 12) == 0) {
            return ipv4_bytes_global(bytes + 12);
        }
        return ipv6_bytes_global(bytes);
    }
    return false;
}

static bool address_loopback(const struct sockaddr *address) {
    if (!address) {
        return false;
    }
    if (address->sa_family == AF_INET) {
        const struct sockaddr_in *v4 = (const struct sockaddr_in *)address;
        return ((const unsigned char *)&v4->sin_addr)[0] == 127;
    }
    if (address->sa_family == AF_INET6) {
        const struct sockaddr_in6 *v6 = (const struct sockaddr_in6 *)address;
        const unsigned char *bytes = (const unsigned char *)&v6->sin6_addr;
        static const unsigned char loopback[16] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1};
        return memcmp(bytes, loopback, 16) == 0;
    }
    return false;
}

bool nc_ip_text_is_loopback(const char *text) {
    struct in_addr v4;
    struct in6_addr v6;
    if (!text) {
        return false;
    }
    if (InetPtonA(AF_INET, text, &v4) == 1) {
        const unsigned char *bytes = (const unsigned char *)&v4;
        return bytes[0] == 127;
    }
    if (InetPtonA(AF_INET6, text, &v6) == 1) {
        const unsigned char *bytes = (const unsigned char *)&v6;
        static const unsigned char loopback[16] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1};
        return memcmp(bytes, loopback, 16) == 0;
    }
    return false;
}

bool nc_ip_text_is_global(const char *text) {
    struct in_addr v4;
    struct in6_addr v6;
    if (!text) {
        return false;
    }
    if (InetPtonA(AF_INET, text, &v4) == 1) {
        return ipv4_bytes_global((const unsigned char *)&v4);
    }
    if (InetPtonA(AF_INET6, text, &v6) == 1) {
        return ipv6_bytes_global((const unsigned char *)&v6);
    }
    return false;
}

bool nc_url_validate_feed(const char *text, NcErr *err) {
    NcUrl url;
    if (!nc_url_parse(text, "更新地址", &url, err)) {
        return false;
    }
    if (nc_ieq_ascii(url.scheme, "https")) {
        return true;
    }
    if (nc_ieq_ascii(url.scheme, "http") && nc_url_is_loopback(&url)) {
        return true;
    }
    return nc_fail(err, "更新地址必须使用 https（本机回环地址可用 http 调试）。");
}

bool nc_url_validate_installer(const char *text, NcErr *err) {
    NcUrl url;
    if (!nc_url_parse(text, "更新包地址", &url, err)) {
        return false;
    }
    if (nc_ieq_ascii(url.scheme, "https") || nc_url_is_loopback(&url)) {
        return true;
    }
    return nc_fail(err, "更新包地址必须使用 https。");
}

static void build_authority(const NcUrl *url, Str *out) {
    str_addf(out, "%s://%s", url->scheme, url->host);
    if (url->port > 0) {
        str_addf(out, ":%d", url->port);
    }
}

bool nc_url_join(const NcUrl *base, const char *location, NcUrl *out, NcErr *err) {
    Str target;
    char *absolute;
    bool ok;
    if (!base || !location || !out) {
        return nc_fail(err, "重定向地址无效。");
    }
    if (strstr(location, "://") != NULL) {
        return nc_url_parse(location, "重定向地址", out, err);
    }
    str_init(&target);
    if (location[0] == '/' && location[1] == '/') {
        str_addf(&target, "%s:%s", base->scheme, location);
    } else if (location[0] == '/') {
        build_authority(base, &target);
        str_add(&target, location);
    } else if (location[0] == '?') {
        build_authority(base, &target);
        {
            const char *query = strchr(base->path, '?');
            if (query) {
                str_addn(&target, base->path, (size_t)(query - base->path));
            } else {
                str_add(&target, base->path);
            }
        }
        str_add(&target, location);
    } else {
        build_authority(base, &target);
        {
            const char *query = strchr(base->path, '?');
            const char *end = query ? query : base->path + strlen(base->path);
            const char *slash = NULL;
            const char *cursor;
            for (cursor = base->path; cursor < end; cursor++) {
                if (*cursor == '/') {
                    slash = cursor;
                }
            }
            if (slash) {
                str_addn(&target, base->path, (size_t)(slash - base->path + 1));
            } else {
                str_add(&target, "/");
            }
        }
        str_add(&target, location);
    }
    absolute = str_take(&target);
    if (!absolute) {
        return nc_fail(err, "内存不足。");
    }
    ok = nc_url_parse(absolute, "重定向地址", out, err);
    free(absolute);
    return ok;
}

bool nc_url_validate_redirect(const NcUrl *current, const NcUrl *next, NcErr *err) {
    bool next_loopback;
    bool current_loopback;
    if (!current || !next) {
        return nc_fail(err, "重定向地址无效。");
    }
    next_loopback = nc_url_is_loopback(next);
    current_loopback = nc_url_is_loopback(current);
    if (next_loopback && !(nc_ieq_ascii(current->scheme, "http") && current_loopback)) {
        return nc_fail(err, "重定向到本机回环地址，已拒绝。");
    }
    if (nc_ieq_ascii(current->scheme, "http") && !next_loopback) {
        return nc_fail(err, "本机调试重定向离开回环地址，已拒绝。");
    }
    if (nc_ieq_ascii(next->scheme, "http") &&
        !(nc_ieq_ascii(current->scheme, "http") && current_loopback && next_loopback)) {
        return nc_fail(err, "重定向降级或离开本机回环地址，已拒绝。");
    }
    {
        struct in_addr v4;
        struct in6_addr v6;
        bool is_ip = InetPtonA(AF_INET, next->host, &v4) == 1 || InetPtonA(AF_INET6, next->host, &v6) == 1;
        if (is_ip && !next_loopback && !nc_ip_text_is_global(next->host)) {
            return nc_fail(err, "重定向到非公网地址，已拒绝。");
        }
    }
    if (nc_ieq_ascii(next->scheme, "https") && next->host[0] == 0) {
        return nc_fail(err, "重定向地址无效。");
    }
    return true;
}

bool nc_url_validate_public_target(const NcUrl *url, bool allow_loopback, NcErr *err) {
    struct addrinfo hints;
    struct addrinfo *result = NULL;
    struct addrinfo *entry;
    char service[16];
    int port;
    int status;
    bool found = false;
    bool loopback_url;
    if (!url || !url->host[0]) {
        return nc_fail(err, "更新地址无效。");
    }
    if (!ensure_winsock()) {
        return nc_fail(err, "无法初始化网络。");
    }
    port = url->port > 0 ? url->port : nc_url_default_port(url);
    snprintf(service, sizeof(service), "%d", port);
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    status = getaddrinfo(url->host, service, &hints, &result);
    if (status != 0 || !result) {
        if (result) {
            freeaddrinfo(result);
        }
        return nc_fail(err, "无法解析更新服务器地址。");
    }
    loopback_url = nc_url_is_loopback(url);
    for (entry = result; entry; entry = entry->ai_next) {
        bool permitted;
        found = true;
        if (allow_loopback && loopback_url) {
            permitted = address_loopback(entry->ai_addr);
        } else {
            permitted = address_global(entry->ai_addr);
        }
        if (!permitted) {
            freeaddrinfo(result);
            return nc_fail(err, "更新服务器解析到非公网地址，已拒绝。");
        }
    }
    freeaddrinfo(result);
    if (!found) {
        return nc_fail(err, "无法解析更新服务器地址。");
    }
    return true;
}

bool nc_url_redact(const char *url, char *out, size_t cap) {
    NcUrl parsed;
    NcErr ignored;
    Str text;
    if (!out || cap == 0) {
        return false;
    }
    out[0] = 0;
    if (!nc_url_parse(url, "更新地址", &parsed, &ignored)) {
        snprintf(out, cap, "(无效地址)");
        return false;
    }
    str_init(&text);
    build_authority(&parsed, &text);
    str_add(&text, parsed.path);
    if (text.len >= cap) {
        memcpy(out, str_cstr(&text), cap - 1);
        out[cap - 1] = 0;
    } else {
        memcpy(out, str_cstr(&text), text.len + 1);
    }
    str_free(&text);
    return true;
}

bool nc_neturl_normalize_sha256(const char *value, char out[65]) {
    const char *text = value;
    size_t index;
    size_t length;
    if (!out) {
        return false;
    }
    out[0] = 0;
    if (!text) {
        return false;
    }
    while (*text && isspace((unsigned char)*text)) {
        text++;
    }
    length = strlen(text);
    while (length > 0 && isspace((unsigned char)text[length - 1])) {
        length--;
    }
    if (length == 0) {
        return false;
    }
    if (length > 7 && _strnicmp(text, "sha256:", 7) == 0) {
        text += 7;
        while (*text && isspace((unsigned char)*text)) {
            text++;
        }
        length = strlen(text);
        while (length > 0 && isspace((unsigned char)text[length - 1])) {
            length--;
        }
    }
    if (length != 64) {
        return false;
    }
    for (index = 0; index < 64; index++) {
        unsigned char c = (unsigned char)text[index];
        if (c > 0x7F || !isxdigit(c)) {
            return false;
        }
        out[index] = (char)toupper(c);
    }
    out[64] = 0;
    return true;
}
