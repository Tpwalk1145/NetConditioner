//! 从 WinDivert 包缓冲区读取协议与本地端点。

/// 本地端点：地址族（4/6）、地址和大端序数值、主机字节序端口。
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash, PartialOrd, Ord)]
pub struct FlowEndpoint {
    pub family: u8,
    pub address: u128,
    pub port: u16,
}

/// 解析后的流信息：协议号、本端端点和远端端点。
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct FlowPacket {
    pub protocol: u8,
    pub local: FlowEndpoint,
    pub remote: FlowEndpoint,
}

/// 仅处理常见的 IPv4/IPv6 TCP/UDP 包；返回 (协议号, 本端端点)。
#[cfg(test)]
pub fn parse(packet: &[u8], outbound: bool) -> Option<(u8, FlowEndpoint)> {
    parse_packet(packet, outbound).map(|flow| (flow.protocol, flow.local))
}

/// 解析 TCP/UDP 五元组。`outbound` 表示调用方希望把源端还是目的端视为本端；
/// 它描述的是引擎方向，而不是 WinDivert 的 `outbound` 标志。
pub fn parse_packet(packet: &[u8], outbound: bool) -> Option<FlowPacket> {
    if packet.len() < 24 {
        return None;
    }

    let version = packet[0] >> 4;
    if version == 4 {
        let header_length = ((packet[0] & 0x0F) as usize) << 2;
        if header_length < 20 || packet.len() < header_length + 4 {
            return None;
        }

        let protocol = packet[9];
        if protocol != 6 && protocol != 17 {
            return None;
        }

        // 非首片 IP 分片没有端口信息。
        if read_u16(packet, 6) & 0x1FFF != 0 {
            return None;
        }

        let source = read_u32(packet, 12) as u128;
        let destination = read_u32(packet, 16) as u128;
        let source_port = read_u16(packet, header_length);
        let destination_port = read_u16(packet, header_length + 2);
        let (local, remote) = if outbound {
            (
                FlowEndpoint {
                    family: 4,
                    address: source,
                    port: source_port,
                },
                FlowEndpoint {
                    family: 4,
                    address: destination,
                    port: destination_port,
                },
            )
        } else {
            (
                FlowEndpoint {
                    family: 4,
                    address: destination,
                    port: destination_port,
                },
                FlowEndpoint {
                    family: 4,
                    address: source,
                    port: source_port,
                },
            )
        };
        return Some(FlowPacket {
            protocol,
            local,
            remote,
        });
    }

    if version == 6 {
        let mut offset = 40usize;
        if packet.len() < offset + 4 {
            return None;
        }

        let mut next = packet[6];
        let mut guard = 0;
        while matches!(next, 0 | 43 | 44 | 60) && guard < 8 {
            guard += 1;
            if packet.len() < offset + 8 {
                return None;
            }

            let current = next;
            next = packet[offset];
            if current == 44 {
                // 分片头：非首片没有端口信息。
                if read_u16(packet, offset + 2) & 0xFFF8 != 0 {
                    return None;
                }
                offset += 8;
            } else {
                offset += (packet[offset + 1] as usize + 1) * 8;
            }
        }

        if (next != 6 && next != 17) || packet.len() < offset + 4 {
            return None;
        }

        let source = read_u128(packet, 8);
        let destination = read_u128(packet, 24);
        let source_port = read_u16(packet, offset);
        let destination_port = read_u16(packet, offset + 2);
        let (source_family, source_address) = normalize_address(source);
        let (destination_family, destination_address) = normalize_address(destination);
        let source_endpoint = FlowEndpoint {
            family: source_family,
            address: source_address,
            port: source_port,
        };
        let destination_endpoint = FlowEndpoint {
            family: destination_family,
            address: destination_address,
            port: destination_port,
        };
        let (local, remote) = if outbound {
            (source_endpoint, destination_endpoint)
        } else {
            (destination_endpoint, source_endpoint)
        };
        return Some(FlowPacket {
            protocol: next,
            local,
            remote,
        });
    }

    None
}

/// 把 IPv4-mapped IPv6 地址（::ffff:a.b.c.d）统一成 IPv4 地址。
pub fn normalize_address(address: u128) -> (u8, u128) {
    if (address >> 32) == 0xFFFF {
        (4, (address as u32) as u128)
    } else {
        (6, address)
    }
}

