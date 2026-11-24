//! WinDivert 2.2.x 动态加载与薄 FFI 封装（对应原 `WinDivertNative.cs`）。
//!
//! 用 `LoadLibraryW` + `GetProcAddress` 加载随程序附带的 `WinDivert.dll`，
//! 因此不需要 import lib，也能给出与旧版本一致的错误提示。

use std::ffi::{c_void, CString};
use std::sync::OnceLock;

use windows_sys::Win32::System::LibraryLoader::{GetProcAddress, LoadLibraryW};

pub const LAYER_NETWORK: i32 = 0;

pub const PARAM_QUEUE_LENGTH: i32 = 0;
pub const PARAM_QUEUE_TIME: i32 = 1;
pub const PARAM_QUEUE_SIZE: i32 = 2;

pub const SHUTDOWN_RECV: i32 = 1;

/// `WINDIVERT_ADDRESS` 在 2.2.x 中按 80 字节处理；接收缓冲区留出余量。
pub const ADDRESS_BUFFER_SIZE: usize = 128;
pub const ADDRESS_STRUCT_SIZE: usize = 80;

/// 最大 IP 包长度 + 协议头余量。
pub const MAX_PACKET_SIZE: usize = 65535 + 256;

type OpenFn = unsafe extern "system" fn(*const i8, i32, i16, u64) -> isize;
type RecvFn = unsafe extern "system" fn(isize, *mut u8, u32, *mut u32, *mut u8) -> i32;
type SendFn = unsafe extern "system" fn(isize, *const u8, u32, *mut u32, *const u8) -> i32;
type CloseFn = unsafe extern "system" fn(isize) -> i32;
type ShutdownFn = unsafe extern "system" fn(isize, i32) -> i32;
type SetParamFn = unsafe extern "system" fn(isize, i32, u64) -> i32;
type ParsePacketFn = unsafe extern "system" fn(
    *const u8,
    u32,
    *mut *mut c_void,
    *mut *mut c_void,
    *mut u8,
    *mut *mut c_void,
    *mut *mut c_void,
    *mut *mut c_void,
    *mut *mut c_void,
    *mut *mut c_void,
    *mut u32,
    *mut *mut c_void,
    *mut u32,
) -> i32;

struct Api {
    open: OpenFn,
    recv: RecvFn,
    send: SendFn,
    close: CloseFn,
    shutdown: ShutdownFn,
    set_param: SetParamFn,
    parse_packet: ParsePacketFn,
}

unsafe impl Send for Api {}
unsafe impl Sync for Api {}

static API: OnceLock<Result<Api, String>> = OnceLock::new();

fn symbol<T: Copy>(module: *mut c_void, name: &str) -> Result<T, String> {
    let cname = CString::new(name).map_err(|_| format!("非法导出名：{name}"))?;
    let proc = unsafe { GetProcAddress(module, cname.as_ptr() as *const u8) };
    match proc {
        Some(func) => {
            let value: T = unsafe { std::mem::transmute_copy(&func) };
            Ok(value)
        }
        None => Err(format!(
            "WinDivert.dll 缺少导出函数 {name}。请使用随程序附带的 2.2.2 官方文件。"
        )),
    }
}

fn load() -> Result<Api, String> {
    let mut module_name: Vec<u16> = "WinDivert.dll".encode_utf16().collect();
    module_name.push(0);
    let module = unsafe { LoadLibraryW(module_name.as_ptr()) };
    if module.is_null() {
        let error = std::io::Error::last_os_error();
        return Err(format!(
            "找不到 WinDivert.dll。请确认 WinDivert.dll 和 WinDivert64.sys 与程序在同一目录。（{error}）"
        ));
    }

    Ok(Api {
        open: symbol(module, "WinDivertOpen")?,
        recv: symbol(module, "WinDivertRecv")?,
        send: symbol(module, "WinDivertSend")?,
        close: symbol(module, "WinDivertClose")?,
        shutdown: symbol(module, "WinDivertShutdown")?,
        set_param: symbol(module, "WinDivertSetParam")?,
        parse_packet: symbol(module, "WinDivertHelperParsePacket")?,
    })
}

fn api() -> Result<&'static Api, String> {
    API.get_or_init(load)
        .as_ref()
        .map_err(|error| error.clone())
}

