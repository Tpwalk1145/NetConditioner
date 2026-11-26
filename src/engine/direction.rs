//! 单个方向（出站/入站）的弱网处理引擎。

use std::cmp::Ordering;
use std::collections::BinaryHeap;
use std::sync::atomic::{AtomicBool, AtomicU32, AtomicU64, Ordering as AtomicOrdering};
use std::sync::{Arc, Condvar, Mutex};
use std::thread::{self, JoinHandle};
use std::time::{Duration, Instant};

use crate::config::{ConditionProtocol, DirectionProfile};
use crate::engine::process_filter::ProcessFlowFilter;
use crate::engine::windivert;
use crate::engine::{EngineEvent, EngineEvents};

const MAX_QUEUE_PACKETS: usize = 4096;
const MAX_QUEUE_BYTES: usize = 8 * 1024 * 1024;
const POOL_LIMIT: usize = 1024;
const ERROR_NO_DATA: i32 = 232;
const WAIT_SLICE: Duration = Duration::from_millis(100);

/// 实时统计：发送、丢弃、队列长度；发送包含直通放行的包。
pub struct DirectionStats {
    sent: AtomicU64,
    dropped: AtomicU64,
    queue_count: AtomicU32,
    failed: AtomicBool,
}

impl DirectionStats {
    fn new() -> Self {
        Self {
            sent: AtomicU64::new(0),
            dropped: AtomicU64::new(0),
            queue_count: AtomicU32::new(0),
            failed: AtomicBool::new(false),
        }
    }

    pub fn sent(&self) -> u64 {
        self.sent.load(AtomicOrdering::Relaxed)
    }

    pub fn dropped(&self) -> u64 {
        self.dropped.load(AtomicOrdering::Relaxed)
    }

    pub fn queue_count(&self) -> u32 {
        self.queue_count.load(AtomicOrdering::Relaxed)
    }

    pub fn failed(&self) -> bool {
        self.failed.load(AtomicOrdering::Acquire)
    }

    pub fn reset(&self) {
        self.sent.store(0, AtomicOrdering::Relaxed);
        self.dropped.store(0, AtomicOrdering::Relaxed);
        self.queue_count.store(0, AtomicOrdering::Relaxed);
    }

    fn add_sent(&self) {
        self.sent.fetch_add(1, AtomicOrdering::Relaxed);
    }

    fn add_dropped(&self) {
        self.dropped.fetch_add(1, AtomicOrdering::Relaxed);
    }

    fn mark_failed(&self) -> bool {
        !self.failed.swap(true, AtomicOrdering::AcqRel)
    }

    fn set_queue(&self, value: usize) {
        self.queue_count
            .store(value as u32, AtomicOrdering::Relaxed);
    }
}

/// 连续丢包（放行/丢包时长循环）状态机。
struct BurstLossState {
    allow_ns: u64,
    drop_ns: u64,
    phase_start_ns: u64,
}

impl BurstLossState {
    fn new(allow_ms: i32, drop_ms: i32, now_ns: u64) -> Self {
        Self {
            allow_ns: allow_ms.max(0) as u64 * 1_000_000,
            drop_ns: drop_ms.max(0) as u64 * 1_000_000,
            phase_start_ns: now_ns,
        }
    }

    fn should_drop(&self, now_ns: u64) -> bool {
        if self.allow_ns == 0 && self.drop_ns == 0 {
            return false;
        }
        if self.allow_ns == 0 {
            return true;
        }
        if self.drop_ns == 0 {
            return false;
        }

        let cycle = self.allow_ns.saturating_add(self.drop_ns);
        let elapsed = now_ns.saturating_sub(self.phase_start_ns);
        elapsed % cycle >= self.allow_ns
    }
}

#[derive(Debug, Clone, Copy)]
struct SendPlan {
    due_ns: u64,
    next_send_ns: u64,
    expires_ns: u64,
}

/// 接收线程只计划发送时间；只有成功入队后才提交带宽进度。
struct BandwidthScheduler {
    next_send_ns: u64,
}

impl BandwidthScheduler {
    fn new() -> Self {
        Self { next_send_ns: 0 }
    }

