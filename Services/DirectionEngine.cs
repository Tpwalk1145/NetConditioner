using System.Buffers;
using System.Collections.Concurrent;
using System.Diagnostics;
using System.Runtime.InteropServices;
using NetConditioner.Models;

namespace NetConditioner.Services;

public sealed class DirectionStats
{
    private long _sent;
    private long _dropped;
    private int _queueCount;

    public long Sent => Interlocked.Read(ref _sent);
    public long Dropped => Interlocked.Read(ref _dropped);
    public int QueueCount => Volatile.Read(ref _queueCount);

    public void AddSent() => Interlocked.Increment(ref _sent);
    public void AddDropped() => Interlocked.Increment(ref _dropped);
    public void SetQueueCount(int value) => Volatile.Write(ref _queueCount, value);
    public void Reset()
    {
        Interlocked.Exchange(ref _sent, 0);
        Interlocked.Exchange(ref _dropped, 0);
        Volatile.Write(ref _queueCount, 0);
    }
}

/// <summary>连续丢包（放行/丢包时长循环）状态机。不是线程安全的，每个方向独立使用。</summary>
internal sealed class BurstLossState
{
    private readonly int _allowMs;
    private readonly int _dropMs;
    private long _phaseStartTicks;
    private bool _dropping;

    public BurstLossState(int allowMs, int dropMs)
    {
        _allowMs = Math.Max(0, allowMs);
        _dropMs = Math.Max(0, dropMs);
        _phaseStartTicks = Stopwatch.GetTimestamp();
    }

    public bool ShouldDrop(long nowTicks)
    {
        if (_allowMs == 0 && _dropMs == 0)
            return false;
        if (_allowMs == 0)
            return true;
        if (_dropMs == 0)
            return false;

        // 最多循环几次，防止线程长时间停滞后死循环。
        for (var i = 0; i < 4; i++)
        {
            var elapsedMs = (nowTicks - _phaseStartTicks) * 1000.0 / Stopwatch.Frequency;
            var duration = _dropping ? _dropMs : _allowMs;
            if (elapsedMs < duration)
                return _dropping;

            _dropping = !_dropping;
            _phaseStartTicks = nowTicks;
        }

        return _dropping;
    }
}

/// <summary>可池化的数据包描述。数据/地址数组来自 ArrayPool，发送完成后归还。</summary>
internal sealed class PacketItem
{
    public byte[] Data = Array.Empty<byte>();
    public byte[] Address = Array.Empty<byte>();
    public int Length;
    public long DueTicks;
    public long Sequence;

    public void Reset()
    {
        Data = Array.Empty<byte>();
        Address = Array.Empty<byte>();
        Length = 0;
        DueTicks = 0;
        Sequence = 0;
    }
}

/// <summary>处理一个方向（出站或入站）的弱网逻辑。</summary>
internal sealed class DirectionEngine : IDisposable
{
    private const int MaxQueuePackets = 4096;
    private const long MaxQueueBytes = 8 * 1024 * 1024;
    private const int SendBatchSize = 64;

    private readonly string _displayName;
    private readonly string _filter;
    private readonly bool _outbound;
    private readonly DirectionProfile _profile;
    private readonly ConditionProtocol _protocol;
    private readonly ProcessFlowFilter? _processFilter;
    private readonly Action<string>? _faulted;
    private readonly DirectionStats _stats = new();
    private readonly BurstLossState _burst;
    private readonly object _queueLock = new();
    private readonly PriorityQueue<PacketItem, (long Due, long Seq)> _queue = new();
    private readonly ConcurrentBag<PacketItem> _itemPool = new();
    private readonly long _maxQueueTicks;
    private readonly bool _needsQueue;

    private IntPtr _handle;
    private Thread? _receiver;
    private Thread? _sender;
    private int _stopFlag;
    private long _nextSendTicks;

    private bool StopRequested => Volatile.Read(ref _stopFlag) != 0;
    private long _sequence;
    private long _queueBytes;
    private int _faultReported;

