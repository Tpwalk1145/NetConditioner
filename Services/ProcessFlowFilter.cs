using System.Buffers.Binary;
using System.Diagnostics;
using System.Runtime.InteropServices;

namespace NetConditioner.Services;

/// <summary>本地端点：地址族（4/6）、地址和主机字节序端口。</summary>
internal readonly record struct FlowEndpoint(byte Family, UInt128 Address, ushort Port);

/// <summary>从 WinDivert 包缓冲区读取协议和本地端点。仅处理常见的 IPv4/IPv6 TCP/UDP 包。</summary>
internal static class PacketFlowReader
{
    public static unsafe bool TryRead(
        IntPtr packet, int length, bool outbound, out byte protocol, out FlowEndpoint local)
    {
        protocol = 0;
        local = default;

        if (length < 24)
            return false;

        var p = (byte*)packet;
        var version = p[0] >> 4;

        if (version == 4)
        {
            var headerLength = (p[0] & 0x0F) << 2;
            if (headerLength < 20 || length < headerLength + 4)
                return false;

            var protocolValue = p[9];
            if (protocolValue is not (6 or 17))
                return false;

            // 非首片 IP 分片没有端口信息。
            if ((ReadU16(p + 6) & 0x1FFF) != 0)
                return false;

            var src = ReadU32(p + 12);
            var dst = ReadU32(p + 16);
            var srcPort = ReadU16(p + headerLength);
            var dstPort = ReadU16(p + headerLength + 2);

            protocol = protocolValue;
            local = outbound
                ? new FlowEndpoint(4, src, srcPort)
                : new FlowEndpoint(4, dst, dstPort);
            return true;
        }

        if (version == 6)
        {
            var offset = 40;
            if (length < offset + 4)
                return false;

            var next = p[6];
            var guard = 0;
            while (next is 0 or 43 or 44 or 60 && guard++ < 8)
            {
                if (length < offset + 8)
                    return false;

                var current = next;
                next = p[offset];
                if (current == 44)
                {
                    // 分片头：非首片没有端口信息。
                    if ((ReadU16(p + offset + 2) & 0xFFF8) != 0)
                        return false;
                    offset += 8;
                }
                else
                {
                    offset += (p[offset + 1] + 1) * 8;
                }
            }

            if (next is not (6 or 17) || length < offset + 4)
                return false;

            var src = ReadU128(p + 8);
            var dst = ReadU128(p + 24);
            var srcPort = ReadU16(p + offset);
            var dstPort = ReadU16(p + offset + 2);

            protocol = next;
            var (family, address) =
                ProcessFlowFilter.NormalizeAddress(outbound ? src : dst);
            local = new FlowEndpoint(family, address, outbound ? srcPort : dstPort);
            return true;
        }

        return false;
    }

    private static unsafe ushort ReadU16(byte* p) => (ushort)((p[0] << 8) | p[1]);

    private static unsafe uint ReadU32(byte* p) =>
        ((uint)p[0] << 24) | ((uint)p[1] << 16) | ((uint)p[2] << 8) | p[3];

    private static unsafe UInt128 ReadU128(byte* p) =>
        ((UInt128)BinaryPrimitives.ReadUInt64BigEndian(new ReadOnlySpan<byte>(p, 8)) << 64) |
        BinaryPrimitives.ReadUInt64BigEndian(new ReadOnlySpan<byte>(p + 8, 8));
}

/// <summary>
/// 根据系统 TCP/UDP 表把本地端点映射到进程 ID，用于“只对指定进程生效”。
/// 映射每 2 秒刷新一次；遇到新连接时按需刷新（带节流）。
/// </summary>
internal sealed class ProcessFlowFilter : IDisposable
{
    public const int MaxProcessCount = 64;

    private const int RefreshIntervalMs = 2000;
    private static readonly long OnDemandIntervalTicks =
        Math.Max(1, (long)(Stopwatch.Frequency * 0.05));

