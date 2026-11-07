using NetConditioner.Models;

namespace NetConditioner.Services;

public static class HotkeyFormatter
{
    public static bool IsValid(uint modifiers, int key)
    {
        if (key == 0)
            return false;

        // 允许单键热键（modifiers == 0），例如 F8；仅排除修饰键本身。
        // 单键会全局占用该按键，设置界面会提示。
        return !IsModifierKey(key);
    }

    public static bool IsModifierKey(int key) => key switch
    {
        0x10 or 0x11 or 0x12 => true,       // Shift / Control / Alt
        0x5B or 0x5C => true,               // LWin / RWin
        0x5D => false,                      // Apps 可以作为普通键
        0xA0 or 0xA1 or 0xA2 or 0xA3 or 0xA4 or 0xA5 => true, // L/R Shift, Ctrl, Alt
        _ => false,
    };

    public static string Format(uint modifiers, int key)
    {
        var parts = new List<string>(5);
        if ((modifiers & HotkeyDefaults.ModControl) != 0) parts.Add("Ctrl");
        if ((modifiers & HotkeyDefaults.ModAlt) != 0) parts.Add("Alt");
        if ((modifiers & HotkeyDefaults.ModShift) != 0) parts.Add("Shift");
        if ((modifiers & HotkeyDefaults.ModWin) != 0) parts.Add("Win");
        parts.Add(GetKeyName(key));
        return string.Join(" + ", parts);
    }

    public static string GetKeyName(int key)
    {
        var name = ((Keys)key).ToString();
        return name switch
        {
            "D0" => "0",
            "D1" => "1",
            "D2" => "2",
            "D3" => "3",
            "D4" => "4",
            "D5" => "5",
            "D6" => "6",
            "D7" => "7",
            "D8" => "8",
            "D9" => "9",
            "OemMinus" => "-",
            "Oemplus" => "=",
            "OemOpenBrackets" => "[",
            "OemCloseBrackets" => "]",
            "OemPipe" => "\\",
            "OemSemicolon" => ";",
            "OemQuotes" => "'",
            "OemComma" => ",",
            "OemPeriod" => ".",
            "OemQuestion" => "/",
            "Oemtilde" => "`",
            "Return" => "Enter",
            "Escape" => "Esc",
            "Space" => "Space",
            "PageUp" => "PageUp",
            "PageDown" => "PageDown",
            _ => name,
        };
    }
}