    public DirectionEngine(
        string displayName,
        string filter,
        bool outbound,
        DirectionProfile profile,
        ConditionProtocol protocol,
        ProcessFlowFilter? processFilter,
        int maxQueueDelayMs,
        Action<string>? faulted)
    {
        _displayName = displayName;
        _filter = filter;
        _outbound = outbound;
        _profile = profile.Clone();
        _protocol = protocol;
        _processFilter = processFilter;
        _faulted = faulted;
        _burst = new BurstLossState(_profile.BurstAllowMs, _profile.BurstDropMs);
        _maxQueueTicks = maxQueueDelayMs > 0 ? MsToTicks(maxQueueDelayMs) : 0;
        _needsQueue = _profile.DelayMs > 0 || _profile.JitterMs > 0 || _profile.BandwidthKbps > 0;
    }

    public DirectionStats Stats => _stats;

    public void Start()
    {
        if (_handle != IntPtr.Zero)
            throw new InvalidOperationException("引擎已经启动。");

        _handle = WinDivertNative.Open(_filter);
        WinDivertNative.SetParams(_handle, out _);

        _receiver = new Thread(ReceiverLoop)
        {
            IsBackground = true,
            Name = $"NetConditioner-{_displayName}-recv",
        };
        _sender = new Thread(SenderLoop)
        {
            IsBackground = true,
            Name = $"NetConditioner-{_displayName}-send",
        };
        _receiver.Start();
        _sender.Start();
    }

    private void ReceiverLoop()
    {
        var packetBuffer = Marshal.AllocHGlobal(WinDivertNative.MaxPacketSize);
        var addressBuffer = Marshal.AllocHGlobal(WinDivertNative.AddressBufferSize);
        try
        {
            while (!StopRequested)
            {
                if (!WinDivertNative.WinDivertRecv(
                        _handle,
                        packetBuffer,
                        WinDivertNative.MaxPacketSize,
                        out var length,
                        addressBuffer))
                {
                    if (StopRequested)
                        break;

                    var code = Marshal.GetLastWin32Error();
                    ReportFault($"{_displayName}接收数据包失败（Win32 错误 {code}）。弱网模拟已停止。");
                    return;
                }

                if (length == 0)
                    continue;

                // 只有按进程过滤或非 ALL 协议时，才需要解析每个包。
                if (_protocol is not ConditionProtocol.All || _processFilter is not null)
                {
                    var parsed = PacketFlowReader.TryRead(
                        packetBuffer, (int)length, _outbound, out var protocol, out var localEndpoint);

                    if (!parsed && _processFilter is not null)
                    {
                        // 解析不了的包（如少见的 IPv6 扩展头）无法归属进程，直接放行。
                        SendDirect(packetBuffer, length, addressBuffer);
                        continue;
                    }

                    if (!parsed && !WinDivertNative.WinDivertHelperParsePacket(
                            packetBuffer,
                            length,
                            out _,
                            out _,
                            out protocol,
                            out _,
                            out _,
                            out _,
                            out _,
                            out _,
                            out _,
                            out _,
                            out _))
                    {
                        SendDirect(packetBuffer, length, addressBuffer);
                        continue;
                    }

                    if (!ProtocolMatches(protocol))
                    {
                        SendDirect(packetBuffer, length, addressBuffer);
                        continue;
                    }

                    if (parsed && _processFilter is not null &&
                        !_processFilter.IsMatch(protocol, localEndpoint))
                    {
                        SendDirect(packetBuffer, length, addressBuffer);
                        continue;
                    }
                }

                var now = Stopwatch.GetTimestamp();
                if (_burst.ShouldDrop(now))
                {
                    _stats.AddDropped();
                    continue;
                }

                if (_profile.RandomLossPercent > 0 &&
                    Random.Shared.Next(100) < _profile.RandomLossPercent)
                {
                    _stats.AddDropped();
                    continue;
                }

                // 只有丢包/随机丢包的配置无需进入延时队列，直接重注入。
                if (!_needsQueue)
                {
                    SendDirect(packetBuffer, length, addressBuffer);
                    continue;
                }

                var jitter = _profile.JitterMs > 0
                    ? Random.Shared.Next(_profile.JitterMs + 1)
                    : 0;
                var intendedDelayTicks = MsToTicks(_profile.DelayMs + jitter);
                var due = now + intendedDelayTicks;

                if (_profile.BandwidthKbps > 0)
                {
                    // 串行化发送：每个包按自身长度累加发送时间并排队，
                    // 保证低带宽配置从第一个包开始生效。
                    var seconds = length * 8.0 / (_profile.BandwidthKbps * 1000.0);
                    var packetTicks = Math.Max(1, (long)(seconds * Stopwatch.Frequency));
                    var start = Math.Max(due, _nextSendTicks);
                    _nextSendTicks = start + packetTicks;
                    due = _nextSendTicks;
                }

                // 只限制“超出用户配置延时之外的额外排队”，避免低带宽下队列无限增长，
                // 同时不会因为用户故意配置了大延时而误丢包。
                if (_maxQueueTicks > 0 && due - (now + intendedDelayTicks) > _maxQueueTicks)
                {
                    _stats.AddDropped();
                    continue;
                }

                Enqueue(packetBuffer, length, addressBuffer, due);
            }
        }
        catch (Exception ex)
        {
            if (!StopRequested)
                ReportFault($"{_displayName}引擎异常：{ex.Message}");
        }
        finally
        {
            Marshal.FreeHGlobal(packetBuffer);
            Marshal.FreeHGlobal(addressBuffer);
            lock (_queueLock)
                Monitor.PulseAll(_queueLock);
        }
    }