    fn plan(
        &self,
        now_ns: u64,
        intended_delay_ns: u64,
        length: usize,
        bandwidth_kbps: u64,
        max_queue_ns: u64,
    ) -> Option<SendPlan> {
        let intended_due_ns = now_ns.saturating_add(intended_delay_ns);
        let packet_ns = packet_service_ns(length, bandwidth_kbps);
        let start_ns = intended_due_ns.max(self.next_send_ns);
        let due_ns = start_ns.saturating_add(packet_ns);
        let extra_ns = due_ns.saturating_sub(intended_due_ns);
        if max_queue_ns > 0 && extra_ns > max_queue_ns {
            return None;
        }
        let expires_ns = if max_queue_ns > 0 {
            intended_due_ns.saturating_add(max_queue_ns)
        } else {
            0
        };
        Some(SendPlan {
            due_ns,
            next_send_ns: due_ns,
            expires_ns,
        })
    }

    fn commit(&mut self, plan: SendPlan) {
        self.next_send_ns = self.next_send_ns.max(plan.next_send_ns);
    }
}

fn packet_service_ns(length: usize, bandwidth_kbps: u64) -> u64 {
    if bandwidth_kbps == 0 || length == 0 {
        return 0;
    }
    ((length as u64 * 8 * 1_000_000_000) / (bandwidth_kbps * 1000)).max(1)
}

struct PacketItem {
    data: Vec<u8>,
    address: [u8; windivert::ADDRESS_STRUCT_SIZE],
    length: usize,
    due_ns: u64,
    expires_ns: u64,
    sequence: u64,
}

impl PacketItem {
    fn new() -> Self {
        Self {
            data: Vec::new(),
            address: [0u8; windivert::ADDRESS_STRUCT_SIZE],
            length: 0,
            due_ns: 0,
            expires_ns: 0,
            sequence: 0,
        }
    }

    fn reset(&mut self) {
        self.data.clear();
        self.length = 0;
        self.due_ns = 0;
        self.expires_ns = 0;
        self.sequence = 0;
    }
}

/// 按“到期时间优先”排序的最小堆元素。
struct QueuedItem {
    due_ns: u64,
    sequence: u64,
    item: PacketItem,
}

impl PartialEq for QueuedItem {
    fn eq(&self, other: &Self) -> bool {
        self.due_ns == other.due_ns && self.sequence == other.sequence
    }
}

impl Eq for QueuedItem {}

impl PartialOrd for QueuedItem {
    fn partial_cmp(&self, other: &Self) -> Option<Ordering> {
        Some(self.cmp(other))
    }
}

impl Ord for QueuedItem {
    fn cmp(&self, other: &Self) -> Ordering {
        // 反转比较，让 BinaryHeap 成为最小堆。
        other
            .due_ns
            .cmp(&self.due_ns)
            .then_with(|| other.sequence.cmp(&self.sequence))
    }
}

struct QueueState {
    heap: BinaryHeap<QueuedItem>,
    bytes: usize,
}

struct ReceiverDoneGuard {
    done: Arc<AtomicBool>,
    queue: Arc<(Mutex<QueueState>, Condvar)>,
}

impl Drop for ReceiverDoneGuard {
    fn drop(&mut self) {
        self.done.store(true, AtomicOrdering::Release);
        self.queue.1.notify_all();
    }
}

/// 一个方向的引擎：接收线程 + 发送线程。
pub struct DirectionEngine {
    display_name: &'static str,
    filter: String,
    outbound: bool,
    profile: DirectionProfile,
    protocol: ConditionProtocol,
    process_filter: Option<Arc<ProcessFlowFilter>>,
    events: Arc<EngineEvents>,
    stats: Arc<DirectionStats>,
    max_queue_ns: u64,
    needs_queue: bool,
    handle: Mutex<Option<isize>>,
    queue: Arc<(Mutex<QueueState>, Condvar)>,
    pool: Arc<Mutex<Vec<PacketItem>>>,
    stop: Arc<AtomicBool>,
    receiver_done: Arc<AtomicBool>,
    receiver: Option<JoinHandle<()>>,
    sender: Option<JoinHandle<()>>,
    started: bool,
    start_attempted: bool,
}

