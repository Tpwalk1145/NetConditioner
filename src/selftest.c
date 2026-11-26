#include "selftest.h"

#include <process.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "engine/engine.h"
#include "engine/process_filter.h"
#include "hotkey.h"
#include "json.h"
#include "neturl.h"
#include "update/update.h"
#include "update/selfupdate.h"
#include "util.h"
#include "version.h"
#include "win.h"

typedef struct {
    Str lines;
    int failures;
} NcTestLog;

static void log_write(NcTestLog *log, const char *text) {
    char stamp[32];
    nc_format_local_hms_ms(stamp, sizeof(stamp));
    str_addf(&log->lines, "[%s] %s\r\n", stamp, text);
}

static void log_pass(NcTestLog *log, const char *text) {
    log_write(log, text);
    {
        char buffer[2400];
        snprintf(buffer, sizeof(buffer), "PASS  %s", text);
        log_write(log, buffer);
    }
}

static void log_fail(NcTestLog *log, const char *text) {
    log->failures++;
    {
        char buffer[2400];
        snprintf(buffer, sizeof(buffer), "FAIL  %s", text);
        log_write(log, buffer);
    }
}

static void log_skip(NcTestLog *log, const char *text) {
    char buffer[2400];
    snprintf(buffer, sizeof(buffer), "SKIP  %s", text);
    log_write(log, buffer);
}

static int finish(NcTestLog *log, const wchar_t *path) {
    NcErr error;
    void *data;
    if (log->failures == 0) {
        log_write(log, "RESULT: PASS");
    } else {
        char buffer[64];
        snprintf(buffer, sizeof(buffer), "RESULT: FAIL (%d)", log->failures);
        log_write(log, buffer);
    }
    data = str_take(&log->lines);
    if (data) {
        nc_write_file(path, data, strlen((char *)data), &error);
        free(data);
    }
    return log->failures == 0 ? 0 : 1;
}

static bool ensure_winsock(void) {
    static LONG started = 0;
    if (InterlockedCompareExchange(&started, 1, 0) != 0) {
        return true;
    }
    {
        WSADATA data;
        if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
            InterlockedExchange(&started, 0);
            return false;
        }
    }
    return true;
}

static bool fill_sockaddr(const char *address, uint16_t port, int family, SOCKADDR_STORAGE *storage,
                          int *length) {
    memset(storage, 0, sizeof(*storage));
    if (family == AF_INET) {
        SOCKADDR_IN *v4 = (SOCKADDR_IN *)storage;
        v4->sin_family = AF_INET;
        v4->sin_port = htons(port);
        if (InetPtonA(AF_INET, address, &v4->sin_addr) != 1) {
            return false;
        }
        *length = sizeof(SOCKADDR_IN);
        return true;
    }
    {
        SOCKADDR_IN6 *v6 = (SOCKADDR_IN6 *)storage;
        v6->sin6_family = AF_INET6;
        v6->sin6_port = htons(port);
        if (InetPtonA(AF_INET6, address, &v6->sin6_addr) != 1) {
            return false;
        }
        *length = sizeof(SOCKADDR_IN6);
        return true;
    }
}

typedef struct {
    SOCKET socket;
    HANDLE thread;
    volatile LONG stop;
} NcEchoServer;

static unsigned __stdcall echo_connection(void *parameter) {
    SOCKET client = (SOCKET)(uintptr_t)parameter;
    char buffer[4096];
    for (;;) {
        int read = recv(client, buffer, sizeof(buffer), 0);
        int sent = 0;
        if (read <= 0) {
            break;
        }
        while (sent < read) {
            int result = send(client, buffer + sent, read - sent, 0);
            if (result <= 0) {
                break;
            }
            sent += result;
        }
        if (sent < read) {
            break;
        }
    }
    closesocket(client);
    return 0;
}

static unsigned __stdcall echo_accept_thread(void *parameter) {
    NcEchoServer *server = (NcEchoServer *)parameter;
    while (InterlockedCompareExchange(&server->stop, 0, 0) == 0) {
        SOCKET client = accept(server->socket, NULL, NULL);
        if (client == INVALID_SOCKET) {
            if (InterlockedCompareExchange(&server->stop, 0, 0) != 0) {
                break;
            }
            Sleep(5);
            continue;
        }
        {
            uintptr_t thread = _beginthreadex(NULL, 0, echo_connection, (void *)(uintptr_t)client, 0, NULL);
            if (thread) {
                CloseHandle((HANDLE)thread);
            } else {
                closesocket(client);
            }
        }
    }
    return 0;
}

static bool echo_server_start(NcEchoServer *server, int family, const char *address, uint16_t port,
                              uint16_t *actual_port) {
    SOCKADDR_STORAGE storage;
    int length = 0;
    if (!server || !ensure_winsock()) {
        return false;
    }
    memset(server, 0, sizeof(*server));
    server->socket = INVALID_SOCKET;
    if (!fill_sockaddr(address, port, family, &storage, &length)) {
        return false;
    }
    server->socket = socket(family, SOCK_STREAM, IPPROTO_TCP);
    if (server->socket == INVALID_SOCKET) {
        return false;
    }
    {
        BOOL reuse = TRUE;
        setsockopt(server->socket, SOL_SOCKET, SO_REUSEADDR, (const char *)&reuse, sizeof(reuse));
    }
    if (bind(server->socket, (const SOCKADDR *)&storage, length) != 0 ||
        listen(server->socket, SOMAXCONN) != 0) {
        closesocket(server->socket);
        server->socket = INVALID_SOCKET;
        return false;
    }
    if (actual_port) {
        SOCKADDR_STORAGE local;
        int local_length = sizeof(local);
        if (getsockname(server->socket, (SOCKADDR *)&local, &local_length) == 0) {
            *actual_port = ntohs(family == AF_INET ? ((SOCKADDR_IN *)&local)->sin_port
                                                   : ((SOCKADDR_IN6 *)&local)->sin6_port);
        }
    }
    server->thread = (HANDLE)_beginthreadex(NULL, 0, echo_accept_thread, server, 0, NULL);
    if (!server->thread) {
        closesocket(server->socket);
        server->socket = INVALID_SOCKET;
        return false;
    }
    return true;
}

static void echo_server_stop(NcEchoServer *server) {
    if (!server || server->socket == INVALID_SOCKET) {
        return;
    }
    InterlockedExchange(&server->stop, 1);
    closesocket(server->socket);
    server->socket = INVALID_SOCKET;
    if (server->thread) {
        WaitForSingleObject(server->thread, 3000);
        CloseHandle(server->thread);
        server->thread = NULL;
    }
}

typedef struct {
    SOCKET socket;
    HANDLE thread;
    volatile LONG stop;
} NcUdpEchoServer;

static unsigned __stdcall udp_echo_thread(void *parameter) {
    NcUdpEchoServer *server = (NcUdpEchoServer *)parameter;
    char buffer[2048];
    while (InterlockedCompareExchange(&server->stop, 0, 0) == 0) {
        SOCKADDR_STORAGE remote;
        int remote_length = sizeof(remote);
        int read = recvfrom(server->socket, buffer, sizeof(buffer), 0, (SOCKADDR *)&remote,
                            &remote_length);
        if (read > 0) {
            sendto(server->socket, buffer, read, 0, (SOCKADDR *)&remote, remote_length);
        }
    }
    return 0;
}

