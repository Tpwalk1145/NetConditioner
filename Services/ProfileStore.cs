using System.Text.Json;
using System.Text.Json.Serialization;
using NetConditioner.Models;

namespace NetConditioner.Services;

/// <summary>负责配置文件的读取、校验和原子写入。</summary>
public sealed class ProfileStore
{
    private static readonly JsonSerializerOptions JsonOptions = new()
    {
        WriteIndented = true,
        PropertyNamingPolicy = JsonNamingPolicy.CamelCase,
        Converters = { new JsonStringEnumConverter() },
    };

    public ProfileStore(string? path = null)
    {
        ConfigPath = path ?? Path.Combine(
            Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData),
            "NetConditioner",
            "config.json");
    }

    public string ConfigPath { get; }

    public AppConfig Load(out string? warning)
    {
        warning = null;

        if (!File.Exists(ConfigPath))
        {
            var fresh = new AppConfig();
            fresh.EnsureDefaultProfile();
            Save(fresh);
            return fresh;
        }

        try
        {
            var json = File.ReadAllText(ConfigPath);
            var config = JsonSerializer.Deserialize<AppConfig>(json, JsonOptions)
                         ?? throw new JsonException("配置文件内容为空。");

            Sanitize(config, ref warning);
            Save(config);
            return config;
        }
        catch (Exception ex)
        {
            warning = $"配置文件读取失败，已恢复默认配置。\r\n{ex.Message}";
            TryBackupBrokenConfig();
            var fresh = new AppConfig();
            fresh.EnsureDefaultProfile();
            Save(fresh);
            return fresh;
        }
    }

    public void Save(AppConfig config)
    {
        var dir = Path.GetDirectoryName(ConfigPath)!;
        Directory.CreateDirectory(dir);

        var tempPath = ConfigPath + ".tmp";
        var json = JsonSerializer.Serialize(config, JsonOptions);
        File.WriteAllText(tempPath, json);
        File.Move(tempPath, ConfigPath, overwrite: true);
    }

    private static void Sanitize(AppConfig config, ref string? warning)
    {
        config.Profiles ??= new List<NetworkProfile>();
        config.Hotkey ??= new HotkeySettings();
        config.Settings ??= new AppSettings();
        config.Settings.MaxRunMinutes = Math.Clamp(config.Settings.MaxRunMinutes, 0, 1440);
        config.Settings.MaxQueueDelayMs = Math.Clamp(config.Settings.MaxQueueDelayMs, 0, 60000);
        config.Settings.ProcessNames = SanitizeProcessNames(config.Settings.ProcessNames);

        // 去掉明显损坏的配置项，并修正重复 Id。
        var seenIds = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        var removed = 0;
        for (var i = config.Profiles.Count - 1; i >= 0; i--)
        {
            var p = config.Profiles[i];
            if (p is null || string.IsNullOrWhiteSpace(p.Name))
            {
                config.Profiles.RemoveAt(i);
                removed++;
                continue;
            }

            if (string.IsNullOrWhiteSpace(p.Id) || !seenIds.Add(p.Id))
            {
                p.Id = Guid.NewGuid().ToString("N");
                seenIds.Add(p.Id);
            }

            p.Uplink ??= new DirectionProfile();
            p.Downlink ??= new DirectionProfile();
            Clamp(p.Uplink);
            Clamp(p.Downlink);
        }

        config.EnsureDefaultProfile();

        if (removed > 0)
            warning = $"已忽略 {removed} 个损坏的配置项。";
    }

    private static List<string> SanitizeProcessNames(List<string>? names)
    {
        var result = new List<string>();
        if (names is null)
            return result;

        foreach (var raw in names)
        {
            if (string.IsNullOrWhiteSpace(raw))
                continue;

            var name = ProcessFlowFilter.NormalizeDisplayName(raw);
            if (name.Length == 0)
                continue;

            // 只保存文件名，避免绝对路径泄露或失效；同一进程名只保留一次。
            if (result.Any(existing =>
                    string.Equals(existing, name, StringComparison.OrdinalIgnoreCase)))
            {
                continue;
            }

            result.Add(name);
            if (result.Count >= ProcessFlowFilter.MaxProcessCount)
                break;
        }

        return result;
    }

    private static void Clamp(DirectionProfile d)
    {
        d.DelayMs = Math.Clamp(d.DelayMs, 0, 60000);
        d.JitterMs = Math.Clamp(d.JitterMs, 0, 60000);
        d.BandwidthKbps = Math.Clamp(d.BandwidthKbps, 0, 1_000_000);
        d.RandomLossPercent = Math.Clamp(d.RandomLossPercent, 0, 100);
        d.BurstAllowMs = Math.Clamp(d.BurstAllowMs, 0, 600_000);
        d.BurstDropMs = Math.Clamp(d.BurstDropMs, 0, 600_000);
    }

    private void TryBackupBrokenConfig()
    {
        try
        {
            var backup = ConfigPath + ".bad-" + DateTime.Now.ToString("yyyyMMdd-HHmmss");
            File.Copy(ConfigPath, backup, overwrite: false);
        }
        catch
        {
            // 备份失败不影响主流程。
        }
    }
}
