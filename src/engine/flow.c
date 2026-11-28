#include "engine/flow.h"

#include <string.h>

static uint16_t read_u16(const unsigned char *bytes, size_t offset) {
    return (uint16_t)(((uint16_t)bytes[offset] << 8) | bytes[offset + 1]);
}

static uint32_t read_u32(const unsigned char *bytes, size_t offset) {
    return ((uint32_t)bytes[offset] << 24) | ((uint32_t)bytes[offset + 1] << 16) |
           ((uint32_t)bytes[offset + 2] << 8) | (uint32_t)bytes[offset + 3];
}

static void read_u128(const unsigned char *bytes, size_t offset, uint64_t *high, uint64_t *low) {
    size_t index;
    uint64_t first = 0;
    uint64_t second = 0;
    for (index = 0; index < 8; index++) {
        first = (first << 8) | bytes[offset + index];
        second = (second << 8) | bytes[offset + 8 + index];
    }
    *high = first;
    *low = second;
}

static void normalize_address(uint64_t high, uint64_t low, NcFlowEndpoint *endpoint) {
    if (high == 0 && (low >> 32) == 0xFFFFull) {
        endpoint->family = 4;
        endpoint->address_high = 0;
        endpoint->address_low = low & 0xFFFFFFFFull;
    } else {
        endpoint->family = 6;
        endpoint->address_high = high;
        endpoint->address_low = low;
    }
}

bool nc_flow_parse(const unsigned char *packet, size_t length, bool outbound, NcFlowPacket *out) {
    unsigned version;
    if (!packet || !out || length < 24) {
        return false;
    }
    version = packet[0] >> 4;
    if (version == 4) {
        size_t header_length = (size_t)(packet[0] & 0x0F) << 2;
        uint8_t protocol;
        uint32_t source;
        uint32_t destination;
        uint16_t source_port;
        uint16_t destination_port;
        if (header_length < 20 || length < header_length + 4) {
            return false;
        }
        protocol = packet[9];
        if (protocol != 6 && protocol != 17) {
            return false;
        }
        if ((read_u16(packet, 6) & 0x1FFF) != 0) {
            return false;
        }
        source = read_u32(packet, 12);
        destination = read_u32(packet, 16);
        source_port = read_u16(packet, header_length);
        destination_port = read_u16(packet, header_length + 2);
        out->protocol = protocol;
        if (outbound) {
            out->local.family = 4;
            out->local.address_high = 0;
            out->local.address_low = source;
            out->local.port = source_port;
            out->remote.family = 4;
            out->remote.address_high = 0;
            out->remote.address_low = destination;
            out->remote.port = destination_port;
        } else {
            out->local.family = 4;
            out->local.address_high = 0;
            out->local.address_low = destination;
            out->local.port = destination_port;
            out->remote.family = 4;
            out->remote.address_high = 0;
            out->remote.address_low = source;
            out->remote.port = source_port;
        }
        return true;
    }
    if (version == 6) {
        size_t offset = 40;
        uint8_t next = packet[6];
        int guard = 0;
        if (length < offset + 4) {
            return false;
        }
        while ((next == 0 || next == 43 || next == 44 || next == 60) && guard < 8) {
            guard++;
            if (length < offset + 8) {
                return false;
            }
            {
                uint8_t current = next;
                next = packet[offset];
                if (current == 44) {
                    if ((read_u16(packet, offset + 2) & 0xFFF8) != 0) {
                        return false;
                    }
                    offset += 8;
                } else {
                    offset += ((size_t)packet[offset + 1] + 1) * 8;
                }
            }
        }
        if ((next != 6 && next != 17) || length < offset + 4) {
            return false;
        }
        {
            uint64_t source_high;
            uint64_t source_low;
            uint64_t destination_high;
            uint64_t destination_low;
            uint16_t source_port;
            uint16_t destination_port;
            NcFlowEndpoint source_endpoint;
            NcFlowEndpoint destination_endpoint;
            read_u128(packet, 8, &source_high, &source_low);
            read_u128(packet, 24, &destination_high, &destination_low);
            source_port = read_u16(packet, offset);
            destination_port = read_u16(packet, offset + 2);
            normalize_address(source_high, source_low, &source_endpoint);
            normalize_address(destination_high, destination_low, &destination_endpoint);
            source_endpoint.port = source_port;
            destination_endpoint.port = destination_port;
            out->protocol = next;
            if (outbound) {
                out->local = source_endpoint;
                out->remote = destination_endpoint;
            } else {
                out->local = destination_endpoint;
                out->remote = source_endpoint;
            }
            return true;
        }
    }
    return false;
}

bool nc_flow_endpoint_equal(const NcFlowEndpoint *left, const NcFlowEndpoint *right) {
    if (!left || !right) {
        return false;
    }
    return left->family == right->family && left->address_high == right->address_high &&
           left->address_low == right->address_low && left->port == right->port;
}

static uint64_t hash_mix(uint64_t hash, uint64_t value) {
    int index;
    for (index = 0; index < 8; index++) {
        hash ^= (value >> (index * 8)) & 0xFF;
        hash *= 1099511628211ull;
    }
    return hash;
}

uint64_t nc_flow_endpoint_hash(const NcFlowEndpoint *endpoint) {
    uint64_t hash = 14695981039346656037ull;
    if (!endpoint) {
        return hash;
    }
    hash = hash_mix(hash, endpoint->family);
    hash = hash_mix(hash, endpoint->port);
    hash = hash_mix(hash, endpoint->address_high);
    hash = hash_mix(hash, endpoint->address_low);
    return hash;
}

uint64_t nc_flow_key_hash(uint8_t protocol, const NcFlowEndpoint *local,
                          const NcFlowEndpoint *remote) {
    uint64_t hash = 14695981039346656037ull;
    hash = hash_mix(hash, protocol);
    hash ^= nc_flow_endpoint_hash(local) * 1099511628211ull;
    if (remote) {
        hash ^= nc_flow_endpoint_hash(remote) * 14029467366897019727ull;
    }
    return hash;
}