impl DirectionEngine {
    #[allow(clippy::too_many_arguments)]
    pub fn new(
        display_name: &'static str,
        filter: &str,
        outbound: bool,
        profile: &DirectionProfile,
        protocol: ConditionProtocol,
        process_filter: Option<Arc<ProcessFlowFilter>>,
        max_queue_delay_ms: i32,
        events: Arc<EngineEvents>,
    ) -> Self {
        let max_queue_ns = if max_queue_delay_ms > 0 {
            max_queue_delay_ms as u64 * 1_000_000
        } else {
            0
        };
        let needs_queue =
            profile.delay_ms > 0 || profile.jitter_ms > 0 || profile.bandwidth_kbps > 0;

        Self {
            display_name,
            filter: filter.to_string(),
            outbound,
            profile: profile.clone(),
            protocol,
            process_filter,
            events,
            stats: Arc::new(DirectionStats::new()),
            max_queue_ns,
            needs_queue,
            handle: Mutex::new(None),
            queue: Arc::new((
                Mutex::new(QueueState {
                    heap: BinaryHeap::new(),
                    bytes: 0,
                }),
                Condvar::new(),
            )),
            pool: Arc::new(Mutex::new(Vec::new())),
            stop: Arc::new(AtomicBool::new(false)),
            receiver_done: Arc::new(AtomicBool::new(false)),
            receiver: None,
            sender: None,
            started: false,
            start_attempted: false,
        }
    }

    pub fn stats(&self) -> Arc<DirectionStats> {
        self.stats.clone()
    }

    pub fn start(&mut self) -> Result<(), String> {
        if self.start_attempted {
            return Err("DirectionEngine 只能启动一次，停止后请创建新实例。".to_string());
        }
        self.start_attempted = true;

        let handle = windivert::open(&self.filter)?;
        let _ = windivert::set_params(handle);
        *self.handle.lock().unwrap() = Some(handle);

        let base = Instant::now();
        let stop = self.stop.clone();
        let queue = self.queue.clone();
        let pool = self.pool.clone();
        let stats = self.stats.clone();
        let events = self.events.clone();
        let fault_reported = Arc::new(AtomicBool::new(false));
        let receiver_done = self.receiver_done.clone();
        let display = self.display_name;

        // 接收线程。
        let receiver_stop = stop.clone();
        let receiver_queue = queue.clone();
        let receiver_pool = pool.clone();
        let receiver_stats = stats.clone();
        let receiver_events = events.clone();
        let receiver_fault_reported = fault_reported.clone();
        let receiver_done_guard = receiver_done.clone();
        let profile = self.profile.clone();
        let protocol = self.protocol;
        let process_filter = self.process_filter.clone();
        let max_queue_ns = self.max_queue_ns;
        let needs_queue = self.needs_queue;
        let outbound = self.outbound;

        let receiver = thread::Builder::new()
            .name(format!("NetConditioner-{display}-recv"))
            .spawn(move || {
                let _guard = ReceiverDoneGuard {
                    done: receiver_done_guard,
                    queue: receiver_queue.clone(),
                };
                receiver_loop(
                    display,
                    handle,
                    outbound,
                    &profile,
                    protocol,
                    process_filter.as_deref(),
                    max_queue_ns,
                    needs_queue,
                    base,
                    &receiver_queue,
                    &receiver_pool,
                    &receiver_stats,
                    &receiver_stop,
                    &receiver_events,
                    &receiver_fault_reported,
                );
            })
            .map_err(|error| {
                windivert::close(handle);
                *self.handle.lock().unwrap() = None;
                format!("启动{display}接收线程失败：{error}")
            })?;

        // 发送线程。
        let sender_stop = stop.clone();
        let sender_queue = queue.clone();
        let sender_pool = pool.clone();
        let sender_stats = stats;
        let sender_events = events;
        let sender_fault_reported = fault_reported;
        let sender_receiver_done = receiver_done;
        let bandwidth = self.profile.bandwidth_kbps.max(0) as u64;
        let sender = match thread::Builder::new()
            .name(format!("NetConditioner-{display}-send"))
            .spawn(move || {
                sender_loop(
                    display,
                    handle,
                    base,
                    bandwidth,
                    &sender_queue,
                    &sender_pool,
                    &sender_stats,
                    &sender_stop,
                    &sender_receiver_done,
                    &sender_events,
                    &sender_fault_reported,
                );
            }) {
            Ok(sender) => sender,
            Err(error) => {
                stop.store(true, AtomicOrdering::Release);
                // 先关闭接收，接收线程才能从 Recv 返回；否则 join 可能永久阻塞。
                let _ = windivert::shutdown(handle, windivert::SHUTDOWN_RECV);
                queue.1.notify_all();
                let _ = receiver.join();
                windivert::close(handle);
                *self.handle.lock().unwrap() = None;
                discard_all(&queue, &pool, &self.stats);
                return Err(format!("启动{display}发送线程失败：{error}"));
            }
        };

        self.receiver = Some(receiver);
        self.sender = Some(sender);
        self.started = true;
        Ok(())
    }

