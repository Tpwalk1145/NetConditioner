using System.Globalization;
using System.Net.Http;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using Microsoft.Win32;
using NetConditioner.Models;

namespace NetConditioner.Services;

public enum UpdateCheckStatus
{
    UpToDate,
    UpdateAvailable,
    NotConfigured,
    Failed,
}

public sealed class UpdateCheckResult
{
    public UpdateCheckStatus Status { get; init; }
    public UpdateManifest? Manifest { get; init; }
    public string? Error { get; init; }

    public static UpdateCheckResult UpToDate() => new() { Status = UpdateCheckStatus.UpToDate };

    public static UpdateCheckResult Available(UpdateManifest manifest) =>
        new() { Status = UpdateCheckStatus.UpdateAvailable, Manifest = manifest };

    public static UpdateCheckResult NotConfigured(string error) =>
        new() { Status = UpdateCheckStatus.NotConfigured, Error = error };

    public static UpdateCheckResult Failed(string error) =>
        new() { Status = UpdateCheckStatus.Failed, Error = error };
}

/// <summary>
/// 云更新的网络与安装部分：拉取 latest.json、下载安装包、校验 SHA256、静默启动安装包。
/// 所有失败只写日志，不弹任何界面。
/// </summary>
public sealed class UpdateService
{
    /// <summary>与 installer\NetConditioner.iss 中的 AppId 保持一致（Inno 会写成 {AppId}_is1）。</summary>
    private const string InnoAppId = "7E1F4A9C-3D2B-4B8E-9F6A-1C5D2E8B7A34";

    public const int MaxManifestBytes = 64 * 1024;
    public const long MaxInstallerBytes = 300L * 1024 * 1024;

    private static readonly TimeSpan CheckTimeout = TimeSpan.FromSeconds(10);
    private static readonly TimeSpan DownloadTimeout = TimeSpan.FromMinutes(5);
    private static readonly object LogLock = new();
    private static readonly HttpClient Client = CreateClient();

    /// <summary>开发者/CI 钩子：只记录安装命令，不真正运行安装包、也不退出程序。</summary>
    public static bool IsDryRun => string.Equals(
        Environment.GetEnvironmentVariable("NC_UPDATE_DRY_RUN"), "1", StringComparison.Ordinal);

    private const string ApplyUpdateScript = """
        @echo off
        ping -n 3 127.0.0.1 >nul
        "%NC_SETUP%" /VERYSILENT /SUPPRESSMSGBOXES /NORESTART /SP- /LOG="%NC_LOG%"
        if exist "%NC_APP%" start "" "%NC_APP%"
        del "%~f0" >nul 2>&1
        """;

    public static string UpdateDirectory =>
        Path.Combine(Path.GetTempPath(), "NetConditioner", "update");

    public static string LogPath => Path.Combine(UpdateDirectory, "update.log");

    public static string InstallerPath(string version) =>
        Path.Combine(UpdateDirectory, $"NetConditioner-{version}-win-x64-Setup.exe");

    private static HttpClient CreateClient()
    {
        var client = new HttpClient(new HttpClientHandler { AllowAutoRedirect = true })
        {
            Timeout = Timeout.InfiniteTimeSpan,
        };
        client.DefaultRequestHeaders.UserAgent.ParseAdd("NetConditioner-Updater/1.0");
        return client;
    }

    /// <summary>更新地址必须是 https；本机回环地址允许 http，便于离线联调。</summary>
    public static bool TryValidateFeedUrl(string? url, out string error)
    {
        error = string.Empty;
        if (string.IsNullOrWhiteSpace(url))
        {
            error = "未配置更新地址。";
            return false;
        }

        var text = url.Trim();
        if (text.Length > 2048)
        {
            error = "更新地址过长。";
            return false;
        }

        if (!Uri.TryCreate(text, UriKind.Absolute, out var uri) || string.IsNullOrEmpty(uri.Host))
        {
            error = "更新地址不是有效的绝对 URL。";
            return false;
        }

        if (uri.Scheme == Uri.UriSchemeHttps)
            return true;

        if (uri.Scheme == Uri.UriSchemeHttp && uri.IsLoopback)
            return true;

        error = "更新地址必须使用 https（本机回环地址可用 http 调试）。";
        return false;
    }

    public static bool TryNormalizeSha256(string? value, out string hex)
    {
        hex = string.Empty;
        if (string.IsNullOrWhiteSpace(value))
            return false;

        var text = value.Trim();
        if (text.StartsWith("sha256:", StringComparison.OrdinalIgnoreCase))
            text = text[7..].Trim();

        if (text.Length != 64)
            return false;

        foreach (var ch in text)
        {
            if (!Uri.IsHexDigit(ch))
                return false;
        }

        hex = text.ToUpperInvariant();
        return true;
    }

