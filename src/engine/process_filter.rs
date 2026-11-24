//! 根据系统 TCP/UDP 表把本地端点映射到进程 ID（对应原 `ProcessFlowFilter.cs`）。

use std::collections::{HashMap, HashSet};
use std::ffi::c_void;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, Mutex, RwLock, Weak};
use std::thread::{self, JoinHandle};
use std::time::{Duration, Instant};

use windows_sys::Win32::Foundation::{CloseHandle, INVALID_HANDLE_VALUE};
use windows_sys::Win32::NetworkManagement::IpHelper::{
    GetExtendedTcpTable, GetExtendedUdpTable, MIB_TCP6ROW_OWNER_PID, MIB_TCPROW_OWNER_PID,
    MIB_UDP6ROW_OWNER_PID, MIB_UDPROW_OWNER_PID, TCP_TABLE_OWNER_PID_ALL, UDP_TABLE_OWNER_PID,
};
use windows_sys::Win32::System::Diagnostics::ToolHelp::{
    CreateToolhelp32Snapshot, Process32FirstW, Process32NextW, PROCESSENTRY32W, TH32CS_SNAPPROCESS,
};
use windows_sys::Win32::System::Threading::{
    OpenProcess, QueryFullProcessImageNameW, PROCESS_QUERY_LIMITED_INFORMATION,
};

use crate::engine::flow::{normalize_address, FlowEndpoint};

const AF_INET: u32 = 2;
const AF_INET6: u32 = 23;
const ERROR_INSUFFICIENT_BUFFER: u32 = 122;
const REFRESH_INTERVAL: Duration = Duration::from_secs(2);
const ON_DEMAND_INTERVAL: Duration = Duration::from_millis(50);

/// 系统进程表里的一条记录（用于“选择目标进程”界面）。
#[derive(Debug, Clone)]
pub struct ProcessInfo {
    /// 规范化后的显示名，如 `chrome.exe`。
    pub name: String,
    /// 可执行文件路径；受保护进程可能为空。
    pub path: String,
}

/// 快照：TCP/UDP 本地端点 → PID，以及目标进程的 PID 集合。
struct Snapshot {
    tcp: HashMap<FlowEndpoint, u32>,
    udp: HashMap<FlowEndpoint, u32>,
    pids: HashSet<u32>,
}

impl Snapshot {
    fn empty() -> Self {
        Self {
            tcp: HashMap::new(),
            udp: HashMap::new(),
            pids: HashSet::new(),
        }
    }

    fn is_target(&self, protocol: u8, local: FlowEndpoint) -> bool {
        let map = match protocol {
            6 => &self.tcp,
            17 => &self.udp,
            _ => return false,
        };

        if self.pids.is_empty() {
            return false;
        }

        if let Some(pid) = map.get(&local) {
            if self.pids.contains(pid) {
                return true;
            }
        }

        // 绑定在通配地址（0.0.0.0 / ::）上的监听端口。
        if local.address != 0 {
            let wildcard = FlowEndpoint {
                family: local.family,
                address: 0,
                port: local.port,
            };
            if let Some(pid) = map.get(&wildcard) {
                if self.pids.contains(pid) {
                    return true;
                }
            }
        }

        // 双栈监听（绑定 ::）可能承载 IPv4 连接。
        if local.family == 4 && local.address != 0 {
            let dual_stack = FlowEndpoint {
                family: 6,
                address: 0,
                port: local.port,
            };
            if let Some(pid) = map.get(&dual_stack) {
                if self.pids.contains(pid) {
                    return true;
                }
            }
        }

        false
    }
}

pub struct ProcessFlowFilter {
    names: HashSet<String>,
    snapshot: RwLock<Arc<Snapshot>>,
    stop: AtomicBool,
    last_refresh: Mutex<Instant>,
    thread: Mutex<Option<JoinHandle<()>>>,
}

