#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

typedef struct {
    uint8_t family; /* 4 or 6 */
    uint64_t address_high;
    uint64_t address_low;
    uint16_t port;
} NcFlowEndpoint;

typedef struct {
    uint8_t protocol; /* 6 = TCP, 17 = UDP */
    NcFlowEndpoint local;
    NcFlowEndpoint remote;
} NcFlowPacket;

bool nc_flow_parse(const unsigned char *packet, size_t length, bool outbound, NcFlowPacket *out);
bool nc_flow_endpoint_equal(const NcFlowEndpoint *left, const NcFlowEndpoint *right);
uint64_t nc_flow_endpoint_hash(const NcFlowEndpoint *endpoint);
uint64_t nc_flow_key_hash(uint8_t protocol, const NcFlowEndpoint *local,
                          const NcFlowEndpoint *remote);
