using System.Drawing.Drawing2D;
using System.Runtime.InteropServices;
using NetConditioner.Models;
using NetConditioner.Services;

namespace NetConditioner.Forms;

public sealed class MainForm : Form
{
    private static readonly Color Accent = Color.FromArgb(21, 101, 192);
    private static readonly Color Surface = Color.FromArgb(243, 247, 252);
    private static readonly Color Card = Color.White;
    private static readonly Color TextDark = Color.FromArgb(32, 33, 36);
    private static readonly Color TextMuted = Color.FromArgb(104, 107, 112);
    private static readonly Color Green = Color.FromArgb(46, 125, 50);
    private static readonly Color Red = Color.FromArgb(198, 40, 40);
    private static readonly Color Orange = Color.FromArgb(239, 108, 0);

    private readonly ProfileStore _store = new();
    private readonly NetworkConditioner _conditioner = new();
    private readonly List<IntPtr> _iconHandles = new();

    private AppConfig _config;
    private string? _loadWarning;
    private GlobalHotkey? _hotkey;
    private bool _hotkeyRegistered;
    private string? _hotkeyError;

    private NotifyIcon? _tray;
    private ContextMenuStrip? _trayMenu;
    private Icon? _normalIcon;
    private Icon? _activeIcon;
    private ToolStripMenuItem? _toggleMenuItem;

    private readonly System.Windows.Forms.Timer _statsTimer = new() { Interval = 500 };

    private bool _isRunning;
    private bool _isBusy;
    private bool _exitRequested;
    private bool _hideBalloonShown;
    private bool _remoteWarningShown;
    private bool _suppressProfileChange;
    private DateTime _runStartedAt;
    private DateTime? _autoStopAt;

    private readonly Label _statusDot = new();
    private readonly Label _statusLabel = new();
    private readonly Label _statusDetail = new();
    private readonly ComboBox _profileCombo = new();
    private readonly Button _newButton = new();
    private readonly Button _editButton = new();
    private readonly Button _deleteButton = new();
    private readonly Button _toggleButton = new();
    private readonly Label _hotkeyLabel = new();
    private readonly LinkLabel _settingsLink = new();
    private readonly Label _uplinkStatsLabel = new();
    private readonly Label _downlinkStatsLabel = new();
    private readonly Button _minimizeButton = new();
    private readonly Button _exitButton = new();

    public MainForm()
    {
        _config = _store.Load(out _loadWarning);
        InitializeComponent();
        InitializeTray();

        _conditioner.Faulted += OnConditionerFaulted;
        _statsTimer.Tick += (_, _) => UpdateStats();

        BindProfiles();
        UpdateUi();
        UpdateStats();
    }

