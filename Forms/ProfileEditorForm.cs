using NetConditioner.Models;

namespace NetConditioner.Forms;

public sealed class ProfileEditorForm : Form
{
    private static readonly Color Accent = Color.FromArgb(21, 101, 192);
    private static readonly Color TextDark = Color.FromArgb(32, 33, 36);
    private static readonly Color TextMuted = Color.FromArgb(104, 107, 112);

    private readonly NetworkProfile? _existing;
    private readonly HashSet<string> _existingNames;
    private readonly bool _allowDelete;

    private readonly TextBox _nameBox = new();
    private readonly NumericUpDown _upDelay = CreateNumber(60000, 10);
    private readonly NumericUpDown _upJitter = CreateNumber(60000, 10);
    private readonly NumericUpDown _upBandwidth = CreateNumber(1_000_000, 10);
    private readonly NumericUpDown _upLoss = CreateNumber(100, 1);
    private readonly NumericUpDown _upBurstAllow = CreateNumber(600_000, 10);
    private readonly NumericUpDown _upBurstDrop = CreateNumber(600_000, 10);
    private readonly NumericUpDown _downDelay = CreateNumber(60000, 10);
    private readonly NumericUpDown _downJitter = CreateNumber(60000, 10);
    private readonly NumericUpDown _downBandwidth = CreateNumber(1_000_000, 10);
    private readonly NumericUpDown _downLoss = CreateNumber(100, 1);
    private readonly NumericUpDown _downBurstAllow = CreateNumber(600_000, 10);
    private readonly NumericUpDown _downBurstDrop = CreateNumber(600_000, 10);
    private readonly ComboBox _protocolCombo = new();
    private readonly TextBox _processBox = new();
    private readonly Button _processButton = new();

    private NetworkProfile? _result;
    private List<string> _processNames = new();

    public ProfileEditorForm(NetworkProfile? existing, IEnumerable<string> existingNames, bool allowDelete)
    {
        _existing = existing?.Clone();
        _existingNames = new HashSet<string>(
            existingNames.Where(n => !string.IsNullOrWhiteSpace(n)),
            StringComparer.OrdinalIgnoreCase);
        _allowDelete = allowDelete;

        Text = existing is null ? "新建配置" : "编辑配置";
        ClientSize = new Size(760, 646);
        FormBorderStyle = FormBorderStyle.FixedDialog;
        MaximizeBox = false;
        MinimizeBox = false;
        StartPosition = FormStartPosition.CenterParent;
        BackColor = Color.FromArgb(243, 247, 252);
        Font = new Font("Microsoft YaHei UI", 9F);
        AutoScaleMode = AutoScaleMode.Dpi;

        BuildUi();
        LoadValues();
    }

    public NetworkProfile Profile => _result ?? new NetworkProfile();

    public bool Deleted { get; private set; }

    private static NumericUpDown CreateNumber(int maximum, int increment) => new()
    {
        Minimum = 0,
        Maximum = maximum,
        Increment = increment,
        ThousandsSeparator = true,
        Width = 165,
        TextAlign = HorizontalAlignment.Right,
    };

