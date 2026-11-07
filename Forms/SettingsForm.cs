using System.Runtime.InteropServices;
using NetConditioner.Models;
using NetConditioner.Services;

namespace NetConditioner.Forms;

public sealed class SettingsForm : Form
{
    private static readonly Color Accent = Color.FromArgb(21, 101, 192);
    private static readonly Color TextDark = Color.FromArgb(32, 33, 36);
    private static readonly Color TextMuted = Color.FromArgb(104, 107, 112);

    private readonly TextBox _hotkeyBox = new();
    private readonly Label _hotkeyHint = new();
    private readonly NumericUpDown _maxRunMinutes = new();
    private readonly NumericUpDown _maxQueueDelay = new();
    private HotkeySettings _candidate;

    public SettingsForm(HotkeySettings currentHotkey, int maxRunMinutes, int maxQueueDelayMs)
    {
        _candidate = currentHotkey.Clone();

        Text = "设置";
        ClientSize = new Size(500, 330);
        FormBorderStyle = FormBorderStyle.FixedDialog;
        MaximizeBox = false;
        MinimizeBox = false;
        StartPosition = FormStartPosition.CenterParent;
        BackColor = Color.FromArgb(243, 247, 252);
        Font = new Font("Microsoft YaHei UI", 9F);
        AutoScaleMode = AutoScaleMode.Dpi;

        BuildUi();
        _maxRunMinutes.Value = Math.Clamp(maxRunMinutes, 0, 1440);
        _maxQueueDelay.Value = Math.Clamp(maxQueueDelayMs, 0, 60000);
    }

    public HotkeySettings ResultHotkey { get; private set; } = new();

    public int ResultMaxRunMinutes { get; private set; }

    public int ResultMaxQueueDelayMs { get; private set; }

    private void BuildUi()
    {
        var header = new Panel
        {
            Location = new Point(0, 0),
            Size = new Size(500, 58),
            BackColor = Accent,
        };
        header.Controls.Add(new Label
        {
            Text = "设置",
            Font = new Font("Microsoft YaHei UI", 14F, FontStyle.Bold),
            ForeColor = Color.White,
            AutoSize = true,
            Location = new Point(20, 14),
        });
        Controls.Add(header);

        Controls.Add(new Label
        {
            Text = "全局快捷键",
            ForeColor = TextDark,
            AutoSize = true,
            Location = new Point(20, 82),
        });

        _hotkeyBox.ReadOnly = true;
        _hotkeyBox.TextAlign = HorizontalAlignment.Center;
        _hotkeyBox.Font = new Font("Microsoft YaHei UI", 11F, FontStyle.Bold);
        _hotkeyBox.Location = new Point(20, 104);
        _hotkeyBox.Size = new Size(460, 32);
        _hotkeyBox.Text = HotkeyFormatter.Format(_candidate.Modifiers, _candidate.Key);
        _hotkeyBox.KeyDown += HotkeyBox_KeyDown;
        _hotkeyBox.Enter += (_, _) => _hotkeyBox.SelectAll();
        Controls.Add(_hotkeyBox);

        _hotkeyHint.Text = "可直接按单键（如 F8），也可加 Ctrl/Alt/Shift/Win；单键会全局占用该键。";
        _hotkeyHint.Font = new Font("Microsoft YaHei UI", 8F);
        _hotkeyHint.ForeColor = TextMuted;
        _hotkeyHint.AutoSize = false;
        _hotkeyHint.Location = new Point(20, 140);
        _hotkeyHint.Size = new Size(460, 22);
        Controls.Add(_hotkeyHint);

        Controls.Add(new Label
        {
            Text = "最长运行时间（分钟，0 = 不自动停止）",
            ForeColor = TextDark,
            AutoSize = true,
            Location = new Point(20, 180),
        });

        _maxRunMinutes.Minimum = 0;
        _maxRunMinutes.Maximum = 1440;
        _maxRunMinutes.Increment = 5;
        _maxRunMinutes.Location = new Point(280, 176);
        _maxRunMinutes.Size = new Size(100, 27);
        _maxRunMinutes.TextAlign = HorizontalAlignment.Right;
        Controls.Add(_maxRunMinutes);

        Controls.Add(new Label
        {
            Text = "用于远程桌面等可能被断线的场景：到时自动停止并恢复网络。",
            Font = new Font("Microsoft YaHei UI", 8F),
            ForeColor = TextMuted,
            AutoSize = false,
            Location = new Point(20, 204),
            Size = new Size(460, 20),
        });

        Controls.Add(new Label
        {
            Text = "最大额外排队时延（ms，0 = 不限制）",
            ForeColor = TextDark,
            AutoSize = true,
            Location = new Point(20, 232),
        });

        _maxQueueDelay.Minimum = 0;
        _maxQueueDelay.Maximum = 60000;
        _maxQueueDelay.Increment = 500;
        _maxQueueDelay.Location = new Point(280, 228);
        _maxQueueDelay.Size = new Size(100, 27);
        _maxQueueDelay.TextAlign = HorizontalAlignment.Right;
        Controls.Add(_maxQueueDelay);

        Controls.Add(new Label
        {
            Text = "额外排队上限；越小越不易卡顿，越大越不易丢包。修改后需重启弱网。",
            Font = new Font("Microsoft YaHei UI", 8F),
            ForeColor = TextMuted,
            AutoSize = false,
            Location = new Point(20, 258),
            Size = new Size(460, 20),
        });

        var cancelButton = new Button
        {
            Text = "取消",
            DialogResult = DialogResult.Cancel,
            Location = new Point(260, 288),
            Size = new Size(100, 30),
            FlatStyle = FlatStyle.Flat,
            BackColor = Color.White,
            FlatAppearance = { BorderColor = Color.FromArgb(200, 203, 207) },
        };
        Controls.Add(cancelButton);

        var okButton = new Button
        {
            Text = "保存",
            Location = new Point(370, 288),
            Size = new Size(110, 30),
            FlatStyle = FlatStyle.Flat,
            BackColor = Accent,
            ForeColor = Color.White,
            Font = new Font("Microsoft YaHei UI", 9F, FontStyle.Bold),
            FlatAppearance = { BorderSize = 0 },
        };
        okButton.Click += (_, _) => Save();
        Controls.Add(okButton);
        AcceptButton = okButton;
        CancelButton = cancelButton;
    }