    private void InitializeComponent()
    {
        SuspendLayout();

        Text = "弱网模拟器";
        ClientSize = new Size(560, 540);
        FormBorderStyle = FormBorderStyle.FixedSingle;
        MaximizeBox = false;
        StartPosition = FormStartPosition.CenterScreen;
        BackColor = Surface;
        Font = new Font("Microsoft YaHei UI", 9F, FontStyle.Regular, GraphicsUnit.Point);
        AutoScaleMode = AutoScaleMode.Dpi;

        var header = new Panel
        {
            Location = new Point(0, 0),
            Size = new Size(560, 92),
            BackColor = Accent,
        };
        header.Controls.Add(new Label
        {
            Text = "弱网模拟器",
            Font = new Font("Microsoft YaHei UI", 17F, FontStyle.Bold),
            ForeColor = Color.White,
            AutoSize = true,
            Location = new Point(20, 15),
        });
        header.Controls.Add(new Label
        {
            Text = "系统级上行 / 下行弱网控制 · 默认热键 Ctrl + Alt + U",
            Font = new Font("Microsoft YaHei UI", 8.5F),
            ForeColor = Color.FromArgb(187, 222, 251),
            AutoSize = true,
            Location = new Point(22, 54),
        });
        Controls.Add(header);

        var statusCard = new Panel
        {
            Location = new Point(20, 112),
            Size = new Size(520, 92),
            BackColor = Card,
            BorderStyle = BorderStyle.FixedSingle,
        };
        _statusDot.Text = "●";
        _statusDot.Font = new Font("Segoe UI", 28F);
        _statusDot.ForeColor = Color.Gray;
        _statusDot.TextAlign = ContentAlignment.MiddleCenter;
        _statusDot.AutoSize = false;
        _statusDot.Location = new Point(8, 8);
        _statusDot.Size = new Size(62, 70);
        statusCard.Controls.Add(_statusDot);

        _statusLabel.Text = "未连接";
        _statusLabel.Font = new Font("Microsoft YaHei UI", 16F, FontStyle.Bold);
        _statusLabel.ForeColor = TextDark;
        _statusLabel.AutoSize = false;
        _statusLabel.Location = new Point(78, 16);
        _statusLabel.Size = new Size(425, 31);
        statusCard.Controls.Add(_statusLabel);

        _statusDetail.Text = "网络正常，未应用任何弱网参数";
        _statusDetail.Font = new Font("Microsoft YaHei UI", 8.5F);
        _statusDetail.ForeColor = TextMuted;
        _statusDetail.AutoSize = false;
        _statusDetail.Location = new Point(80, 50);
        _statusDetail.Size = new Size(420, 26);
        statusCard.Controls.Add(_statusDetail);
        Controls.Add(statusCard);

        Controls.Add(new Label
        {
            Text = "当前配置",
            Font = new Font("Microsoft YaHei UI", 9F, FontStyle.Bold),
            ForeColor = TextDark,
            AutoSize = true,
            Location = new Point(20, 218),
        });

        _profileCombo.Location = new Point(20, 240);
        _profileCombo.Size = new Size(322, 28);
        _profileCombo.DropDownStyle = ComboBoxStyle.DropDownList;
        _profileCombo.DisplayMember = nameof(NetworkProfile.Name);
        _profileCombo.SelectedIndexChanged += ProfileCombo_SelectedIndexChanged;
        Controls.Add(_profileCombo);

        ConfigureSmallButton(_newButton, "新建", new Point(350, 239), 56);
        _newButton.Click += (_, _) => CreateProfile();
        Controls.Add(_newButton);

        ConfigureSmallButton(_editButton, "编辑", new Point(412, 239), 56);
        _editButton.Click += async (_, _) => await EditProfileAsync();
        Controls.Add(_editButton);

        ConfigureSmallButton(_deleteButton, "删除", new Point(474, 239), 56);
        _deleteButton.Click += (_, _) => DeleteProfile();
        Controls.Add(_deleteButton);

        _toggleButton.Location = new Point(20, 284);
        _toggleButton.Size = new Size(520, 50);
        _toggleButton.FlatStyle = FlatStyle.Flat;
        _toggleButton.FlatAppearance.BorderSize = 0;
        _toggleButton.Font = new Font("Microsoft YaHei UI", 12.5F, FontStyle.Bold);
        _toggleButton.ForeColor = Color.White;
        _toggleButton.Cursor = Cursors.Hand;
        _toggleButton.Text = "启动弱网";
        _toggleButton.BackColor = Green;
        _toggleButton.Click += async (_, _) => await ToggleAsync();
        Controls.Add(_toggleButton);

        Controls.Add(new Label
        {
            Text = "全局快捷键",
            ForeColor = TextMuted,
            AutoSize = true,
            Location = new Point(20, 350),
        });

        _hotkeyLabel.Text = "Ctrl + Alt + U";
        _hotkeyLabel.Font = new Font("Microsoft YaHei UI", 9F, FontStyle.Bold);
        _hotkeyLabel.ForeColor = TextDark;
        _hotkeyLabel.AutoSize = true;
        _hotkeyLabel.Location = new Point(100, 350);
        Controls.Add(_hotkeyLabel);

        _settingsLink.Text = "设置";
        _settingsLink.AutoSize = true;
        _settingsLink.Location = new Point(486, 350);
        _settingsLink.LinkColor = Color.FromArgb(21, 101, 192);
        _settingsLink.Click += (_, _) => ChangeSettings();
        Controls.Add(_settingsLink);

        var statsCard = new Panel
        {
            Location = new Point(20, 380),
            Size = new Size(520, 58),
            BackColor = Card,
            BorderStyle = BorderStyle.FixedSingle,
        };
        _uplinkStatsLabel.Text = "上行   发送 0   丢弃 0   队列 0";
        _uplinkStatsLabel.ForeColor = TextDark;
        _uplinkStatsLabel.AutoSize = true;
        _uplinkStatsLabel.Location = new Point(12, 8);
        statsCard.Controls.Add(_uplinkStatsLabel);

        _downlinkStatsLabel.Text = "下行   发送 0   丢弃 0   队列 0";
        _downlinkStatsLabel.ForeColor = TextDark;
        _downlinkStatsLabel.AutoSize = true;
        _downlinkStatsLabel.Location = new Point(12, 31);
        statsCard.Controls.Add(_downlinkStatsLabel);
        Controls.Add(statsCard);

        Controls.Add(new Label
        {
            Text = "提示：上行丢包 / 限速也会影响下载 ACK；停止或退出程序会立即恢复网络。",
            Font = new Font("Microsoft YaHei UI", 8F),
            ForeColor = Color.FromArgb(126, 87, 0),
            AutoSize = false,
            Location = new Point(20, 448),
            Size = new Size(520, 28),
        });

        ConfigureFooterButton(_minimizeButton, "最小化到托盘", new Point(20, 486), 140);
        _minimizeButton.Click += (_, _) => HideToTray();
        Controls.Add(_minimizeButton);

        ConfigureFooterButton(_exitButton, "退出", new Point(400, 486), 140);
        _exitButton.Click += (_, _) => ExitApp();
        Controls.Add(_exitButton);

        Shown += MainForm_Shown;
        ResumeLayout(false);
    }