static bool udp_echo_start(NcUdpEchoServer *server, int family, const char *address, uint16_t port,
                           uint16_t *actual_port) {
    SOCKADDR_STORAGE storage;
    int length = 0;
    DWORD timeout = 100;
    if (!server || !ensure_winsock()) {
        return false;
    }
    memset(server, 0, sizeof(*server));
    server->socket = INVALID_SOCKET;
    if (!fill_sockaddr(address, port, family, &storage, &length)) {
        return false;
    }
    server->socket = socket(family, SOCK_DGRAM, IPPROTO_UDP);
    if (server->socket == INVALID_SOCKET) {
        return false;
    }
    setsockopt(server->socket, SOL_SOCKET, SO_RCVTIMEO, (const char *)&timeout, sizeof(timeout));
    if (bind(server->socket, (const SOCKADDR *)&storage, length) != 0) {
        closesocket(server->socket);
        server->socket = INVALID_SOCKET;
        return false;
    }
    if (actual_port) {
        SOCKADDR_STORAGE local;
        int local_length = sizeof(local);
        if (getsockname(server->socket, (SOCKADDR *)&local, &local_length) == 0) {
            *actual_port = ntohs(family == AF_INET ? ((SOCKADDR_IN *)&local)->sin_port
                                                   : ((SOCKADDR_IN6 *)&local)->sin6_port);
        }
    }
    server->thread = (HANDLE)_beginthreadex(NULL, 0, udp_echo_thread, server, 0, NULL);
    if (!server->thread) {
        closesocket(server->socket);
        server->socket = INVALID_SOCKET;
        return false;
    }
    return true;
}

static void udp_echo_stop(NcUdpEchoServer *server) {
    if (!server || server->socket == INVALID_SOCKET) {
        return;
    }
    InterlockedExchange(&server->stop, 1);
    closesocket(server->socket);
    server->socket = INVALID_SOCKET;
    if (server->thread) {
        WaitForSingleObject(server->thread, 3000);
        CloseHandle(server->thread);
        server->thread = NULL;
    }
}

typedef struct {
    SOCKET socket;
    HANDLE thread;
    volatile LONG stop;
    char *manifest;
    unsigned char *payload;
    size_t payload_size;
} NcMiniHttpServer;

static unsigned __stdcall mini_http_thread(void *parameter) {
    NcMiniHttpServer *server = (NcMiniHttpServer *)parameter;
    while (InterlockedCompareExchange(&server->stop, 0, 0) == 0) {
        SOCKET client = accept(server->socket, NULL, NULL);
        char request[4096];
        int used = 0;
        if (client == INVALID_SOCKET) {
            if (InterlockedCompareExchange(&server->stop, 0, 0) != 0) {
                break;
            }
            Sleep(5);
            continue;
        }
        while (used < (int)sizeof(request) - 1) {
            int read = recv(client, request + used, (int)sizeof(request) - 1 - used, 0);
            if (read <= 0) {
                break;
            }
            used += read;
            request[used] = 0;
            if (strstr(request, "\r\n\r\n")) {
                break;
            }
        }
        request[used] = 0;
        if (strncmp(request, "GET /latest.json", 16) == 0) {
            char header[256];
            int header_length = snprintf(header, sizeof(header),
                                         "HTTP/1.1 200 OK\r\nContent-Length: %u\r\nConnection: close\r\n\r\n",
                                         (unsigned)strlen(server->manifest));
            send(client, header, header_length, 0);
            send(client, server->manifest, (int)strlen(server->manifest), 0);
        } else if (strncmp(request, "GET /payload.exe", 16) == 0) {
            char header[256];
            int header_length = snprintf(header, sizeof(header),
                                         "HTTP/1.1 200 OK\r\nContent-Length: %u\r\nConnection: close\r\n\r\n",
                                         (unsigned)server->payload_size);
            send(client, header, header_length, 0);
            if (server->payload_size > 0) {
                send(client, (const char *)server->payload, (int)server->payload_size, 0);
            }
        } else {
            const char *not_found = "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
            send(client, not_found, (int)strlen(not_found), 0);
        }
        closesocket(client);
    }
    return 0;
}

static bool mini_http_start(NcMiniHttpServer *server, const char *manifest, const unsigned char *payload,
                            size_t payload_size, uint16_t desired_port, uint16_t *port) {
    SOCKADDR_STORAGE storage;
    int length = 0;
    if (!server || !ensure_winsock()) {
        return false;
    }
    memset(server, 0, sizeof(*server));
    server->socket = INVALID_SOCKET;
    server->manifest = nc_strdup(manifest ? manifest : "");
    if (payload_size > 0) {
        server->payload = (unsigned char *)malloc(payload_size);
        if (server->payload) {
            memcpy(server->payload, payload, payload_size);
            server->payload_size = payload_size;
        }
    }
    if (!fill_sockaddr("127.0.0.1", desired_port, AF_INET, &storage, &length)) {
        return false;
    }
    server->socket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (server->socket == INVALID_SOCKET) {
        return false;
    }
    {
        BOOL reuse = TRUE;
        setsockopt(server->socket, SOL_SOCKET, SO_REUSEADDR, (const char *)&reuse, sizeof(reuse));
    }
    if (bind(server->socket, (const SOCKADDR *)&storage, length) != 0 ||
        listen(server->socket, SOMAXCONN) != 0) {
        closesocket(server->socket);
        server->socket = INVALID_SOCKET;
        return false;
    }
    {
        SOCKADDR_STORAGE local;
        int local_length = sizeof(local);
        if (getsockname(server->socket, (SOCKADDR *)&local, &local_length) == 0) {
            *port = ntohs(((SOCKADDR_IN *)&local)->sin_port);
        }
    }
    server->thread = (HANDLE)_beginthreadex(NULL, 0, mini_http_thread, server, 0, NULL);
    return server->thread != NULL;
}

static void mini_http_stop(NcMiniHttpServer *server) {
    if (!server || server->socket == INVALID_SOCKET) {
        return;
    }
    InterlockedExchange(&server->stop, 1);
    closesocket(server->socket);
    server->socket = INVALID_SOCKET;
    if (server->thread) {
        WaitForSingleObject(server->thread, 3000);
        CloseHandle(server->thread);
        server->thread = NULL;
    }
    free(server->manifest);
    free(server->payload);
    server->manifest = NULL;
    server->payload = NULL;
}

static uint16_t find_free_tcp_port(int family, const char *address) {
    SOCKADDR_STORAGE storage;
    int length = 0;
    SOCKET socket_handle;
    uint16_t port = 0;
    if (!ensure_winsock() || !fill_sockaddr(address, 0, family, &storage, &length)) {
        return 0;
    }
    socket_handle = socket(family, SOCK_STREAM, IPPROTO_TCP);
    if (socket_handle == INVALID_SOCKET) {
        return 0;
    }
    if (bind(socket_handle, (const SOCKADDR *)&storage, length) == 0) {
        SOCKADDR_STORAGE local;
        int local_length = sizeof(local);
        if (getsockname(socket_handle, (SOCKADDR *)&local, &local_length) == 0) {
            port = ntohs(family == AF_INET ? ((SOCKADDR_IN *)&local)->sin_port
                                           : ((SOCKADDR_IN6 *)&local)->sin6_port);
        }
    }
    closesocket(socket_handle);
    return port;
}

static uint16_t find_free_udp_port(int family, const char *address) {
    SOCKADDR_STORAGE storage;
    int length = 0;
    SOCKET socket_handle;
    uint16_t port = 0;
    if (!ensure_winsock() || !fill_sockaddr(address, 0, family, &storage, &length)) {
        return 0;
    }
    socket_handle = socket(family, SOCK_DGRAM, IPPROTO_UDP);
    if (socket_handle == INVALID_SOCKET) {
        return 0;
    }
    if (bind(socket_handle, (const SOCKADDR *)&storage, length) == 0) {
        SOCKADDR_STORAGE local;
        int local_length = sizeof(local);
        if (getsockname(socket_handle, (SOCKADDR *)&local, &local_length) == 0) {
            port = ntohs(family == AF_INET ? ((SOCKADDR_IN *)&local)->sin_port
                                           : ((SOCKADDR_IN6 *)&local)->sin6_port);
        }
    }
    closesocket(socket_handle);
    return port;
}

