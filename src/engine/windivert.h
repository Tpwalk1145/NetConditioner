#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#include "util.h"

#define NC_WINDIVERT_LAYER_NETWORK 0
#define NC_WINDIVERT_SHUTDOWN_RECV 1
#define NC_WINDIVERT_ADDRESS_SIZE 80
#define NC_WINDIVERT_ADDRESS_BUFFER 128
#define NC_MAX_PACKET_SIZE (65535 + 256)

const char *nc_windivert_payload_version(void);
bool nc_windivert_open(const char *filter, INT_PTR *handle_out, NcErr *err);
void nc_windivert_set_params(INT_PTR handle);
bool nc_windivert_recv(INT_PTR handle, void *packet, UINT *length, void *address, int *error_code);
bool nc_windivert_send(INT_PTR handle, const void *packet, UINT length, const void *address,
                       int *error_code);
void nc_windivert_close(INT_PTR handle);
bool nc_windivert_shutdown(INT_PTR handle, int how);
bool nc_windivert_parse_protocol(const void *packet, UINT length, uint8_t *protocol);