    private static void ConfigureSmallButton(Button button, string text, Point location, int width)
    {
        button.Text = text;
        button.Location = location;
        button.Size = new Size(width, 28);
        button.FlatStyle = FlatStyle.Flat;
        button.BackColor = Color.White;
        button.ForeColor = Color.FromArgb(60, 64, 67);
        button.FlatAppearance.BorderColor = Color.FromArgb(200, 203, 207);
    }

    private static void ConfigureFooterButton(Button button, string text, Point location, int width)
    {
        button.Text = text;
        button.Location = location;
        button.Size = new Size(width, 30);
        button.FlatStyle = FlatStyle.Flat;
        button.BackColor = Color.White;
        button.ForeColor = Color.FromArgb(60, 64, 67);
        button.FlatAppearance.BorderColor = Color.FromArgb(200, 203, 207);
    }

    private void InitializeTray()
    {
        _normalIcon = CreateStatusIcon(Color.FromArgb(76, 175, 80));
        _activeIcon = CreateStatusIcon(Orange);

        _trayMenu = new ContextMenuStrip();
        var showItem = new ToolStripMenuItem("显示主窗口");
        showItem.Click += (_, _) => ShowFromTray();
        _trayMenu.Items.Add(showItem);

        _toggleMenuItem = new ToolStripMenuItem("启动弱网");
        _toggleMenuItem.Click += async (_, _) => await ToggleAsync();
        _trayMenu.Items.Add(_toggleMenuItem);

        _trayMenu.Items.Add(new ToolStripSeparator());
        var settingsItem = new ToolStripMenuItem("设置");
        settingsItem.Click += (_, _) => ChangeSettings();
        _trayMenu.Items.Add(settingsItem);

        _trayMenu.Items.Add(new ToolStripSeparator());
        var exitItem = new ToolStripMenuItem("退出");
        exitItem.Click += (_, _) => ExitApp();
        _trayMenu.Items.Add(exitItem);

        _tray = new NotifyIcon
        {
            Icon = _normalIcon,
            Text = "弱网模拟器 - 未连接",
            Visible = true,
            ContextMenuStrip = _trayMenu,
        };
        _tray.DoubleClick += (_, _) => ShowFromTray();
    }

    private Icon CreateStatusIcon(Color color)
    {
        using var bitmap = new Bitmap(32, 32);
        using var graphics = Graphics.FromImage(bitmap);
        graphics.SmoothingMode = SmoothingMode.AntiAlias;
        graphics.Clear(Color.Transparent);
        using (var brush = new SolidBrush(color))
            graphics.FillEllipse(brush, 3, 3, 26, 26);
        using (var pen = new Pen(Color.White, 3))
            graphics.DrawEllipse(pen, 4, 4, 24, 24);

        var handle = bitmap.GetHicon();
        _iconHandles.Add(handle);
        return Icon.FromHandle(handle);
    }