impl ProcessFlowFilter {
    pub fn new(names: &[String]) -> Arc<Self> {
        let names = normalize_names(names);
        let stale = Instant::now() - REFRESH_INTERVAL;
        let filter = Arc::new(Self {
            names,
            snapshot: RwLock::new(Arc::new(Snapshot::empty())),
            stop: AtomicBool::new(false),
            last_refresh: Mutex::new(stale),
            thread: Mutex::new(None),
        });

        if filter.names.is_empty() {
            return filter;
        }

        filter.refresh_now();

        let weak: Weak<ProcessFlowFilter> = Arc::downgrade(&filter);
        let handle = thread::Builder::new()
            .name("NetConditioner-process-filter".to_string())
            .spawn(move || {
                let step = Duration::from_millis(100);
                let mut elapsed = Duration::ZERO;
                loop {
                    let Some(filter) = weak.upgrade() else {
                        break;
                    };
                    if filter.stop.load(Ordering::Acquire) {
                        break;
                    }
                    drop(filter);
                    thread::sleep(step);
                    elapsed += step;
                    if elapsed >= REFRESH_INTERVAL {
                        elapsed = Duration::ZERO;
                        if let Some(filter) = weak.upgrade() {
                            filter.refresh_now();
                        } else {
                            break;
                        }
                    }
                }
            })
            .ok();

        *filter.thread.lock().unwrap() = handle;
        filter
    }

    /// 只判断本端端点；协议必须是 6 (TCP) 或 17 (UDP)。
    pub fn is_match(&self, protocol: u8, local: FlowEndpoint) -> bool {
        if self.names.is_empty() {
            return true;
        }

        if self.current_snapshot().is_target(protocol, local) {
            return true;
        }

        // 新连接可能还没有出现在上一次快照里。
        self.refresh_on_demand();
        self.current_snapshot().is_target(protocol, local)
    }

    fn current_snapshot(&self) -> Arc<Snapshot> {
        match self.snapshot.read() {
            Ok(guard) => guard.clone(),
            Err(poisoned) => poisoned.into_inner().clone(),
        }
    }

    fn refresh_on_demand(&self) {
        let Ok(mut last) = self.last_refresh.try_lock() else {
            return;
        };
        if last.elapsed() < ON_DEMAND_INTERVAL {
            return;
        }
        *last = Instant::now();
        drop(last);
        self.refresh_now();
    }

    fn refresh_now(&self) {
        if self.stop.load(Ordering::Acquire) {
            return;
        }

        let pids = resolve_pids(&self.names);
        let mut snapshot = Snapshot {
            tcp: HashMap::new(),
            udp: HashMap::new(),
            pids,
        };
        read_tcp_table(AF_INET, &mut snapshot.tcp);
        read_tcp_table(AF_INET6, &mut snapshot.tcp);
        read_udp_table(AF_INET, &mut snapshot.udp);
        read_udp_table(AF_INET6, &mut snapshot.udp);

        match self.snapshot.write() {
            Ok(mut guard) => *guard = Arc::new(snapshot),
            Err(poisoned) => *poisoned.into_inner() = Arc::new(snapshot),
        }

        if let Ok(mut last) = self.last_refresh.lock() {
            *last = Instant::now();
        }
    }
}

impl Drop for ProcessFlowFilter {
    fn drop(&mut self) {
        self.stop.store(true, Ordering::Release);
        if let Ok(mut guard) = self.thread.lock() {
            if let Some(handle) = guard.take() {
                let _ = handle.join();
            }
        }
    }
}

fn normalize_names(names: &[String]) -> HashSet<String> {
    let mut result = HashSet::new();
    for name in names {
        let normalized = crate::config::normalize_process_name(name);
        if !normalized.is_empty() {
            result.insert(normalized);
        }
    }
    result
}

fn resolve_pids(names: &HashSet<String>) -> HashSet<u32> {
    let mut pids = HashSet::new();
    for_each_process(|pid, image_name| {
        let normalized = crate::config::normalize_process_name(image_name);
        if names.contains(&normalized) {
            pids.insert(pid);
        }
    });
    pids
}

/// 枚举当前所有进程（用于“选择目标进程”界面）。
pub fn enumerate_processes() -> Vec<ProcessInfo> {
    let mut result = Vec::new();
    for_each_process(|pid, image_name| {
        let display = crate::config::normalize_display_name(image_name);
        if display.is_empty() {
            return;
        }
        let path = process_path(pid).unwrap_or_default();
        let _ = pid;
        result.push(ProcessInfo {
            name: display,
            path,
        });
    });
    result
}