fn read_u16(bytes: &[u8], offset: usize) -> u16 {
    u16::from_be_bytes([bytes[offset], bytes[offset + 1]])
}

fn read_u32(bytes: &[u8], offset: usize) -> u32 {
    u32::from_be_bytes([
        bytes[offset],
        bytes[offset + 1],
        bytes[offset + 2],
        bytes[offset + 3],
    ])
}

fn read_u128(bytes: &[u8], offset: usize) -> u128 {
    let mut value = [0u8; 16];
    value.copy_from_slice(&bytes[offset..offset + 16]);
    u128::from_be_bytes(value)
}

#[cfg(test)]
mod tests {
    use super::*;

    fn ipv4_tcp(
        source: [u8; 4],
        destination: [u8; 4],
        source_port: u16,
        destination_port: u16,
    ) -> Vec<u8> {
        let mut packet = vec![0u8; 24];
        packet[0] = 0x45;
        packet[8] = 64;
        packet[9] = 6;
        packet[12..16].copy_from_slice(&source);
        packet[16..20].copy_from_slice(&destination);
        packet[20..22].copy_from_slice(&source_port.to_be_bytes());
        packet[22..24].copy_from_slice(&destination_port.to_be_bytes());
        packet
    }

    #[test]
    fn parses_ipv4_tcp_outbound_and_inbound() {
        let packet = ipv4_tcp([127, 0, 0, 1], [127, 0, 0, 1], 40000, 8080);
        let (protocol, local) = parse(&packet, true).unwrap();
        assert_eq!(protocol, 6);
        assert_eq!(
            local,
            FlowEndpoint {
                family: 4,
                address: 0x7F000001,
                port: 40000
            }
        );

        let (_, local) = parse(&packet, false).unwrap();
        assert_eq!(
            local,
            FlowEndpoint {
                family: 4,
                address: 0x7F000001,
                port: 8080
            }
        );
    }

    #[test]
    fn parses_remote_endpoint_for_process_attribution() {
        let packet = ipv4_tcp([127, 0, 0, 1], [127, 0, 0, 1], 40000, 8080);
        let flow = parse_packet(&packet, true).unwrap();
        assert_eq!(flow.local.port, 40000);
        assert_eq!(flow.remote.port, 8080);

        let response = parse_packet(&packet, false).unwrap();
        assert_eq!(response.local.port, 8080);
        assert_eq!(response.remote.port, 40000);
    }

    #[test]
    fn rejects_non_first_fragments_and_other_protocols() {
        let mut fragment = ipv4_tcp([10, 0, 0, 1], [10, 0, 0, 2], 1000, 2000);
        fragment[6] = 0x00;
        fragment[7] = 0x10; // MF=0，offset=1 → 非首片
        assert!(parse(&fragment, true).is_none());

        let mut icmp = ipv4_tcp([10, 0, 0, 1], [10, 0, 0, 2], 1, 2);
        icmp[9] = 1;
        assert!(parse(&icmp, true).is_none());
    }

    #[test]
    fn parses_ipv6_with_extension_header() {
        let mut packet = vec![0u8; 40 + 8 + 4];
        packet[0] = 0x60;
        packet[6] = 0; // hop-by-hop
        packet[7] = 17; // UDP
        packet[40] = 17; // next header
        packet[41] = 0; // 8 字节
        packet[48..50].copy_from_slice(&7777u16.to_be_bytes());
        packet[50..52].copy_from_slice(&53u16.to_be_bytes());
        packet[8..24].copy_from_slice(&[0u8; 16]);
        let mut destination = [0u8; 16];
        destination[15] = 1;
        packet[24..40].copy_from_slice(&destination);

        let (protocol, local) = parse(&packet, true).unwrap();
        assert_eq!(protocol, 17);
        assert_eq!(local.family, 6);
        assert_eq!(local.port, 7777);
    }

    #[test]
    fn rejects_truncated_packets() {
        assert!(parse(&[0x45, 0, 0, 20], true).is_none());
        assert!(parse(&[], true).is_none());
    }

    #[test]
    fn normalizes_ipv4_mapped_addresses() {
        let mapped = 0x0000_0000_0000_0000_0000_FFFF_7F00_0001u128;
        assert_eq!(normalize_address(mapped), (4, 0x7F000001));
        assert_eq!(normalize_address(1), (6, 1));
    }
}
