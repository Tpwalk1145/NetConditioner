using NetConditioner.Models;

namespace NetConditioner.Services;

/// <summary>更新升级需要主窗口提供的状态与操作（全部在 UI 线程调用）。</summary>
public interface IUpdateHost
{
    /// <summary>弱网未运行、没有正在切换、也没有模态对话框。</summary>
    bool IsIdle { get; }

    Task StopEngineAsync();

    void SaveConfig();

    /// <summary>启动静默安装后退出当前进程；安装完成由批处理重新启动新版。</summary>
    void ExitForUpdate();
}

/// <summary>
/// 全静默云更新状态机：按 24 小时节流检查、下载、校验，空闲时静默安装并重启。
/// 任何失败只写日志，不弹界面。
/// </summary>
public sealed class UpdateCoordinator
{
    private const long CheckIntervalSeconds = 24 * 60 * 60;
    private const long RetryAfterFailureSeconds = 24 * 60 * 60;

    private readonly UpdateService _service = new();
    private bool _busy;
    private bool _installStarted;
    private bool _deferredLogged;
    private UpdateManifest? _pendingManifest;
    private string? _pendingInstallerPath;

    public async Task TickAsync(AppConfig config, IUpdateHost host)
    {
        if (_busy || _installStarted)
            return;

        var settings = config.Settings;
        if (settings is null || !settings.AutoUpdate ||
            string.IsNullOrWhiteSpace(settings.UpdateFeedUrl))
        {
            return;
        }

        _busy = true;
        try
        {
            var now = DateTimeOffset.UtcNow.ToUnixTimeSeconds();

            if (_pendingManifest is null)
            {
                if (!IsCheckDue(settings, now))
                    return;

                UpdateService.Log($"开始检查更新：{settings.UpdateFeedUrl}");
                var result = await _service.CheckAsync(
                    settings.UpdateFeedUrl, AppVersion.Current, CancellationToken.None);

                if (result.Status is UpdateCheckStatus.NotConfigured or UpdateCheckStatus.Failed)
                {
                    UpdateService.Log("检查更新失败：" + result.Error);
                    return;
                }

                settings.LastUpdateCheckUnix = now;
                host.SaveConfig();

                if (result.Status == UpdateCheckStatus.UpToDate)
                {
                    UpdateService.Log("已是最新版本。");
                    return;
                }

                var manifest = result.Manifest!;
                if (IsRetryBlocked(settings, manifest.Version, now))
                {
                    UpdateService.Log($"版本 {manifest.Version} 最近尝试升级失败，24 小时后再试。");
                    return;
                }

                var destPath = UpdateService.InstallerPath(manifest.Version!);
                UpdateService.Log($"发现新版本 {manifest.Version}，开始下载。");
                if (!await _service.DownloadAsync(manifest, destPath, CancellationToken.None))
                {
                    settings.LastUpdateAttemptUnix = now;
                    settings.LastAttemptedVersion = manifest.Version ?? string.Empty;
                    host.SaveConfig();
                    return;
                }

                _pendingManifest = manifest;
                _pendingInstallerPath = destPath;
                UpdateService.Log($"版本 {manifest.Version} 已下载并校验，等待空闲时安装。");
            }

            if (_pendingManifest is null || _pendingInstallerPath is null)
                return;

            if (!host.IsIdle)
            {
                if (!_deferredLogged)
                {
                    _deferredLogged = true;
                    UpdateService.Log("当前弱网运行中或有窗口打开，暂不安装，等空闲后再继续。");
                }

                return;
            }

            _installStarted = true;
            _deferredLogged = false;
            UpdateService.Log($"开始静默安装版本 {_pendingManifest.Version}。");

            await host.StopEngineAsync();

            var attemptedAt = DateTimeOffset.UtcNow.ToUnixTimeSeconds();
            settings.LastUpdateAttemptUnix = attemptedAt;
            settings.LastAttemptedVersion = _pendingManifest.Version ?? string.Empty;
            host.SaveConfig();

            if (!_service.LaunchInstallerSilently(_pendingInstallerPath))
            {
                _installStarted = false;
                UpdateService.Log("启动安装失败，继续使用当前版本。");
                return;
            }

            if (UpdateService.IsDryRun)
            {
                UpdateService.Log("DRY RUN：保持当前进程运行，不重启。");
            }
            else
            {
                host.ExitForUpdate();
            }
        }
        catch (Exception ex)
        {
            UpdateService.Log("更新流程异常：" + ex.Message);
        }
        finally
        {
            _busy = false;
        }
    }

    private static bool IsCheckDue(AppSettings settings, long now)
    {
        if (settings.LastUpdateCheckUnix <= 0)
            return true;

        // 时钟被往回调过时也允许检查，避免永远不再检查。
        if (settings.LastUpdateCheckUnix > now)
            return true;

        return now - settings.LastUpdateCheckUnix >= CheckIntervalSeconds;
    }

    private static bool IsRetryBlocked(AppSettings settings, string? version, long now)
    {
        if (string.IsNullOrWhiteSpace(version))
            return false;

        if (!string.Equals(settings.LastAttemptedVersion, version, StringComparison.OrdinalIgnoreCase))
            return false;

        if (settings.LastUpdateAttemptUnix <= 0)
            return false;

        return now - settings.LastUpdateAttemptUnix < RetryAfterFailureSeconds;
    }
}