fn process_path(pid: u32) -> Option<String> {
    if pid == 0 {
        return None;
    }
    let handle = unsafe { OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, 0, pid) };
    if handle.is_null() {
        return None;
    }

    let mut buffer = vec![0u16; 1024];
    let mut size = buffer.len() as u32;
    let ok = unsafe { QueryFullProcessImageNameW(handle, 0, buffer.as_mut_ptr(), &mut size) };
    unsafe { CloseHandle(handle) };
    if ok == 0 || size == 0 {
        return None;
    }

    Some(String::from_utf16_lossy(&buffer[..size as usize]))
}

fn for_each_process<F: FnMut(u32, &str)>(mut callback: F) {
    let snapshot = unsafe { CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0) };
    if snapshot.is_null() || snapshot == INVALID_HANDLE_VALUE {
        return;
    }

    let mut entry: PROCESSENTRY32W = unsafe { std::mem::zeroed() };
    entry.dwSize = std::mem::size_of::<PROCESSENTRY32W>() as u32;

    if unsafe { Process32FirstW(snapshot, &mut entry) } != 0 {
        loop {
            let name = wide_to_string(&entry.szExeFile);
            callback(entry.th32ProcessID, &name);
            if unsafe { Process32NextW(snapshot, &mut entry) } == 0 {
                break;
            }
        }
    }

    unsafe { CloseHandle(snapshot) };
}

fn wide_to_string(buffer: &[u16]) -> String {
    let length = buffer.iter().position(|&c| c == 0).unwrap_or(buffer.len());
    String::from_utf16_lossy(&buffer[..length])
}

fn read_tcp_table(family: u32, map: &mut HashMap<FlowEndpoint, u32>) {
    let mut size: u32 = 0;
    let mut result = unsafe {
        GetExtendedTcpTable(
            std::ptr::null_mut(),
            &mut size,
            0,
            family,
            TCP_TABLE_OWNER_PID_ALL,
            0,
        )
    };
    if (result != 0 && result != ERROR_INSUFFICIENT_BUFFER) || size == 0 {
        return;
    }

    let mut buffer = vec![0u8; size as usize];
    result = unsafe {
        GetExtendedTcpTable(
            buffer.as_mut_ptr() as *mut c_void,
            &mut size,
            0,
            family,
            TCP_TABLE_OWNER_PID_ALL,
            0,
        )
    };
    if result != 0 || buffer.len() < 4 {
        return;
    }

    let count = u32::from_ne_bytes([buffer[0], buffer[1], buffer[2], buffer[3]]) as usize;

    if family == AF_INET {
        let row_size = std::mem::size_of::<MIB_TCPROW_OWNER_PID>();
        for index in 0..count {
            let offset = 4 + index * row_size;
            if offset + row_size > buffer.len() {
                break;
            }
            let row: MIB_TCPROW_OWNER_PID =
                unsafe { std::ptr::read_unaligned(buffer.as_ptr().add(offset) as *const _) };
            let port = (row.dwLocalPort as u16).swap_bytes();
            if port == 0 {
                continue;
            }
            let address = u32::from_be_bytes(row.dwLocalAddr.to_ne_bytes());
            map.insert(
                FlowEndpoint {
                    family: 4,
                    address: address as u128,
                    port,
                },
                row.dwOwningPid,
            );
        }
    } else {
        let row_size = std::mem::size_of::<MIB_TCP6ROW_OWNER_PID>();
        for index in 0..count {
            let offset = 4 + index * row_size;
            if offset + row_size > buffer.len() {
                break;
            }
            let row: MIB_TCP6ROW_OWNER_PID =
                unsafe { std::ptr::read_unaligned(buffer.as_ptr().add(offset) as *const _) };
            let port = (row.dwLocalPort as u16).swap_bytes();
            if port == 0 {
                continue;
            }
            let (family, address) = normalize_address(u128::from_be_bytes(row.ucLocalAddr));
            map.insert(
                FlowEndpoint {
                    family,
                    address,
                    port,
                },
                row.dwOwningPid,
            );
        }
    }
}