    private const int AfInet = 2;
    private const int AfInet6 = 23;
    private const int TcpTableOwnerPidAll = 5;
    private const int UdpTableOwnerPid = 1;
    private const uint ErrorInsufficientBuffer = 122;

    private readonly HashSet<string> _names;
    private readonly object _refreshLock = new();
    private readonly System.Threading.Timer? _timer;
    private volatile Snapshot _snapshot = Snapshot.Empty;
    private long _lastRefreshTicks;
    private int _disposed;

    public ProcessFlowFilter(IEnumerable<string>? processNames)
    {
        _names = NormalizeNames(processNames);
        if (_names.Count == 0)
            return;

        RefreshNow();
        _timer = new System.Threading.Timer(
            _ => RefreshNow(), null, RefreshIntervalMs, RefreshIntervalMs);
    }

    public bool HasTargets => _names.Count > 0;

    /// <summary>只判断本端端点；协议必须是 6 (TCP) 或 17 (UDP)。</summary>
    public bool IsMatch(byte protocol, in FlowEndpoint local)
    {
        if (_names.Count == 0)
            return true;

        if (_snapshot.IsTarget(protocol, local))
            return true;

        // 新连接可能还没有出现在上一次快照里。
        RefreshOnDemand();
        return _snapshot.IsTarget(protocol, local);
    }

    /// <summary>把用户输入（可含路径、大小写、.exe）规范成不带扩展名的进程名。</summary>
    public static string NormalizeProcessName(string? name)
    {
        if (string.IsNullOrWhiteSpace(name))
            return string.Empty;

        var fileName = Path.GetFileName(name.Trim());
        if (fileName.EndsWith(".exe", StringComparison.OrdinalIgnoreCase))
            fileName = fileName[..^4];

        return fileName.Trim().ToLowerInvariant();
    }

    /// <summary>配置和界面里保存、显示的文件名，如 chrome.exe。</summary>
    public static string NormalizeDisplayName(string? name)
    {
        var normalized = NormalizeProcessName(name);
        return normalized.Length == 0 ? string.Empty : normalized + ".exe";
    }

    private static HashSet<string> NormalizeNames(IEnumerable<string>? names)
    {
        var result = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        if (names is null)
            return result;

        foreach (var name in names)
        {
            var normalized = NormalizeProcessName(name);
            if (normalized.Length > 0)
                result.Add(normalized);
        }

        return result;
    }

    private void RefreshOnDemand()
    {
        var now = Stopwatch.GetTimestamp();
        var last = Interlocked.Read(ref _lastRefreshTicks);
        if (now - last < OnDemandIntervalTicks)
            return;

        // 上下行可能同时遇到新连接，只让一个线程刷新。
        if (Interlocked.CompareExchange(ref _lastRefreshTicks, now, last) != last)
            return;

        RefreshNow();
    }

    private void RefreshNow()
    {
        if (Volatile.Read(ref _disposed) != 0)
            return;

        lock (_refreshLock)
        {
            if (Volatile.Read(ref _disposed) != 0)
                return;

            try
            {
                var pids = ResolvePids(_names);
                _snapshot = BuildSnapshot(pids);
                Interlocked.Exchange(ref _lastRefreshTicks, Stopwatch.GetTimestamp());
            }
            catch
            {
                // 枚举失败时保留已有快照，下一次刷新会重试。
            }
        }
    }

    private static HashSet<int> ResolvePids(HashSet<string> names)
    {
        var pids = new HashSet<int>();
        foreach (var name in names)
        {
            Process[] matches;
            try
            {
                matches = Process.GetProcessesByName(name);
            }
            catch
            {
                continue;
            }

            foreach (var process in matches)
            {
                try
                {
                    pids.Add(process.Id);
                }
                catch
                {
                    // 进程可能已经退出。
                }
                finally
                {
                    process.Dispose();
                }
            }
        }

        return pids;
    }

