using System.Diagnostics;
using NetConditioner.Services;

namespace NetConditioner.Forms;

/// <summary>选择一个或多个目标进程；一个都不选表示对全部进程生效。</summary>
public sealed class ProcessSelectorForm : Form
{
    private static readonly Color Accent = Color.FromArgb(21, 101, 192);
    private static readonly Color Surface = Color.FromArgb(243, 247, 252);
    private static readonly Color TextDark = Color.FromArgb(32, 33, 36);
    private static readonly Color TextMuted = Color.FromArgb(104, 107, 112);

    private readonly ListView _list = new();
    private readonly TextBox _searchBox = new();
    private readonly TextBox _manualBox = new();
    private readonly Label _countLabel = new();
    private readonly List<ProcessEntry> _entries = new();
    private readonly HashSet<string> _selected = new(StringComparer.OrdinalIgnoreCase);
    private bool _suppressCheck;

    public ProcessSelectorForm(IEnumerable<string>? selectedNames)
    {
        if (selectedNames is not null)
        {
            foreach (var name in selectedNames)
            {
                var display = ProcessFlowFilter.NormalizeDisplayName(name);
                if (display.Length > 0)
                    _selected.Add(display);
            }
        }

        Text = "选择目标进程";
        ClientSize = new Size(640, 612);
        FormBorderStyle = FormBorderStyle.FixedDialog;
        MaximizeBox = false;
        MinimizeBox = false;
        StartPosition = FormStartPosition.CenterParent;
        BackColor = Surface;
        Font = new Font("Microsoft YaHei UI", 9F);
        AutoScaleMode = AutoScaleMode.Dpi;

        BuildUi();
        ReloadProcesses();
    }

    public IReadOnlyList<string> SelectedNames =>
        _selected.OrderBy(n => n, StringComparer.OrdinalIgnoreCase).ToList();