    public static string ComputeSha256Hex(ReadOnlySpan<byte> data) =>
        Convert.ToHexString(SHA256.HashData(data));

    /// <summary>解析并校验 latest.json；version 为规范化后的 4 段版本。</summary>
    public static bool TryParseManifest(
        string json,
        out UpdateManifest manifest,
        out Version version,
        out string error)
    {
        manifest = null!;
        version = new Version(0, 0, 0, 0);
        error = string.Empty;

        UpdateManifest? parsed;
        try
        {
            parsed = JsonSerializer.Deserialize(json, UpdateJsonContext.Default.UpdateManifest);
        }
        catch (JsonException)
        {
            error = "更新信息不是有效的 JSON。";
            return false;
        }

        if (parsed is null)
        {
            error = "更新信息内容为空。";
            return false;
        }

        if (parsed.SchemaVersion != 1)
        {
            error = $"不支持的更新信息版本：{parsed.SchemaVersion}。";
            return false;
        }

        if (!AppVersion.TryParse(parsed.Version, out version))
        {
            error = "更新信息中的版本号无效。";
            return false;
        }

        if (parsed.Installer is null)
        {
            error = "更新信息缺少安装包信息。";
            return false;
        }

        if (!Uri.TryCreate(parsed.Installer.Url, UriKind.Absolute, out var uri))
        {
            error = "安装包地址无效。";
            return false;
        }

        if (uri.Scheme != Uri.UriSchemeHttps &&
            !(uri.Scheme == Uri.UriSchemeHttp && uri.IsLoopback))
        {
            error = "安装包地址必须使用 https。";
            return false;
        }

        if (!TryNormalizeSha256(parsed.Installer.Sha256, out var sha))
        {
            error = "更新信息中的 SHA256 无效。";
            return false;
        }

        if (parsed.Installer.Size < 0 || parsed.Installer.Size > MaxInstallerBytes)
        {
            error = "安装包大小超出允许范围。";
            return false;
        }

        if (parsed.Notes is { Length: > 4000 })
        {
            error = "更新说明过长。";
            return false;
        }

        if (!string.IsNullOrWhiteSpace(parsed.PageUrl) &&
            !Uri.TryCreate(parsed.PageUrl, UriKind.Absolute, out _))
        {
            error = "更新页面地址无效。";
            return false;
        }

        parsed.Installer.Url = uri.AbsoluteUri;
        parsed.Installer.Sha256 = sha;
        manifest = parsed;
        return true;
    }

    public async Task<UpdateCheckResult> CheckAsync(
        string feedUrl,
        Version current,
        CancellationToken cancellationToken)
    {
        try
        {
            if (!TryValidateFeedUrl(feedUrl, out var feedError))
                return UpdateCheckResult.NotConfigured(feedError);

            using var cts = CancellationTokenSource.CreateLinkedTokenSource(cancellationToken);
            cts.CancelAfter(CheckTimeout);

            using var response = await Client.GetAsync(
                feedUrl, HttpCompletionOption.ResponseHeadersRead, cts.Token);
            if (!response.IsSuccessStatusCode)
                return UpdateCheckResult.Failed($"更新服务器返回 HTTP {(int)response.StatusCode}。");

            var bytes = await ReadLimitedAsync(response.Content, MaxManifestBytes, cts.Token);
            if (bytes is null)
                return UpdateCheckResult.Failed("更新信息过大，已忽略。");

            var json = Encoding.UTF8.GetString(bytes);
            if (!TryParseManifest(json, out var manifest, out var version, out var error))
                return UpdateCheckResult.Failed(error);

            return AppVersion.IsNewer(current, version)
                ? UpdateCheckResult.Available(manifest)
                : UpdateCheckResult.UpToDate();
        }
        catch (OperationCanceledException) when (!cancellationToken.IsCancellationRequested)
        {
            return UpdateCheckResult.Failed("检查更新超时。");
        }
        catch (Exception ex)
        {
            return UpdateCheckResult.Failed("检查更新失败：" + ex.Message);
        }
    }