static bool connect_with_timeout(const char *address, uint16_t port, int family, uint32_t timeout_ms,
                                 SOCKET *out) {
    SOCKADDR_STORAGE storage;
    int length = 0;
    SOCKET socket_handle;
    u_long nonblocking = 1;
    fd_set write_set;
    fd_set error_set;
    struct timeval timeout;
    if (!ensure_winsock() || !fill_sockaddr(address, port, family, &storage, &length)) {
        return false;
    }
    socket_handle = socket(family, SOCK_STREAM, IPPROTO_TCP);
    if (socket_handle == INVALID_SOCKET) {
        return false;
    }
    ioctlsocket(socket_handle, FIONBIO, &nonblocking);
    if (connect(socket_handle, (const SOCKADDR *)&storage, length) != 0) {
        int error = WSAGetLastError();
        if (error != WSAEWOULDBLOCK && error != WSAEINPROGRESS) {
            closesocket(socket_handle);
            return false;
        }
    }
    FD_ZERO(&write_set);
    FD_ZERO(&error_set);
    FD_SET(socket_handle, &write_set);
    FD_SET(socket_handle, &error_set);
    timeout.tv_sec = (long)(timeout_ms / 1000);
    timeout.tv_usec = (long)((timeout_ms % 1000) * 1000);
    if (select(0, NULL, &write_set, &error_set, &timeout) <= 0) {
        closesocket(socket_handle);
        return false;
    }
    {
        int error = 0;
        int error_length = sizeof(error);
        if (getsockopt(socket_handle, SOL_SOCKET, SO_ERROR, (char *)&error, &error_length) != 0 ||
            error != 0) {
            closesocket(socket_handle);
            return false;
        }
    }
    nonblocking = 0;
    ioctlsocket(socket_handle, FIONBIO, &nonblocking);
    *out = socket_handle;
    return true;
}

static bool set_socket_timeout(SOCKET socket_handle, uint32_t timeout_ms) {
    DWORD timeout = timeout_ms;
    return setsockopt(socket_handle, SOL_SOCKET, SO_RCVTIMEO, (const char *)&timeout, sizeof(timeout)) == 0 &&
           setsockopt(socket_handle, SOL_SOCKET, SO_SNDTIMEO, (const char *)&timeout, sizeof(timeout)) == 0;
}

static bool measure_echo_once(const char *address, uint16_t port, int family, uint32_t timeout_ms,
                              uint64_t *rtt_ms) {
    SOCKET socket_handle;
    char buffer[64];
    uint64_t start;
    int sent;
    int read;
    if (!connect_with_timeout(address, port, family, timeout_ms, &socket_handle)) {
        return false;
    }
    if (!set_socket_timeout(socket_handle, timeout_ms)) {
        closesocket(socket_handle);
        return false;
    }
    memset(buffer, 0x5A, sizeof(buffer));
    start = nc_monotonic_ns();
    sent = send(socket_handle, buffer, (int)sizeof(buffer), 0);
    if (sent != (int)sizeof(buffer)) {
        closesocket(socket_handle);
        return false;
    }
    read = recv(socket_handle, buffer, sizeof(buffer), 0);
    if (read <= 0) {
        closesocket(socket_handle);
        return false;
    }
    *rtt_ms = (nc_monotonic_ns() - start) / 1000000ull;
    closesocket(socket_handle);
    return true;
}

static int compare_u64(const void *left, const void *right) {
    uint64_t a = *(const uint64_t *)left;
    uint64_t b = *(const uint64_t *)right;
    return a < b ? -1 : (a > b ? 1 : 0);
}

static bool measure_echo(const char *address, uint16_t port, int family, int attempts,
                         uint32_t timeout_ms, uint64_t *median) {
    uint64_t values[16];
    int count = 0;
    int index;
    if (attempts > 16) {
        attempts = 16;
    }
    for (index = 0; index < attempts; index++) {
        uint64_t value = 0;
        if (measure_echo_once(address, port, family, timeout_ms, &value)) {
            values[count++] = value;
        }
    }
    if (count == 0) {
        return false;
    }
    qsort(values, (size_t)count, sizeof(uint64_t), compare_u64);
    *median = values[count / 2];
    return true;
}

static bool measure_transfer(const char *address, uint16_t port, int family, size_t bytes,
                             uint32_t timeout_ms, uint64_t *elapsed_ms) {
    SOCKET socket_handle;
    unsigned char *buffer;
    size_t sent = 0;
    size_t received = 0;
    uint64_t start;
    if (!connect_with_timeout(address, port, family, timeout_ms, &socket_handle)) {
        return false;
    }
    if (!set_socket_timeout(socket_handle, timeout_ms)) {
        closesocket(socket_handle);
        return false;
    }
    buffer = (unsigned char *)malloc(bytes);
    if (!buffer) {
        closesocket(socket_handle);
        return false;
    }
    memset(buffer, 0x33, bytes);
    start = nc_monotonic_ns();
    while (sent < bytes) {
        int result = send(socket_handle, (const char *)buffer + sent, (int)(bytes - sent), 0);
        if (result <= 0) {
            free(buffer);
            closesocket(socket_handle);
            return false;
        }
        sent += (size_t)result;
    }
    while (received < bytes) {
        int result = recv(socket_handle, (char *)buffer + received, (int)(bytes - received), 0);
        if (result <= 0) {
            free(buffer);
            closesocket(socket_handle);
            return false;
        }
        received += (size_t)result;
    }
    *elapsed_ms = (nc_monotonic_ns() - start) / 1000000ull;
    free(buffer);
    closesocket(socket_handle);
    return true;
}

static bool measure_udp_echo(const char *address, uint16_t port, int family, int attempts,
                            uint32_t timeout_ms, uint64_t *median) {
    uint64_t values[16];
    int count = 0;
    int index;
    for (index = 0; index < attempts && index < 16; index++) {
        SOCKADDR_STORAGE storage;
        int length = 0;
        SOCKET socket_handle;
        char buffer[64];
        DWORD timeout = timeout_ms;
        uint64_t start;
        int read;
        int remote_length = sizeof(storage);
        if (!fill_sockaddr(address, port, family, &storage, &length)) {
            return false;
        }
        socket_handle = socket(family, SOCK_DGRAM, IPPROTO_UDP);
        if (socket_handle == INVALID_SOCKET) {
            return false;
        }
        {
            SOCKADDR_STORAGE local;
            int local_length = 0;
            if (fill_sockaddr(address, 0, family, &local, &local_length)) {
                bind(socket_handle, (const SOCKADDR *)&local, local_length);
            }
        }
        setsockopt(socket_handle, SOL_SOCKET, SO_RCVTIMEO, (const char *)&timeout, sizeof(timeout));
        memset(buffer, 0x77, sizeof(buffer));
        start = nc_monotonic_ns();
        if (sendto(socket_handle, buffer, sizeof(buffer), 0, (const SOCKADDR *)&storage, length) <= 0) {
            closesocket(socket_handle);
            continue;
        }
        read = recvfrom(socket_handle, buffer, sizeof(buffer), 0, (SOCKADDR *)&storage, &remote_length);
        if (read > 0) {
            values[count++] = (nc_monotonic_ns() - start) / 1000000ull;
        }
        closesocket(socket_handle);
    }
    if (count == 0) {
        return false;
    }
    qsort(values, (size_t)count, sizeof(uint64_t), compare_u64);
    *median = values[count / 2];
    return true;
}

static bool measure_connect(const char *address, uint16_t port, int family, uint32_t timeout_ms,
                            uint64_t *elapsed_ms) {
    SOCKET socket_handle;
    uint64_t start = nc_monotonic_ns();
    if (!connect_with_timeout(address, port, family, timeout_ms, &socket_handle)) {
        return false;
    }
    *elapsed_ms = (nc_monotonic_ns() - start) / 1000000ull;
    closesocket(socket_handle);
    return true;
}

static bool measure_connect_best(const char *address, uint16_t port, int family, int attempts,
                                 uint32_t timeout_ms, uint64_t *best) {
    uint64_t minimum = UINT64_MAX;
    int index;
    for (index = 0; index < attempts; index++) {
        uint64_t value = 0;
        if (measure_connect(address, port, family, timeout_ms, &value) && value < minimum) {
            minimum = value;
        }
    }
    if (minimum == UINT64_MAX) {
        return false;
    }
    *best = minimum;
    return true;
}

typedef struct {
    NcEngineEvents *events;
    NcNetworkConditioner *conditioner;
} NcTestEngine;

