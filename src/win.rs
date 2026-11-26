//! 原生 Win32 薄封装：窗口、消息、GDI 绘制、字体与常用系统 API。
//!
//! 这个模块只做“最小可用”的封装，业务逻辑都在 `ui/` 与 `app.rs`。

use std::ffi::c_void;

use windows_sys::Win32::Foundation::*;
use windows_sys::Win32::Graphics::Gdi::*;
use windows_sys::Win32::Security::{
    AllocateAndInitializeSid, CheckTokenMembership, FreeSid, PSID, SECURITY_NT_AUTHORITY,
};
use windows_sys::Win32::System::LibraryLoader::GetModuleHandleW;
use windows_sys::Win32::System::Threading::CreateMutexW;
use windows_sys::Win32::UI::Controls::{
    InitCommonControlsEx, EM_SETCUEBANNER, ICC_LISTVIEW_CLASSES, ICC_STANDARD_CLASSES,
    ICC_UPDOWN_CLASS, INITCOMMONCONTROLSEX,
};
use windows_sys::Win32::UI::HiDpi::{
    AdjustWindowRectExForDpi, GetDpiForSystem, GetDpiForWindow, SetProcessDpiAwarenessContext,
    DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2,
};
use windows_sys::Win32::UI::Input::KeyboardAndMouse::{
    EnableWindow, GetKeyState, RegisterHotKey, SetActiveWindow, SetFocus, UnregisterHotKey,
};
use windows_sys::Win32::UI::WindowsAndMessaging::*;

use crate::config::MOD_NOREPEAT;

/// 自定义消息：引擎/更新后台线程完成后唤醒 UI。
pub const WM_APP_EVENTS: u32 = 0x8000 + 1;
pub const WM_APP_TRAY: u32 = 0x8000 + 2;

/// 窗口样式常量集中在这里，避免各窗口到处写魔法数。
pub const STYLE_MAIN: u32 =
    WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_CLIPCHILDREN;
pub const STYLE_DIALOG: u32 = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN;

pub fn wide(text: &str) -> Vec<u16> {
    let mut buffer: Vec<u16> = text.encode_utf16().collect();
    buffer.push(0);
    buffer
}

pub fn rgb(r: u8, g: u8, b: u8) -> u32 {
    r as u32 | ((g as u32) << 8) | ((b as u32) << 16)
}

pub fn hinstance() -> HINSTANCE {
    unsafe { GetModuleHandleW(std::ptr::null()) }
}

// ---------------------------------------------------------------------------
// 窗口
// ---------------------------------------------------------------------------

pub fn register_class(name: &str, proc: WNDPROC) -> bool {
    let class_name = wide(name);
    unsafe {
        let mut class: WNDCLASSEXW = std::mem::zeroed();
        class.cbSize = std::mem::size_of::<WNDCLASSEXW>() as u32;
        class.style = CS_HREDRAW | CS_VREDRAW;
        class.lpfnWndProc = proc;
        class.hInstance = hinstance();
        class.hCursor = LoadCursorW(std::ptr::null_mut(), IDC_ARROW);
        class.hbrBackground = std::ptr::null_mut();
        class.lpszClassName = class_name.as_ptr();
        let atom = RegisterClassExW(&class);
        atom != 0
    }
}

#[allow(clippy::too_many_arguments)]
pub fn create_window(
    class: &str,
    title: &str,
    style: u32,
    ex_style: u32,
    parent: HWND,
    menu: usize,
    param: *mut c_void,
) -> HWND {
    let class = wide(class);
    let title = wide(title);
    unsafe {
        CreateWindowExW(
            ex_style,
            class.as_ptr(),
            title.as_ptr(),
            style,
            0,
            0,
            0,
            0,
            parent,
            menu as HMENU,
            hinstance(),
            param,
        )
    }
}

#[allow(clippy::too_many_arguments)]
pub fn create_child(
    parent: HWND,
    class: &str,
    text: &str,
    style: u32,
    ex_style: u32,
    x: i32,
    y: i32,
    width: i32,
    height: i32,
    id: usize,
) -> HWND {
    let class = wide(class);
    let text = wide(text);
    unsafe {
        CreateWindowExW(
            ex_style,
            class.as_ptr(),
            text.as_ptr(),
            style,
            x,
            y,
            width,
            height,
            parent,
            id as HMENU,
            hinstance(),
            std::ptr::null(),
        )
    }
}

