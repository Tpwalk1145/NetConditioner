namespace NetConditioner.Models;

/// <summary>云端 latest.json 中安装包的描述。</summary>
public sealed class UpdateInstallerInfo
{
    public string? Url { get; set; }
    public string? Sha256 { get; set; }
    public long Size { get; set; }
}

/// <summary>云端更新清单（latest.json）。字段为 camelCase，由源生成上下文反序列化。</summary>
public sealed class UpdateManifest
{
    public int SchemaVersion { get; set; } = 1;
    public string? Version { get; set; }
    public string? PublishedAt { get; set; }
    public string? Notes { get; set; }
    public string? PageUrl { get; set; }
    public UpdateInstallerInfo? Installer { get; set; }
}
