#include "test_harness.h"

#include <winsock2.h>
#include <ws2tcpip.h>

#include "core/fs.h"
#include "core/str.h"
#include "engine/process_filter.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void exe_stem(char *out, size_t cap) {
    WStr path;
    const wchar_t *file;
    char *utf8;
    wstr_init(&path);
    if (!nc_get_module_path_w(&path)) {
        wstr_free(&path);
        snprintf(out, cap, "NetConditionerTests");
        return;
    }
    file = nc_path_file_name(wstr_cstr(&path));
    utf8 = nc_utf8_dup(file);
    wstr_free(&path);
    if (!utf8) {
        snprintf(out, cap, "NetConditionerTests");
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

static NcNegativeKey make_neg_key(uint16_t port) {
    NcNegativeKey key;
    memset(&key, 0, sizeof(key));
    key.protocol = 17;
    key.local.family = 4;
    key.local.address_low = 0x7F000001u;
    key.local.port = port;
    key.has_remote = false;
    return key;
}

static void test_negative_cache(void) {
    NcNegativeCache *cache = nc_negative_cache_new(16);
    uint16_t port;
    CHECK(cache != NULL);
    /* Saturate well past capacity (16) to prove bounded probing never infinite-loops. */
    for (port = 1; port <= 128; port++) {
        NcNegativeKey k = make_neg_key(port);
        nc_negative_cache_insert(cache, &k, 1000ull + port, 5000ull);
    }
    /* Most recently inserted keys remain cached before expiry. */
    {
        NcNegativeKey k = make_neg_key(128);
        CHECK(nc_negative_cache_contains(cache, &k, 2000ull));
        /* After expiry, lookup converts slot to tombstone without breaking probe chains. */
        CHECK(!nc_negative_cache_contains(cache, &k, 10000ull));
    }
    /* Tombstone chain preservation test on a fresh small table. */
    nc_negative_cache_free(cache);
    cache = nc_negative_cache_new(16);
    CHECK(cache != NULL);
    {
        NcNegativeKey a = make_neg_key(1001);
        NcNegativeKey b = make_neg_key(1002);
        NcNegativeKey c = make_neg_key(1003);
        nc_negative_cache_insert(cache, &a, 100ull, 50ull);   /* expires at 150 */
        nc_negative_cache_insert(cache, &b, 100ull, 500ull);  /* expires at 600 */
        nc_negative_cache_insert(cache, &c, 100ull, 500ull);  /* expires at 600 */
        CHECK(!nc_negative_cache_contains(cache, &a, 200ull));
        CHECK(nc_negative_cache_contains(cache, &b, 200ull));
        CHECK(nc_negative_cache_contains(cache, &c, 200ull));
    }
    nc_negative_cache_free(cache);
}

void test_process_filter_run(void) {
    WSADATA data;
    char name[256];
    SOCKET socket_handle;
    SOCKADDR_IN address;
    int length = sizeof(address);
    uint16_t port;
    const char *names[1];
    NcProcessFlowFilter *filter;
    NcFlowEndpoint local;
    NcFlowEndpoint remote;
    test_negative_cache();
    if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
        return;
    }
    exe_stem(name, sizeof(name));
    socket_handle = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    CHECK(socket_handle != INVALID_SOCKET);
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(0x7F000001u);
    CHECK(bind(socket_handle, (const SOCKADDR *)&address, sizeof(address)) == 0);
    CHECK(getsockname(socket_handle, (SOCKADDR *)&address, &length) == 0);
    port = ntohs(address.sin_port);
    names[0] = name;
    filter = nc_process_filter_new(names, 1);
    CHECK(filter != NULL);
    local.family = 4;
    local.address_high = 0;
    local.address_low = 0x7F000001u;
    local.port = port;
    remote.family = 4;
    remote.address_high = 0;
    remote.address_low = 0x7F000001u;
    remote.port = 9;
    CHECK(nc_process_filter_match(filter, 17, &local, &remote));
    nc_process_filter_free(filter);
    closesocket(socket_handle);
    /* Unknown flow must wake the background thread asynchronously (well before the 2s interval). */
    {
        SOCKET late_socket;
        SOCKADDR_IN late_address;
        int late_length = sizeof(late_address);
        uint16_t late_port;
        NcFlowEndpoint late_local;
        NcProcessFlowFilter *late_filter;
        bool matched = false;
        int attempt;
        late_filter = nc_process_filter_new(names, 1);
        CHECK(late_filter != NULL);
        Sleep(150);
        late_socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        CHECK(late_socket != INVALID_SOCKET);
        memset(&late_address, 0, sizeof(late_address));
        late_address.sin_family = AF_INET;
        late_address.sin_addr.s_addr = htonl(0x7F000001u);
        CHECK(bind(late_socket, (const SOCKADDR *)&late_address, sizeof(late_address)) == 0);
        CHECK(getsockname(late_socket, (SOCKADDR *)&late_address, &late_length) == 0);
        late_port = ntohs(late_address.sin_port);
        late_local.family = 4;
        late_local.address_high = 0;
        late_local.address_low = 0x7F000001u;
        late_local.port = late_port;
        for (attempt = 0; attempt < 40; attempt++) {
            if (nc_process_filter_match(late_filter, 17, &late_local, &remote)) {
                matched = true;
                break;
            }
            Sleep(15);
        }
        CHECK(matched);
        nc_process_filter_free(late_filter);
        closesocket(late_socket);
    }
    WSACleanup();
}