    private static Snapshot BuildSnapshot(HashSet<int> pids)
    {
        var tcp = new Dictionary<FlowEndpoint, int>();
        var udp = new Dictionary<FlowEndpoint, int>();

        ReadTcpTable(AfInet, tcp);
        ReadTcpTable(AfInet6, tcp);
        ReadUdpTable(AfInet, udp);
        ReadUdpTable(AfInet6, udp);

        return new Snapshot(tcp, udp, pids);
    }

    private static void ReadTcpTable(int family, Dictionary<FlowEndpoint, int> map)
    {
        var buffer = ReadIpHelperTable(tcp: true, family);
        if (buffer == IntPtr.Zero)
            return;

        try
        {
            var count = Marshal.ReadInt32(buffer);
            var rowPtr = IntPtr.Add(buffer, 4);
            // MIB_TCPROW_OWNER_PID: state, localAddr, localPort, ...；
            // MIB_TCP6ROW_OWNER_PID 的 state 在末尾，字段顺序与 IPv4 不同。
            var rowSize = family == AfInet ? 24 : 56;
            var addressOffset = family == AfInet ? 4 : 0;
            var portOffset = family == AfInet ? 8 : 20;
            var pidOffset = family == AfInet ? 20 : 52;

            for (var i = 0; i < count; i++, rowPtr = IntPtr.Add(rowPtr, rowSize))
            {
                var port = NetworkToHostPort((uint)Marshal.ReadInt32(rowPtr, portOffset));
                if (port == 0)
                    continue;

                var pid = Marshal.ReadInt32(rowPtr, pidOffset);
                var (familyValue, address) = ReadAddress(rowPtr, addressOffset, family);
                map[new FlowEndpoint(familyValue, address, port)] = pid;
            }
        }
        finally
        {
            Marshal.FreeHGlobal(buffer);
        }
    }

    private static void ReadUdpTable(int family, Dictionary<FlowEndpoint, int> map)
    {
        var buffer = ReadIpHelperTable(tcp: false, family);
        if (buffer == IntPtr.Zero)
            return;

        try
        {
            var count = Marshal.ReadInt32(buffer);
            var rowPtr = IntPtr.Add(buffer, 4);
            var rowSize = family == AfInet ? 12 : 28;
            var addressOffset = 0;
            var portOffset = family == AfInet ? 4 : 20;
            var pidOffset = family == AfInet ? 8 : 24;

            for (var i = 0; i < count; i++, rowPtr = IntPtr.Add(rowPtr, rowSize))
            {
                var port = NetworkToHostPort((uint)Marshal.ReadInt32(rowPtr, portOffset));
                if (port == 0)
                    continue;

                var pid = Marshal.ReadInt32(rowPtr, pidOffset);
                var (familyValue, address) = ReadAddress(rowPtr, addressOffset, family);
                map[new FlowEndpoint(familyValue, address, port)] = pid;
            }
        }
        finally
        {
            Marshal.FreeHGlobal(buffer);
        }
    }

    private static (byte Family, UInt128 Address) ReadAddress(
        IntPtr rowPtr, int offset, int family)
    {
        if (family == AfInet)
        {
            var raw = (uint)Marshal.ReadInt32(rowPtr, offset);
            return (4, BinaryPrimitives.ReverseEndianness(raw));
        }

        var address = ReadUInt128(rowPtr, offset);
        return NormalizeAddress(address);
    }

    private static unsafe UInt128 ReadUInt128(IntPtr ptr, int offset)
    {
        var p = (byte*)ptr + offset;
        return ((UInt128)BinaryPrimitives.ReadUInt64BigEndian(new ReadOnlySpan<byte>(p, 8)) << 64) |
               BinaryPrimitives.ReadUInt64BigEndian(new ReadOnlySpan<byte>(p + 8, 8));
    }