    protected override void OnHandleCreated(EventArgs e)
    {
        base.OnHandleCreated(e);
        _hotkey = new GlobalHotkey(Handle);
        _hotkeyRegistered = _hotkey.TryRegister(
            _config.Hotkey.Modifiers, _config.Hotkey.Key, out _hotkeyError);
        UpdateUi();
    }

    protected override void OnHandleDestroyed(EventArgs e)
    {
        _hotkey?.Dispose();
        _hotkey = null;
        base.OnHandleDestroyed(e);
    }

    protected override void WndProc(ref Message m)
    {
        const int WmHotkey = 0x0312;
        if (m.Msg == WmHotkey && m.WParam.ToInt32() == GlobalHotkey.HotkeyId)
        {
            _ = ToggleAsync();
            return;
        }
        base.WndProc(ref m);
    }

    private void MainForm_Shown(object? sender, EventArgs e)
    {
        if (!string.IsNullOrWhiteSpace(_loadWarning))
        {
            MessageBox.Show(this, _loadWarning, "配置提示", MessageBoxButtons.OK,
                MessageBoxIcon.Warning);
        }

        if (!_hotkeyRegistered)
        {
            MessageBox.Show(this,
                $"全局快捷键注册失败：{_hotkeyError}\r\n你也可以用窗口按钮启动/停止。",
                "快捷键", MessageBoxButtons.OK, MessageBoxIcon.Warning);
        }
    }

    private void BindProfiles()
    {
        _suppressProfileChange = true;
        try
        {
            _profileCombo.Items.Clear();
            foreach (var profile in _config.Profiles)
                _profileCombo.Items.Add(profile);

            var active = _config.FindActiveProfile();
            if (active is not null)
            {
                _profileCombo.SelectedItem = active;
                _config.ActiveProfileId = active.Id;
            }
        }
        finally
        {
            _suppressProfileChange = false;
        }
    }

    private NetworkProfile? ActiveProfile => _profileCombo.SelectedItem as NetworkProfile;

    private void ProfileCombo_SelectedIndexChanged(object? sender, EventArgs e)
    {
        if (_suppressProfileChange)
            return;

        if (ActiveProfile is { } profile)
        {
            _config.ActiveProfileId = profile.Id;
            SaveConfig();
        }
    }

    private void SaveConfig()
    {
        try
        {
            _store.Save(_config);
        }
        catch (Exception ex)
        {
            MessageBox.Show(this, $"保存配置失败：{ex.Message}", "NetConditioner",
                MessageBoxButtons.OK, MessageBoxIcon.Error);
        }
    }

    private async Task ToggleAsync()
    {
        if (_isBusy)
            return;

        if (_isRunning)
            await StopAsync();
        else
            await StartAsync();
    }

    private async Task StartAsync()
    {
        if (_isBusy || _isRunning)
            return;

        var profile = ActiveProfile;
        if (profile is null)
            return;

        var validationError = profile.Validate();
        if (validationError is not null)
        {
            MessageBox.Show(this, validationError, "配置无效",
                MessageBoxButtons.OK, MessageBoxIcon.Warning);
            return;
        }

        if (profile.IsAllZero &&
            MessageBox.Show(this, "当前配置所有参数均为 0，启动后不会产生弱网效果。仍要启动吗？",
                "配置为空", MessageBoxButtons.YesNo, MessageBoxIcon.Question) != DialogResult.Yes)
        {
            return;
        }

        if (IsRemoteSession() && !_remoteWarningShown)
        {
            _remoteWarningShown = true;
            if (MessageBox.Show(this,
                    "当前是远程桌面会话。开启弱网（尤其是高丢包/低带宽）可能导致远程连接卡顿或断开。\r\n\r\n" +
                    "建议先设置“最长运行时间”，确保断线后能自动恢复。仍要启动吗？",
                    "远程会话提醒", MessageBoxButtons.YesNo, MessageBoxIcon.Warning) != DialogResult.Yes)
            {
                return;
            }
        }

        _isBusy = true;
        UpdateUi();
        try
        {
            await Task.Run(() => _conditioner.Start(
                profile, maxQueueDelayMs: _config.Settings.MaxQueueDelayMs));
            if (_exitRequested || IsDisposed || Disposing)
            {
                _conditioner.Stop();
                return;
            }

            _isRunning = true;
            _runStartedAt = DateTime.UtcNow;
            ScheduleAutoStop();
            ResetStats();
            _statsTimer.Start();
            ShowBalloon("弱网模拟已启动", $"配置：{profile.Name}");
        }
        catch (Exception ex)
        {
            _conditioner.Stop();
            MessageBox.Show(this,
                $"启动弱网模拟失败：\r\n{ex.Message}",
                "NetConditioner", MessageBoxButtons.OK, MessageBoxIcon.Error);
        }
        finally
        {
            _isBusy = false;
            UpdateUi();
            UpdateStats();
        }
    }