    pub fn stop(&mut self) {
        if !self.started {
            return;
        }

        self.stop.store(true, AtomicOrdering::Release);

        // 停止期间保留 WinDivert 句柄，供发送线程重注入队列中的剩余包。
        {
            let mut guard = match self.handle.lock() {
                Ok(guard) => guard,
                Err(poisoned) => poisoned.into_inner(),
            };
            if let Some(handle) = *guard {
                if !windivert::shutdown(handle, windivert::SHUTDOWN_RECV) {
                    // 关闭句柄是关闭失败时的兜底，用于唤醒阻塞中的 Recv。
                    windivert::close(handle);
                    *guard = None;
                }
            }
        }

        self.queue.1.notify_all();

        if let Some(handle) = self.receiver.take() {
            let _ = handle.join();
        }
        if let Some(handle) = self.sender.take() {
            let _ = handle.join();
        }

        let handle = match self.handle.lock() {
            Ok(mut guard) => guard.take(),
            Err(poisoned) => poisoned.into_inner().take(),
        };
        if let Some(handle) = handle {
            windivert::close(handle);
        }

        // 发送线程异常退出时，剩余包也必须计入丢弃数。
        discard_all(&self.queue, &self.pool, &self.stats);
        self.started = false;
    }
}

impl Drop for DirectionEngine {
    fn drop(&mut self) {
        self.stop();
    }
}

#[repr(C, align(8))]
struct AlignedAddress([u8; windivert::ADDRESS_BUFFER_SIZE]);

fn elapsed_ns(base: Instant) -> u64 {
    base.elapsed().as_nanos() as u64
}

fn protocol_matches(protocol: ConditionProtocol, value: u8) -> bool {
    match protocol {
        ConditionProtocol::Tcp => value == 6,
        ConditionProtocol::Udp => value == 17,
        ConditionProtocol::All => value == 6 || value == 17,
    }
}

fn report_fault(
    display: &str,
    events: &Arc<EngineEvents>,
    fault_reported: &AtomicBool,
    message: String,
) {
    if fault_reported.swap(true, AtomicOrdering::AcqRel) {
        return;
    }
    events.send(EngineEvent::Fault(format!("{display}：{message}")));
}

struct FailureReporter<'a> {
    display: &'a str,
    handle: isize,
    stats: &'a DirectionStats,
    events: &'a Arc<EngineEvents>,
    fault_reported: &'a AtomicBool,
}

impl FailureReporter<'_> {
    fn fail(&self, message: String) {
        if self.stats.mark_failed() {
            report_fault(self.display, self.events, self.fault_reported, message);
        }
        // 直通和排队发送共用同一种失败策略：停止继续接收。
        let _ = windivert::shutdown(self.handle, windivert::SHUTDOWN_RECV);
    }

    fn send(&self, packet: *const u8, length: u32, address: *const u8) -> bool {
        match windivert::send(self.handle, packet, length, address) {
            Ok(_) => {
                self.stats.add_sent();
                true
            }
            Err(code) => {
                self.stats.add_dropped();
                self.fail(format!(
                    "发送数据包失败（Win32 错误 {code}）。弱网模拟已停止。"
                ));
                false
            }
        }
    }
}

fn rent_item(pool: &Arc<Mutex<Vec<PacketItem>>>) -> PacketItem {
    if let Ok(mut guard) = pool.lock() {
        if let Some(mut item) = guard.pop() {
            item.reset();
            return item;
        }
    }
    PacketItem::new()
}

fn return_item(pool: &Arc<Mutex<Vec<PacketItem>>>, mut item: PacketItem) {
    item.reset();
    if let Ok(mut guard) = pool.lock() {
        if guard.len() < POOL_LIMIT {
            guard.push(item);
        }
    }
}