    private void HotkeyBox_KeyDown(object? sender, KeyEventArgs e)
    {
        if (e.KeyCode == Keys.Escape)
        {
            DialogResult = DialogResult.Cancel;
            return;
        }

        e.SuppressKeyPress = true;
        e.Handled = true;

        if (HotkeyFormatter.IsModifierKey((int)e.KeyCode))
            return;

        var modifiers = 0u;
        if (e.Control) modifiers |= HotkeyDefaults.ModControl;
        if (e.Alt) modifiers |= HotkeyDefaults.ModAlt;
        if (e.Shift) modifiers |= HotkeyDefaults.ModShift;
        if (IsWinDown()) modifiers |= HotkeyDefaults.ModWin;

        if (!HotkeyFormatter.IsValid(modifiers, (int)e.KeyCode))
        {
            _hotkeyHint.ForeColor = Color.FromArgb(198, 40, 40);
            _hotkeyHint.Text = "请按一个普通按键（不能只按修饰键）；单键会全局占用该键。";
            return;
        }

        _candidate = new HotkeySettings
        {
            Modifiers = modifiers,
            Key = (int)e.KeyCode,
        };
        _hotkeyBox.Text = HotkeyFormatter.Format(_candidate.Modifiers, _candidate.Key);
        _hotkeyHint.ForeColor = TextMuted;
        _hotkeyHint.Text = "可直接按单键（如 F8），也可加 Ctrl/Alt/Shift/Win；单键会全局占用该键。";
    }

    private static bool IsWinDown() =>
        GetKeyState(0x5B) < 0 || GetKeyState(0x5C) < 0;

    private void Save()
    {
        if (!HotkeyFormatter.IsValid(_candidate.Modifiers, _candidate.Key))
        {
            MessageBox.Show(this, "请先设置一个有效的快捷键。", "设置",
                MessageBoxButtons.OK, MessageBoxIcon.Warning);
            return;
        }

        ResultHotkey = _candidate;
        ResultMaxRunMinutes = (int)_maxRunMinutes.Value;
        ResultMaxQueueDelayMs = (int)_maxQueueDelay.Value;
        DialogResult = DialogResult.OK;
    }

    [DllImport("user32.dll")]
    private static extern short GetKeyState(int nVirtKey);
}