    private void BuildUi()
    {
        var header = new Panel
        {
            Location = new Point(0, 0),
            Size = new Size(640, 58),
            BackColor = Accent,
        };
        header.Controls.Add(new Label
        {
            Text = "选择目标进程",
            Font = new Font("Microsoft YaHei UI", 14F, FontStyle.Bold),
            ForeColor = Color.White,
            AutoSize = true,
            Location = new Point(20, 14),
        });
        Controls.Add(header);

        Controls.Add(new Label
        {
            Text = "勾选后仅对这些程序生效；不勾选任何程序 = 全局（所有进程）。\r\n" +
                   "按进程名匹配，同一程序的多开实例都会生效。",
            Font = new Font("Microsoft YaHei UI", 8.5F),
            ForeColor = TextMuted,
            AutoSize = false,
            Location = new Point(20, 70),
            Size = new Size(600, 36),
        });

        Controls.Add(new Label
        {
            Text = "搜索",
            ForeColor = TextDark,
            AutoSize = true,
            Location = new Point(20, 118),
        });

        _searchBox.Location = new Point(76, 112);
        _searchBox.Size = new Size(544, 27);
        _searchBox.TextChanged += (_, _) => RebuildItems();
        Controls.Add(_searchBox);

        _list.Location = new Point(20, 148);
        _list.Size = new Size(600, 368);
        _list.View = View.Details;
        _list.CheckBoxes = true;
        _list.FullRowSelect = true;
        _list.HeaderStyle = ColumnHeaderStyle.Nonclickable;
        _list.HideSelection = false;
        _list.MultiSelect = true;
        _list.Columns.Add("进程", 180);
        _list.Columns.Add("实例", 55);
        _list.Columns.Add("路径", 320);
        _list.ItemChecked += List_ItemChecked;
        Controls.Add(_list);

        Controls.Add(new Label
        {
            Text = "手动添加",
            ForeColor = TextDark,
            AutoSize = true,
            Location = new Point(20, 532),
        });

        _manualBox.Location = new Point(96, 526);
        _manualBox.Size = new Size(314, 27);
        _manualBox.KeyDown += (_, e) =>
        {
            if (e.KeyCode != Keys.Enter)
                return;
            e.SuppressKeyPress = true;
            AddManual();
        };
        Controls.Add(_manualBox);

        var addButton = new Button
        {
            Text = "添加",
            Location = new Point(418, 525),
            Size = new Size(64, 30),
            FlatStyle = FlatStyle.Flat,
            BackColor = Color.White,
            FlatAppearance = { BorderColor = Color.FromArgb(200, 203, 207) },
        };
        addButton.Click += (_, _) => AddManual();
        Controls.Add(addButton);

        var browseButton = new Button
        {
            Text = "浏览 EXE…",
            Location = new Point(490, 525),
            Size = new Size(130, 30),
            FlatStyle = FlatStyle.Flat,
            BackColor = Color.White,
            FlatAppearance = { BorderColor = Color.FromArgb(200, 203, 207) },
        };
        browseButton.Click += (_, _) => BrowseExe();
        Controls.Add(browseButton);

        var refreshButton = CreateDialogButton("刷新", new Point(20, 572), 88);
        refreshButton.Click += (_, _) => ReloadProcesses();
        Controls.Add(refreshButton);

        var selectAllButton = CreateDialogButton("全选", new Point(116, 572), 80);
        selectAllButton.Click += (_, _) => SelectAllVisible();
        Controls.Add(selectAllButton);

        var clearButton = CreateDialogButton("清除", new Point(204, 572), 80);
        clearButton.Click += (_, _) => ClearSelection();
        Controls.Add(clearButton);

        _countLabel.Text = "当前：全局";
        _countLabel.ForeColor = TextMuted;
        _countLabel.AutoSize = true;
        _countLabel.Location = new Point(296, 579);
        Controls.Add(_countLabel);

        var cancelButton = CreateDialogButton("取消", new Point(420, 572), 100);
        cancelButton.DialogResult = DialogResult.Cancel;
        Controls.Add(cancelButton);

        var okButton = new Button
        {
            Text = "确定",
            Location = new Point(528, 572),
            Size = new Size(92, 30),
            FlatStyle = FlatStyle.Flat,
            BackColor = Accent,
            ForeColor = Color.White,
            Font = new Font("Microsoft YaHei UI", 9F, FontStyle.Bold),
            FlatAppearance = { BorderSize = 0 },
        };
        okButton.Click += (_, _) =>
        {
            if (_selected.Count > ProcessFlowFilter.MaxProcessCount)
            {
                MessageBox.Show(this,
                    $"最多选择 {ProcessFlowFilter.MaxProcessCount} 个进程。", "选择目标进程",
                    MessageBoxButtons.OK, MessageBoxIcon.Warning);
                return;
            }

            DialogResult = DialogResult.OK;
        };
        Controls.Add(okButton);

        AcceptButton = okButton;
        CancelButton = cancelButton;
    }

    private static Button CreateDialogButton(string text, Point location, int width) => new()
    {
        Text = text,
        Location = location,
        Size = new Size(width, 30),
        FlatStyle = FlatStyle.Flat,
        BackColor = Color.White,
        ForeColor = Color.FromArgb(60, 64, 67),
        FlatAppearance = { BorderColor = Color.FromArgb(200, 203, 207) },
    };

    private void ReloadProcesses()
    {
        Cursor = Cursors.WaitCursor;
        try
        {
            var map = new Dictionary<string, ProcessEntry>(StringComparer.OrdinalIgnoreCase);
            foreach (var process in Process.GetProcesses())
            {
                try
                {
                    var display = ProcessFlowFilter.NormalizeDisplayName(process.ProcessName);
                    if (display.Length == 0)
                        continue;

                    if (!map.TryGetValue(display, out var entry))
                    {
                        entry = new ProcessEntry { Name = display };
                        map.Add(display, entry);
                    }

                    entry.Count++;

                    if (!entry.PathAttempted)
                    {
                        entry.PathAttempted = true;
                        try
                        {
                            entry.Path = process.MainModule?.FileName ?? string.Empty;
                        }
                        catch
                        {
                            // 受保护的系统进程可能读不到路径。
                        }
                    }
                }
                catch
                {
                    // 进程可能在枚举期间退出。
                }
                finally
                {
                    process.Dispose();
                }
            }

            _entries.Clear();
            _entries.AddRange(map.Values);

            // 已选但当前未运行的程序也要显示，方便取消选择。
            foreach (var name in _selected)
            {
                if (!map.ContainsKey(name))
                    _entries.Add(new ProcessEntry { Name = name, Count = 0 });
            }

            RebuildItems();
        }
        finally
        {
            Cursor = Cursors.Default;
        }
    }

