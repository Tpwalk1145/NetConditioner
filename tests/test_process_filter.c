#include "test_harness.h"

#include "../src/engine/process_filter.h"

#include <winsock2.h>
#include <ws2tcpip.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void exe_stem(char *out, size_t cap) {
    wchar_t path[32768];
    const wchar_t *file;
    char *utf8;
    if (!nc_get_module_path(path, sizeof(path) / sizeof(path[0]))) {
        snprintf(out, cap, "NetConditionerTests");
        return;
    }
    file = nc_path_file_name(path);
    utf8 = nc_utf8_dup(file);
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
    /* The filter can be created before a new UDP socket appears; an unknown flow
       must trigger an on-demand refresh. */
    {
        SOCKET late_socket;
        SOCKADDR_IN late_address;
        int late_length = sizeof(late_address);
        uint16_t late_port;
        NcFlowEndpoint late_local;
        NcProcessFlowFilter *late_filter;
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
        CHECK(nc_process_filter_match(late_filter, 17, &late_local, &remote));
        nc_process_filter_free(late_filter);
        closesocket(late_socket);
    }
    WSACleanup();
}