#[allow(clippy::too_many_arguments)]
fn receiver_loop(
    display: &str,
    handle: isize,
    outbound: bool,
    profile: &DirectionProfile,
    protocol: ConditionProtocol,
    process_filter: Option<&ProcessFlowFilter>,
    max_queue_ns: u64,
    needs_queue: bool,
    base: Instant,
    queue: &Arc<(Mutex<QueueState>, Condvar)>,
    pool: &Arc<Mutex<Vec<PacketItem>>>,
    stats: &Arc<DirectionStats>,
    stop: &AtomicBool,
    events: &Arc<EngineEvents>,
    fault_reported: &AtomicBool,
) {
    let mut packet = vec![0u8; windivert::MAX_PACKET_SIZE];
    let mut address = AlignedAddress([0u8; windivert::ADDRESS_BUFFER_SIZE]);
    let burst = BurstLossState::new(
        profile.burst_allow_ms,
        profile.burst_drop_ms,
        elapsed_ns(base),
    );
    let mut scheduler = BandwidthScheduler::new();
    let mut sequence: u64 = 0;
    let reporter = FailureReporter {
        display,
        handle,
        stats,
        events,
        fault_reported,
    };
    let queue_context = QueueContext { pool, queue, stats };

    let delay_ms = profile.delay_ms.max(0) as u64;
    let jitter_ms = profile.jitter_ms.max(0) as u64;
    let bandwidth = profile.bandwidth_kbps.max(0) as u64;
    let loss_percent = profile.random_loss_percent.clamp(0, 100) as u32;

    loop {
        if stats.failed() {
            break;
        }

        let length = match windivert::recv(handle, packet.as_mut_ptr(), address.0.as_mut_ptr()) {
            Ok(length) => length,
            Err(code) => {
                if code == ERROR_NO_DATA && stop.load(AtomicOrdering::Acquire) {
                    break;
                }
                if stats.failed() || stop.load(AtomicOrdering::Acquire) {
                    break;
                }
                reporter.fail(format!(
                    "接收数据包失败（Win32 错误 {code}）。弱网模拟已停止。"
                ));
                break;
            }
        };

        if length == 0 {
            continue;
        }

        // 只有按进程过滤或非 ALL 协议时，才需要解析每个包。
        if protocol != ConditionProtocol::All || process_filter.is_some() {
            let parsed = crate::engine::flow::parse_packet(&packet[..length as usize], outbound);
            let protocol_value: u8;
            match parsed {
                Some(flow) => {
                    protocol_value = flow.protocol;
                    if !protocol_matches(protocol, protocol_value) {
                        if !reporter.send(packet.as_ptr(), length, address.0.as_ptr()) {
                            break;
                        }
                        continue;
                    }
                    if let Some(filter) = process_filter {
                        if !filter.is_match_flow(protocol_value, flow.local, Some(flow.remote)) {
                            if !reporter.send(packet.as_ptr(), length, address.0.as_ptr()) {
                                break;
                            }
                            continue;
                        }
                    }
                }
                None => {
                    if process_filter.is_some() {
                        // 解析不了的包无法归属进程，直接放行。
                        if !reporter.send(packet.as_ptr(), length, address.0.as_ptr()) {
                            break;
                        }
                        continue;
                    }

                    match windivert::parse_protocol(packet.as_ptr(), length) {
                        Some(value) => protocol_value = value,
                        None => {
                            if !reporter.send(packet.as_ptr(), length, address.0.as_ptr()) {
                                break;
                            }
                            continue;
                        }
                    }
                    if !protocol_matches(protocol, protocol_value) {
                        if !reporter.send(packet.as_ptr(), length, address.0.as_ptr()) {
                            break;
                        }
                        continue;
                    }
                }
            }
        }

        let now = elapsed_ns(base);
        if burst.should_drop(now) {
            stats.add_dropped();
            continue;
        }

        if loss_percent > 0 && fastrand::u32(0..100) < loss_percent {
            stats.add_dropped();
            continue;
        }

        if !needs_queue {
            if !reporter.send(packet.as_ptr(), length, address.0.as_ptr()) {
                break;
            }
            continue;
        }

        let jitter = if jitter_ms > 0 {
            fastrand::u64(0..=jitter_ms)
        } else {
            0
        };
        let intended_delay_ns = delay_ms.saturating_add(jitter).saturating_mul(1_000_000);
        let Some(plan) = scheduler.plan(
            now,
            intended_delay_ns,
            length as usize,
            bandwidth,
            max_queue_ns,
        ) else {
            stats.add_dropped();
            continue;
        };

        sequence += 1;
        if queue_context.enqueue(
            &packet[..length as usize],
            &address.0,
            plan.due_ns,
            plan.expires_ns,
            sequence,
        ) {
            // 只有成功入队才消耗未来带宽；队列满/超限的包不会留下时间债。
            scheduler.commit(plan);
        } else {
            stats.add_dropped();
        }
    }
}

