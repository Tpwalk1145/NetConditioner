using System.Diagnostics;
using System.Runtime.InteropServices;
using NetConditioner.Services;

namespace NetConditioner.Forms;

/// <summary>选择一个或多个目标进程；一个都不选表示对全部进程生效。</summary>
public sealed class ProcessSelectorForm : Form
{
    private const int EmSetCueBanner = 0x1501;

    private static readonly Color Accent = Color.FromArgb(21, 101, 192);
    private static readonly Color Surface = Color.FromArgb(243, 247, 252);
    private static readonly Color TextDark = Color.FromArgb(32, 33, 36);
    private static readonly Color TextMuted = Color.FromArgb(104, 107, 112);
    private static readonly Color Border = Color.FromArgb(200, 203, 207);

    private readonly ListView _list = new();
    private readonly TextBox _searchBox = new();
    private readonly Label _countLabel = new();
    private readonly Label _emptyLabel = new();
    private readonly ToolTip _toolTip = new() { ShowAlways = true };
    private readonly List<ProcessEntry> _entries = new();
    private readonly HashSet<string> _selected = new(StringComparer.OrdinalIgnoreCase);
    private bool _suppressCheck;

    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    private static extern IntPtr SendMessage(IntPtr hWnd, int msg, IntPtr wParam, string lParam);

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
        ClientSize = new Size(460, 520);
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
        Controls.Add(new Label
        {
            Text = "选择目标进程",
            Font = new Font("Microsoft YaHei UI", 11F, FontStyle.Bold),
            ForeColor = TextDark,
            AutoSize = true,
            Location = new Point(16, 14),
        });

        Controls.Add(new Label
        {
            Text = "勾选后仅对这些程序生效；不勾选 = 全局（所有进程）",
            Font = new Font("Microsoft YaHei UI", 8.5F),
            ForeColor = TextMuted,
            AutoSize = true,
            Location = new Point(16, 42),
        });

        _searchBox.Location = new Point(16, 68);
        _searchBox.Size = new Size(372, 28);
        _searchBox.TextChanged += (_, _) => RebuildItems();
        Controls.Add(_searchBox);

        var refreshButton = new Button
        {
            Text = "刷新",
            Location = new Point(396, 68),
            Size = new Size(48, 28),
            FlatStyle = FlatStyle.Flat,
            BackColor = Color.White,
            ForeColor = Color.FromArgb(60, 64, 67),
            FlatAppearance = { BorderColor = Border },
        };
        refreshButton.Click += (_, _) => ReloadProcesses();
        Controls.Add(refreshButton);

        _list.Location = new Point(16, 108);
        _list.Size = new Size(428, 332);
        _list.View = View.Details;
        _list.CheckBoxes = true;
        _list.FullRowSelect = true;
        _list.HeaderStyle = ColumnHeaderStyle.Nonclickable;
        _list.HideSelection = false;
        _list.MultiSelect = true;
        _list.ShowItemToolTips = false;
        _list.Columns.Add("进程", 384);
        _list.ItemChecked += List_ItemChecked;
        _list.ItemMouseHover += List_ItemMouseHover;
        _list.MouseLeave += (_, _) => _toolTip.Hide(_list);
        _list.DoubleClick += List_DoubleClick;
        Controls.Add(_list);

        _emptyLabel.Text = "没有匹配的进程";
        _emptyLabel.ForeColor = TextMuted;
        _emptyLabel.BackColor = Color.White;
        _emptyLabel.TextAlign = ContentAlignment.MiddleCenter;
        _emptyLabel.AutoSize = false;
        _emptyLabel.Location = new Point(17, 129);
        _emptyLabel.Size = new Size(426, 309);
        _emptyLabel.Visible = false;
        Controls.Add(_emptyLabel);
        _emptyLabel.BringToFront();

        _countLabel.Text = "当前：全局";
        _countLabel.ForeColor = TextMuted;
        _countLabel.AutoSize = true;
        _countLabel.Location = new Point(16, 452);
        Controls.Add(_countLabel);

        var selectAllLink = CreateLinkLabel("全选", new Point(326, 451));
        selectAllLink.Click += (_, _) => SelectAllVisible();
        Controls.Add(selectAllLink);

        var clearLink = CreateLinkLabel("清除", new Point(376, 451));
        clearLink.Click += (_, _) => ClearSelection();
        Controls.Add(clearLink);

        var cancelButton = CreateDialogButton("取消", new Point(252, 478), 92);
        cancelButton.DialogResult = DialogResult.Cancel;
        Controls.Add(cancelButton);

        var okButton = new Button
        {
            Text = "确定",
            Location = new Point(352, 478),
            Size = new Size(92, 32),
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

    protected override void Dispose(bool disposing)
    {
        if (disposing)
            _toolTip.Dispose();
        base.Dispose(disposing);
    }

    protected override void OnShown(EventArgs e)
    {
        base.OnShown(e);
        SendMessage(_searchBox.Handle, EmSetCueBanner, (IntPtr)1, "搜索进程名…");
        _searchBox.Focus();
    }

    private static LinkLabel CreateLinkLabel(string text, Point location) => new()
    {
        Text = text,
        AutoSize = true,
        Location = location,
        Font = new Font("Microsoft YaHei UI", 8.5F),
        LinkColor = Accent,
        ActiveLinkColor = Accent,
        VisitedLinkColor = Accent,
        LinkBehavior = LinkBehavior.HoverUnderline,
    };

    private static Button CreateDialogButton(string text, Point location, int width) => new()
    {
        Text = text,
        Location = location,
        Size = new Size(width, 32),
        FlatStyle = FlatStyle.Flat,
        BackColor = Color.White,
        ForeColor = Color.FromArgb(60, 64, 67),
        FlatAppearance = { BorderColor = Border },
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
                    entry.Name.IndexOf(filter, StringComparison.OrdinalIgnoreCase) < 0)
                {
                    continue;
                }

                var item = new ListViewItem(entry.Name) { Tag = entry };
                if (entry.Count == 0)
                    item.ForeColor = TextMuted;
                _list.Items.Add(item);
                item.Checked = _selected.Contains(entry.Name);
            }
        }
        finally
        {
            _list.EndUpdate();
            _suppressCheck = false;
        }

        _emptyLabel.Visible = _list.Items.Count == 0;
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

    private void List_ItemMouseHover(object? sender, ListViewItemMouseHoverEventArgs e)
    {
        if (e.Item?.Tag is not ProcessEntry entry)
        {
            _toolTip.Hide(_list);
            return;
        }

        var tip = entry.Count == 0
            ? "当前未运行（按进程名匹配，启动后生效）"
            : $"实例：{entry.Count}";
        if (entry.Path.Length > 0)
            tip += $"\n{entry.Path}";

        _toolTip.Show(tip, _list, e.Item.Position.X + 24, e.Item.Position.Y + 22, 8000);
    }

    private void List_DoubleClick(object? sender, EventArgs e)
    {
        if (_list.FocusedItem is { } item)
            item.Checked = !item.Checked;
    }

    private void UpdateCount()
    {
        _countLabel.Text = _selected.Count == 0
            ? "当前：全局"
            : $"已选 {_selected.Count} 个";
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