pub fn set_window_client_size(hwnd: HWND, client_width: i32, client_height: i32) {
    unsafe {
        let style = GetWindowLongW(hwnd, GWL_STYLE) as u32;
        let ex_style = GetWindowLongW(hwnd, GWL_EXSTYLE) as u32;
        let mut rect = RECT {
            left: 0,
            top: 0,
            right: client_width,
            bottom: client_height,
        };
        AdjustWindowRectExForDpi(&mut rect, style, 0, ex_style, dpi_of(hwnd));
        SetWindowPos(
            hwnd,
            std::ptr::null_mut(),
            0,
            0,
            rect.right - rect.left,
            rect.bottom - rect.top,
            SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE,
        );
    }
}

pub fn center_on_screen(hwnd: HWND) {
    unsafe {
        let monitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTOPRIMARY);
        let mut info: MONITORINFO = std::mem::zeroed();
        info.cbSize = std::mem::size_of::<MONITORINFO>() as u32;
        if GetMonitorInfoW(monitor, &mut info) == 0 {
            return;
        }

        let mut rect: RECT = std::mem::zeroed();
        GetWindowRect(hwnd, &mut rect);
        let width = rect.right - rect.left;
        let height = rect.bottom - rect.top;
        let work = info.rcWork;
        let x = work.left + ((work.right - work.left) - width) / 2;
        let y = work.top + ((work.bottom - work.top) - height) / 2;
        SetWindowPos(
            hwnd,
            std::ptr::null_mut(),
            x,
            y,
            0,
            0,
            SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE,
        );
    }
}

pub fn dpi_of(hwnd: HWND) -> u32 {
    unsafe {
        let dpi = GetDpiForWindow(hwnd);
        if dpi == 0 {
            GetDpiForSystem()
        } else {
            dpi
        }
    }
}

pub fn post(hwnd: HWND, message: u32) {
    unsafe {
        PostMessageW(hwnd, message, 0, 0);
    }
}

pub fn send(hwnd: HWND, message: u32, wparam: WPARAM, lparam: LPARAM) -> LRESULT {
    unsafe { SendMessageW(hwnd, message, wparam, lparam) }
}

pub fn set_text(hwnd: HWND, text: &str) {
    let text = wide(text);
    unsafe {
        SetWindowTextW(hwnd, text.as_ptr());
    }
}

pub fn get_text(hwnd: HWND) -> String {
    unsafe {
        let length = GetWindowTextLengthW(hwnd);
        if length <= 0 {
            return String::new();
        }
        let mut buffer = vec![0u16; length as usize + 1];
        let copied = GetWindowTextW(hwnd, buffer.as_mut_ptr(), buffer.len() as i32);
        if copied <= 0 {
            return String::new();
        }
        String::from_utf16_lossy(&buffer[..copied as usize])
    }
}

pub fn message_box(owner: HWND, text: &str, title: &str, flags: u32) -> i32 {
    let text = wide(text);
    let title = wide(title);
    unsafe { MessageBoxW(owner, text.as_ptr(), title.as_ptr(), flags) }
}

pub fn enable(hwnd: HWND, enabled: bool) {
    unsafe {
        EnableWindow(hwnd, if enabled { 1 } else { 0 });
    }
}

pub fn show(hwnd: HWND, command: i32) {
    unsafe {
        ShowWindow(hwnd, command);
    }
}

pub fn destroy(hwnd: HWND) {
    unsafe {
        DestroyWindow(hwnd);
    }
}

pub fn invalidate(hwnd: HWND) {
    unsafe {
        InvalidateRect(hwnd, std::ptr::null(), 1);
    }
}

pub fn set_font(hwnd: HWND, font: HFONT) {
    send(hwnd, WM_SETFONT, font as usize, 1);
}

pub fn set_timer(hwnd: HWND, id: usize, milliseconds: u32) {
    unsafe {
        SetTimer(hwnd, id, milliseconds, None);
    }
}

pub fn kill_timer(hwnd: HWND, id: usize) {
    unsafe {
        KillTimer(hwnd, id);
    }
}

