//! 根据系统 TCP/UDP 表把本地端点映射到进程 ID。

use std::collections::{HashMap, HashSet};
use std::ffi::c_void;
use std::sync::atomic::{AtomicBool, AtomicU64, Ordering};
use std::sync::{Arc, Mutex, RwLock, Weak};
use std::thread::{self, JoinHandle, ThreadId};
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
const ON_DEMAND_INTERVAL: Duration = Duration::from_millis(100);
const NEGATIVE_CACHE_TTL: Duration = Duration::from_secs(2);
const NEGATIVE_CACHE_LIMIT: usize = 4096;
const TABLE_READ_ATTEMPTS: usize = 4;

/// 系统进程表里的一条记录（用于“选择目标进程”界面）。
#[derive(Debug, Clone)]
pub struct ProcessInfo {
    /// 规范化后的显示名，如 `chrome.exe`。
    pub name: String,
    /// 可执行文件路径；受保护进程可能为空。
    pub path: String,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
struct TcpFlowKey {
    local: FlowEndpoint,
    remote: FlowEndpoint,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
struct FlowKey {
    protocol: u8,
    local: FlowEndpoint,
    remote: Option<FlowEndpoint>,
}

/// 快照：TCP 五元组/本地端点、UDP 本地端点 → PID 集合，以及目标进程 PID 集合。
struct Snapshot {
    tcp_flows: HashMap<TcpFlowKey, HashSet<u32>>,
    tcp: HashMap<FlowEndpoint, HashSet<u32>>,
    udp: HashMap<FlowEndpoint, HashSet<u32>>,
    pids: HashSet<u32>,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum MatchState {
    Target,
    NonTarget,
    Unknown,
}

impl Snapshot {
    fn empty() -> Self {
        Self {
            tcp_flows: HashMap::new(),
            tcp: HashMap::new(),
            udp: HashMap::new(),
            pids: HashSet::new(),
        }
    }

    fn lookup(
        &self,
        protocol: u8,
        local: FlowEndpoint,
        remote: Option<FlowEndpoint>,
    ) -> MatchState {
        if self.pids.is_empty() {
            return MatchState::NonTarget;
        }

        match protocol {
            6 => {
                if let Some(remote) = remote {
                    if let Some(owners) = self.tcp_flows.get(&TcpFlowKey { local, remote }) {
                        return owners_match(owners, &self.pids, false);
                    }
                }
                lookup_endpoint(&self.tcp, local, &self.pids, false)
            }
            17 => lookup_endpoint(&self.udp, local, &self.pids, true),
            _ => MatchState::NonTarget,
        }
    }

    #[cfg(test)]
    fn is_target(&self, protocol: u8, local: FlowEndpoint) -> bool {
        self.lookup(protocol, local, None) == MatchState::Target
    }
}

fn owners_match(
    owners: &HashSet<u32>,
    pids: &HashSet<u32>,
    fail_open_ambiguous: bool,
) -> MatchState {
    if owners.is_empty() {
        return MatchState::Unknown;
    }
    if fail_open_ambiguous && owners.len() > 1 {
        // UDP 表无法区分共享同一本地端口的多个 socket，直通以避免误限速非目标进程。
        return MatchState::NonTarget;
    }
    if owners.iter().any(|pid| pids.contains(pid)) {
        MatchState::Target
    } else {
        MatchState::NonTarget
    }
}

fn lookup_endpoint(
    map: &HashMap<FlowEndpoint, HashSet<u32>>,
    local: FlowEndpoint,
    pids: &HashSet<u32>,
    fail_open_ambiguous: bool,
) -> MatchState {
    // 精确端点始终优先；只有不存在精确行时才回退到通配监听。
    if let Some(owners) = map.get(&local) {
        return owners_match(owners, pids, fail_open_ambiguous);
    }

    if local.address != 0 {
        let wildcard = FlowEndpoint {
            family: local.family,
            address: 0,
            port: local.port,
        };
        if let Some(owners) = map.get(&wildcard) {
            return owners_match(owners, pids, fail_open_ambiguous);
        }
    }

    // 双栈监听（绑定 ::）可能承载 IPv4 连接。
    if local.family == 4 && local.address != 0 {
        let dual_stack = FlowEndpoint {
            family: 6,
            address: 0,
            port: local.port,
        };
        if let Some(owners) = map.get(&dual_stack) {
            return owners_match(owners, pids, fail_open_ambiguous);
        }
    }

    MatchState::Unknown
}

struct SnapshotSlot {
    snapshot: RwLock<Arc<Snapshot>>,
    published_generation: Mutex<u64>,
}

impl SnapshotSlot {
    fn new(snapshot: Snapshot) -> Self {
        Self {
            snapshot: RwLock::new(Arc::new(snapshot)),
            published_generation: Mutex::new(0),
        }
    }