fn read_udp_table(family: u32, map: &mut HashMap<FlowEndpoint, u32>) {
    let mut size: u32 = 0;
    let mut result = unsafe {
        GetExtendedUdpTable(
            std::ptr::null_mut(),
            &mut size,
            0,
            family,
            UDP_TABLE_OWNER_PID,
            0,
        )
    };
    if (result != 0 && result != ERROR_INSUFFICIENT_BUFFER) || size == 0 {
        return;
    }

    let mut buffer = vec![0u8; size as usize];
    result = unsafe {
        GetExtendedUdpTable(
            buffer.as_mut_ptr() as *mut c_void,
            &mut size,
            0,
            family,
            UDP_TABLE_OWNER_PID,
            0,
        )
    };
    if result != 0 || buffer.len() < 4 {
        return;
    }

    let count = u32::from_ne_bytes([buffer[0], buffer[1], buffer[2], buffer[3]]) as usize;

    if family == AF_INET {
        let row_size = std::mem::size_of::<MIB_UDPROW_OWNER_PID>();
        for index in 0..count {
            let offset = 4 + index * row_size;
            if offset + row_size > buffer.len() {
                break;
            }
            let row: MIB_UDPROW_OWNER_PID =
                unsafe { std::ptr::read_unaligned(buffer.as_ptr().add(offset) as *const _) };
            let port = (row.dwLocalPort as u16).swap_bytes();
            if port == 0 {
                continue;
            }
            let address = u32::from_be_bytes(row.dwLocalAddr.to_ne_bytes());
            map.insert(
                FlowEndpoint {
                    family: 4,
                    address: address as u128,
                    port,
                },
                row.dwOwningPid,
            );
        }
    } else {
        let row_size = std::mem::size_of::<MIB_UDP6ROW_OWNER_PID>();
        for index in 0..count {
            let offset = 4 + index * row_size;
            if offset + row_size > buffer.len() {
                break;
            }
            let row: MIB_UDP6ROW_OWNER_PID =
                unsafe { std::ptr::read_unaligned(buffer.as_ptr().add(offset) as *const _) };
            let port = (row.dwLocalPort as u16).swap_bytes();
            if port == 0 {
                continue;
            }
            let (family, address) = normalize_address(u128::from_be_bytes(row.ucLocalAddr));
            map.insert(
                FlowEndpoint {
                    family,
                    address,
                    port,
                },
                row.dwOwningPid,
            );
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn normalizes_process_names() {
        assert_eq!(
            crate::config::normalize_process_name("Chrome.EXE"),
            "chrome"
        );
        assert_eq!(
            crate::config::normalize_process_name(r"C:\Tools\MyApp.EXE"),
            "myapp"
        );
        assert_eq!(
            crate::config::normalize_process_name("  notepad.exe  "),
            "notepad"
        );
        assert_eq!(
            crate::config::normalize_display_name("chrome.exe"),
            "chrome.exe"
        );
        assert_eq!(crate::config::normalize_display_name(""), "");
    }

    #[test]
    fn snapshot_matching_handles_wildcards() {
        let mut snapshot = Snapshot::empty();
        snapshot.pids.insert(42);
        snapshot.tcp.insert(
            FlowEndpoint {
                family: 4,
                address: 0,
                port: 80,
            },
            42,
        );
        snapshot.udp.insert(
            FlowEndpoint {
                family: 6,
                address: 0,
                port: 5000,
            },
            42,
        );

        // 通配监听命中。
        assert!(snapshot.is_target(
            6,
            FlowEndpoint {
                family: 4,
                address: 0x0A000001,
                port: 80
            }
        ));
        // 双栈监听承载 IPv4 连接。
        assert!(snapshot.is_target(
            17,
            FlowEndpoint {
                family: 4,
                address: 0xC0A80001,
                port: 5000
            }
        ));
        // 未命中的端口。
        assert!(!snapshot.is_target(
            6,
            FlowEndpoint {
                family: 4,
                address: 0x0A000001,
                port: 81
            }
        ));
        // 非 TCP/UDP 协议。
        assert!(!snapshot.is_target(
            1,
            FlowEndpoint {
                family: 4,
                address: 0x0A000001,
                port: 80
            }
        ));
    }
}
