namespace NetConditioner.Models;

/// <summary>全局热键。Modifiers 使用 Win32 MOD_* 标志（不含 MOD_NOREPEAT）。</summary>
public sealed class HotkeySettings
{
    public uint Modifiers { get; set; } = HotkeyDefaults.ModControl | HotkeyDefaults.ModAlt;
    public int Key { get; set; } = 0x55; // VK_U

    public HotkeySettings Clone() => new() { Modifiers = Modifiers, Key = Key };
}

public static class HotkeyDefaults
{
    public const uint ModAlt = 0x0001;
    public const uint ModControl = 0x0002;
    public const uint ModShift = 0x0004;
    public const uint ModWin = 0x0008;
    public const uint ModNoRepeat = 0x4000;
}

public sealed class AppSettings
{
    /// <summary>0 表示不自动停止。</summary>
    public int MaxRunMinutes { get; set; }

    /// <summary>
    /// 除配置延时之外允许的额外排队时延上限。0 表示不限制（突发时可能明显卡顿）。
    /// </summary>
    public int MaxQueueDelayMs { get; set; } = 2000;

    /// <summary>目标进程文件名（如 chrome.exe）。为空表示对所有进程生效（全局）。</summary>
    public List<string> ProcessNames { get; set; } = new();

    /// <summary>latest.json 地址。为空表示关闭云更新（不联网）。</summary>
    public string UpdateFeedUrl { get; set; } = string.Empty;

    /// <summary>是否启用全静默自动更新（发现新版后后台下载并重启，不提示）。</summary>
    public bool AutoUpdate { get; set; } = true;

    /// <summary>上次检查更新的时间（Unix 秒，UTC），用于 24 小时节流。</summary>
    public long LastUpdateCheckUnix { get; set; }

    /// <summary>上次尝试升级的时间（Unix 秒，UTC）。</summary>
    public long LastUpdateAttemptUnix { get; set; }

    /// <summary>上次尝试升级的版本；同一版本失败后 24 小时内不重试。</summary>
    public string LastAttemptedVersion { get; set; } = string.Empty;

    public AppSettings Clone() => new()
    {
        MaxRunMinutes = MaxRunMinutes,
        MaxQueueDelayMs = MaxQueueDelayMs,
        ProcessNames = new List<string>(ProcessNames ?? new List<string>()),
        UpdateFeedUrl = UpdateFeedUrl,
        AutoUpdate = AutoUpdate,
        LastUpdateCheckUnix = LastUpdateCheckUnix,
        LastUpdateAttemptUnix = LastUpdateAttemptUnix,
        LastAttemptedVersion = LastAttemptedVersion,
    };
}

public sealed class AppConfig
{
    public int Version { get; set; } = 1;
    public string? ActiveProfileId { get; set; }
    public List<NetworkProfile> Profiles { get; set; } = new();
    public HotkeySettings Hotkey { get; set; } = new();
    public AppSettings Settings { get; set; } = new();

    public NetworkProfile? FindActiveProfile()
    {
        if (Profiles.Count == 0)
            return null;
        return Profiles.FirstOrDefault(p => p.Id == ActiveProfileId) ?? Profiles[0];
    }

    public NetworkProfile EnsureDefaultProfile()
    {
        Profiles ??= new List<NetworkProfile>();

        if (Profiles.Count == 0)
        {
            var profile = new NetworkProfile
            {
                Name = "默认配置",
                Id = Guid.NewGuid().ToString("N"),
            };
            Profiles.Add(profile);
        }

        ActiveProfileId ??= Profiles[0].Id;
        if (Profiles.All(p => p.Id != ActiveProfileId))
            ActiveProfileId = Profiles[0].Id;

        Hotkey ??= new HotkeySettings();
        Settings ??= new AppSettings();
        return FindActiveProfile()!;
    }
}