    /// <summary>下载安装包到 destPath，边下边算 SHA256，校验 size 与哈希后原子落盘。</summary>
    public async Task<bool> DownloadAsync(
        UpdateManifest manifest,
        string destPath,
        CancellationToken cancellationToken)
    {
        var installer = manifest.Installer;
        if (installer?.Url is null || !TryNormalizeSha256(installer.Sha256, out var expected))
        {
            Log("下载失败：安装包地址或 SHA256 无效。");
            return false;
        }

        var partPath = destPath + ".part";
        try
        {
            Directory.CreateDirectory(UpdateDirectory);
            using var cts = CancellationTokenSource.CreateLinkedTokenSource(cancellationToken);
            cts.CancelAfter(DownloadTimeout);

            using var response = await Client.GetAsync(
                installer.Url, HttpCompletionOption.ResponseHeadersRead, cts.Token);
            if (!response.IsSuccessStatusCode)
            {
                Log($"下载失败：服务器返回 HTTP {(int)response.StatusCode}。");
                return false;
            }

            long total = 0;
            var failReason = string.Empty;
            await using (var target = new FileStream(
                partPath, FileMode.Create, FileAccess.Write, FileShare.None, 64 * 1024, useAsync: true))
            await using (var source = await response.Content.ReadAsStreamAsync(cts.Token))
            {
                using var hash = IncrementalHash.CreateHash(HashAlgorithmName.SHA256);
                var buffer = new byte[64 * 1024];
                int read;
                while ((read = await source.ReadAsync(buffer, cts.Token)) > 0)
                {
                    total += read;
                    if (total > MaxInstallerBytes)
                    {
                        failReason = "安装包超过允许的最大体积。";
                        break;
                    }

                    hash.AppendData(buffer, 0, read);
                    await target.WriteAsync(buffer.AsMemory(0, read), cts.Token);
                }

                if (failReason.Length == 0)
                {
                    await target.FlushAsync(cts.Token);
                    var actual = Convert.ToHexString(hash.GetHashAndReset());
                    if (installer.Size > 0 && total != installer.Size)
                        failReason = $"文件大小 {total} 与清单 {installer.Size} 不一致。";
                    else if (!string.Equals(actual, expected, StringComparison.OrdinalIgnoreCase))
                        failReason = "SHA256 不匹配。";
                }
            }

            if (failReason.Length > 0)
            {
                TryDelete(partPath);
                Log("下载校验失败：" + failReason);
                return false;
            }

            File.Move(partPath, destPath, overwrite: true);
            Log($"下载完成并校验通过：{destPath}（{total} 字节）");
            return true;
        }
        catch (OperationCanceledException)
        {
            TryDelete(partPath);
            Log("下载被取消或超时。");
            return false;
        }
        catch (Exception ex)
        {
            TryDelete(partPath);
            Log("下载失败：" + ex.Message);
            return false;
        }
    }

    /// <summary>写一个隐藏执行的批处理，静默安装并重启新版。路径全部通过环境变量传入，批处理本身只用 ASCII。</summary>
    public bool LaunchInstallerSilently(string installerPath)
    {
        if (!File.Exists(installerPath))
        {
            Log("安装包不存在：" + installerPath);
            return false;
        }

        try
        {
            Directory.CreateDirectory(UpdateDirectory);
            var installDir = ResolveInstallDirectory();
            var appPath = Path.Combine(installDir, "NetConditioner.exe");
            var scriptPath = Path.Combine(UpdateDirectory, "apply-update.cmd");
            var installLog = Path.Combine(
                UpdateDirectory, $"install-{DateTime.Now:yyyyMMdd-HHmmss}.log");

            File.WriteAllText(scriptPath, ApplyUpdateScript.ReplaceLineEndings("\r\n"), Encoding.ASCII);

            var startInfo = new System.Diagnostics.ProcessStartInfo
            {
                FileName = "cmd.exe",
                Arguments = "/c \"\"" + scriptPath + "\"\"",
                UseShellExecute = false,
                CreateNoWindow = true,
                WorkingDirectory = UpdateDirectory,
            };
            startInfo.Environment["NC_SETUP"] = installerPath;
            startInfo.Environment["NC_APP"] = appPath;
            startInfo.Environment["NC_LOG"] = installLog;

            if (IsDryRun)
            {
                Log("DRY RUN：跳过实际安装。");
                Log($"DRY RUN 安装包：{installerPath}");
                Log($"DRY RUN 目标：{appPath}");
                Log("DRY RUN 命令：cmd.exe /c \"\"" + scriptPath + "\"\"（脚本内容见 " +
                    Path.Combine(UpdateDirectory, "apply-update.cmd") + "）");
                return true;
            }

            using var process = System.Diagnostics.Process.Start(startInfo);
            Log($"已启动静默安装：{installerPath}");
            return true;
        }
        catch (Exception ex)
        {
            Log("启动静默安装失败：" + ex.Message);
            return false;
        }
    }

    private static string ResolveInstallDirectory()
    {
        try
        {
            using var baseKey = RegistryKey.OpenBaseKey(RegistryHive.LocalMachine, RegistryView.Registry64);
            using var key = baseKey.OpenSubKey(
                "SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\" +
                "{" + InnoAppId + "}_is1");
            if (key?.GetValue("InstallLocation") is string location &&
                !string.IsNullOrWhiteSpace(location))
            {
                return location;
            }
        }
        catch
        {
            // 读注册表失败时走回退路径。
        }

        // 如果当前 exe 所在目录就是安装目录（带 unins000.exe），直接用它；
        // 否则按安装包默认目录重启，避免绿色版/开发目录反复触发更新。
        var processDirectory = Path.GetDirectoryName(Environment.ProcessPath);
        if (!string.IsNullOrWhiteSpace(processDirectory) &&
            File.Exists(Path.Combine(processDirectory, "unins000.exe")))
        {
            return processDirectory;
        }

        return Path.Combine(
            Environment.GetFolderPath(Environment.SpecialFolder.ProgramFiles), "NetConditioner");
    }