    private bool ProtocolMatches(byte protocol)
    {
        return _protocol switch
        {
            ConditionProtocol.Tcp => protocol == 6,
            ConditionProtocol.Udp => protocol == 17,
            _ => protocol is 6 or 17,
        };
    }

    private void Enqueue(IntPtr packetBuffer, uint length, IntPtr addressBuffer, long due)
    {
        var item = RentItem();
        try
        {
            item.Data = ArrayPool<byte>.Shared.Rent((int)length);
            item.Address = ArrayPool<byte>.Shared.Rent(WinDivertNative.AddressStructSize);
            item.Length = (int)length;
            item.DueTicks = due;
            item.Sequence = Interlocked.Increment(ref _sequence);
            CopyToArray(packetBuffer, item.Data, (int)length);
            CopyToArray(addressBuffer, item.Address, WinDivertNative.AddressStructSize);
        }
        catch
        {
            ReturnItem(item);
            _stats.AddDropped();
            return;
        }

        var dropped = false;
        lock (_queueLock)
        {
            if (_queue.Count >= MaxQueuePackets || _queueBytes + item.Length > MaxQueueBytes)
            {
                dropped = true;
            }
            else
            {
                var oldHeadDue = _queue.TryPeek(out _, out var headPriority)
                    ? headPriority.Due
                    : long.MaxValue;

                _queueBytes += item.Length;
                _queue.Enqueue(item, (item.DueTicks, item.Sequence));
                _stats.SetQueueCount(_queue.Count);

                // 仅在新包会成为更早的队首，或队列从空变为非空时唤醒发送线程。
                // 大量突发时避免每个包都 PulseAll 造成锁竞争。
                if (oldHeadDue == long.MaxValue || item.DueTicks < oldHeadDue)
                    Monitor.Pulse(_queueLock);
            }
        }

        if (dropped)
        {
            ReturnItem(item);
            _stats.AddDropped();
        }
    }

