using System.Globalization;

namespace NetConditioner.Services;

/// <summary>程序版本号（来自 csproj 的 &lt;Version&gt;，编译进程序集版本）。</summary>
public static class AppVersion
{
    public static Version Current { get; } = ReadAssemblyVersion();

    /// <summary>形如 1.1.0（revision 为 0 时省略）。</summary>
    public static string Display => Format(Current);

    private static Version ReadAssemblyVersion()
    {
        var version = typeof(AppVersion).Assembly.GetName().Version ?? new Version(1, 0, 0, 0);
        return new Version(
            Math.Max(0, version.Major),
            Math.Max(0, version.Minor),
            Math.Max(0, version.Build),
            Math.Max(0, version.Revision));
    }

    public static string Format(Version version)
    {
        var text = $"{version.Major}.{version.Minor}.{version.Build}";
        if (version.Revision > 0)
            text += $".{version.Revision}";
        return text;
    }

    /// <summary>接受 2~4 段数字，统一补成 4 段 Version。</summary>
    public static bool TryParse(string? text, out Version version)
    {
        version = new Version(0, 0, 0, 0);
        if (string.IsNullOrWhiteSpace(text))
            return false;

        var parts = text.Trim().Split('.');
        if (parts.Length is < 2 or > 4)
            return false;

        var numbers = new int[4];
        for (var i = 0; i < parts.Length; i++)
        {
            if (!int.TryParse(parts[i], NumberStyles.None, CultureInfo.InvariantCulture, out var value) || value < 0)
                return false;
            numbers[i] = value;
        }

        version = new Version(numbers[0], numbers[1], numbers[2], numbers[3]);
        return true;
    }

    public static bool IsNewer(Version current, Version candidate) => candidate > current;

    /// <summary>规范化为 4 段字符串；无效时返回 null。</summary>
    public static string? Normalize(string? text) => TryParse(text, out var version) ? Format(version) : null;
}