pub fn describe_open_error(code: i32) -> String {
    match code {
        2 => "找不到 WinDivert64.sys。请确认 WinDivert.dll 和 WinDivert64.sys 与程序在同一目录。"
            .to_string(),
        5 => "拒绝访问。WinDivert 需要管理员权限，请以管理员身份运行。".to_string(),
        577 => {
            "WinDivert 驱动签名无效或被系统拦截。请使用随程序附带的官方 2.2.2 文件，并检查杀毒软件。"
                .to_string()
        }
        1275 => {
            "WinDivert 驱动被系统策略阻止加载（错误 1275）。请检查驱动签名/安全软件。".to_string()
        }
        1058 => "WinDivert 驱动服务无法启动。请检查系统服务与安全软件。".to_string(),
        _ => format!(
            "WinDivert 启动失败（Win32 错误 {code}）。常见原因：未以管理员运行、驱动文件缺失、杀毒软件拦截或已有不兼容的 WinDivert 驱动。"
        ),
    }
}

fn last_error_code() -> i32 {
    std::io::Error::last_os_error().raw_os_error().unwrap_or(0)
}

pub fn open(filter: &str) -> Result<isize, String> {
    let api = api()?;
    let filter = CString::new(filter).map_err(|_| "WinDivert 过滤器包含非法字符。".to_string())?;
    let handle = unsafe { (api.open)(filter.as_ptr() as *const i8, LAYER_NETWORK, 0, 0) };
    if handle == 0 || handle == -1 {
        return Err(describe_open_error(last_error_code()));
    }
    Ok(handle)
}

/// 返回告警文本（队列长度设置失败时），失败不影响启动。
pub fn set_params(handle: isize) -> Option<String> {
    let api = api().ok()?;
    let mut warning = None;
    let ok = unsafe { (api.set_param)(handle, PARAM_QUEUE_LENGTH, 16384) };
    if ok == 0 {
        warning = Some(format!(
            "设置 WinDivert 队列长度失败：{}",
            std::io::Error::last_os_error()
        ));
    }
    unsafe {
        (api.set_param)(handle, PARAM_QUEUE_TIME, 2000);
        (api.set_param)(handle, PARAM_QUEUE_SIZE, 32 * 1024 * 1024);
    }
    warning
}

pub fn recv(handle: isize, packet: *mut u8, addr: *mut u8) -> Result<u32, i32> {
    let api = api().map_err(|_| 0)?;
    let mut length: u32 = 0;
    let ok = unsafe { (api.recv)(handle, packet, MAX_PACKET_SIZE as u32, &mut length, addr) };
    if ok == 0 {
        Err(last_error_code())
    } else {
        Ok(length)
    }
}

pub fn send(handle: isize, packet: *const u8, length: u32, addr: *const u8) -> Result<u32, i32> {
    let api = api().map_err(|_| 0)?;
    let mut sent: u32 = 0;
    let ok = unsafe { (api.send)(handle, packet, length, &mut sent, addr) };
    if ok == 0 {
        Err(last_error_code())
    } else {
        Ok(sent)
    }
}

pub fn close(handle: isize) {
    if let Ok(api) = api() {
        unsafe {
            (api.close)(handle);
        }
    }
}

pub fn shutdown(handle: isize, how: i32) -> bool {
    match api() {
        Ok(api) => unsafe { (api.shutdown)(handle, how) != 0 },
        Err(_) => false,
    }
}

/// 解析包首部并返回 IP 协议号（6=TCP，17=UDP）。
pub fn parse_protocol(packet: *const u8, length: u32) -> Option<u8> {
    let api = api().ok()?;
    let mut protocol: u8 = 0;
    let mut ip_header = std::ptr::null_mut();
    let mut ipv6_header = std::ptr::null_mut();
    let mut icmp_header = std::ptr::null_mut();
    let mut icmpv6_header = std::ptr::null_mut();
    let mut tcp_header = std::ptr::null_mut();
    let mut udp_header = std::ptr::null_mut();
    let mut data = std::ptr::null_mut();
    let mut data_len: u32 = 0;
    let mut next = std::ptr::null_mut();
    let mut next_len: u32 = 0;
    let ok = unsafe {
        (api.parse_packet)(
            packet,
            length,
            &mut ip_header,
            &mut ipv6_header,
            &mut protocol,
            &mut icmp_header,
            &mut icmpv6_header,
            &mut tcp_header,
            &mut udp_header,
            &mut data,
            &mut data_len,
            &mut next,
            &mut next_len,
        )
    };
    if ok == 0 {
        None
    } else {
        Some(protocol)
    }
}