struct QueueContext<'a> {
    pool: &'a Arc<Mutex<Vec<PacketItem>>>,
    queue: &'a Arc<(Mutex<QueueState>, Condvar)>,
    stats: &'a Arc<DirectionStats>,
}

impl QueueContext<'_> {
    fn enqueue(
        &self,
        data: &[u8],
        address: &[u8],
        due_ns: u64,
        expires_ns: u64,
        sequence: u64,
    ) -> bool {
        let mut item = rent_item(self.pool);
        item.data.extend_from_slice(data);
        item.address
            .copy_from_slice(&address[..windivert::ADDRESS_STRUCT_SIZE]);
        item.length = data.len();
        item.due_ns = due_ns;
        item.expires_ns = expires_ns;
        item.sequence = sequence;

        let mut guard = match self.queue.0.lock() {
            Ok(guard) => guard,
            Err(poisoned) => poisoned.into_inner(),
        };

        if guard.heap.len() >= MAX_QUEUE_PACKETS || guard.bytes + item.length > MAX_QUEUE_BYTES {
            drop(guard);
            return_item(self.pool, item);
            return false;
        }

        let old_head_due = guard.heap.peek().map(|head| head.due_ns);
        guard.bytes += item.length;
        guard.heap.push(QueuedItem {
            due_ns,
            sequence,
            item,
        });
        self.stats.set_queue(guard.heap.len());

        // 仅在新包会成为更早的队首，或队列为空时唤醒发送线程。
        if old_head_due.is_none_or(|head| due_ns < head) {
            self.queue.1.notify_one();
        }
        true
    }
}

fn wait_until(
    base: Instant,
    target_ns: u64,
    queue: &Arc<(Mutex<QueueState>, Condvar)>,
    stop: &AtomicBool,
    stats: &DirectionStats,
) {
    loop {
        if stop.load(AtomicOrdering::Acquire) || stats.failed() {
            return;
        }
        let now = elapsed_ns(base);
        if now >= target_ns {
            return;
        }
        let wait = Duration::from_nanos(target_ns - now).min(WAIT_SLICE);
        let guard = match queue.0.lock() {
            Ok(guard) => guard,
            Err(poisoned) => poisoned.into_inner(),
        };
        let (next, _) = match queue.1.wait_timeout(guard, wait) {
            Ok(pair) => pair,
            Err(poisoned) => poisoned.into_inner(),
        };
        drop(next);
    }
}

fn paced_send_time(due_ns: u64, last_send_end_ns: Option<u64>, now_ns: u64) -> u64 {
    let mut send_at = due_ns;
    if let Some(last_end) = last_send_end_ns {
        send_at = send_at.max(last_end);
    }
    if send_at < now_ns {
        // 发送线程曾经停顿：以当前实际进度重新锚定，避免把积压包突发出去。
        send_at = now_ns;
    }
    send_at
}