    private void BuildUi()
    {
        var header = new Panel
        {
            Location = new Point(0, 0),
            Size = new Size(760, 58),
            BackColor = Accent,
        };
        header.Controls.Add(new Label
        {
            Text = Text,
            Font = new Font("Microsoft YaHei UI", 14F, FontStyle.Bold),
            ForeColor = Color.White,
            AutoSize = true,
            Location = new Point(20, 14),
        });
        Controls.Add(header);

        Controls.Add(new Label
        {
            Text = "名称",
            ForeColor = TextDark,
            AutoSize = true,
            Location = new Point(20, 80),
        });
        _nameBox.Location = new Point(70, 76);
        _nameBox.Size = new Size(670, 27);
        _nameBox.MaxLength = 60;
        Controls.Add(_nameBox);

        var upGroup = CreateDirectionGroup("上行", 20);
        var downGroup = CreateDirectionGroup("下行", 390);
        Controls.Add(upGroup);
        Controls.Add(downGroup);

        AddDirectionField(upGroup, 0, "上行延时 (ms)", _upDelay);
        AddDirectionField(upGroup, 1, "上行延时抖动 (ms)", _upJitter);
        AddDirectionField(upGroup, 2, "上行带宽 (kbps)", _upBandwidth);
        AddDirectionField(upGroup, 3, "上行随机丢包 (%)", _upLoss);
        AddDirectionField(upGroup, 4, "上行连续丢包-放行 (ms)", _upBurstAllow);
        AddDirectionField(upGroup, 5, "上行连续丢包-丢包 (ms)", _upBurstDrop);

        AddDirectionField(downGroup, 0, "下行延时 (ms)", _downDelay);
        AddDirectionField(downGroup, 1, "下行延时抖动 (ms)", _downJitter);
        AddDirectionField(downGroup, 2, "下行带宽 (kbps)", _downBandwidth);
        AddDirectionField(downGroup, 3, "下行随机丢包 (%)", _downLoss);
        AddDirectionField(downGroup, 4, "下行连续丢包-放行 (ms)", _downBurstAllow);
        AddDirectionField(downGroup, 5, "下行连续丢包-丢包 (ms)", _downBurstDrop);

        Controls.Add(new Label
        {
            Text = "弱网控制协议",
            ForeColor = TextDark,
            AutoSize = true,
            Location = new Point(20, 470),
        });

        _protocolCombo.Location = new Point(130, 466);
        _protocolCombo.Size = new Size(170, 28);
        _protocolCombo.DropDownStyle = ComboBoxStyle.DropDownList;
        _protocolCombo.Items.AddRange(new object[] { "TCP (1)", "UDP (2)", "ALL (3)" });
        _protocolCombo.SelectedIndex = 2;
        Controls.Add(_protocolCombo);

        Controls.Add(new Label
        {
            Text = "目标进程",
            ForeColor = TextDark,
            AutoSize = true,
            Location = new Point(20, 506),
        });

        _processBox.Location = new Point(100, 502);
        _processBox.Size = new Size(480, 27);
        _processBox.ReadOnly = true;
        _processBox.BackColor = Color.White;
        Controls.Add(_processBox);

        _processButton.Text = "选择进程…";
        _processButton.Location = new Point(590, 500);
        _processButton.Size = new Size(150, 31);
        _processButton.FlatStyle = FlatStyle.Flat;
        _processButton.BackColor = Color.White;
        _processButton.ForeColor = Color.FromArgb(60, 64, 67);
        _processButton.FlatAppearance.BorderColor = Color.FromArgb(200, 203, 207);
        _processButton.Click += (_, _) => SelectProcesses();
        Controls.Add(_processButton);

        Controls.Add(new Label
        {
            Text = "带宽单位 kbps；随机丢包单位 %；延时 / 连续丢包单位 ms；0 表示不启用。\r\n" +
                   "连续丢包：放行 X ms、丢包 Y ms 循环；只填“丢包”表示一直丢包。上行丢包/限速也会影响下载 ACK。\r\n" +
                   "目标进程不选 = 全局；选择后仅对这些程序生效（同名多开实例都生效）。",
            Font = new Font("Microsoft YaHei UI", 8F),
            ForeColor = TextMuted,
            AutoSize = false,
            Location = new Point(20, 538),
            Size = new Size(720, 58),
        });

        var cancelButton = new Button
        {
            Text = "取消",
            DialogResult = DialogResult.Cancel,
            Location = new Point(520, 604),
            Size = new Size(100, 30),
            FlatStyle = FlatStyle.Flat,
            BackColor = Color.White,
            FlatAppearance = { BorderColor = Color.FromArgb(200, 203, 207) },
        };
        Controls.Add(cancelButton);

        var okButton = new Button
        {
            Text = "确定",
            Location = new Point(630, 604),
            Size = new Size(110, 30),
            FlatStyle = FlatStyle.Flat,
            BackColor = Accent,
            ForeColor = Color.White,
            Font = new Font("Microsoft YaHei UI", 9F, FontStyle.Bold),
            FlatAppearance = { BorderSize = 0 },
        };
        okButton.Click += (_, _) => Confirm();
        Controls.Add(okButton);
        AcceptButton = okButton;
        CancelButton = cancelButton;

        if (_allowDelete)
        {
            var deleteButton = new Button
            {
                Text = "删除配置",
                Location = new Point(20, 604),
                Size = new Size(110, 30),
                FlatStyle = FlatStyle.Flat,
                BackColor = Color.FromArgb(255, 235, 238),
                ForeColor = Color.FromArgb(183, 28, 28),
                FlatAppearance = { BorderColor = Color.FromArgb(239, 154, 154) },
            };
            deleteButton.Click += (_, _) => DeleteProfile();
            Controls.Add(deleteButton);
        }
    }

    private static GroupBox CreateDirectionGroup(string title, int x) => new()
    {
        Text = title,
        Location = new Point(x, 112),
        Size = new Size(350, 340),
        ForeColor = TextDark,
    };

    private static void AddDirectionField(GroupBox group, int row, string labelText, NumericUpDown input)
    {
        var y = 28 + row * 42;
        group.Controls.Add(new Label
        {
            Text = labelText,
            ForeColor = Color.FromArgb(60, 64, 67),
            AutoSize = true,
            Location = new Point(14, y + 4),
        });
        input.Location = new Point(170, y);
        group.Controls.Add(input);
    }

    private void LoadValues()
    {
        if (_existing is null)
        {
            _nameBox.Text = MakeDefaultName();
            UpdateProcessSummary();
            return;
        }

        _nameBox.Text = _existing.Name;
        LoadDirection(_existing.Uplink, _upDelay, _upJitter, _upBandwidth, _upLoss, _upBurstAllow, _upBurstDrop);
        LoadDirection(_existing.Downlink, _downDelay, _downJitter, _downBandwidth, _downLoss, _downBurstAllow, _downBurstDrop);
        _protocolCombo.SelectedIndex = _existing.Protocol switch
        {
            ConditionProtocol.Tcp => 0,
            ConditionProtocol.Udp => 1,
            _ => 2,
        };
        _processNames = new List<string>(_existing.ProcessNames ?? new List<string>());
        UpdateProcessSummary();
    }