    private async Task StopAsync(bool silent = false)
    {
        if (!_isRunning || _isBusy)
            return;

        _isBusy = true;
        UpdateUi();
        try
        {
            await Task.Run(() => _conditioner.Stop());
        }
        catch (Exception ex)
        {
            if (!silent)
            {
                MessageBox.Show(this, $"停止弱网模拟时出错：{ex.Message}", "NetConditioner",
                    MessageBoxButtons.OK, MessageBoxIcon.Error);
            }
        }
        finally
        {
            _isRunning = false;
            _isBusy = false;
            _autoStopAt = null;
            _statsTimer.Stop();
            UpdateUi();
            UpdateStats();
            if (!silent)
                ShowBalloon("已恢复网络", "弱网模拟已停止");
        }
    }

    private void ScheduleAutoStop()
    {
        var minutes = Math.Max(0, _config.Settings.MaxRunMinutes);
        _autoStopAt = minutes > 0 ? _runStartedAt.AddMinutes(minutes) : null;
    }

    private void UpdateStats()
    {
        if (IsDisposed || Disposing || !IsHandleCreated)
            return;

        if (_isRunning)
        {
            var up = _conditioner.UplinkStats;
            var down = _conditioner.DownlinkStats;
            _uplinkStatsLabel.Text = up is null
                ? "上行   发送 0   丢弃 0   队列 0"
                : $"上行   发送 {up.Sent}   丢弃 {up.Dropped}   队列 {up.QueueCount}";
            _downlinkStatsLabel.Text = down is null
                ? "下行   发送 0   丢弃 0   队列 0"
                : $"下行   发送 {down.Sent}   丢弃 {down.Dropped}   队列 {down.QueueCount}";

            if (_autoStopAt is { } autoStopAt && DateTime.UtcNow >= autoStopAt)
            {
                _autoStopAt = null;
                _ = StopAsync(silent: true);
                ShowBalloon("已自动恢复网络", "已达到最长运行时间", ToolTipIcon.Info);
            }
        }
    }

    private void ResetStats()
    {
        _conditioner.UplinkStats?.Reset();
        _conditioner.DownlinkStats?.Reset();
        _uplinkStatsLabel.Text = "上行   发送 0   丢弃 0   队列 0";
        _downlinkStatsLabel.Text = "下行   发送 0   丢弃 0   队列 0";
    }

    private void UpdateUi()
    {
        if (IsDisposed || Disposing)
            return;

        if (_isBusy)
        {
            _toggleButton.Text = "正在应用…";
            _toggleButton.BackColor = Color.FromArgb(158, 158, 158);
            _toggleButton.Enabled = false;
            _statusDot.ForeColor = Orange;
            _statusLabel.Text = "正在切换…";
            _statusDetail.Text = "请稍候";
        }
        else if (_isRunning)
        {
            _toggleButton.Text = "停止并恢复";
            _toggleButton.BackColor = Red;
            _toggleButton.Enabled = true;
            _statusDot.ForeColor = Green;
            _statusLabel.Text = $"已连接：{ActiveProfile?.Name ?? "配置"}";
            _statusDetail.Text = ActiveProfile is { } active
                ? $"弱网参数生效：{FormatProcessScope(active)}"
                : "弱网参数正在全系统生效";
        }
        else
        {
            _toggleButton.Text = "启动弱网";
            _toggleButton.BackColor = Green;
            _toggleButton.Enabled = true;
            _statusDot.ForeColor = Color.Gray;
            _statusLabel.Text = "未连接";
            _statusDetail.Text = "网络正常，未应用任何弱网参数";
        }

        _toggleButton.FlatAppearance.MouseOverBackColor =
            _isRunning ? Color.FromArgb(183, 28, 28) : Color.FromArgb(27, 94, 32);

        _profileCombo.Enabled = !_isRunning && !_isBusy;
        _newButton.Enabled = !_isRunning && !_isBusy;
        _editButton.Enabled = !_isBusy;
        _deleteButton.Enabled = !_isRunning && !_isBusy;
        _settingsLink.Enabled = !_isBusy;

        if (_toggleMenuItem is not null)
            _toggleMenuItem.Text = _isRunning ? "停止并恢复" : "启动弱网";

        if (_tray is not null)
        {
            _tray.Icon = _isRunning ? _activeIcon : _normalIcon;
            _tray.Text = _isRunning ? "弱网模拟器 - 已连接" : "弱网模拟器 - 未连接";
        }

        _hotkeyLabel.Text = _hotkeyRegistered
            ? HotkeyFormatter.Format(_config.Hotkey.Modifiers, _config.Hotkey.Key)
            : "未注册（点击设置）";
    }