static NcTestEngine make_engine(void) {
    NcTestEngine engine;
    engine.events = nc_engine_events_new(NULL, 0);
    engine.conditioner = engine.events ? nc_conditioner_new(engine.events) : NULL;
    return engine;
}

static void destroy_engine(NcTestEngine *engine) {
    if (!engine) {
        return;
    }
    if (engine->conditioner) {
        nc_conditioner_stop(engine->conditioner);
        nc_conditioner_free(engine->conditioner);
    }
    if (engine->events) {
        nc_engine_events_free(engine->events);
    }
    memset(engine, 0, sizeof(*engine));
}

static void make_profile(NcProfile *profile, const char *name, NcDirectionProfile uplink,
                         NcDirectionProfile downlink, NcConditionProtocol protocol) {
    memset(profile, 0, sizeof(*profile));
    profile->id = nc_strdup("");
    profile->name = nc_strdup(name);
    profile->uplink = uplink;
    profile->downlink = downlink;
    profile->protocol = protocol;
}

static bool start_engine(NcTestEngine *engine, const NcProfile *profile, const char *out_filter,
                         const char *in_filter, const char *const *names, size_t name_count,
                         NcErr *error) {
    return nc_conditioner_start(engine->conditioner, profile, out_filter, in_filter, 2000, names,
                                name_count, error);
}

static void current_exe_stem(char *out, size_t cap) {
    wchar_t path[32768];
    const wchar_t *file;
    char *utf8;
    if (!nc_get_module_path(path, sizeof(path) / sizeof(path[0]))) {
        snprintf(out, cap, "NetConditioner");
        return;
    }
    file = nc_path_file_name(path);
    utf8 = nc_utf8_dup(file);
    if (!utf8) {
        snprintf(out, cap, "NetConditioner");
        return;
    }
    snprintf(out, cap, "%s", utf8);
    out[cap - 1] = 0;
    {
        size_t length = strlen(out);
        if (length > 4 && _stricmp(out + length - 4, ".exe") == 0) {
            out[length - 4] = 0;
        }
    }
    free(utf8);
}

static int loopback_child(uint16_t port) {
    NcEchoServer server;
    if (!echo_server_start(&server, AF_INET, "127.0.0.1", port, NULL)) {
        return 2;
    }
    Sleep(30000);
    echo_server_stop(&server);
    return 0;
}

