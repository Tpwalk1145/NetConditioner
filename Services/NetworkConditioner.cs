using NetConditioner.Models;

namespace NetConditioner.Services;

/// <summary>组合上下行两个方向的弱网引擎。</summary>
public sealed class NetworkConditioner : IDisposable
{
    public const string DefaultOutboundFilter = "outbound and (tcp or udp)";
    public const string DefaultInboundFilter = "inbound and (tcp or udp)";

    private readonly object _sync = new();
    private DirectionEngine? _uplink;
    private DirectionEngine? _downlink;
    private ProcessFlowFilter? _processFilter;
    private bool _started;

    public event Action<string>? Faulted;

    public bool IsRunning
    {
        get
        {
            lock (_sync)
                return _started;
        }
    }

    public DirectionStats? UplinkStats
    {
        get { lock (_sync) return _uplink?.Stats; }
    }

    public DirectionStats? DownlinkStats
    {
        get { lock (_sync) return _downlink?.Stats; }
    }

    /// <summary>
    /// 启动弱网模拟。filter 参数用于自测；常规启动使用默认全系统过滤器。
    /// processNames 为空表示对所有进程生效。
    /// </summary>
    public void Start(
        NetworkProfile profile,
        string? outboundFilter = null,
        string? inboundFilter = null,
        int maxQueueDelayMs = 2000,
        IReadOnlyList<string>? processNames = null)
    {
        ArgumentNullException.ThrowIfNull(profile);

        lock (_sync)
        {
            if (_started)
                throw new InvalidOperationException("弱网模拟已经在运行。");

            var snapshot = profile.Clone();
            var processFilter = processNames is { Count: > 0 }
                ? new ProcessFlowFilter(processNames)
                : null;

            var uplink = new DirectionEngine(
                "上行",
                outboundFilter ?? DefaultOutboundFilter,
                outbound: true,
                snapshot.Uplink,
                snapshot.Protocol,
                processFilter,
                maxQueueDelayMs,
                OnEngineFaulted);
            var downlink = new DirectionEngine(
                "下行",
                inboundFilter ?? DefaultInboundFilter,
                outbound: false,
                snapshot.Downlink,
                snapshot.Protocol,
                processFilter,
                maxQueueDelayMs,
                OnEngineFaulted);

            try
            {
                uplink.Start();
                downlink.Start();
            }
            catch
            {
                uplink.Dispose();
                downlink.Dispose();
                processFilter?.Dispose();
                throw;
            }

            _uplink = uplink;
            _downlink = downlink;
            _processFilter = processFilter;
            _started = true;
        }
    }

    public void Stop()
    {
        DirectionEngine? uplink;
        DirectionEngine? downlink;
        ProcessFlowFilter? processFilter;

        lock (_sync)
        {
            if (!_started)
                return;

            _started = false;
            uplink = _uplink;
            downlink = _downlink;
            processFilter = _processFilter;
            _uplink = null;
            _downlink = null;
            _processFilter = null;
        }

        uplink?.Dispose();
        downlink?.Dispose();
        processFilter?.Dispose();
    }

    private void OnEngineFaulted(string message)
    {
        try
        {
            Faulted?.Invoke(message);
        }
        catch
        {
            // 事件处理器异常不能影响引擎线程。
        }
    }

    public void Dispose() => Stop();
}