    private void SenderLoop()
    {
        var batch = new List<PacketItem>(SendBatchSize);
        try
        {
            while (true)
            {
                batch.Clear();
                var shouldExit = false;

                lock (_queueLock)
                {
                    while (true)
                    {
                        if (_queue.Count == 0)
                        {
                            if (StopRequested)
                            {
                                shouldExit = true;
                                break;
                            }
                            Monitor.Wait(_queueLock, 100);
                            continue;
                        }

                        var now = Stopwatch.GetTimestamp();
                        if (!StopRequested &&
                            _queue.TryPeek(out _, out var priority) &&
                            priority.Due > now)
                        {
                            var waitMs = (priority.Due - now) * 1000.0 / Stopwatch.Frequency;
                            Monitor.Wait(_queueLock, Math.Clamp((int)waitMs, 1, 100));
                            continue;
                        }

                        while (batch.Count < SendBatchSize &&
                               _queue.TryPeek(out _, out var nextPriority) &&
                               (StopRequested || nextPriority.Due <= now))
                        {
                            var item = _queue.Dequeue();
                            _queueBytes -= item.Length;
                            batch.Add(item);
                        }

                        _stats.SetQueueCount(_queue.Count);
                        if (batch.Count > 0)
                            break;

                        if (StopRequested)
                        {
                            shouldExit = true;
                            break;
                        }
                    }
                }

                if (shouldExit && batch.Count == 0)
                    break;

                var sendFailed = false;
                foreach (var item in batch)
                {
                    if (sendFailed)
                    {
                        ReturnItem(item);
                        continue;
                    }

                    var sent = SendItem(item);
                    if (sent)
                    {
                        _stats.AddSent();
                    }
                    else if (!StopRequested)
                    {
                        _stats.AddDropped();
                        var code = Marshal.GetLastWin32Error();
                        ReportFault($"{_displayName}发送数据包失败（Win32 错误 {code}）。弱网模拟已停止。");
                        sendFailed = true;
                    }

                    ReturnItem(item);
                }

                if (sendFailed || shouldExit)
                    break;
            }
        }
        catch (Exception ex)
        {
            if (!StopRequested)
                ReportFault($"{_displayName}发送线程异常：{ex.Message}");
        }
        finally
        {
            lock (_queueLock)
            {
                while (_queue.TryDequeue(out var left, out _))
                {
                    _queueBytes -= left.Length;
                    ReturnItem(left);
                }
                _stats.SetQueueCount(0);
            }
        }
    }

    private bool SendItem(PacketItem item)
    {
        unsafe
        {
            fixed (byte* data = item.Data)
            fixed (byte* address = item.Address)
            {
                return WinDivertNative.WinDivertSend(
                    _handle,
                    (IntPtr)data,
                    (uint)item.Length,
                    out _,
                    (IntPtr)address);
            }
        }
    }

    private void SendDirect(IntPtr packetBuffer, uint length, IntPtr addressBuffer)
    {
        if (WinDivertNative.WinDivertSend(_handle, packetBuffer, length, out _, addressBuffer))
            _stats.AddSent();
        else if (!StopRequested)
            _stats.AddDropped();
    }

    private PacketItem RentItem() =>
        _itemPool.TryTake(out var item) ? item : new PacketItem();

    private void ReturnItem(PacketItem item)
    {
        if (item.Data.Length > 0)
            ArrayPool<byte>.Shared.Return(item.Data);
        if (item.Address.Length > 0)
            ArrayPool<byte>.Shared.Return(item.Address);

        item.Reset();
        _itemPool.Add(item);
    }

    private static unsafe void CopyToArray(IntPtr source, byte[] destination, int bytes)
    {
        fixed (byte* destinationPtr = destination)
        {
            Buffer.MemoryCopy(
                source.ToPointer(),
                destinationPtr,
                destination.Length,
                bytes);
        }
    }

    private void ReportFault(string message)
    {
        if (Interlocked.Exchange(ref _faultReported, 1) != 0)
            return;
        _faulted?.Invoke(message);
    }

    private static long MsToTicks(int ms) =>
        (long)(Math.Max(0, ms) * (Stopwatch.Frequency / 1000.0));

    public void Stop()
    {
        if (Interlocked.Exchange(ref _stopFlag, 1) != 0)
            return;

        // 注意：这里不能提前把 _handle 置 0，否则 sender 在停止时
        // 无法把队列中剩余包重注入。等 sender/receiver 退出后再关闭。
        var handle = _handle;
        if (handle != IntPtr.Zero)
        {
            try
            {
                WinDivertNative.WinDivertShutdown(handle, WinDivertNative.ShutdownRecv);
            }
            catch
            {
                // WinDivertShutdown 调用失败时，关闭句柄以唤醒接收线程。
                WinDivertNative.WinDivertClose(handle);
                _handle = IntPtr.Zero;
                handle = IntPtr.Zero;
            }
            lock (_queueLock)
                Monitor.PulseAll(_queueLock);
        }

        _receiver?.Join(3000);
        _sender?.Join(5000);

        if (handle != IntPtr.Zero)
        {
            _handle = IntPtr.Zero;
            WinDivertNative.WinDivertClose(handle);
        }

        lock (_queueLock)
        {
            while (_queue.TryDequeue(out var left, out _))
            {
                _queueBytes -= left.Length;
                ReturnItem(left);
            }
            _stats.SetQueueCount(0);
        }
    }

    public void Dispose()
    {
        Stop();
    }
}