int nc_selftest_run(void) {
    char env_port[32];
    wchar_t log_path[32768];
    char log_path_utf8[32768];
    NcTestLog log;
    uint16_t port;
    NcEchoServer echo;
    uint64_t baseline = 0;
    char self_name[256];
    char out_filter[128];
    char in_filter[128];
    char down_filter[128];
    memset(&log, 0, sizeof(log));
    str_init(&log.lines);
    if (GetEnvironmentVariableA("NC_SELFTEST_LOOPBACK_SERVER", env_port, sizeof(env_port)) > 0) {
        int child_port = atoi(env_port);
        return loopback_child((uint16_t)child_port);
    }
    {
        wchar_t temp[32768];
        if (!nc_env_absolute_dir(L"TEMP", temp, sizeof(temp) / sizeof(temp[0]))) {
            DWORD length = GetTempPathW(sizeof(temp) / sizeof(temp[0]), temp);
            if (length == 0) {
                nc_copy_wide(temp, sizeof(temp) / sizeof(temp[0]), L".");
            }
        }
        nc_path_join(log_path, sizeof(log_path) / sizeof(log_path[0]), temp,
                     L"NetConditioner-selftest.log");
    }
    {
        char *utf8 = nc_utf8_dup(log_path);
        snprintf(log_path_utf8, sizeof(log_path_utf8), "%s", utf8 ? utf8 : "");
        free(utf8);
    }
    log_write(&log, "NetConditioner self-test, windows x86_64");
    {
        Str header;
        str_init(&header);
        str_addf(&header, "Log: %s", log_path_utf8);
        log_write(&log, str_cstr(&header));
        str_free(&header);
    }
    if (!nc_is_elevated()) {
        log_fail(&log, "需要管理员权限。请以管理员身份运行 --self-test。");
        return finish(&log, log_path);
    }
    current_exe_stem(self_name, sizeof(self_name));

    /* 1) 快捷键校验。 */
    if (nc_hotkey_is_valid(0, 0x77) && nc_hotkey_is_valid(NC_MOD_CONTROL, 0x55) &&
        !nc_hotkey_is_valid(0, 0x10)) {
        char text[64];
        nc_hotkey_format(0, 0x77, text, sizeof(text));
        if (strcmp(text, "F8") == 0) {
            log_pass(&log, "快捷键校验：支持单键 F8，修饰键本身被排除");
        } else {
            log_fail(&log, "快捷键格式化失败。");
        }
    } else {
        log_fail(&log, "快捷键校验失败。");
    }

    /* 2) 配置读写与进程名规范化。 */
    {
        NcStringList names;
        nc_string_list_init(&names);
        nc_string_list_push(&names, "chrome.exe");
        nc_string_list_push(&names, "C:\\Tools\\MyApp.EXE");
        nc_string_list_push(&names, "chrome");
        nc_string_list_push(&names, "myapp.exe");
        nc_config_sanitize_process_names(&names);
        if (names.count == 2 && strcmp(names.items[0], "chrome.exe") == 0 &&
            strcmp(names.items[1], "myapp.exe") == 0) {
            log_pass(&log, "配置读写：目标进程列表正确去重并规范化");
        } else {
            log_fail(&log, "配置读写：目标进程列表规范化失败。");
        }
        nc_string_list_free(&names);
    }

    /* 3) 版本号与 SHA256。 */
    {
        NcVersion current;
        NcVersion candidate;
        char sha[65];
        nc_sha256_hex("abc", 3, sha);
        if (nc_version_parse("1.0.1", &candidate) && nc_version_parse("1.0.0", &current) &&
            nc_version_is_newer(&current, &candidate) &&
            strcmp(sha, "BA7816BF8F01CFEA414140DE5DAE2223B00361A396177A9CB410FF61F20015AD") == 0) {
            log_pass(&log, "云更新：版本号解析/比较/SHA256 计算正确");
        } else {
            log_fail(&log, "云更新：版本号解析、比较或 SHA256 计算异常。");
        }
    }

    /* 4) latest.json 校验规则。 */
    {
        char manifest[1024];
        char sha[65];
        NcUpdateManifest parsed;
        NcVersion version;
        NcErr error;
        bool ok = true;
        nc_sha256_hex("payload", 7, sha);
        snprintf(manifest, sizeof(manifest),
                 "{\"schemaVersion\":2,\"version\":\"9.9.9\",\"publishedAt\":\"2026-01-01T00:00:00Z\","
                 "\"notes\":\"test\",\"pageUrl\":\"https://example.com/\","
                 "\"installer\":{\"url\":\"https://example.com/app.exe\",\"sha256\":\"%s\",\"size\":7}}",
                 sha);
        if (!nc_update_parse_manifest(manifest, strlen(manifest), &parsed, &version, &error)) {
            ok = false;
        }
        nc_update_manifest_free(&parsed);
        {
            const char *invalid[] = {
                "{\"schemaVersion\":1,\"version\":\"9.9.9\",\"installer\":{\"url\":\"https://e/x\",\"sha256\":\"%s\",\"size\":1}}",
                "{\"schemaVersion\":2,\"version\":\"bad\",\"installer\":{\"url\":\"https://e/x\",\"sha256\":\"%s\",\"size\":1}}",
                "{\"schemaVersion\":2,\"version\":\"9.9.9\"}",
                "{\"schemaVersion\":2,\"version\":\"9.9.9\",\"installer\":{\"url\":\"http://e/x\",\"sha256\":\"%s\",\"size\":1}}",
                "{\"schemaVersion\":2,\"version\":\"9.9.9\",\"installer\":{\"url\":\"https://e/x\",\"sha256\":\"zz\",\"size\":1}}",
                "{\"schemaVersion\":2,\"version\":\"9.9.9\",\"installer\":{\"url\":\"https://e/x\",\"sha256\":\"%s\",\"size\":0}}",
                "{\"schemaVersion\":2,\"version\":\"9.9.9\",\"installer\":{\"url\":\"https://e/x\",\"sha256\":\"%s\",\"size\":1},\"pageUrl\":\"not a url\"}"
            };
            size_t index;
            for (index = 0; index < sizeof(invalid) / sizeof(invalid[0]); index++) {
                char buffer[1200];
                snprintf(buffer, sizeof(buffer), invalid[index], sha);
                if (nc_update_parse_manifest(buffer, strlen(buffer), &parsed, &version, &error)) {
                    ok = false;
                    nc_update_manifest_free(&parsed);
                }
            }
        }
        if (ok) {
            log_pass(&log, "云更新：latest.json 校验规则正确（无效内容全部拒绝）");
        } else {
            log_fail(&log, "云更新：latest.json 校验异常。");
        }
    }

    /* 5-6) 配置默认值与清洗 + https 地址保留。 */
    {
        NcErr error;
        JsonValue *root = json_parse_cstr(
            "{\"settings\":{\"maxRunMinutes\":9999,\"maxQueueDelayMs\":-5,\"autoUpdate\":false,"
            "\"updateFeedUrl\":\"https://example.com/latest.json\"}}",
            &error);
        NcConfig config;
        size_t removed = 0;
        bool ok = false;
        nc_config_init(&config);
        if (root && nc_config_from_json(root, &config)) {
            nc_config_sanitize(&config, 1000, &removed);
            ok = config.settings.max_run_minutes == 1440 &&
                 config.settings.max_queue_delay_ms == 0 && !config.settings.auto_update &&
                 config.settings.update_feed_url &&
                 strcmp(config.settings.update_feed_url, "https://example.com/latest.json") == 0;
        }
        json_free(root);
        nc_config_free(&config);
        if (ok) {
            log_pass(&log, "云更新：配置读写与清洗正确");
            log_pass(&log, "云更新：https 更新地址可正常保存");
        } else {
            log_fail(&log, "云更新：配置读写或清洗异常。");
        }
    }

    /* 7) 本地回环：检查、下载、SHA256、自更新 dry-run。 */
    {
        const unsigned char payload[] = "self-test-payload-bytes";
        char sha[65];
        char manifest[1200];
        NcMiniHttpServer http;
        uint16_t http_port = 0;
        char feed[128];
        NcUpdateCheckResult check;
        NcVersion current = nc_version_current();
        nc_sha256_hex(payload, sizeof(payload) - 1, sha);
        http_port = find_free_tcp_port(AF_INET, "127.0.0.1");
        snprintf(manifest, sizeof(manifest),
                 "{\"schemaVersion\":2,\"version\":\"9.9.9\",\"publishedAt\":\"2026-01-01T00:00:00Z\","
                 "\"notes\":\"self-test\",\"pageUrl\":\"https://example.com/\","
                 "\"installer\":{\"url\":\"http://127.0.0.1:%u/payload.exe\",\"sha256\":\"%s\",\"size\":%u}}",
                 (unsigned)http_port, sha, (unsigned)(sizeof(payload) - 1));
        {
            if (mini_http_start(&http, manifest, payload, sizeof(payload) - 1, http_port, &http_port)) {
                snprintf(feed, sizeof(feed), "http://127.0.0.1:%u/latest.json", (unsigned)http_port);
                check = nc_update_check(feed, &current);
                if (check.kind == NC_UPDATE_CHECK_AVAILABLE &&
                    check.manifest.version && strcmp(check.manifest.version, "9.9.9") == 0) {
                    log_pass(&log, "云更新：本地回环清单解析与版本判断正确");
                    {
                        wchar_t temp[32768];
                        wchar_t destination[32768];
                        wchar_t name[128];
                        NcErr error;
                        char id[33];
                        wchar_t wide_id[64];
                        if (!nc_env_absolute_dir(L"TEMP", temp, sizeof(temp) / sizeof(temp[0]))) {
                            GetTempPathW(sizeof(temp) / sizeof(temp[0]), temp);
                        }
                        nc_new_id(id);
                        nc_write_utf8(wide_id, sizeof(wide_id) / sizeof(wide_id[0]), id);
                        swprintf(name, sizeof(name) / sizeof(name[0]), L"nc-selftest-%ls.exe", wide_id);
                        nc_path_join(destination, sizeof(destination) / sizeof(destination[0]), temp,
                                     name);
                        if (nc_update_download(&check.manifest, destination, &error)) {
                            log_pass(&log, "云更新：更新包下载 + SHA256 校验通过");
                        } else {
                            char text[1400];
                            snprintf(text, sizeof(text), "云更新：更新包下载或 SHA256 校验失败：%s",
                                     error.msg);
                            log_fail(&log, text);
                        }
                        nc_remove_file(destination);
                        {
                            NcUpdateManifest bad_manifest = check.manifest;
                            wchar_t bad_destination[32768];
                            bool rejected;
                            bad_manifest.installer.sha256 = nc_strdup(
                                "0000000000000000000000000000000000000000000000000000000000000000");
                            nc_path_join(bad_destination, sizeof(bad_destination) / sizeof(bad_destination[0]),
                                         temp, L"nc-selftest-bad.exe");
                            rejected = bad_manifest.installer.sha256 &&
                                       !nc_update_download(&bad_manifest, bad_destination, &error);
                            nc_remove_file(bad_destination);
                            free(bad_manifest.installer.sha256);
                            if (rejected) {
                                log_pass(&log, "云更新：SHA256 不匹配时拒绝更新包");
                            } else {
                                log_fail(&log, "云更新：SHA256 不匹配未被拒绝。");
                            }
                        }
                    }
                } else {
                    log_fail(&log, "云更新：本地回环检查异常（版本或结果不符）。");
                }
                nc_update_check_result_free(&check);
                {
                    NcUpdateCheckResult bad = nc_update_check(feed, &current);
                    nc_update_check_result_free(&bad);
                }
                mini_http_stop(&http);
            } else {
                log_skip(&log, "云更新：本地回环测试服务无法启动。");
            }
        }
    }
    {
        wchar_t temp[32768];
        wchar_t dummy[32768];
        NcErr error;
        if (!nc_env_absolute_dir(L"TEMP", temp, sizeof(temp) / sizeof(temp[0]))) {
            GetTempPathW(sizeof(temp) / sizeof(temp[0]), temp);
        }
        nc_path_join(dummy, sizeof(dummy) / sizeof(dummy[0]), temp, L"nc-selftest-dry-run.exe");
        if (nc_write_file(dummy, "x", 1, &error)) {
            SetEnvironmentVariableA("NC_UPDATE_DRY_RUN", "1");
            if (nc_update_apply_self_update(dummy)) {
                wchar_t directory[32768];
                wchar_t log_file[32768];
                char *log_text = NULL;
                if (nc_update_directory(directory, sizeof(directory) / sizeof(directory[0])) &&
                    nc_path_join(log_file, sizeof(log_file) / sizeof(log_file[0]), directory,
                                 L"update.log")) {
                    void *data = NULL;
                    size_t size = 0;
                    NcErr read_error;
                    if (nc_read_file(log_file, 1024 * 1024, &data, &size, &read_error)) {
                        log_text = (char *)data;
                    }
                }
                if (log_text && strstr(log_text, "DRY RUN 更新包")) {
                    log_pass(&log, "云更新：自更新 dry-run 命令生成正确");
                } else {
                    log_fail(&log, "云更新：自更新 dry-run 命令生成失败。");
                }
                free(log_text);
            } else {
                log_fail(&log, "云更新：自更新 dry-run 命令生成失败。");
            }
            SetEnvironmentVariableA("NC_UPDATE_DRY_RUN", NULL);
            nc_remove_file(dummy);
        }
    }

    /* 本地回环基线。 */
    port = find_free_tcp_port(AF_INET, "127.0.0.1");
    {
        char text[128];
        snprintf(text, sizeof(text), "测试端口：127.0.0.1:%u", (unsigned)port);
        log_write(&log, text);
    }
    if (!echo_server_start(&echo, AF_INET, "127.0.0.1", port, NULL)) {
        log_fail(&log, "本地回环基线连接失败，无法继续。");
        return finish(&log, log_path);
    }
    if (!measure_echo("127.0.0.1", port, AF_INET, 3, 3000, &baseline)) {
        log_fail(&log, "本地回环基线连接失败，无法继续。");
        echo_server_stop(&echo);
        return finish(&log, log_path);
    }
    {
        char text[128];
        snprintf(text, sizeof(text), "基线 RTT 中位数：%llu ms", (unsigned long long)baseline);
        log_write(&log, text);
    }

    /* 8) 生产过滤器必须对回环 fail closed。 */
    {
        wchar_t current[32768];
        wchar_t command[32768];
        STARTUPINFOW startup;
        PROCESS_INFORMATION process;
        uint16_t loopback_port = find_free_tcp_port(AF_INET, "127.0.0.1");
        char port_text[32];
        snprintf(port_text, sizeof(port_text), "%u", (unsigned)loopback_port);
        SetEnvironmentVariableA("NC_SELFTEST_LOOPBACK_SERVER", port_text);
        if (nc_get_module_path(current, sizeof(current) / sizeof(current[0]))) {
            NcTestEngine engine;
            NcProfile profile;
            NcErr error;
            memset(&startup, 0, sizeof(startup));
            startup.cb = sizeof(startup);
            memset(&process, 0, sizeof(process));
            swprintf(command, sizeof(command) / sizeof(command[0]), L"\"%ls\" --self-test", current);
            if (CreateProcessW(current, command, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL,
                               &startup, &process)) {
                NcDirectionProfile uplink;
                Sleep(300);
                engine = make_engine();
                memset(&uplink, 0, sizeof(uplink));
                uplink.delay_ms = 300;
                make_profile(&profile, "self-test-production-loopback", uplink,
                             (NcDirectionProfile){0}, NC_PROTOCOL_TCP);
                if (start_engine(&engine, &profile, NULL, NULL, NULL, 0, &error)) {
                    uint64_t value = 0;
                    Sleep(200);
                    if (measure_echo("127.0.0.1", loopback_port, AF_INET, 3, 3000, &value)) {
                        if (value <= baseline + 150) {
                            char text[256];
                            snprintf(text, sizeof(text),
                                     "生产过滤器排除回环：独立客户端/服务器 RTT %llu ms",
                                     (unsigned long long)value);
                            log_pass(&log, text);
                        } else {
                            char text[256];
                            snprintf(text, sizeof(text),
                                     "生产过滤器错误处理了回环响应：RTT %llu ms",
                                     (unsigned long long)value);
                            log_fail(&log, text);
                        }
                    } else {
                        log_fail(&log, "生产过滤器回环测试连接失败。");
                    }
                    nc_conditioner_stop(engine.conditioner);
                } else {
                    char text[512];
                    snprintf(text, sizeof(text), "生产过滤器回环测试启动失败：%s", error.msg);
                    log_fail(&log, text);
                }
                destroy_engine(&engine);
                nc_config_free_profile(&profile);
                TerminateProcess(process.hProcess, 1);
                WaitForSingleObject(process.hProcess, 3000);
                CloseHandle(process.hThread);
                CloseHandle(process.hProcess);
            } else {
                log_skip(&log, "无法启动独立回环服务器进程。");
            }
        }
        SetEnvironmentVariableA("NC_SELFTEST_LOOPBACK_SERVER", NULL);
    }

    snprintf(out_filter, sizeof(out_filter), "outbound and tcp and tcp.DstPort == %u", (unsigned)port);
    snprintf(in_filter, sizeof(in_filter), "inbound and tcp and tcp.SrcPort == %u", (unsigned)port);
    snprintf(down_filter, sizeof(down_filter), "tcp and tcp.SrcPort == %u", (unsigned)port);

    /* 9) 上行/下行延时与统计。 */
    {
        NcTestEngine engine = make_engine();
        NcProfile profile;
        NcDirectionProfile uplink;
        NcErr error;
        memset(&uplink, 0, sizeof(uplink));
        uplink.delay_ms = 300;
        make_profile(&profile, "self-test-delay", uplink, (NcDirectionProfile){0}, NC_PROTOCOL_TCP);
        if (!start_engine(&engine, &profile, out_filter, in_filter, NULL, 0, &error)) {
            char text[512];
            snprintf(text, sizeof(text), "上行延时测试启动失败：%s", error.msg);
            log_fail(&log, text);
        } else {
            uint64_t delayed = 0;
            Sleep(200);
            if (measure_echo("127.0.0.1", port, AF_INET, 3, 5000, &delayed)) {
                if (delayed >= baseline + 200) {
                    char text[256];
                    snprintf(text, sizeof(text), "上行 300ms 延时生效：%llu ms -> %llu ms",
                             (unsigned long long)baseline, (unsigned long long)delayed);
                    log_pass(&log, text);
                } else {
                    char text[256];
                    snprintf(text, sizeof(text), "上行延时未生效：%llu ms -> %llu ms",
                             (unsigned long long)baseline, (unsigned long long)delayed);
                    log_fail(&log, text);
                }
            } else {
                log_fail(&log, "延时测试连接失败。");
            }
            {
                NcStatsSnapshot stats;
                if (nc_conditioner_uplink_stats(engine.conditioner, &stats) && stats.sent > 0) {
                    char text[128];
                    snprintf(text, sizeof(text), "上行发送计数：%llu",
                             (unsigned long long)stats.sent);
                    log_pass(&log, text);
                } else {
                    log_fail(&log, "上行发送计数为 0，WinDivert 可能没有捕获回环流量。");
                }
            }
            nc_conditioner_stop(engine.conditioner);
        }
        destroy_engine(&engine);
        nc_config_free_profile(&profile);
    }
    {
        NcTestEngine engine = make_engine();
        NcProfile profile;
        NcDirectionProfile downlink;
        NcErr error;
        memset(&downlink, 0, sizeof(downlink));
        downlink.delay_ms = 300;
        make_profile(&profile, "self-test-down-delay", (NcDirectionProfile){0}, downlink,
                     NC_PROTOCOL_TCP);
        if (!start_engine(&engine, &profile, out_filter, down_filter, NULL, 0, &error)) {
            char text[512];
            snprintf(text, sizeof(text), "下行延时测试启动失败：%s", error.msg);
            log_fail(&log, text);
        } else {
            uint64_t delayed = 0;
            Sleep(200);
            if (measure_echo("127.0.0.1", port, AF_INET, 3, 5000, &delayed)) {
                if (delayed >= baseline + 200) {
                    char text[256];
                    snprintf(text, sizeof(text), "下行 300ms 延时生效：%llu ms -> %llu ms",
                             (unsigned long long)baseline, (unsigned long long)delayed);
                    log_pass(&log, text);
                } else {
                    char text[256];
                    snprintf(text, sizeof(text), "下行延时未生效：%llu ms -> %llu ms",
                             (unsigned long long)baseline, (unsigned long long)delayed);
                    log_fail(&log, text);
                }
            } else {
                log_fail(&log, "下行延时测试连接失败。");
            }
            {
                NcStatsSnapshot stats;
                if (nc_conditioner_downlink_stats(engine.conditioner, &stats) && stats.sent > 0) {
                    char text[128];
                    snprintf(text, sizeof(text), "下行发送计数：%llu",
                             (unsigned long long)stats.sent);
                    log_pass(&log, text);
                } else {
                    log_fail(&log, "下行发送计数为 0。");
                }
            }
            nc_conditioner_stop(engine.conditioner);
        }
        destroy_engine(&engine);
        nc_config_free_profile(&profile);
    }

    /* 10) 进程过滤（命中/未命中/下行/IPv6/UDP）。 */
    {
        const char *match_names[1];
        match_names[0] = self_name;
        {
            NcTestEngine engine = make_engine();
            NcProfile profile;
            NcDirectionProfile uplink;
            NcErr error;
            memset(&uplink, 0, sizeof(uplink));
            uplink.delay_ms = 300;
            make_profile(&profile, "self-test-process-match", uplink, (NcDirectionProfile){0},
                         NC_PROTOCOL_TCP);
            if (!start_engine(&engine, &profile, out_filter, in_filter, match_names, 1, &error)) {
                char text[512];
                snprintf(text, sizeof(text), "进程过滤（命中）测试启动失败：%s", error.msg);
                log_fail(&log, text);
            } else {
                uint64_t delayed = 0;
                Sleep(200);
                if (measure_echo("127.0.0.1", port, AF_INET, 3, 5000, &delayed)) {
                    if (delayed >= baseline + 200) {
                        char text[256];
                        snprintf(text, sizeof(text), "进程过滤生效：%s 的 RTT %llu ms -> %llu ms",
                                 self_name, (unsigned long long)baseline,
                                 (unsigned long long)delayed);
                        log_pass(&log, text);
                    } else {
                        char text[256];
                        snprintf(text, sizeof(text), "进程过滤未生效：RTT 仅 %llu ms",
                                 (unsigned long long)delayed);
                        log_fail(&log, text);
                    }
                } else {
                    log_fail(&log, "进程过滤（命中）测试连接失败。");
                }
                nc_conditioner_stop(engine.conditioner);
            }
            destroy_engine(&engine);
            nc_config_free_profile(&profile);
        }
        {
            const char *miss_names[1];
            NcTestEngine engine = make_engine();
            NcProfile profile;
            NcDirectionProfile uplink;
            NcErr error;
            miss_names[0] = "netconditioner-not-selected.exe";
            memset(&uplink, 0, sizeof(uplink));
            uplink.delay_ms = 300;
            make_profile(&profile, "self-test-process-miss", uplink, (NcDirectionProfile){0},
                         NC_PROTOCOL_TCP);
            if (!start_engine(&engine, &profile, out_filter, in_filter, miss_names, 1, &error)) {
                char text[512];
                snprintf(text, sizeof(text), "进程过滤（未命中）测试启动失败：%s", error.msg);
                log_fail(&log, text);
            } else {
                uint64_t value = 0;
                Sleep(200);
                if (measure_echo("127.0.0.1", port, AF_INET, 3, 3000, &value)) {
                    if (value <= baseline + 150) {
                        char text[128];
                        snprintf(text, sizeof(text), "未选中的进程直通：RTT %llu ms",
                                 (unsigned long long)value);
                        log_pass(&log, text);
                    } else {
                        char text[128];
                        snprintf(text, sizeof(text), "未选中的进程被错误限速：RTT %llu ms",
                                 (unsigned long long)value);
                        log_fail(&log, text);
                    }
                } else {
                    log_fail(&log, "进程过滤（未命中）测试连接失败。");
                }
                nc_conditioner_stop(engine.conditioner);
            }
            destroy_engine(&engine);
            nc_config_free_profile(&profile);
        }
        {
            NcTestEngine engine = make_engine();
            NcProfile profile;
            NcDirectionProfile downlink;
            NcErr error;
            memset(&downlink, 0, sizeof(downlink));
            downlink.delay_ms = 300;
            make_profile(&profile, "self-test-process-down", (NcDirectionProfile){0}, downlink,
                         NC_PROTOCOL_TCP);
            if (!start_engine(&engine, &profile, out_filter, down_filter, match_names, 1, &error)) {
                char text[512];
                snprintf(text, sizeof(text), "下行进程过滤测试启动失败：%s", error.msg);
                log_fail(&log, text);
            } else {
                uint64_t delayed = 0;
                Sleep(200);
                if (measure_echo("127.0.0.1", port, AF_INET, 3, 5000, &delayed) &&
                    delayed >= baseline + 200) {
                    char text[256];
                    snprintf(text, sizeof(text), "下行进程过滤生效：%llu ms -> %llu ms",
                             (unsigned long long)baseline, (unsigned long long)delayed);
                    log_pass(&log, text);
                } else {
                    log_fail(&log, "下行进程过滤未生效。");
                }
                nc_conditioner_stop(engine.conditioner);
            }
            destroy_engine(&engine);
            nc_config_free_profile(&profile);
        }
        {
            uint16_t port6 = find_free_tcp_port(AF_INET6, "::1");
            NcEchoServer server6;
            if (port6 && echo_server_start(&server6, AF_INET6, "::1", port6, NULL)) {
                char out6[128];
                char in6[128];
                NcTestEngine engine = make_engine();
                NcProfile profile;
                NcDirectionProfile uplink;
                NcErr error;
                snprintf(out6, sizeof(out6), "outbound and tcp and tcp.DstPort == %u",
                         (unsigned)port6);
                snprintf(in6, sizeof(in6), "tcp and tcp.SrcPort == %u", (unsigned)port6);
                memset(&uplink, 0, sizeof(uplink));
                uplink.delay_ms = 300;
                make_profile(&profile, "self-test-process-ipv6", uplink, (NcDirectionProfile){0},
                             NC_PROTOCOL_TCP);
                if (!start_engine(&engine, &profile, out6, in6, match_names, 1, &error)) {
                    log_fail(&log, "IPv6 进程过滤测试启动失败。");
                } else {
                    uint64_t value = 0;
                    Sleep(200);
                    if (!measure_echo("::1", port6, AF_INET6, 3, 5000, &value)) {
                        log_skip(&log, "IPv6 回环测试连接失败，跳过。");
                    } else if (value >= 200) {
                        char text[128];
                        snprintf(text, sizeof(text), "IPv6 进程过滤生效：RTT %llu ms",
                                 (unsigned long long)value);
                        log_pass(&log, text);
                    } else {
                        char text[128];
                        snprintf(text, sizeof(text), "IPv6 进程过滤未生效：RTT 仅 %llu ms",
                                 (unsigned long long)value);
                        log_fail(&log, text);
                    }
                    nc_conditioner_stop(engine.conditioner);
                }
                destroy_engine(&engine);
                nc_config_free_profile(&profile);
                echo_server_stop(&server6);
            } else {
                log_skip(&log, "IPv6 回环不可用，跳过。");
            }
        }
        {
            uint16_t udp_port = find_free_udp_port(AF_INET, "127.0.0.1");
            NcUdpEchoServer udp;
            char udp_out[128];
            char udp_in[128];
            if (udp_port && udp_echo_start(&udp, AF_INET, "127.0.0.1", udp_port, NULL)) {
                NcTestEngine engine = make_engine();
                NcProfile profile;
                NcDirectionProfile uplink;
                NcErr error;
                snprintf(udp_out, sizeof(udp_out), "outbound and udp and udp.DstPort == %u",
                         (unsigned)udp_port);
                snprintf(udp_in, sizeof(udp_in), "udp and udp.SrcPort == %u", (unsigned)udp_port);
                memset(&uplink, 0, sizeof(uplink));
                uplink.delay_ms = 300;
                make_profile(&profile, "self-test-process-udp", uplink, (NcDirectionProfile){0},
                             NC_PROTOCOL_UDP);
                if (!start_engine(&engine, &profile, udp_out, udp_in, match_names, 1, &error)) {
                    log_fail(&log, "UDP 进程过滤测试启动失败。");
                } else {
                    uint64_t value = 0;
                    Sleep(200);
                    if (!measure_udp_echo("127.0.0.1", udp_port, AF_INET, 3, 5000, &value)) {
                        log_fail(&log, "UDP 进程过滤测试收发失败。");
                    } else if (value >= 200) {
                        char text[128];
                        snprintf(text, sizeof(text), "UDP 进程过滤生效：RTT %llu ms",
                                 (unsigned long long)value);
                        log_pass(&log, text);
                    } else {
                        char text[128];
                        snprintf(text, sizeof(text), "UDP 进程过滤未生效：RTT 仅 %llu ms",
                                 (unsigned long long)value);
                        log_fail(&log, text);
                    }
                    nc_conditioner_stop(engine.conditioner);
                }
                destroy_engine(&engine);
                nc_config_free_profile(&profile);
                udp_echo_stop(&udp);
            } else {
                log_skip(&log, "UDP 回环不可用，跳过。");
            }
        }
    }

    /* 11) 带宽、突发、100% 丢包、恢复。 */
    {
        NcTestEngine engine = make_engine();
        NcProfile profile;
        NcDirectionProfile uplink;
        NcErr error;
        memset(&uplink, 0, sizeof(uplink));
        uplink.bandwidth_kbps = 64;
        make_profile(&profile, "self-test-bandwidth", uplink, (NcDirectionProfile){0},
                     NC_PROTOCOL_TCP);
        if (!start_engine(&engine, &profile, out_filter, in_filter, NULL, 0, &error)) {
            char text[512];
            snprintf(text, sizeof(text), "带宽测试启动失败：%s", error.msg);
            log_fail(&log, text);
        } else {
            uint64_t elapsed = 0;
            Sleep(200);
            if (measure_transfer("127.0.0.1", port, AF_INET, 8192, 10000, &elapsed) &&
                elapsed >= 600) {
                char text[128];
                snprintf(text, sizeof(text), "上行 64 kbps 限速生效：8KB 用时 %llu ms",
                         (unsigned long long)elapsed);
                log_pass(&log, text);
            } else {
                log_fail(&log, "上行带宽未生效。");
            }
            nc_conditioner_stop(engine.conditioner);
        }
        destroy_engine(&engine);
        nc_config_free_profile(&profile);
    }
    {
        NcTestEngine engine = make_engine();
        NcProfile profile;
        NcDirectionProfile uplink;
        NcErr error;
        memset(&uplink, 0, sizeof(uplink));
        uplink.delay_ms = 10;
        make_profile(&profile, "self-test-burst", uplink, (NcDirectionProfile){0}, NC_PROTOCOL_TCP);
        if (!start_engine(&engine, &profile, out_filter, in_filter, NULL, 0, &error)) {
            char text[512];
            snprintf(text, sizeof(text), "突发测试启动失败：%s", error.msg);
            log_fail(&log, text);
        } else {
            uint64_t elapsed = 0;
            Sleep(200);
            if (measure_transfer("127.0.0.1", port, AF_INET, 64 * 1024, 15000, &elapsed)) {
                char text[128];
                snprintf(text, sizeof(text), "64KB 突发传输完成：%llu ms",
                         (unsigned long long)elapsed);
                log_pass(&log, text);
            } else {
                log_fail(&log, "64KB 突发传输失败或超时。");
            }
            if (measure_transfer("127.0.0.1", port, AF_INET, 1024 * 1024, 20000, &elapsed)) {
                char text[128];
                snprintf(text, sizeof(text), "1MB 突发传输完成：%llu ms",
                         (unsigned long long)elapsed);
                log_pass(&log, text);
            } else {
                log_fail(&log, "1MB 突发传输失败或超时。");
            }
            {
                NcStatsSnapshot stats;
                if (nc_conditioner_uplink_stats(engine.conditioner, &stats) && stats.dropped == 0) {
                    log_pass(&log, "突发测试无额外丢包");
                } else {
                    log_fail(&log, "突发测试发生额外丢包。");
                }
            }
            nc_conditioner_stop(engine.conditioner);
        }
        destroy_engine(&engine);
        nc_config_free_profile(&profile);
    }
    {
        NcTestEngine engine = make_engine();
        NcProfile profile;
        NcDirectionProfile uplink;
        NcErr error;
        memset(&uplink, 0, sizeof(uplink));
        uplink.random_loss_percent = 100;
        make_profile(&profile, "self-test-loss", uplink, (NcDirectionProfile){0}, NC_PROTOCOL_TCP);
        if (!start_engine(&engine, &profile, out_filter, in_filter, NULL, 0, &error)) {
            char text[512];
            snprintf(text, sizeof(text), "丢包测试启动失败：%s", error.msg);
            log_fail(&log, text);
        } else {
            SOCKET socket_handle;
            Sleep(200);
            if (connect_with_timeout("127.0.0.1", port, AF_INET, 1500, &socket_handle)) {
                closesocket(socket_handle);
                log_fail(&log, "上行 100% 丢包：连接意外成功");
            } else {
                log_pass(&log, "上行 100% 丢包：连接按预期超时");
            }
            {
                NcStatsSnapshot stats;
                if (nc_conditioner_uplink_stats(engine.conditioner, &stats) && stats.dropped > 0) {
                    char text[128];
                    snprintf(text, sizeof(text), "上行丢弃计数：%llu",
                             (unsigned long long)stats.dropped);
                    log_pass(&log, text);
                } else {
                    log_fail(&log, "上行丢弃计数为 0。");
                }
            }
            nc_conditioner_stop(engine.conditioner);
        }
        destroy_engine(&engine);
        nc_config_free_profile(&profile);
    }
    {
        uint64_t restored = 0;
        if (measure_echo("127.0.0.1", port, AF_INET, 3, 3000, &restored) &&
            restored <= baseline + 300) {
            char text[128];
            snprintf(text, sizeof(text), "恢复后 RTT：%llu ms", (unsigned long long)restored);
            log_pass(&log, text);
        } else {
            log_fail(&log, "停止后未恢复。");
        }
    }

    /* 可选：真实网卡方向验证（1.1.1.1:80）。 */
    {
        uint64_t remote_baseline = 0;
        if (measure_connect_best("1.1.1.1", 80, AF_INET, 3, 2000, &remote_baseline) &&
            remote_baseline <= 1000) {
            const char *remote_out = "outbound and tcp and tcp.DstPort == 80";
            const char *remote_in = "inbound and tcp and tcp.SrcPort == 80";
            NcTestEngine engine = make_engine();
            NcProfile profile;
            NcDirectionProfile uplink;
            NcErr error;
            memset(&uplink, 0, sizeof(uplink));
            uplink.delay_ms = 300;
            make_profile(&profile, "self-test-remote-up", uplink, (NcDirectionProfile){0},
                         NC_PROTOCOL_TCP);
            if (!start_engine(&engine, &profile, remote_out, remote_in, NULL, 0, &error)) {
                log_fail(&log, "真实网卡上行延时测试启动失败。");
            } else {
                uint64_t value = 0;
                Sleep(200);
                if (measure_connect_best("1.1.1.1", 80, AF_INET, 3, 3000, &value) &&
                    value >= remote_baseline + 200) {
                    char text[256];
                    snprintf(text, sizeof(text),
                             "真实网卡上行 300ms 延时生效：%llu ms -> %llu ms",
                             (unsigned long long)remote_baseline, (unsigned long long)value);
                    log_pass(&log, text);
                } else {
                    log_fail(&log, "真实网卡上行延时未生效。");
                }
                nc_conditioner_stop(engine.conditioner);
            }
            destroy_engine(&engine);
            nc_config_free_profile(&profile);
            {
                NcTestEngine engine2 = make_engine();
                NcProfile profile2;
                NcDirectionProfile downlink;
                NcErr error2;
                memset(&downlink, 0, sizeof(downlink));
                downlink.delay_ms = 300;
                make_profile(&profile2, "self-test-remote-down", (NcDirectionProfile){0}, downlink,
                             NC_PROTOCOL_TCP);
                if (!start_engine(&engine2, &profile2, remote_out, remote_in, NULL, 0, &error2)) {
                    log_fail(&log, "真实网卡下行延时测试启动失败。");
                } else {
                    uint64_t value = 0;
                    Sleep(200);
                    if (measure_connect_best("1.1.1.1", 80, AF_INET, 3, 3000, &value) &&
                        value >= remote_baseline + 200) {
                        char text[256];
                        snprintf(text, sizeof(text),
                                 "真实网卡下行 300ms 延时生效：%llu ms -> %llu ms",
                                 (unsigned long long)remote_baseline, (unsigned long long)value);
                        log_pass(&log, text);
                    } else {
                        log_fail(&log, "真实网卡下行延时未生效。");
                    }
                    nc_conditioner_stop(engine2.conditioner);
                }
                destroy_engine(&engine2);
                nc_config_free_profile(&profile2);
            }
        } else {
            log_skip(&log, "公网 80 端口不可达或基线过慢，跳过真实网卡方向测试。");
        }
    }

    echo_server_stop(&echo);
    return finish(&log, log_path);
}
