using System.Text.Json.Serialization;

namespace NetConditioner.Models;

/// <summary>弱网控制协议，数值与界面显示的 TCP (1) / UDP (2) / ALL (3) 一致。</summary>
public enum ConditionProtocol
{
    Tcp = 1,
    Udp = 2,
    All = 3,
}

/// <summary>单个方向的弱网参数。</summary>
public sealed class DirectionProfile
{
    public int DelayMs { get; set; }
    public int JitterMs { get; set; }
    public int BandwidthKbps { get; set; }
    public int RandomLossPercent { get; set; }
    public int BurstAllowMs { get; set; }
    public int BurstDropMs { get; set; }

    [JsonIgnore]
    public bool IsAllZero =>
        DelayMs == 0 && JitterMs == 0 && BandwidthKbps == 0 &&
        RandomLossPercent == 0 && BurstAllowMs == 0 && BurstDropMs == 0;

    public DirectionProfile Clone() => (DirectionProfile)MemberwiseClone();

    /// <summary>返回 null 表示合法，否则返回错误描述。</summary>
    public string? Validate(string directionName)
    {
        if (DelayMs is < 0 or > 60000)
            return $"{directionName}延时必须在 0 到 60000 ms 之间。";
        if (JitterMs is < 0 or > 60000)
            return $"{directionName}延时抖动必须在 0 到 60000 ms 之间。";
        if (BandwidthKbps is < 0 or > 1_000_000)
            return $"{directionName}带宽必须在 0 到 1000000 kbps 之间。";
        if (RandomLossPercent is < 0 or > 100)
            return $"{directionName}随机丢包必须在 0 到 100% 之间。";
        if (BurstAllowMs is < 0 or > 600_000)
            return $"{directionName}连续丢包-放行必须在 0 到 600000 ms 之间。";
        if (BurstDropMs is < 0 or > 600_000)
            return $"{directionName}连续丢包-丢包必须在 0 到 600000 ms 之间。";
        return null;
    }
}

/// <summary>一套可保存、可切换的弱网配置。</summary>
public sealed class NetworkProfile
{
    public string Id { get; set; } = Guid.NewGuid().ToString("N");
    public string Name { get; set; } = "默认配置";
    public DirectionProfile Uplink { get; set; } = new();
    public DirectionProfile Downlink { get; set; } = new();
    public ConditionProtocol Protocol { get; set; } = ConditionProtocol.All;

    /// <summary>目标进程文件名（如 chrome.exe）。为空表示对所有进程生效（全局）。</summary>
    public List<string> ProcessNames { get; set; } = new();

    [JsonIgnore]
    public bool IsAllZero => Uplink.IsAllZero && Downlink.IsAllZero;

    [JsonIgnore]
    public bool IsGlobal => ProcessNames is not { Count: > 0 };

    public NetworkProfile Clone() => new()
    {
        Id = Id,
        Name = Name,
        Uplink = Uplink.Clone(),
        Downlink = Downlink.Clone(),
        Protocol = Protocol,
        ProcessNames = new List<string>(ProcessNames ?? new List<string>()),
    };

    public string? Validate()
    {
        if (string.IsNullOrWhiteSpace(Name))
            return "配置名称不能为空。";
        if (Name.Length > 60)
            return "配置名称不能超过 60 个字符。";
        if (ProcessNames is { Count: > 64 })
            return "目标进程最多 64 个。";
        return Uplink.Validate("上行") ?? Downlink.Validate("下行");
    }

    public override string ToString() => Name;
}
