using System.ComponentModel;
using System.Runtime.InteropServices;

namespace NetConditioner.Services;

/// <summary>WinDivert 2.2.2 的原生 API 封装。地址结构按不透明缓冲区处理，避免 bitfield 布局问题。</summary>
internal static class WinDivertNative
{
    public const int LayerNetwork = 0;

    public const int ParamQueueLength = 0;
    public const int ParamQueueTime = 1;
    public const int ParamQueueSize = 2;

    public const int ShutdownRecv = 0x1;

    // WINDIVERT_ADDRESS 在 2.2.x 中为 80 字节；接收缓冲区留出余量，
    // 但队列里按 80 字节保存，减少每个包的内存拷贝。
    public const int AddressBufferSize = 128;
    public const int AddressStructSize = 80;

    // 最大 IP 包长度为 65535，额外留出协议头余量。
    public const int MaxPacketSize = 65535 + 256;

    public static readonly IntPtr InvalidHandle = new(-1);

    public static IntPtr Open(string filter)
    {
        var handle = WinDivertOpen(filter, LayerNetwork, 0, 0);
        if (handle == InvalidHandle || handle == IntPtr.Zero)
        {
            var code = Marshal.GetLastWin32Error();
            throw new WinDivertException(code, DescribeOpenError(code));
        }
        return handle;
    }

    public static bool SetParams(IntPtr handle, out string? warning)
    {
        warning = null;
        var ok = true;
        // 突发流量时让内核队列多吸收一些，减少用户态处理不及时造成的丢包。
        if (!WinDivertSetParam(handle, ParamQueueLength, 16384))
        {
            ok = false;
            warning = $"设置 WinDivert 队列长度失败：{new Win32Exception(Marshal.GetLastWin32Error()).Message}";
        }
        if (!WinDivertSetParam(handle, ParamQueueTime, 2000))
            ok = false;
        if (!WinDivertSetParam(handle, ParamQueueSize, 32 * 1024 * 1024))
            ok = false;
        return ok;
    }

    public static string DescribeOpenError(int code) => code switch
    {
        2 => "找不到 WinDivert64.sys。请确认 WinDivert.dll 和 WinDivert64.sys 与程序在同一目录。",
        5 => "拒绝访问。WinDivert 需要管理员权限，请以管理员身份运行。",
        577 => "WinDivert 驱动签名无效或被系统拦截。请使用随程序附带的官方 2.2.2 文件，并检查杀毒软件。",
        1275 => "WinDivert 驱动被系统策略阻止加载（错误 1275）。请检查驱动签名/安全软件。",
        1058 => "WinDivert 驱动服务无法启动。请检查系统服务与安全软件。",
        _ => $"WinDivert 启动失败（Win32 错误 {code}）。常见原因：未以管理员运行、驱动文件缺失、杀毒软件拦截或已有不兼容的 WinDivert 驱动。",
    };

    [DllImport("WinDivert.dll", CallingConvention = CallingConvention.Cdecl, SetLastError = true,
        CharSet = CharSet.Ansi)]
    private static extern IntPtr WinDivertOpen(
        [MarshalAs(UnmanagedType.LPStr)] string filter,
        int layer,
        short priority,
        ulong flags);

    [DllImport("WinDivert.dll", CallingConvention = CallingConvention.Cdecl, SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    public static extern bool WinDivertRecv(
        IntPtr handle,
        IntPtr packet,
        uint packetLen,
        out uint recvLen,
        IntPtr addr);

    [DllImport("WinDivert.dll", CallingConvention = CallingConvention.Cdecl, SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    public static extern bool WinDivertSend(
        IntPtr handle,
        IntPtr packet,
        uint packetLen,
        out uint sendLen,
        IntPtr addr);

    [DllImport("WinDivert.dll", CallingConvention = CallingConvention.Cdecl, SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    public static extern bool WinDivertClose(IntPtr handle);

    [DllImport("WinDivert.dll", CallingConvention = CallingConvention.Cdecl, SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    public static extern bool WinDivertShutdown(IntPtr handle, int how);

    [DllImport("WinDivert.dll", CallingConvention = CallingConvention.Cdecl, SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool WinDivertSetParam(IntPtr handle, int param, ulong value);

    [DllImport("WinDivert.dll", CallingConvention = CallingConvention.Cdecl, SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    public static extern bool WinDivertHelperParsePacket(
        IntPtr packet,
        uint packetLen,
        out IntPtr ipHdr,
        out IntPtr ipv6Hdr,
        out byte protocol,
        out IntPtr icmpHdr,
        out IntPtr icmpv6Hdr,
        out IntPtr tcpHdr,
        out IntPtr udpHdr,
        out IntPtr data,
        out uint dataLen,
        out IntPtr next,
        out uint nextLen);
}

public sealed class WinDivertException : Exception
{
    public WinDivertException(int errorCode, string message) : base(message)
    {
        ErrorCode = errorCode;
    }

    public int ErrorCode { get; }
}