#[allow(clippy::too_many_arguments)]
fn sender_loop(
    display: &str,
    handle: isize,
    base: Instant,
    bandwidth_kbps: u64,
    queue: &Arc<(Mutex<QueueState>, Condvar)>,
    pool: &Arc<Mutex<Vec<PacketItem>>>,
    stats: &Arc<DirectionStats>,
    stop: &AtomicBool,
    receiver_done: &Arc<AtomicBool>,
    events: &Arc<EngineEvents>,
    fault_reported: &AtomicBool,
) {
    let reporter = FailureReporter {
        display,
        handle,
        stats,
        events,
        fault_reported,
    };
    let mut last_send_end_ns: Option<u64> = None;

    loop {
        let mut guard = match queue.0.lock() {
            Ok(guard) => guard,
            Err(poisoned) => poisoned.into_inner(),
        };
        let stopping = stop.load(AtomicOrdering::Acquire);
        let failed = stats.failed();

        if stopping || failed {
            if let Some(queued) = guard.heap.pop() {
                guard.bytes = guard.bytes.saturating_sub(queued.item.length);
                stats.set_queue(guard.heap.len());
                drop(guard);

                if failed {
                    stats.add_dropped();
                } else if !reporter.send(
                    queued.item.data.as_ptr(),
                    queued.item.length as u32,
                    queued.item.address.as_ptr(),
                ) {
                    // 失败包已在 FailureReporter::send 中计数。
                }
                return_item(pool, queued.item);
                continue;
            }

            if receiver_done.load(AtomicOrdering::Acquire) {
                break;
            }
            drop(guard);
            let guard = match queue.0.lock() {
                Ok(guard) => guard,
                Err(poisoned) => poisoned.into_inner(),
            };
            let (next, _) = match queue.1.wait_timeout(guard, WAIT_SLICE) {
                Ok(pair) => pair,
                Err(poisoned) => poisoned.into_inner(),
            };
            drop(next);
            continue;
        }

        let now = elapsed_ns(base);
        let action = match guard.heap.peek() {
            Some(head) if head.item.expires_ns > 0 && now > head.item.expires_ns => 1,
            Some(head) if head.due_ns > now => 2,
            Some(_) => 3,
            None if receiver_done.load(AtomicOrdering::Acquire) => 0,
            None => 2,
        };

        match action {
            0 => break,
            1 => {
                let queued = guard.heap.pop().unwrap();
                guard.bytes = guard.bytes.saturating_sub(queued.item.length);
                stats.set_queue(guard.heap.len());
                drop(guard);
                stats.add_dropped();
                return_item(pool, queued.item);
            }
            2 => {
                let wait = guard
                    .heap
                    .peek()
                    .map(|head| Duration::from_nanos(head.due_ns.saturating_sub(now)))
                    .unwrap_or(WAIT_SLICE)
                    .min(WAIT_SLICE);
                let (next, _) = match queue.1.wait_timeout(guard, wait) {
                    Ok(pair) => pair,
                    Err(poisoned) => poisoned.into_inner(),
                };
                drop(next);
            }
            _ => {
                let queued = guard.heap.pop().unwrap();
                guard.bytes = guard.bytes.saturating_sub(queued.item.length);
                stats.set_queue(guard.heap.len());
                drop(guard);

                let packet_ns = packet_service_ns(queued.item.length, bandwidth_kbps);
                let mut send_at = queued.item.due_ns;
                if bandwidth_kbps > 0 {
                    send_at = paced_send_time(send_at, last_send_end_ns, elapsed_ns(base));
                }
                wait_until(base, send_at, queue, stop, stats);

                let stopping = stop.load(AtomicOrdering::Acquire);
                let failed = stats.failed();
                let expired = !stopping
                    && queued.item.expires_ns > 0
                    && elapsed_ns(base) > queued.item.expires_ns;
                if failed || expired {
                    stats.add_dropped();
                } else if reporter.send(
                    queued.item.data.as_ptr(),
                    queued.item.length as u32,
                    queued.item.address.as_ptr(),
                ) && bandwidth_kbps > 0
                {
                    last_send_end_ns = Some(send_at.saturating_add(packet_ns));
                }
                return_item(pool, queued.item);
            }
        }
    }
}

fn discard_all(
    queue: &Arc<(Mutex<QueueState>, Condvar)>,
    pool: &Arc<Mutex<Vec<PacketItem>>>,
    stats: &Arc<DirectionStats>,
) {
    let mut guard = match queue.0.lock() {
        Ok(guard) => guard,
        Err(poisoned) => poisoned.into_inner(),
    };
    while let Some(queued) = guard.heap.pop() {
        guard.bytes = guard.bytes.saturating_sub(queued.item.length);
        stats.add_dropped();
        return_item(pool, queued.item);
    }
    stats.set_queue(0);
}

#[cfg(test)]
mod tests {
    use super::*;

    fn engine_for_test() -> DirectionEngine {
        let (sender, _receiver) = std::sync::mpsc::channel();
        let events = Arc::new(EngineEvents::new(sender, Box::new(|| {})));
        DirectionEngine::new(
            "test",
            "false",
            true,
            &DirectionProfile::default(),
            ConditionProtocol::All,
            None,
            1000,
            events,
        )
    }