    fn current(&self) -> Arc<Snapshot> {
        match self.snapshot.read() {
            Ok(guard) => guard.clone(),
            Err(poisoned) => poisoned.into_inner().clone(),
        }
    }

    fn try_publish(&self, generation: u64, snapshot: Snapshot) -> bool {
        let mut published = match self.published_generation.lock() {
            Ok(guard) => guard,
            Err(poisoned) => poisoned.into_inner(),
        };
        if generation <= *published {
            return false;
        }

        let mut current = match self.snapshot.write() {
            Ok(guard) => guard,
            Err(poisoned) => poisoned.into_inner(),
        };
        *current = Arc::new(snapshot);
        *published = generation;
        true
    }
}

#[derive(Default)]
struct NegativeCache {
    entries: HashMap<FlowKey, Instant>,
}

impl NegativeCache {
    fn contains(&self, key: FlowKey, now: Instant) -> bool {
        self.entries
            .get(&key)
            .is_some_and(|expires| now.duration_since(*expires) < NEGATIVE_CACHE_TTL)
    }

    fn insert(&mut self, key: FlowKey, now: Instant) {
        if self.entries.len() >= NEGATIVE_CACHE_LIMIT && !self.entries.contains_key(&key) {
            self.entries.clear();
        }
        self.entries.insert(key, now);
    }
}

pub struct ProcessFlowFilter {
    names: HashSet<String>,
    snapshot: SnapshotSlot,
    stop: AtomicBool,
    last_refresh: Mutex<Instant>,
    refresh_lock: Mutex<()>,
    next_generation: AtomicU64,
    refresh_count: AtomicU64,
    negative_cache: Mutex<NegativeCache>,
    thread: Mutex<Option<JoinHandle<()>>>,
    thread_id: Mutex<Option<ThreadId>>,
}

impl ProcessFlowFilter {
    pub fn new(names: &[String]) -> Arc<Self> {
        let names = normalize_names(names);
        let stale = Instant::now()
            .checked_sub(REFRESH_INTERVAL)
            .unwrap_or_else(Instant::now);
        let filter = Arc::new(Self {
            names,
            snapshot: SnapshotSlot::new(Snapshot::empty()),
            stop: AtomicBool::new(false),
            last_refresh: Mutex::new(stale),
            refresh_lock: Mutex::new(()),
            next_generation: AtomicU64::new(0),
            refresh_count: AtomicU64::new(0),
            negative_cache: Mutex::new(NegativeCache::default()),
            thread: Mutex::new(None),
            thread_id: Mutex::new(None),
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
                    if let Ok(mut thread_id) = filter.thread_id.lock() {
                        *thread_id = Some(thread::current().id());
                    }
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

    #[cfg(test)]
    pub fn is_match(&self, protocol: u8, local: FlowEndpoint) -> bool {
        self.is_match_flow(protocol, local, None)
    }

    /// 优先使用完整 TCP 五元组；UDP 没有远端信息。
    pub fn is_match_flow(
        &self,
        protocol: u8,
        local: FlowEndpoint,
        remote: Option<FlowEndpoint>,
    ) -> bool {
        if self.names.is_empty() {
            return true;
        }

        let snapshot = self.current_snapshot();
        match snapshot.lookup(protocol, local, remote) {
            MatchState::Target => return true,
            MatchState::NonTarget => return false,
            MatchState::Unknown => {}
        }

        let key = FlowKey {
            protocol,
            local,
            remote,
        };
        let now = Instant::now();
        if self
            .negative_cache
            .lock()
            .unwrap_or_else(|poisoned| poisoned.into_inner())
            .contains(key, now)
        {
            return false;
        }

        // 只有未知流才触发按需刷新；已知非目标流量不会造成刷新风暴。
        let refreshed = self.refresh_on_demand();
        match self.current_snapshot().lookup(protocol, local, remote) {
            MatchState::Target => true,
            MatchState::NonTarget => false,
            MatchState::Unknown => {
                if refreshed {
                    self.negative_cache
                        .lock()
                        .unwrap_or_else(|poisoned| poisoned.into_inner())
                        .insert(key, Instant::now());
                }
                false
            }
        }
    }

    fn current_snapshot(&self) -> Arc<Snapshot> {
        self.snapshot.current()
    }

    fn refresh_on_demand(&self) -> bool {
        let now = Instant::now();
        let Ok(mut last) = self.last_refresh.try_lock() else {
            return false;
        };
        if !refresh_allowed(*last, now, ON_DEMAND_INTERVAL) {
            return false;
        }
        *last = now;
        drop(last);
        self.refresh_now()
    }

    fn refresh_now(&self) -> bool {
        if self.stop.load(Ordering::Acquire) {
            return false;
        }

        // 串行化所有刷新，避免后台刷新与按需刷新交叉发布。
        let _refresh_guard = match self.refresh_lock.lock() {
            Ok(guard) => guard,
            Err(poisoned) => poisoned.into_inner(),
        };
        if self.stop.load(Ordering::Acquire) {
            return false;
        }

        let pids = resolve_pids(&self.names);
        let Some(tcp4) = read_tcp_table(AF_INET) else {
            return false;
        };
        let Some(tcp6) = read_tcp_table(AF_INET6) else {
            return false;
        };
        let Some(udp4) = read_udp_table(AF_INET) else {
            return false;
        };
        let Some(udp6) = read_udp_table(AF_INET6) else {
            return false;
        };

        let mut snapshot = Snapshot {
            tcp_flows: tcp4.flows,
            tcp: tcp4.local,
            udp: udp4,
            pids,
        };
        snapshot.tcp_flows.extend(tcp6.flows);
        snapshot.tcp.extend(tcp6.local);
        snapshot.udp.extend(udp6);

        let generation = self.next_generation.fetch_add(1, Ordering::AcqRel) + 1;
        if self.snapshot.try_publish(generation, snapshot) {
            self.refresh_count.fetch_add(1, Ordering::Relaxed);
        }
        if let Ok(mut last) = self.last_refresh.lock() {
            *last = Instant::now();
        }
        true
    }

    /// 显式停止后台线程。调用方应在最终释放所有 `Arc` 前调用。
    pub fn shutdown(&self) {
        self.stop.store(true, Ordering::Release);
        let handle = match self.thread.lock() {
            Ok(mut guard) => guard.take(),
            Err(poisoned) => poisoned.into_inner().take(),
        };
        let Some(handle) = handle else {
            return;
        };

        let current = thread::current().id();
        let is_self = self
            .thread_id
            .lock()
            .ok()
            .and_then(|guard| *guard)
            .is_some_and(|id| id == current);
        if is_self {
            drop(handle);
        } else {
            let _ = handle.join();
        }
    }

    #[cfg(test)]
    fn with_snapshot(names: &[String], snapshot: Snapshot) -> Arc<Self> {
        Arc::new(Self {
            names: normalize_names(names),
            snapshot: SnapshotSlot::new(snapshot),
            stop: AtomicBool::new(false),
            last_refresh: Mutex::new(Instant::now()),
            refresh_lock: Mutex::new(()),
            next_generation: AtomicU64::new(0),
            refresh_count: AtomicU64::new(0),
            negative_cache: Mutex::new(NegativeCache::default()),
            thread: Mutex::new(None),
            thread_id: Mutex::new(None),
        })
    }
}

impl Drop for ProcessFlowFilter {
    fn drop(&mut self) {
        self.shutdown();
    }
}

fn refresh_allowed(last: Instant, now: Instant, interval: Duration) -> bool {
    now.duration_since(last) >= interval
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

fn read_dynamic_table(mut query: impl FnMut(*mut c_void, &mut u32) -> u32) -> Option<Vec<u8>> {
    for _ in 0..TABLE_READ_ATTEMPTS {
        let mut size = 0u32;
        let result = query(std::ptr::null_mut(), &mut size);
        if result != 0 && result != ERROR_INSUFFICIENT_BUFFER {
            return None;
        }
        if size == 0 {
            return None;
        }

        let mut buffer = vec![0u8; size as usize];
        let result = query(buffer.as_mut_ptr() as *mut c_void, &mut size);
        match result {
            0 => {
                if size as usize > buffer.len() {
                    return None;
                }
                buffer.truncate(size as usize);
                return Some(buffer);
            }
            ERROR_INSUFFICIENT_BUFFER => continue,
            _ => return None,
        }
    }
    None
}

struct TcpTable {
    local: HashMap<FlowEndpoint, HashSet<u32>>,
    flows: HashMap<TcpFlowKey, HashSet<u32>>,
}

fn read_tcp_table(family: u32) -> Option<TcpTable> {
    let buffer = read_dynamic_table(|buffer, size| unsafe {
        GetExtendedTcpTable(buffer, size, 0, family, TCP_TABLE_OWNER_PID_ALL, 0)
    })?;
    if buffer.len() < 4 {
        return None;
    }

    let count = u32::from_ne_bytes([buffer[0], buffer[1], buffer[2], buffer[3]]) as usize;
    let mut table = TcpTable {
        local: HashMap::new(),
        flows: HashMap::new(),
    };

    if family == AF_INET {
        let row_size = std::mem::size_of::<MIB_TCPROW_OWNER_PID>();
        for index in 0..count {
            let offset = 4 + index * row_size;
            if offset + row_size > buffer.len() {
                return None;
            }
            let row: MIB_TCPROW_OWNER_PID =
                unsafe { std::ptr::read_unaligned(buffer.as_ptr().add(offset) as *const _) };
            let local = FlowEndpoint {
                family: 4,
                address: u32::from_be_bytes(row.dwLocalAddr.to_ne_bytes()) as u128,
                port: (row.dwLocalPort as u16).swap_bytes(),
            };
            if local.port == 0 {
                continue;
            }
            let remote = FlowEndpoint {
                family: 4,
                address: u32::from_be_bytes(row.dwRemoteAddr.to_ne_bytes()) as u128,
                port: (row.dwRemotePort as u16).swap_bytes(),
            };
            insert_owner(&mut table.local, local, row.dwOwningPid);
            insert_owner(
                &mut table.flows,
                TcpFlowKey { local, remote },
                row.dwOwningPid,
            );
        }
    } else {
        let row_size = std::mem::size_of::<MIB_TCP6ROW_OWNER_PID>();
        for index in 0..count {
            let offset = 4 + index * row_size;
            if offset + row_size > buffer.len() {
                return None;
            }
            let row: MIB_TCP6ROW_OWNER_PID =
                unsafe { std::ptr::read_unaligned(buffer.as_ptr().add(offset) as *const _) };
            let (local_family, local_address) =
                normalize_address(u128::from_be_bytes(row.ucLocalAddr));
            let (remote_family, remote_address) =
                normalize_address(u128::from_be_bytes(row.ucRemoteAddr));
            let local = FlowEndpoint {
                family: local_family,
                address: local_address,
                port: (row.dwLocalPort as u16).swap_bytes(),
            };
            if local.port == 0 {
                continue;
            }
            let remote = FlowEndpoint {
                family: remote_family,
                address: remote_address,
                port: (row.dwRemotePort as u16).swap_bytes(),
            };
            insert_owner(&mut table.local, local, row.dwOwningPid);
            insert_owner(
                &mut table.flows,
                TcpFlowKey { local, remote },
                row.dwOwningPid,
            );
        }
    }

    Some(table)
}

fn read_udp_table(family: u32) -> Option<HashMap<FlowEndpoint, HashSet<u32>>> {
    let buffer = read_dynamic_table(|buffer, size| unsafe {
        GetExtendedUdpTable(buffer, size, 0, family, UDP_TABLE_OWNER_PID, 0)
    })?;
    if buffer.len() < 4 {
        return None;
    }

    let count = u32::from_ne_bytes([buffer[0], buffer[1], buffer[2], buffer[3]]) as usize;
    let mut map = HashMap::new();

    if family == AF_INET {
        let row_size = std::mem::size_of::<MIB_UDPROW_OWNER_PID>();
        for index in 0..count {
            let offset = 4 + index * row_size;
            if offset + row_size > buffer.len() {
                return None;
            }
            let row: MIB_UDPROW_OWNER_PID =
                unsafe { std::ptr::read_unaligned(buffer.as_ptr().add(offset) as *const _) };
            let endpoint = FlowEndpoint {
                family: 4,
                address: u32::from_be_bytes(row.dwLocalAddr.to_ne_bytes()) as u128,
                port: (row.dwLocalPort as u16).swap_bytes(),
            };
            if endpoint.port != 0 {
                insert_owner(&mut map, endpoint, row.dwOwningPid);
            }
        }
    } else {
        let row_size = std::mem::size_of::<MIB_UDP6ROW_OWNER_PID>();
        for index in 0..count {
            let offset = 4 + index * row_size;
            if offset + row_size > buffer.len() {
                return None;
            }
            let row: MIB_UDP6ROW_OWNER_PID =
                unsafe { std::ptr::read_unaligned(buffer.as_ptr().add(offset) as *const _) };
            let (family, address) = normalize_address(u128::from_be_bytes(row.ucLocalAddr));
            let endpoint = FlowEndpoint {
                family,
                address,
                port: (row.dwLocalPort as u16).swap_bytes(),
            };
            if endpoint.port != 0 {
                insert_owner(&mut map, endpoint, row.dwOwningPid);
            }
        }
    }

    Some(map)
}

fn insert_owner<K: Eq + std::hash::Hash>(map: &mut HashMap<K, HashSet<u32>>, key: K, pid: u32) {
    map.entry(key).or_default().insert(pid);
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::sync::atomic::AtomicUsize;

    fn endpoint(family: u8, address: u128, port: u16) -> FlowEndpoint {
        FlowEndpoint {
            family,
            address,
            port,
        }
    }

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
        insert_owner(&mut snapshot.tcp, endpoint(4, 0, 80), 42);
        insert_owner(&mut snapshot.udp, endpoint(6, 0, 5000), 42);

        assert!(snapshot.is_target(6, endpoint(4, 0x0A000001, 80)));
        assert!(snapshot.is_target(17, endpoint(4, 0xC0A80101, 5000)));
        assert!(!snapshot.is_target(6, endpoint(4, 0x0A000001, 81)));
        assert!(!snapshot.is_target(1, endpoint(4, 0x0A000001, 80)));
    }

    #[test]
    fn exact_endpoint_is_authoritative_over_wildcard() {
        let mut snapshot = Snapshot::empty();
        snapshot.pids.insert(42);
        insert_owner(&mut snapshot.tcp, endpoint(4, 0, 80), 42);
        insert_owner(&mut snapshot.tcp, endpoint(4, 0x0A000001, 80), 77);

        assert_eq!(
            snapshot.lookup(6, endpoint(4, 0x0A000001, 80), None),
            MatchState::NonTarget
        );

        let mut snapshot = Snapshot::empty();
        snapshot.pids.insert(42);
        insert_owner(&mut snapshot.tcp, endpoint(4, 0, 80), 77);
        insert_owner(&mut snapshot.tcp, endpoint(4, 0x0A000001, 80), 42);
        assert_eq!(
            snapshot.lookup(6, endpoint(4, 0x0A000001, 80), None),
            MatchState::Target
        );
    }

    #[test]
    fn tcp_full_tuple_is_authoritative() {
        let mut snapshot = Snapshot::empty();
        snapshot.pids.insert(42);
        let local = endpoint(4, 0x0A000001, 40000);
        let remote = endpoint(4, 0x0A000002, 443);
        insert_owner(&mut snapshot.tcp, local, 42);
        insert_owner(&mut snapshot.tcp_flows, TcpFlowKey { local, remote }, 77);

        assert_eq!(
            snapshot.lookup(6, local, Some(remote)),
            MatchState::NonTarget
        );
    }

    #[test]
    fn duplicate_endpoints_preserve_all_owners() {
        let mut snapshot = Snapshot::empty();
        snapshot.pids.insert(42);
        let local = endpoint(4, 0x0A000001, 40000);
        insert_owner(&mut snapshot.tcp, local, 77);
        insert_owner(&mut snapshot.tcp, local, 42);
        assert_eq!(snapshot.lookup(6, local, None), MatchState::Target);

        let mut udp = Snapshot::empty();
        udp.pids.insert(42);
        let shared = endpoint(4, 0x0A000001, 5353);
        insert_owner(&mut udp.udp, shared, 77);
        insert_owner(&mut udp.udp, shared, 88);
        assert_eq!(udp.lookup(17, shared, None), MatchState::NonTarget);
    }

    #[test]
    fn known_non_target_traffic_does_not_refresh() {
        let mut snapshot = Snapshot::empty();
        snapshot.pids.insert(42);
        let local = endpoint(4, 0x0A000001, 40000);
        insert_owner(&mut snapshot.tcp, local, 77);
        let filter = ProcessFlowFilter::with_snapshot(&["target.exe".to_string()], snapshot);

        for _ in 0..10_000 {
            assert!(!filter.is_match(6, local));
        }
        assert_eq!(filter.refresh_count.load(Ordering::Relaxed), 0);
    }

    #[test]
    fn unknown_flows_are_rate_limited_and_negatively_cached() {
        let start = Instant::now();
        assert!(refresh_allowed(
            start.checked_sub(Duration::from_secs(1)).unwrap(),
            start,
            ON_DEMAND_INTERVAL
        ));
        assert!(!refresh_allowed(start, start, ON_DEMAND_INTERVAL));

        let mut cache = NegativeCache::default();
        let key = FlowKey {
            protocol: 6,
            local: endpoint(4, 0x0A000001, 40000),
            remote: None,
        };
        cache.insert(key, start);
        assert!(cache.contains(key, start + Duration::from_millis(500)));
        assert!(!cache.contains(key, start + NEGATIVE_CACHE_TTL + Duration::from_nanos(1)));
    }

    #[test]
    fn older_snapshot_cannot_overwrite_newer() {
        let slot = SnapshotSlot::new(Snapshot::empty());
        let mut newer = Snapshot::empty();
        newer.pids.insert(2);
        assert!(slot.try_publish(2, newer));

        let mut older = Snapshot::empty();
        older.pids.insert(1);
        assert!(!slot.try_publish(1, older));
        assert!(slot.current().pids.contains(&2));
        assert!(!slot.current().pids.contains(&1));
    }

    #[test]
    fn dynamic_table_read_retries_buffer_growth() {
        let attempts = AtomicUsize::new(0);
        let bytes = read_dynamic_table(|buffer, size| {
            let attempt = attempts.fetch_add(1, Ordering::SeqCst);
            if attempt == 0 {
                *size = 8;
                return ERROR_INSUFFICIENT_BUFFER;
            }
            assert!(!buffer.is_null());
            *size = 4;
            unsafe { std::ptr::write_unaligned(buffer as *mut u32, 7u32) };
            0
        });
        assert_eq!(attempts.load(Ordering::SeqCst), 2);
        assert_eq!(bytes.unwrap(), 7u32.to_ne_bytes());
    }

    #[test]
    fn dynamic_table_read_gives_up_after_repeated_growth() {
        let attempts = AtomicUsize::new(0);
        let bytes = read_dynamic_table(|_buffer, size| {
            attempts.fetch_add(1, Ordering::SeqCst);
            *size = 8;
            ERROR_INSUFFICIENT_BUFFER
        });
        assert!(bytes.is_none());
        assert_eq!(attempts.load(Ordering::SeqCst), TABLE_READ_ATTEMPTS * 2);
    }
}