    private void CreateProfile()
    {
        if (_isRunning || _isBusy)
            return;

        var names = _config.Profiles.Select(p => p.Name);
        using var dialog = new ProfileEditorForm(null, names, allowDelete: false);
        if (dialog.ShowDialog(this) != DialogResult.OK || dialog.Deleted)
            return;

        _config.Profiles.Add(dialog.Profile);
        _config.ActiveProfileId = dialog.Profile.Id;
        SaveConfig();
        BindProfiles();
        UpdateUi();
    }

    private async Task EditProfileAsync()
    {
        if (_isBusy || ActiveProfile is not { } current)
            return;

        var wasRunning = _isRunning;
        var otherNames = _config.Profiles
            .Where(p => p.Id != current.Id)
            .Select(p => p.Name);

        using var dialog = new ProfileEditorForm(
            current.Clone(), otherNames, allowDelete: !wasRunning);
        if (dialog.ShowDialog(this) != DialogResult.OK)
            return;

        if (dialog.Deleted)
        {
            DeleteProfileById(current.Id);
            return;
        }

        var index = _config.Profiles.FindIndex(p => p.Id == current.Id);
        if (index < 0)
            return;

        var updated = dialog.Profile;
        updated.Id = current.Id;
        _config.Profiles[index] = updated;
        _config.ActiveProfileId = updated.Id;
        SaveConfig();
        BindProfiles();
        UpdateUi();

        if (wasRunning)
        {
            await StopAsync(silent: true);
            await StartAsync();
        }
    }

    private void DeleteProfile()
    {
        if (_isRunning || _isBusy || ActiveProfile is not { } current)
            return;

        if (MessageBox.Show(this, $"确定删除配置“{current.Name}”吗？", "删除配置",
                MessageBoxButtons.YesNo, MessageBoxIcon.Question) != DialogResult.Yes)
        {
            return;
        }

        DeleteProfileById(current.Id);
    }

    private void DeleteProfileById(string id)
    {
        if (_isRunning || _isBusy)
            return;

        var index = _config.Profiles.FindIndex(p => p.Id == id);
        if (index < 0)
            return;

        _config.Profiles.RemoveAt(index);
        if (_config.Profiles.Count == 0)
        {
            var fallback = new NetworkProfile { Name = "默认配置" };
            _config.Profiles.Add(fallback);
            _config.ActiveProfileId = fallback.Id;
        }
        else if (_config.ActiveProfileId == id)
        {
            _config.ActiveProfileId = _config.Profiles[0].Id;
        }

        SaveConfig();
        BindProfiles();
        UpdateUi();
    }

