#include "test_harness.h"

#include "engine/flow.h"

#include <string.h>

static void write_u16(unsigned char *bytes, size_t offset, unsigned value) {
    bytes[offset] = (unsigned char)(value >> 8);
    bytes[offset + 1] = (unsigned char)(value & 0xFF);
}

static void write_u32(unsigned char *bytes, size_t offset, unsigned value) {
    bytes[offset] = (unsigned char)(value >> 24);
    bytes[offset + 1] = (unsigned char)((value >> 16) & 0xFF);
    bytes[offset + 2] = (unsigned char)((value >> 8) & 0xFF);
    bytes[offset + 3] = (unsigned char)(value & 0xFF);
}

static void make_ipv4(unsigned char *packet, size_t *length, unsigned char protocol,
                      unsigned source, unsigned destination, unsigned source_port,
                      unsigned destination_port, unsigned fragment) {
    memset(packet, 0, 64);
    packet[0] = 0x45;
    write_u16(packet, 6, fragment);
    packet[9] = protocol;
    write_u32(packet, 12, source);
    write_u32(packet, 16, destination);
    write_u16(packet, 20, source_port);
    write_u16(packet, 22, destination_port);
    *length = 28;
}

void test_flow_run(void) {
    unsigned char packet[128];
    size_t length = 0;
    NcFlowPacket flow;
    NcFlowEndpoint left;
    NcFlowEndpoint right;

    make_ipv4(packet, &length, 6, 0x0A000001u, 0x0A000002u, 1234, 80, 0);
    CHECK(nc_flow_parse(packet, length, true, &flow));
    CHECK_INT(flow.protocol, 6);
    CHECK_INT(flow.local.family, 4);
    CHECK_INT(flow.local.address_low, 0x0A000001u);
    CHECK_INT(flow.local.port, 1234);
    CHECK_INT(flow.remote.address_low, 0x0A000002u);
    CHECK_INT(flow.remote.port, 80);
    CHECK(nc_flow_parse(packet, length, false, &flow));
    CHECK_INT(flow.local.address_low, 0x0A000002u);
    CHECK_INT(flow.local.port, 80);

    make_ipv4(packet, &length, 6, 0x0A000001u, 0x0A000002u, 1234, 80, 0x0001);
    CHECK(!nc_flow_parse(packet, length, true, &flow));
    make_ipv4(packet, &length, 1, 0x0A000001u, 0x0A000002u, 0, 0, 0);
    CHECK(!nc_flow_parse(packet, length, true, &flow));
    make_ipv4(packet, &length, 17, 0x0A000001u, 0x0A000002u, 5353, 53, 0);
    CHECK(nc_flow_parse(packet, length, true, &flow));
    CHECK_INT(flow.protocol, 17);

    /* IPv6 TCP with ::ffff mapping. */
    memset(packet, 0, sizeof(packet));
    packet[0] = 0x60;
    packet[6] = 6;
    packet[7] = 64;
    packet[8] = 0;
    packet[9] = 0;
    packet[10] = 0;
    packet[11] = 0;
    packet[12] = 0;
    packet[13] = 0;
    packet[14] = 0;
    packet[15] = 0;
    packet[16] = 0;
    packet[17] = 0;
    packet[18] = 0;
    packet[19] = 0;
    packet[20] = 0;
    packet[21] = 0;
    packet[22] = 0;
    packet[23] = 0;
    packet[24] = 0;
    packet[25] = 0;
    packet[26] = 0;
    packet[27] = 0;
    packet[28] = 0;
    packet[29] = 0;
    packet[30] = 0;
    packet[31] = 0;
    packet[32] = 0;
    packet[33] = 0;
    packet[34] = 0;
    packet[35] = 0;
    packet[36] = 0;
    packet[37] = 0;
    packet[38] = 0;
    packet[39] = 0;
    write_u16(packet, 40, 1234);
    write_u16(packet, 42, 443);
    length = 44;
    CHECK(nc_flow_parse(packet, length, true, &flow));
    CHECK_INT(flow.protocol, 6);
    CHECK_INT(flow.local.port, 1234);
    CHECK_INT(flow.remote.port, 443);
    CHECK_INT(flow.local.family, 6);

    /* IPv6 hop-by-hop extension header. */
    memset(packet, 0, sizeof(packet));
    packet[0] = 0x60;
    packet[6] = 0;
    packet[7] = 64;
    packet[40] = 6;
    packet[41] = 0;
    write_u16(packet, 48, 4321);
    write_u16(packet, 50, 53);
    length = 52;
    CHECK(nc_flow_parse(packet, length, true, &flow));
    CHECK_INT(flow.protocol, 6);
    CHECK_INT(flow.local.port, 4321);

    /* IPv6 fragment with non-zero offset is not the first fragment. */
    memset(packet, 0, sizeof(packet));
    packet[0] = 0x60;
    packet[6] = 44;
    packet[7] = 64;
    packet[40] = 6;
    write_u16(packet, 42, 0x0008);
    length = 52;
    CHECK(!nc_flow_parse(packet, length, true, &flow));

    left.family = 4;
    left.address_high = 0;
    left.address_low = 1;
    left.port = 80;
    right = left;
    CHECK(nc_flow_endpoint_equal(&left, &right));
    CHECK_INT(nc_flow_endpoint_hash(&left), nc_flow_endpoint_hash(&right));
    right.port = 81;
    CHECK(!nc_flow_endpoint_equal(&left, &right));
}