    /// <summary>把 IPv4-mapped IPv6 地址（::ffff:a.b.c.d）统一成 IPv4 地址。</summary>
    public static (byte Family, UInt128 Address) NormalizeAddress(UInt128 address)
    {
        if ((address >> 32) == 0xFFFF)
            return (4, (uint)address);
        return (6, address);
    }

    /// <summary>
    /// MIB 表里的端口按网络字节序存放在 DWORD 的低 16 位。
    /// </summary>
    private static ushort NetworkToHostPort(uint value)
    {
        var port = (ushort)(value & 0xFFFF);
        return (ushort)((port >> 8) | (port << 8));
    }

    private static IntPtr ReadIpHelperTable(bool tcp, int family)
    {
        var size = 0;
        var tableClass = tcp ? TcpTableOwnerPidAll : UdpTableOwnerPid;
        var error = tcp
            ? GetExtendedTcpTable(IntPtr.Zero, ref size, false, family, tableClass, 0)
            : GetExtendedUdpTable(IntPtr.Zero, ref size, false, family, tableClass, 0);

        if (error is not (0 or ErrorInsufficientBuffer) || size <= 0)
            return IntPtr.Zero;

        var buffer = Marshal.AllocHGlobal(size);
        error = tcp
            ? GetExtendedTcpTable(buffer, ref size, false, family, tableClass, 0)
            : GetExtendedUdpTable(buffer, ref size, false, family, tableClass, 0);

        if (error != 0)
        {
            Marshal.FreeHGlobal(buffer);
            return IntPtr.Zero;
        }

        return buffer;
    }

    public void Dispose()
    {
        if (Interlocked.Exchange(ref _disposed, 1) != 0)
            return;
        _timer?.Dispose();
    }

    private sealed class Snapshot
    {
        public static readonly Snapshot Empty =
            new(new Dictionary<FlowEndpoint, int>(), new Dictionary<FlowEndpoint, int>(),
                new HashSet<int>());

        private readonly Dictionary<FlowEndpoint, int> _tcp;
        private readonly Dictionary<FlowEndpoint, int> _udp;
        private readonly HashSet<int> _pids;

        public Snapshot(
            Dictionary<FlowEndpoint, int> tcp,
            Dictionary<FlowEndpoint, int> udp,
            HashSet<int> pids)
        {
            _tcp = tcp;
            _udp = udp;
            _pids = pids;
        }

        public bool IsTarget(byte protocol, in FlowEndpoint local)
        {
            var map = protocol switch
            {
                6 => _tcp,
                17 => _udp,
                _ => null,
            };

            if (map is null || _pids.Count == 0)
                return false;

            if (map.TryGetValue(local, out var pid) && _pids.Contains(pid))
                return true;

            // 绑定在通配地址（0.0.0.0 / ::）上的监听端口。
            if (local.Address != 0)
            {
                var wildcard = local with { Address = 0 };
                if (map.TryGetValue(wildcard, out pid) && _pids.Contains(pid))
                    return true;
            }

            // 双栈监听（绑定 ::）可能承载 IPv4 连接。
            if (local.Family == 4 && local.Address != 0 &&
                map.TryGetValue(new FlowEndpoint(6, 0, local.Port), out pid) &&
                _pids.Contains(pid))
            {
                return true;
            }

            return false;
        }
    }

    [DllImport("iphlpapi.dll", SetLastError = true)]
    private static extern uint GetExtendedTcpTable(
        IntPtr pTcpTable,
        ref int pdwSize,
        [MarshalAs(UnmanagedType.Bool)] bool bOrder,
        int ulAf,
        int tableClass,
        int reserved);

    [DllImport("iphlpapi.dll", SetLastError = true)]
    private static extern uint GetExtendedUdpTable(
        IntPtr pUdpTable,
        ref int pdwSize,
        [MarshalAs(UnmanagedType.Bool)] bool bOrder,
        int ulAf,
        int tableClass,
        int reserved);
}