pub fn message_loop() -> i32 {
    unsafe {
        let mut message: MSG = std::mem::zeroed();
        loop {
            let result = GetMessageW(&mut message, std::ptr::null_mut(), 0, 0);
            if result <= 0 {
                break;
            }
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        message.wParam as i32
    }
}

pub fn cursor_pos() -> (i32, i32) {
    unsafe {
        let mut point = POINT { x: 0, y: 0 };
        GetCursorPos(&mut point);
        (point.x, point.y)
    }
}

pub fn set_foreground(hwnd: HWND) {
    unsafe {
        SetForegroundWindow(hwnd);
    }
}

pub fn set_focus(hwnd: HWND) {
    unsafe {
        SetFocus(hwnd);
    }
}

// ---------------------------------------------------------------------------
// 绘制
// ---------------------------------------------------------------------------

pub fn fill_rect(hdc: HDC, rect: &RECT, color: u32) {
    unsafe {
        let brush = CreateSolidBrush(color);
        FillRect(hdc, rect, brush);
        DeleteObject(brush);
    }
}

pub fn frame_rect(hdc: HDC, rect: &RECT, color: u32) {
    unsafe {
        let brush = CreateSolidBrush(color);
        FrameRect(hdc, rect, brush);
        DeleteObject(brush);
    }
}

pub fn system_dpi() -> u32 {
    unsafe { GetDpiForSystem() }
}

pub fn set_window_pos(hwnd: HWND, x: i32, y: i32, width: i32, height: i32) {
    unsafe {
        SetWindowPos(
            hwnd,
            std::ptr::null_mut(),
            x,
            y,
            width,
            height,
            SWP_NOZORDER | SWP_NOACTIVATE,
        );
    }
}

pub fn draw_text(hdc: HDC, text: &str, rect: &mut RECT, font: HFONT, color: u32, format: u32) {
    let text = wide(text);
    unsafe {
        let previous = SelectObject(hdc, font);
        SetBkMode(hdc, TRANSPARENT as i32);
        SetTextColor(hdc, color);
        DrawTextW(hdc, text.as_ptr(), -1, rect, format);
        SelectObject(hdc, previous);
    }
}

pub fn create_font(
    family: &str,
    point_size_x10: u32,
    dpi: u32,
    bold: bool,
    underline: bool,
) -> HFONT {
    let height = -(((point_size_x10 as f64) * (dpi as f64) / 720.0).round() as i32);
    let face = wide(family);
    unsafe {
        CreateFontW(
            height,
            0,
            0,
            0,
            if bold { 700 } else { 400 },
            0,
            underline as u32,
            0,
            DEFAULT_CHARSET as u32,
            OUT_DEFAULT_PRECIS as u32,
            CLIP_DEFAULT_PRECIS as u32,
            CLEARTYPE_QUALITY as u32,
            (DEFAULT_PITCH | FF_DONTCARE) as u32,
            face.as_ptr(),
        )
    }
}

pub fn create_paint_dc(hwnd: HWND) -> (HDC, PAINTSTRUCT) {
    unsafe {
        let mut paint: PAINTSTRUCT = std::mem::zeroed();
        let hdc = BeginPaint(hwnd, &mut paint);
        (hdc, paint)
    }
}

pub fn end_paint(hwnd: HWND, paint: &mut PAINTSTRUCT) {
    unsafe {
        EndPaint(hwnd, paint);
    }
}

pub fn client_rect(hwnd: HWND) -> RECT {
    unsafe {
        let mut rect: RECT = std::mem::zeroed();
        GetClientRect(hwnd, &mut rect);
        rect
    }
}

pub fn create_white_brush() -> HBRUSH {
    unsafe { CreateSolidBrush(rgb(255, 255, 255)) }
}

pub fn set_bk_color(hdc: HDC, color: u32) {
    unsafe {
        SetBkColor(hdc, color);
    }
}

pub fn set_text_color(hdc: HDC, color: u32) {
    unsafe {
        SetTextColor(hdc, color);
    }
}

pub fn last_error_code() -> i32 {
    std::io::Error::last_os_error().raw_os_error().unwrap_or(0)
}

pub fn child_windows(hwnd: HWND) -> Vec<HWND> {
    unsafe {
        let mut children = Vec::new();
        let mut child = GetWindow(hwnd, GW_CHILD);
        while !child.is_null() {
            children.push(child);
            child = GetWindow(child, GW_HWNDNEXT);
        }
        children
    }
}

pub fn fit_dialog_scale(owner: HWND, base_width: i32, base_height: i32, preferred: f64) -> f64 {
    unsafe {
        let monitor = MonitorFromWindow(owner, MONITOR_DEFAULTTOPRIMARY);
        let mut info: MONITORINFO = std::mem::zeroed();
        info.cbSize = std::mem::size_of::<MONITORINFO>() as u32;
        if GetMonitorInfoW(monitor, &mut info) == 0 {
            return preferred;
        }
        let work = info.rcWork;
        let width = (work.right - work.left - 32).max(1);
        let height = (work.bottom - work.top - 32).max(1);
        let fit = (f64::from(width) / f64::from(base_width.max(1)))
            .min(f64::from(height) / f64::from(base_height.max(1)));
        preferred.min(fit).max(0.5)
    }
}

pub fn parent_hwnd(hwnd: HWND) -> HWND {
    unsafe { GetParent(hwnd) }
}

/// 居中到父窗口。
pub fn center_on_owner(hwnd: HWND, owner: HWND) {
    if owner.is_null() {
        center_on_screen(hwnd);
        return;
    }
    unsafe {
        let mut owner_rect: RECT = std::mem::zeroed();
        if GetWindowRect(owner, &mut owner_rect) == 0 {
            center_on_screen(hwnd);
            return;
        }
        let mut rect: RECT = std::mem::zeroed();
        GetWindowRect(hwnd, &mut rect);
        let width = rect.right - rect.left;
        let height = rect.bottom - rect.top;
        let x = owner_rect.left + ((owner_rect.right - owner_rect.left) - width) / 2;
        let y = owner_rect.top + ((owner_rect.bottom - owner_rect.top) - height) / 2;
        SetWindowPos(
            hwnd,
            std::ptr::null_mut(),
            x,
            y,
            0,
            0,
            SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE,
        );
    }
}

/// 运行嵌套模态消息循环，直到对话框窗口被销毁。*/
pub fn run_modal(owner: HWND, dialog: HWND) {
    unsafe {
        if !owner.is_null() {
            EnableWindow(owner, 0);
        }
        ShowWindow(dialog, SW_SHOW);
        SetForegroundWindow(dialog);

        let mut message: MSG = std::mem::zeroed();
        loop {
            if IsWindow(dialog) == 0 {
                break;
            }
            let result = GetMessageW(&mut message, std::ptr::null_mut(), 0, 0);
            if result <= 0 {
                if result == 0 {
                    PostQuitMessage(0);
                }
                break;
            }
            if IsDialogMessageW(dialog, &message) == 0 {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
        }

        if !owner.is_null() && IsWindow(owner) != 0 {
            EnableWindow(owner, 1);
            SetActiveWindow(owner);
        }
    }
}

pub fn delete_object(object: HGDIOBJ) {
    if object.is_null() {
        return;
    }
    unsafe {
        DeleteObject(object);
    }
}

/// 生成托盘用的圆形图标（32×32，带白色描边）。
pub fn create_circle_icon(color: u32, size: i32) -> HICON {
    unsafe {
        let hdc = CreateCompatibleDC(std::ptr::null_mut());
        if hdc.is_null() {
            return std::ptr::null_mut();
        }

        let mut info: BITMAPINFO = std::mem::zeroed();
        info.bmiHeader.biSize = std::mem::size_of::<BITMAPINFOHEADER>() as u32;
        info.bmiHeader.biWidth = size;
        info.bmiHeader.biHeight = size;
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = 0; // BI_RGB

        let mut bits: *mut c_void = std::ptr::null_mut();
        let color_bitmap = CreateDIBSection(hdc, &info, 0, &mut bits, std::ptr::null_mut(), 0);
        if color_bitmap.is_null() || bits.is_null() {
            DeleteDC(hdc);
            return std::ptr::null_mut();
        }

        let previous = SelectObject(hdc, color_bitmap);
        let rect = RECT {
            left: 0,
            top: 0,
            right: size,
            bottom: size,
        };
        fill_rect(hdc, &rect, 0);

        let margin = ((size as f64) * 0.09).round() as i32;
        let ring = ((size as f64) * 0.09).round() as i32;
        let brush = CreateSolidBrush(color);
        let pen = CreatePen(0, ring.max(1), 0x00FF_FFFF);
        let previous_brush = SelectObject(hdc, brush);
        let previous_pen = SelectObject(hdc, pen);
        Ellipse(hdc, margin, margin, size - margin, size - margin);
        SelectObject(hdc, previous_brush);
        SelectObject(hdc, previous_pen);
        DeleteObject(brush);
        DeleteObject(pen);

        SelectObject(hdc, previous);
        DeleteDC(hdc);

        // GDI 不会写入 alpha 通道，这里把非黑色像素标为不透明。
        let pixels = std::slice::from_raw_parts_mut(bits as *mut u8, (size * size * 4) as usize);
        for pixel in pixels.as_chunks_mut::<4>().0 {
            if pixel[0] != 0 || pixel[1] != 0 || pixel[2] != 0 {
                pixel[3] = 255;
            }
        }

        let mask_stride = ((size + 15) / 16 * 2) as usize;
        let mask_bits = vec![0u8; mask_stride * size as usize];
        let mask = CreateBitmap(size, size, 1, 1, mask_bits.as_ptr() as *const c_void);

        let icon_info = ICONINFO {
            fIcon: 1,
            xHotspot: 0,
            yHotspot: 0,
            hbmMask: mask,
            hbmColor: color_bitmap,
        };
        let icon = CreateIconIndirect(&icon_info);

        DeleteObject(mask);
        DeleteObject(color_bitmap);
        icon
    }
}

pub fn destroy_icon(icon: HICON) {
    if icon.is_null() {
        return;
    }
    unsafe {
        DestroyIcon(icon);
    }
}

// ---------------------------------------------------------------------------
// 热键 / 输入 / 系统
// ---------------------------------------------------------------------------

pub fn key_down(vk: i32) -> bool {
    unsafe { (GetKeyState(vk) as u16 & 0x8000) != 0 }
}

pub fn register_hotkey(hwnd: HWND, id: i32, modifiers: u32, key: i32) -> bool {
    unsafe { RegisterHotKey(hwnd, id, modifiers | MOD_NOREPEAT, key as u32) != 0 }
}

pub fn unregister_hotkey(hwnd: HWND, id: i32) {
    unsafe {
        UnregisterHotKey(hwnd, id);
    }
}

pub fn is_elevated() -> bool {
    unsafe {
        let authority = SECURITY_NT_AUTHORITY;
        let mut administrators: PSID = std::ptr::null_mut();
        let created = AllocateAndInitializeSid(
            &authority,
            2u8,
            0x20,  // SECURITY_BUILTIN_DOMAIN_RID
            0x220, // DOMAIN_ALIAS_RID_ADMINS
            0,
            0,
            0,
            0,
            0,
            0,
            &mut administrators,
        );
        if created == 0 {
            return false;
        }
        let mut member: windows_sys::core::BOOL = 0;
        CheckTokenMembership(std::ptr::null_mut(), administrators, &mut member);
        FreeSid(administrators);
        member != 0
    }
}

/// 创建单实例互斥体；已存在时返回 None。
pub fn create_single_instance_mutex() -> Option<HANDLE> {
    let name = wide("Global\\NetConditioner.SingleInstance");
    unsafe {
        let handle = CreateMutexW(std::ptr::null(), 1, name.as_ptr());
        if handle.is_null() {
            return Some(std::ptr::null_mut());
        }
        if GetLastError() == ERROR_ALREADY_EXISTS {
            CloseHandle(handle);
            return None;
        }
        Some(handle)
    }
}

pub fn init_common_controls() {
    unsafe {
        let info = INITCOMMONCONTROLSEX {
            dwSize: std::mem::size_of::<INITCOMMONCONTROLSEX>() as u32,
            dwICC: ICC_LISTVIEW_CLASSES | ICC_UPDOWN_CLASS | ICC_STANDARD_CLASSES,
        };
        InitCommonControlsEx(&info);
    }
}

pub fn set_dpi_awareness() {
    unsafe {
        SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    }
}

pub fn is_remote_session() -> bool {
    unsafe { GetSystemMetrics(SM_REMOTESESSION) != 0 }
}

pub fn set_dc_brush_color(hdc: HDC, color: u32) {
    unsafe {
        SetDCBrushColor(hdc, color);
    }
}

pub fn dc_brush() -> HBRUSH {
    unsafe { GetStockObject(DC_BRUSH) as HBRUSH }
}

pub fn is_window_enabled(hwnd: HWND) -> bool {
    unsafe { GetWindowLongW(hwnd, GWL_STYLE) as u32 & WS_DISABLED == 0 }
}

pub fn set_cue_banner(edit: HWND, text: &str) {
    let text = wide(text);
    send(edit, EM_SETCUEBANNER, 1, text.as_ptr() as isize);
}

pub fn write_fixed(buffer: &mut [u16], text: &str) {
    for slot in buffer.iter_mut() {
        *slot = 0;
    }
    let Some(last) = buffer.len().checked_sub(1) else {
        return;
    };
    for (slot, code) in buffer.iter_mut().take(last).zip(text.encode_utf16()) {
        *slot = code;
    }
}

#[cfg(test)]
mod tests {
    use super::write_fixed;

    #[test]
    fn fixed_buffers_always_keep_a_terminator() {
        let mut buffer = [0u16; 5];
        write_fixed(&mut buffer, "abcd");
        assert_eq!(
            &buffer,
            &[b'a' as u16, b'b' as u16, b'c' as u16, b'd' as u16, 0]
        );

        write_fixed(&mut buffer, "abcdef");
        assert_eq!(
            &buffer,
            &[b'a' as u16, b'b' as u16, b'c' as u16, b'd' as u16, 0]
        );

        write_fixed(&mut buffer, "😀");
        assert_eq!(buffer[0], 0xD83D);
        assert_eq!(buffer[1], 0xDE00);
        assert_eq!(buffer[2], 0);

        write_fixed(&mut [], "ignored");
    }
}