    private void ChangeSettings()
    {
        if (_isBusy)
            return;

        var oldHotkey = _config.Hotkey.Clone();
        _hotkey?.Unregister();
        _hotkeyRegistered = false;

        using var dialog = new SettingsForm(
            oldHotkey, _config.Settings.MaxRunMinutes, _config.Settings.MaxQueueDelayMs);
        if (dialog.ShowDialog(this) != DialogResult.OK)
        {
            _hotkeyRegistered = _hotkey?.TryRegister(
                oldHotkey.Modifiers, oldHotkey.Key, out _hotkeyError) ?? false;
            UpdateUi();
            return;
        }

        if (_hotkey is not null &&
            !_hotkey.TryRegister(dialog.ResultHotkey.Modifiers, dialog.ResultHotkey.Key,
                out _hotkeyError))
        {
            _hotkeyRegistered = _hotkey.TryRegister(
                oldHotkey.Modifiers, oldHotkey.Key, out _);
            MessageBox.Show(this,
                $"新的快捷键无法注册：{_hotkeyError}\r\n" +
                (_hotkeyRegistered ? "已保留原快捷键。" : "原快捷键也无法重新注册，请尽快重新设置。"),
                "快捷键", MessageBoxButtons.OK, MessageBoxIcon.Warning);
            UpdateUi();
            return;
        }

        _hotkeyRegistered = true;
        _config.Hotkey = dialog.ResultHotkey;
        _config.Settings.MaxRunMinutes = dialog.ResultMaxRunMinutes;
        _config.Settings.MaxQueueDelayMs = dialog.ResultMaxQueueDelayMs;
        if (_isRunning)
            ScheduleAutoStop();
        SaveConfig();
        UpdateUi();
    }

    private void OnConditionerFaulted(string message)
    {
        if (!IsHandleCreated || IsDisposed)
            return;

        BeginInvoke(new Action(async () =>
        {
            if (!_isRunning)
                return;

            await StopAsync(silent: true);
            ShowBalloon("弱网模拟已停止", "引擎错误，网络已恢复", ToolTipIcon.Error);
            MessageBox.Show(this, message, "弱网模拟已停止",
                MessageBoxButtons.OK, MessageBoxIcon.Error);
        }));
    }

    private void ShowBalloon(string title, string text, ToolTipIcon icon = ToolTipIcon.Info)
    {
        try
        {
            _tray?.ShowBalloonTip(2500, title, text, icon);
        }
        catch
        {
            // 托盘提示失败不影响主流程。
        }
    }

    private void ShowFromTray()
    {
        ShowInTaskbar = true;
        Show();
        WindowState = FormWindowState.Normal;
        Activate();
        BringToFront();
    }

    private void HideToTray()
    {
        Hide();
        ShowInTaskbar = false;
        if (!_hideBalloonShown)
        {
            _hideBalloonShown = true;
            ShowBalloon("仍在后台运行", "点击托盘图标可重新打开；退出请使用托盘菜单。");
        }
    }

    private void ExitApp()
    {
        _exitRequested = true;
        _statsTimer.Stop();
        StopEngineSync();
        Close();
    }

    private void StopEngineSync()
    {
        try
        {
            _conditioner.Stop();
        }
        catch
        {
            // 进程退出时 WinDivert 句柄也会被系统关闭。
        }
        _isRunning = false;
    }

    protected override void OnFormClosing(FormClosingEventArgs e)
    {
        if (!_exitRequested && e.CloseReason == CloseReason.UserClosing)
        {
            e.Cancel = true;
            HideToTray();
            return;
        }

        StopEngineSync();
        base.OnFormClosing(e);
    }

    protected override void OnFormClosed(FormClosedEventArgs e)
    {
        _statsTimer.Dispose();
        _hotkey?.Dispose();
        _hotkey = null;

        if (_tray is not null)
        {
            _tray.Visible = false;
            _tray.Dispose();
            _tray = null;
        }
        _trayMenu?.Dispose();
        _trayMenu = null;

        _normalIcon?.Dispose();
        _activeIcon?.Dispose();
        foreach (var handle in _iconHandles)
            DestroyIcon(handle);
        _iconHandles.Clear();

        _conditioner.Dispose();
        base.OnFormClosed(e);
    }

    private static bool IsRemoteSession() => GetSystemMetrics(0x1000) != 0;

    private static string FormatProcessScope(NetworkProfile profile)
    {
        var names = profile.ProcessNames;
        if (names is not { Count: > 0 })
            return "全部进程（全局）";
        if (names.Count == 1)
            return names[0];
        if (names.Count <= 3)
            return string.Join("、", names);
        return $"{names[0]} 等 {names.Count} 个进程";
    }

    [DllImport("user32.dll")]
    private static extern int GetSystemMetrics(int nIndex);

    [DllImport("user32.dll", SetLastError = true)]
    private static extern bool DestroyIcon(IntPtr hIcon);
}