    /// <summary>清理更新目录中的旧文件（默认 7 天），应用启动时调用。</summary>
    public static void CleanupOldFiles()
    {
        try
        {
            if (!Directory.Exists(UpdateDirectory))
                return;

            var cutoff = DateTime.UtcNow.AddDays(-7);
            foreach (var file in Directory.EnumerateFiles(UpdateDirectory))
            {
                try
                {
                    if (File.GetLastWriteTimeUtc(file) < cutoff)
                        File.Delete(file);
                }
                catch
                {
                    // 单个文件失败不影响其它文件。
                }
            }
        }
        catch
        {
            // 清理失败不影响主流程。
        }
    }

    public static void Log(string message)
    {
        try
        {
            Directory.CreateDirectory(UpdateDirectory);
            var line = $"[{DateTime.Now:yyyy-MM-dd HH:mm:ss.fff}] {message}{Environment.NewLine}";
            lock (LogLock)
            {
                try
                {
                    var info = new FileInfo(LogPath);
                    if (info.Exists && info.Length > 512 * 1024)
                        File.Move(LogPath, LogPath + ".old", overwrite: true);
                }
                catch
                {
                    // 轮转失败不影响写入。
                }

                File.AppendAllText(LogPath, line, Encoding.UTF8);
            }
        }
        catch
        {
            // 日志失败不能影响更新流程。
        }
    }

    /// <summary>--check-update 诊断命令：按配置检查一次，结果写 %TEMP%\NetConditioner-update-check.log。</summary>
    public static int RunCheckCommand()
    {
        var logPath = Path.Combine(Path.GetTempPath(), "NetConditioner-update-check.log");
        var lines = new List<string>();

        void Write(string text) => lines.Add($"[{DateTime.Now:HH:mm:ss}] {text}");

        var exitCode = 1;
        try
        {
            var store = new ProfileStore();
            var config = store.Load(out _);
            var feedUrl = config.Settings.UpdateFeedUrl;

            Write($"当前版本：{AppVersion.Display}");
            Write($"更新地址：{(string.IsNullOrWhiteSpace(feedUrl) ? "(未配置)" : feedUrl)}");

            if (string.IsNullOrWhiteSpace(feedUrl))
            {
                Write("结果：未配置更新地址，云更新已关闭。");
            }
            else
            {
                var service = new UpdateService();
                var result = service.CheckAsync(feedUrl, AppVersion.Current, CancellationToken.None)
                    .GetAwaiter().GetResult();
                switch (result.Status)
                {
                    case UpdateCheckStatus.UpdateAvailable:
                        Write($"结果：发现新版本 {result.Manifest!.Version}。");
                        Write($"安装包：{result.Manifest.Installer?.Url}");
                        Write($"SHA256：{result.Manifest.Installer?.Sha256}");
                        exitCode = 0;
                        break;
                    case UpdateCheckStatus.UpToDate:
                        Write("结果：已是最新版本。");
                        exitCode = 0;
                        break;
                    case UpdateCheckStatus.NotConfigured:
                        Write("结果：未配置更新地址 - " + result.Error);
                        break;
                    default:
                        Write("结果：检查失败 - " + result.Error);
                        break;
                }
            }
        }
        catch (Exception ex)
        {
            Write("异常：" + ex);
        }

        try
        {
            File.WriteAllText(
                logPath, string.Join(Environment.NewLine, lines) + Environment.NewLine, Encoding.UTF8);
        }
        catch
        {
            // 写日志失败时只返回退出码。
        }

        return exitCode;
    }

    private static async Task<byte[]?> ReadLimitedAsync(
        HttpContent content,
        int maxBytes,
        CancellationToken cancellationToken)
    {
        if (content.Headers.ContentLength is > int.MaxValue)
            return null;

        await using var stream = await content.ReadAsStreamAsync(cancellationToken);
        using var buffer = new MemoryStream();
        var chunk = new byte[8 * 1024];
        int read;
        while ((read = await stream.ReadAsync(chunk, cancellationToken)) > 0)
        {
            if (buffer.Length + read > maxBytes)
                return null;
            buffer.Write(chunk, 0, read);
        }

        return buffer.ToArray();
    }

    private static void TryDelete(string path)
    {
        try
        {
            if (File.Exists(path))
                File.Delete(path);
        }
        catch
        {
            // ignore
        }
    }
}