    private void RebuildItems()
    {
        var filter = _searchBox.Text.Trim();
        _suppressCheck = true;
        _list.BeginUpdate();
        try
        {
            _list.Items.Clear();
            foreach (var entry in _entries
                         .OrderBy(e => _selected.Contains(e.Name) ? 0 : e.Count == 0 ? 2 : 1)
                         .ThenBy(e => e.Name, StringComparer.OrdinalIgnoreCase))
            {
                if (filter.Length > 0 &&
                    entry.Name.IndexOf(filter, StringComparison.OrdinalIgnoreCase) < 0 &&
                    entry.Path.IndexOf(filter, StringComparison.OrdinalIgnoreCase) < 0)
                {
                    continue;
                }

                var item = new ListViewItem(entry.Name) { Tag = entry };
                item.SubItems.Add(entry.Count == 0 ? "未运行" : entry.Count.ToString());
                item.SubItems.Add(entry.Path);
                _list.Items.Add(item);
                item.Checked = _selected.Contains(entry.Name);
            }
        }
        finally
        {
            _list.EndUpdate();
            _suppressCheck = false;
        }

        UpdateCount();
    }

    private void List_ItemChecked(object? sender, ItemCheckedEventArgs e)
    {
        if (_suppressCheck || e.Item.Tag is not ProcessEntry entry)
            return;

        if (e.Item.Checked)
            _selected.Add(entry.Name);
        else
            _selected.Remove(entry.Name);

        UpdateCount();
    }

    private void UpdateCount()
    {
        _countLabel.Text = _selected.Count == 0
            ? "当前：全局"
            : $"已选 {_selected.Count} 个";
    }

    private void AddManual()
    {
        var display = ProcessFlowFilter.NormalizeDisplayName(_manualBox.Text);
        if (display.Length == 0)
        {
            MessageBox.Show(this, "请输入进程名，例如 chrome.exe。", "选择目标进程",
                MessageBoxButtons.OK, MessageBoxIcon.Information);
            return;
        }

        _selected.Add(display);
        if (_entries.All(e => !string.Equals(e.Name, display, StringComparison.OrdinalIgnoreCase)))
            _entries.Add(new ProcessEntry { Name = display, Count = 0 });

        _manualBox.Clear();
        RebuildItems();
    }

    private void BrowseExe()
    {
        using var dialog = new OpenFileDialog
        {
            Title = "选择程序",
            Filter = "程序 (*.exe)|*.exe|所有文件 (*.*)|*.*",
            CheckFileExists = true,
        };

        if (dialog.ShowDialog(this) != DialogResult.OK)
            return;

        var display = ProcessFlowFilter.NormalizeDisplayName(dialog.FileName);
        if (display.Length == 0)
            return;

        _selected.Add(display);
        if (_entries.All(e => !string.Equals(e.Name, display, StringComparison.OrdinalIgnoreCase)))
            _entries.Add(new ProcessEntry { Name = display, Count = 0 });

        RebuildItems();
    }

    private void SelectAllVisible()
    {
        _suppressCheck = true;
        foreach (ListViewItem item in _list.Items)
        {
            if (item.Tag is not ProcessEntry entry)
                continue;

            _selected.Add(entry.Name);
            item.Checked = true;
        }
        _suppressCheck = false;
        UpdateCount();
    }

    private void ClearSelection()
    {
        _selected.Clear();
        RebuildItems();
    }

    private sealed class ProcessEntry
    {
        public required string Name { get; init; }
        public int Count { get; set; }
        public string Path { get; set; } = string.Empty;
        public bool PathAttempted { get; set; }
    }
}