    #[test]
    fn burst_loss_state_handles_sparse_calls() {
        let only_allow = BurstLossState::new(100, 0, 0);
        assert!(!only_allow.should_drop(0));

        let only_drop = BurstLossState::new(0, 100, 0);
        assert!(only_drop.should_drop(0));

        let disabled = BurstLossState::new(0, 0, 0);
        assert!(!disabled.should_drop(u64::MAX));

        let cycling = BurstLossState::new(100, 100, 0);
        assert!(!cycling.should_drop(50_000_000));
        assert!(cycling.should_drop(150_000_000));
        assert!(!cycling.should_drop(250_000_000));
        // 一次跳过多个完整阶段后仍应落在正确阶段。
        assert!(cycling.should_drop(1_350_000_000));
        assert!(!cycling.should_drop(1_450_000_000));
    }

    #[test]
    fn rejected_packet_does_not_consume_future_bandwidth() {
        let mut scheduler = BandwidthScheduler::new();
        let first = scheduler.plan(0, 0, 1000, 8, 0).unwrap();
        let retry = scheduler.plan(0, 0, 1000, 8, 0).unwrap();
        assert_eq!(first.due_ns, retry.due_ns);

        scheduler.commit(retry);
        let next = scheduler.plan(0, 0, 1000, 8, 0).unwrap();
        assert_eq!(next.due_ns, retry.due_ns + packet_service_ns(1000, 8));
    }

    #[test]
    fn scheduler_enforces_extra_queue_latency() {
        let scheduler = BandwidthScheduler::new();
        assert!(scheduler.plan(0, 0, 1000, 1, 1).is_none());
        let plan = scheduler.plan(0, 0, 100, 1_000_000, 1_000_000).unwrap();
        assert!(plan.expires_ns > 0);
    }

    #[test]
    fn actual_send_pacing_rebases_after_stall() {
        let service = packet_service_ns(1000, 8);
        let first = paced_send_time(10, None, 100);
        assert_eq!(first, 100);
        let second = paced_send_time(10, Some(first + service), 100);
        assert_eq!(second, first + service);
    }

    #[test]
    fn queued_items_are_ordered_by_due_time() {
        let mut heap = BinaryHeap::new();
        for (due, seq) in [(30u64, 1u64), (10, 2), (20, 3)] {
            heap.push(QueuedItem {
                due_ns: due,
                sequence: seq,
                item: PacketItem::new(),
            });
        }
        assert_eq!(heap.pop().unwrap().due_ns, 10);
        assert_eq!(heap.pop().unwrap().due_ns, 20);
        assert_eq!(heap.pop().unwrap().due_ns, 30);
    }

    #[test]
    fn discard_all_counts_every_leftover() {
        let queue = Arc::new((
            Mutex::new(QueueState {
                heap: BinaryHeap::new(),
                bytes: 0,
            }),
            Condvar::new(),
        ));
        let pool = Arc::new(Mutex::new(Vec::new()));
        let stats = Arc::new(DirectionStats::new());
        let queue_context = QueueContext {
            pool: &pool,
            queue: &queue,
            stats: &stats,
        };
        for sequence in 0..3 {
            assert!(queue_context.enqueue(
                &[1, 2, 3],
                &[0u8; windivert::ADDRESS_BUFFER_SIZE],
                sequence,
                0,
                sequence,
            ));
        }
        discard_all(&queue, &pool, &stats);
        assert_eq!(stats.dropped(), 3);
        assert_eq!(stats.queue_count(), 0);
    }

    #[test]
    fn receiver_done_guard_publishes_completion() {
        let done = Arc::new(AtomicBool::new(false));
        let queue = Arc::new((
            Mutex::new(QueueState {
                heap: BinaryHeap::new(),
                bytes: 0,
            }),
            Condvar::new(),
        ));
        {
            let _guard = ReceiverDoneGuard {
                done: done.clone(),
                queue: queue.clone(),
            };
            assert!(!done.load(AtomicOrdering::Acquire));
        }
        assert!(done.load(AtomicOrdering::Acquire));
    }

    #[test]
    fn direction_engine_rejects_restart_before_opening_windivert() {
        let mut engine = engine_for_test();
        engine.start_attempted = true;
        assert!(engine.start().unwrap_err().contains("只能启动一次"));
    }
}
