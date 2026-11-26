//! WinDivert 2.2.x 动态加载与薄 FFI 封装。
//!
//! DLL 与驱动编进 exe，首次打开弱网引擎时准备到运行时目录，
//! 再用 `LoadLibraryExW(LOAD_WITH_ALTERED_SEARCH_PATH)` 从该目录加载，
//! 保证 WinDivert 能在 DLL 同目录找到 WinDivert64.sys。

use std::ffi::{c_void, CString};
use std::path::{Path, PathBuf};
use std::sync::OnceLock;

use windows_sys::Win32::System::LibraryLoader::{GetProcAddress, LoadLibraryExW};

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

static API: OnceLock<Api> = OnceLock::new();

const WINDIVERT_DLL_BYTES: &[u8] = include_bytes!("../../third_party/WinDivert/WinDivert.dll");
const WINDIVERT_SYS_BYTES: &[u8] = include_bytes!("../../third_party/WinDivert/WinDivert64.sys");
static WINDIVERT_VERSION: &str = include_str!("../../third_party/WinDivert/VERSION");
const LOAD_WITH_ALTERED_SEARCH_PATH: u32 = 0x0000_0008;

/// 随程序分发的 WinDivert 版本（运行时目录按版本区分）。
pub fn payload_version() -> &'static str {
    WINDIVERT_VERSION.trim()
}

fn to_wide(text: &str) -> Vec<u16> {
    text.encode_utf16().chain(std::iter::once(0)).collect()
}

/// 优先用管理员独占可写的 Program Files，其次 ProgramData / LocalAppData。
fn runtime_candidates() -> Vec<PathBuf> {
    let version = payload_version();
    let mut candidates: Vec<PathBuf> = Vec::new();
    for variable in ["ProgramFiles", "ProgramData", "LOCALAPPDATA"] {
        if let Some(value) = std::env::var_os(variable) {
            let root = PathBuf::from(value)
                .join("NetConditioner")
                .join("runtime")
                .join(version);
            if !candidates.contains(&root) {
                candidates.push(root);
            }
        }
    }
    candidates
}

fn ensure_payload_dir(directory: &Path) -> Result<PathBuf, String> {
    std::fs::create_dir_all(directory)
        .map_err(|error| format!("创建目录失败（{}）：{error}", directory.display()))?;

    let dll_path = directory.join("WinDivert.dll");
    write_if_needed(&dll_path, WINDIVERT_DLL_BYTES)?;
    write_if_needed(&directory.join("WinDivert64.sys"), WINDIVERT_SYS_BYTES)?;
    Ok(dll_path)
}

fn write_if_needed(path: &Path, bytes: &[u8]) -> Result<(), String> {
    if let Ok(existing) = std::fs::read(path) {
        if existing == bytes {
            return Ok(());
        }
    }

    let mut temp = path.as_os_str().to_os_string();
    temp.push(".tmp");
    let temp = PathBuf::from(temp);
    std::fs::write(&temp, bytes)
        .map_err(|error| format!("写入 {} 失败：{error}", temp.display()))?;
    if let Err(error) = std::fs::rename(&temp, path) {
        let _ = std::fs::remove_file(&temp);
        return Err(format!("替换 {} 失败：{error}", path.display()));
    }
    Ok(())
}

/// 返回已就绪的 WinDivert.dll 路径；必要时把内嵌运行时解压到磁盘。
pub fn resolve_dll_path() -> Result<PathBuf, String> {
    let mut last_error = String::from("没有可用的运行时目录。");
    for directory in runtime_candidates() {
        match ensure_payload_dir(&directory) {
            Ok(dll_path) => {
                cleanup_old_versions(&directory);
                return Ok(dll_path);
            }
            Err(error) => last_error = error,
        }
    }
    Err(format!("无法准备 WinDivert 运行时：{last_error}"))
}

fn cleanup_old_versions(current_dll: &Path) {
    let Some(current_dir) = current_dll.parent() else {
        return;
    };
    let Some(root) = current_dir.parent() else {
        return;
    };
    let Ok(entries) = std::fs::read_dir(root) else {
        return;
    };
    for entry in entries.flatten() {
        let path = entry.path();
        if path == current_dir || !path.is_dir() {
            continue;
        }
        let _ = std::fs::remove_dir_all(path);
    }
}

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
    let dll_path = resolve_dll_path()?;
    let wide_path = to_wide(&dll_path.to_string_lossy());
    let module = unsafe {
        LoadLibraryExW(
            wide_path.as_ptr(),
            std::ptr::null_mut(),
            LOAD_WITH_ALTERED_SEARCH_PATH,
        )
    };
    if module.is_null() {
        let error = std::io::Error::last_os_error();
        return Err(format!(
            "无法加载 WinDivert.dll（{}）：{error}",
            dll_path.display()
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
    if let Some(api) = API.get() {
        return Ok(api);
    }
    match load() {
        Ok(api) => {
            // 只缓存成功结果；DLL/驱动暂时不可用时允许后续启动重试。
            let _ = API.set(api);
            Ok(API.get().expect("WinDivert API initialized"))
        }
        Err(error) => Err(error),
    }
}

pub fn describe_open_error(code: i32) -> String {
    match code {
        2 => {
            "找不到 WinDivert64.sys。运行时文件可能不完整，请删除 %ProgramFiles%\\NetConditioner\\runtime 后重试。"
                .to_string()
        }
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
    let handle = unsafe { (api.open)(filter.as_ptr(), LAYER_NETWORK, 0, 0) };
    if handle == 0 || handle == -1 {
        return Err(describe_open_error(last_error_code()));
    }
    Ok(handle)
}

/// 尽力设置 WinDivert 队列参数，返回可选的告警文本。
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

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn embedded_payload_matches_files() {
        assert_eq!(
            WINDIVERT_DLL_BYTES,
            std::fs::read("third_party/WinDivert/WinDivert.dll").expect("dll")
        );
        assert_eq!(
            WINDIVERT_SYS_BYTES,
            std::fs::read("third_party/WinDivert/WinDivert64.sys").expect("sys")
        );
        assert_eq!(payload_version(), "2.2.2");
    }

    #[test]
    fn extracts_payload_to_directory() {
        let directory =
            std::env::temp_dir().join(format!("nc-windivert-runtime-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&directory);

        let dll_path = ensure_payload_dir(&directory).expect("extract");
        assert_eq!(std::fs::read(&dll_path).expect("dll"), WINDIVERT_DLL_BYTES);
        assert_eq!(
            std::fs::read(directory.join("WinDivert64.sys")).expect("sys"),
            WINDIVERT_SYS_BYTES
        );

        // 第二次调用应复用已就绪的文件。
        assert!(ensure_payload_dir(&directory).is_ok());
        assert_eq!(std::fs::read(&dll_path).expect("dll"), WINDIVERT_DLL_BYTES);

        let _ = std::fs::remove_dir_all(&directory);
    }
}