    private void SelectProcesses()
    {
        using var dialog = new ProcessSelectorForm(_processNames);
        if (dialog.ShowDialog(this) != DialogResult.OK)
            return;

        _processNames = dialog.SelectedNames.ToList();
        UpdateProcessSummary();
    }

    private void UpdateProcessSummary()
    {
        _processBox.Text = _processNames.Count == 0
            ? "全部进程（全局）"
            : string.Join(", ", _processNames);
        _processBox.SelectionStart = 0;
        _processBox.SelectionLength = 0;
    }

    private static void LoadDirection(
        DirectionProfile source,
        NumericUpDown delay,
        NumericUpDown jitter,
        NumericUpDown bandwidth,
        NumericUpDown loss,
        NumericUpDown burstAllow,
        NumericUpDown burstDrop)
    {
        delay.Value = Clamp(source.DelayMs, delay.Minimum, delay.Maximum);
        jitter.Value = Clamp(source.JitterMs, jitter.Minimum, jitter.Maximum);
        bandwidth.Value = Clamp(source.BandwidthKbps, bandwidth.Minimum, bandwidth.Maximum);
        loss.Value = Clamp(source.RandomLossPercent, loss.Minimum, loss.Maximum);
        burstAllow.Value = Clamp(source.BurstAllowMs, burstAllow.Minimum, burstAllow.Maximum);
        burstDrop.Value = Clamp(source.BurstDropMs, burstDrop.Minimum, burstDrop.Maximum);
    }

    private static decimal Clamp(int value, decimal min, decimal max) =>
        Math.Clamp((decimal)value, min, max);

    private string MakeDefaultName()
    {
        var baseName = "新配置";
        if (!_existingNames.Contains(baseName))
            return baseName;

        for (var i = 2; i < 1000; i++)
        {
            var candidate = $"{baseName} {i}";
            if (!_existingNames.Contains(candidate))
                return candidate;
        }
        return baseName + " " + Guid.NewGuid().ToString("N")[..6];
    }

    private void Confirm()
    {
        var name = _nameBox.Text.Trim();
        if (string.IsNullOrWhiteSpace(name))
        {
            MessageBox.Show(this, "请输入配置名称。", "配置无效",
                MessageBoxButtons.OK, MessageBoxIcon.Warning);
            _nameBox.Focus();
            return;
        }

        if (_existingNames.Contains(name))
        {
            MessageBox.Show(this, "已存在同名配置，请换一个名称。", "配置无效",
                MessageBoxButtons.OK, MessageBoxIcon.Warning);
            _nameBox.Focus();
            return;
        }

        var profile = new NetworkProfile
        {
            Id = _existing?.Id ?? Guid.NewGuid().ToString("N"),
            Name = name,
            Uplink = ReadDirection(_upDelay, _upJitter, _upBandwidth, _upLoss, _upBurstAllow, _upBurstDrop),
            Downlink = ReadDirection(_downDelay, _downJitter, _downBandwidth, _downLoss, _downBurstAllow, _downBurstDrop),
            Protocol = _protocolCombo.SelectedIndex switch
            {
                0 => ConditionProtocol.Tcp,
                1 => ConditionProtocol.Udp,
                _ => ConditionProtocol.All,
            },
            ProcessNames = _processNames.ToList(),
        };

        var error = profile.Validate();
        if (error is not null)
        {
            MessageBox.Show(this, error, "配置无效",
                MessageBoxButtons.OK, MessageBoxIcon.Warning);
            return;
        }

        if (profile.IsAllZero &&
            MessageBox.Show(this, "所有参数均为 0，启动后不会产生弱网效果。仍要保存吗？",
                "配置为空", MessageBoxButtons.YesNo, MessageBoxIcon.Question) != DialogResult.Yes)
        {
            return;
        }

        _result = profile;
        DialogResult = DialogResult.OK;
    }

    private static DirectionProfile ReadDirection(
        NumericUpDown delay,
        NumericUpDown jitter,
        NumericUpDown bandwidth,
        NumericUpDown loss,
        NumericUpDown burstAllow,
        NumericUpDown burstDrop) => new()
        {
            DelayMs = (int)delay.Value,
            JitterMs = (int)jitter.Value,
            BandwidthKbps = (int)bandwidth.Value,
            RandomLossPercent = (int)loss.Value,
            BurstAllowMs = (int)burstAllow.Value,
            BurstDropMs = (int)burstDrop.Value,
        };

    private void DeleteProfile()
    {
        if (MessageBox.Show(this, "确定删除这个配置吗？", "删除配置",
                MessageBoxButtons.YesNo, MessageBoxIcon.Question) != DialogResult.Yes)
        {
            return;
        }

        Deleted = true;
        _result = null;
        DialogResult = DialogResult.OK;
    }
}
