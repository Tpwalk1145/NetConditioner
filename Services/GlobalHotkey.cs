using System.Runtime.InteropServices;
using NetConditioner.Models;

namespace NetConditioner.Services;

/// <summary>全局热键的注册/注销封装。</summary>
public sealed class GlobalHotkey
{
    public const int HotkeyId = 0x4E43; // 'NC'

    private readonly IntPtr _windowHandle;
    private bool _registered;

    public GlobalHotkey(IntPtr windowHandle)
    {
        _windowHandle = windowHandle;
    }

    public bool TryRegister(uint modifiers, int key, out string error)
    {
        error = string.Empty;

        if (!HotkeyFormatter.IsValid(modifiers, key))
        {
            error = "请指定一个非修饰键；可以直接用单个键（如 F8），也可以加 Ctrl/Alt/Shift/Win。";
            return false;
        }

        Unregister();

        var ok = RegisterHotKey(_windowHandle, HotkeyId,
            modifiers | HotkeyDefaults.ModNoRepeat, (uint)key);
        if (!ok)
        {
            var code = Marshal.GetLastWin32Error();
            error = code == 1409
                ? "该快捷键已被其他程序占用，请换一个组合。"
                : $"注册快捷键失败（Win32 错误 {code}）。";
            return false;
        }

        _registered = true;
        return true;
    }

    public void Unregister()
    {
        if (!_registered)
            return;
        UnregisterHotKey(_windowHandle, HotkeyId);
        _registered = false;
    }

    public void Dispose() => Unregister();

    [DllImport("user32.dll", SetLastError = true)]
    private static extern bool RegisterHotKey(IntPtr hWnd, int id, uint fsModifiers, uint vk);

    [DllImport("user32.dll", SetLastError = true)]
    private static extern bool UnregisterHotKey(IntPtr hWnd, int id);
}
