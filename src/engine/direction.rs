//! 单个方向（出站/入站）的弱网处理引擎（对应原 `DirectionEngine.cs`）。

use std::cmp::Ordering;
use std::collections::BinaryHeap;
use std::sync::atomic::{AtomicBool, AtomicU32, AtomicU64, Ordering as AtomicOrdering};
use std::sync::{Arc, Condvar, Mutex};
use std::thread::{self, JoinHandle};
use std::time::{Duration, Instant};

use crate::config::{ConditionProtocol, DirectionProfile};
use crate::engine::flow::FlowEndpoint;
use crate::engine::process_filter::ProcessFlowFilter;
use crate::engine::windivert;
use crate::engine::{EngineEvent, EngineEvents};

const MAX_QUEUE_PACKETS: usize = 4096;
const MAX_QUEUE_BYTES: usize = 8 * 1024 * 1024;
const SEND_BATCH_SIZE: usize = 64;
const POOL_LIMIT: usize = 1024;

/// 实时统计：发送、丢弃、队列长度。
pub struct DirectionStats {
    sent: AtomicU64,
    dropped: AtomicU64,
    queue_count: AtomicU32,
}

impl DirectionStats {
    fn new() -> Self {
        Self {
            sent: AtomicU64::new(0),
            dropped: AtomicU64::new(0),
            queue_count: AtomicU32::new(0),
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

    fn set_queue(&self, value: usize) {
        self.queue_count
            .store(value as u32, AtomicOrdering::Relaxed);
    }
}

/// 连续丢包（放行/丢包时长循环）状态机。
struct BurstLossState {
    allow_ms: u64,
    drop_ms: u64,
    phase_start_ns: u64,
    dropping: bool,
}

impl BurstLossState {
    fn new(allow_ms: i32, drop_ms: i32, now_ns: u64) -> Self {
        Self {
            allow_ms: allow_ms.max(0) as u64,
            drop_ms: drop_ms.max(0) as u64,
            phase_start_ns: now_ns,
            dropping: false,
        }
    }

    fn should_drop(&mut self, now_ns: u64) -> bool {
        if self.allow_ms == 0 && self.drop_ms == 0 {
            return false;
        }
        if self.allow_ms == 0 {
            return true;
        }
        if self.drop_ms == 0 {
            return false;
        }

        // 最多循环几次，防止线程长时间停滞后死循环。
        for _ in 0..4 {
            let elapsed_ms = now_ns.saturating_sub(self.phase_start_ns) / 1_000_000;
            let duration = if self.dropping {
                self.drop_ms
            } else {
                self.allow_ms
            };
            if elapsed_ms < duration {
                return self.dropping;
            }
            self.dropping = !self.dropping;
            self.phase_start_ns = now_ns;
        }

        self.dropping
    }
}

struct PacketItem {
    data: Vec<u8>,
    address: [u8; windivert::ADDRESS_STRUCT_SIZE],
    length: usize,
    due_ns: u64,
    sequence: u64,
}

impl PacketItem {
    fn new() -> Self {
        Self {
            data: Vec::new(),
            address: [0u8; windivert::ADDRESS_STRUCT_SIZE],
            length: 0,
            due_ns: 0,
            sequence: 0,
        }
    }

    fn reset(&mut self) {
        self.data.clear();
        self.length = 0;
        self.due_ns = 0;
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
    receiver: Option<JoinHandle<()>>,
    sender: Option<JoinHandle<()>>,
    started: bool,
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
            receiver: None,
            sender: None,
            started: false,
        }
    }

    pub fn stats(&self) -> Arc<DirectionStats> {
        self.stats.clone()
    }

    pub fn start(&mut self) -> Result<(), String> {
        if self.started {
            return Err("引擎已经启动。".to_string());
        }

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
        let display = self.display_name;

        // 接收线程。
        let receiver_stop = stop.clone();
        let receiver_queue = queue.clone();
        let receiver_pool = pool.clone();
        let receiver_stats = stats.clone();
        let receiver_events = events.clone();
        let receiver_fault_reported = fault_reported.clone();
        let profile = self.profile.clone();
        let protocol = self.protocol;
        let process_filter = self.process_filter.clone();
        let max_queue_ns = self.max_queue_ns;
        let needs_queue = self.needs_queue;
        let outbound = self.outbound;

        let receiver = thread::Builder::new()
            .name(format!("NetConditioner-{display}-recv"))
            .spawn(move || {
                receiver_loop(
                    display,
                    handle,
                    outbound,
                    &profile,
                    protocol,
                    process_filter.as_ref(),
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
        let sender_stats = stats.clone();
        let sender_events = events;
        let sender_fault_reported = fault_reported;
        let sender = match thread::Builder::new()
            .name(format!("NetConditioner-{display}-send"))
            .spawn(move || {
                sender_loop(
                    display,
                    handle,
                    base,
                    &sender_queue,
                    &sender_pool,
                    &sender_stats,
                    &sender_stop,
                    &sender_events,
                    &sender_fault_reported,
                );
            }) {
            Ok(sender) => sender,
            Err(error) => {
                stop.store(true, AtomicOrdering::Release);
                queue.1.notify_all();
                let _ = receiver.join();
                windivert::close(handle);
                *self.handle.lock().unwrap() = None;
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

        // 注意：不能提前把 handle 置空，否则发送线程在停止时无法重注入队列里剩余包。
        {
            let mut guard = self.handle.lock().unwrap();
            if let Some(handle) = *guard {
                if !windivert::shutdown(handle, windivert::SHUTDOWN_RECV) {
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

        let handle = self.handle.lock().unwrap().take();
        if let Some(handle) = handle {
            windivert::close(handle);
        }

        // 兜底清空队列（发送线程正常退出时已经清空）。
        let mut guard = self.queue.0.lock().unwrap();
        while let Some(item) = guard.heap.pop() {
            guard.bytes = guard.bytes.saturating_sub(item.item.length);
            return_item(&self.pool, item.item);
        }
        self.stats.set_queue(0);

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
    let _ = display;
    if fault_reported.swap(true, AtomicOrdering::AcqRel) {
        return;
    }
    events.send(EngineEvent::Fault(message));
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
            return;
        }
    }
}

fn send_direct(
    handle: isize,
    packet: *const u8,
    length: u32,
    address: *const u8,
    stats: &DirectionStats,
    stop: &AtomicBool,
) {
    match windivert::send(handle, packet, length, address) {
        Ok(_) => stats.add_sent(),
        Err(_) => {
            if !stop.load(AtomicOrdering::Acquire) {
                stats.add_dropped();
            }
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
    process_filter: Option<&Arc<ProcessFlowFilter>>,
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
    let mut burst = BurstLossState::new(
        profile.burst_allow_ms,
        profile.burst_drop_ms,
        elapsed_ns(base),
    );
    let mut next_send_ns: u64 = 0;
    let mut sequence: u64 = 0;

    let delay_ms = profile.delay_ms.max(0) as u64;
    let jitter_ms = profile.jitter_ms.max(0) as u64;
    let bandwidth = profile.bandwidth_kbps.max(0) as u64;
    let loss_percent = profile.random_loss_percent.clamp(0, 100) as u32;

    while !stop.load(AtomicOrdering::Acquire) {
        let length = match windivert::recv(handle, packet.as_mut_ptr(), address.0.as_mut_ptr()) {
            Ok(length) => length,
            Err(code) => {
                if stop.load(AtomicOrdering::Acquire) {
                    break;
                }
                report_fault(
                    display,
                    events,
                    fault_reported,
                    format!("{display}接收数据包失败（Win32 错误 {code}）。弱网模拟已停止。"),
                );
                return;
            }
        };

        if length == 0 {
            continue;
        }

        // 只有按进程过滤或非 ALL 协议时，才需要解析每个包。
        if protocol != ConditionProtocol::All || process_filter.is_some() {
            let parsed = crate::engine::flow::parse(&packet[..length as usize], outbound);
            let protocol_value: u8;
            let mut local: Option<FlowEndpoint> = None;

            match parsed {
                Some((value, endpoint)) => {
                    protocol_value = value;
                    local = Some(endpoint);
                }
                None => {
                    if process_filter.is_some() {
                        // 解析不了的包（如少见的 IPv6 扩展头）无法归属进程，直接放行。
                        send_direct(
                            handle,
                            packet.as_ptr(),
                            length,
                            address.0.as_ptr(),
                            stats,
                            stop,
                        );
                        continue;
                    }

                    match windivert::parse_protocol(packet.as_ptr(), length) {
                        Some(value) => protocol_value = value,
                        None => {
                            send_direct(
                                handle,
                                packet.as_ptr(),
                                length,
                                address.0.as_ptr(),
                                stats,
                                stop,
                            );
                            continue;
                        }
                    }
                }
            }

            if !protocol_matches(protocol, protocol_value) {
                send_direct(
                    handle,
                    packet.as_ptr(),
                    length,
                    address.0.as_ptr(),
                    stats,
                    stop,
                );
                continue;
            }

            if let (Some(filter), Some(endpoint)) = (process_filter, local) {
                if !filter.is_match(protocol_value, endpoint) {
                    send_direct(
                        handle,
                        packet.as_ptr(),
                        length,
                        address.0.as_ptr(),
                        stats,
                        stop,
                    );
                    continue;
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
            send_direct(
                handle,
                packet.as_ptr(),
                length,
                address.0.as_ptr(),
                stats,
                stop,
            );
            continue;
        }

        let jitter = if jitter_ms > 0 {
            fastrand::u64(0..=jitter_ms)
        } else {
            0
        };
        let intended_delay_ns = (delay_ms + jitter) * 1_000_000;
        let mut due = now + intended_delay_ns;

        if bandwidth > 0 {
            // 串行化发送：每个包按自身长度累加发送时间并排队。
            let length = length as u64;
            let packet_ns = ((length * 8 * 1_000_000_000) / (bandwidth * 1000)).max(1);
            let start = due.max(next_send_ns);
            next_send_ns = start + packet_ns;
            due = next_send_ns;
        }

        // 只限制“超出用户配置延时之外的额外排队”。
        if max_queue_ns > 0 && due.saturating_sub(now + intended_delay_ns) > max_queue_ns {
            stats.add_dropped();
            continue;
        }

        sequence += 1;
        enqueue(
            pool,
            queue,
            stats,
            &packet[..length as usize],
            &address.0,
            due,
            sequence,
        );
    }

    // 唤醒发送线程，让它把队列里剩余的包重注入后退出。
    queue.1.notify_all();
}

fn enqueue(
    pool: &Arc<Mutex<Vec<PacketItem>>>,
    queue: &Arc<(Mutex<QueueState>, Condvar)>,
    stats: &Arc<DirectionStats>,
    data: &[u8],
    address: &[u8],
    due_ns: u64,
    sequence: u64,
) {
    let mut item = rent_item(pool);
    item.data.extend_from_slice(data);
    item.address
        .copy_from_slice(&address[..windivert::ADDRESS_STRUCT_SIZE]);
    item.length = data.len();
    item.due_ns = due_ns;
    item.sequence = sequence;

    let mut guard = match queue.0.lock() {
        Ok(guard) => guard,
        Err(poisoned) => poisoned.into_inner(),
    };

    if guard.heap.len() >= MAX_QUEUE_PACKETS || guard.bytes + item.length > MAX_QUEUE_BYTES {
        drop(guard);
        stats.add_dropped();
        return_item(pool, item);
        return;
    }

    let old_head_due = guard.heap.peek().map(|head| head.due_ns);
    guard.bytes += item.length;
    guard.heap.push(QueuedItem {
        due_ns,
        sequence,
        item,
    });
    stats.set_queue(guard.heap.len());

    // 仅在新包会成为更早的队首，或队列为空时唤醒发送线程。
    if old_head_due.map_or(true, |head| due_ns < head) {
        queue.1.notify_one();
    }
}

#[allow(clippy::too_many_arguments)]
fn sender_loop(
    display: &str,
    handle: isize,
    base: Instant,
    queue: &Arc<(Mutex<QueueState>, Condvar)>,
    pool: &Arc<Mutex<Vec<PacketItem>>>,
    stats: &Arc<DirectionStats>,
    stop: &AtomicBool,
    events: &Arc<EngineEvents>,
    fault_reported: &AtomicBool,
) {
    loop {
        let mut guard = match queue.0.lock() {
            Ok(guard) => guard,
            Err(poisoned) => poisoned.into_inner(),
        };

        let ready = loop {
            let stopping = stop.load(AtomicOrdering::Acquire);
            match guard.heap.peek() {
                Some(head) => {
                    let now = elapsed_ns(base);
                    if stopping || head.due_ns <= now {
                        break true;
                    }
                    let wait =
                        Duration::from_nanos(head.due_ns - now).min(Duration::from_millis(100));
                    let (next, _) = match queue.1.wait_timeout(guard, wait) {
                        Ok(pair) => pair,
                        Err(poisoned) => poisoned.into_inner(),
                    };
                    guard = next;
                }
                None => {
                    if stopping {
                        break false;
                    }
                    let (next, _) = match queue.1.wait_timeout(guard, Duration::from_millis(100)) {
                        Ok(pair) => pair,
                        Err(poisoned) => poisoned.into_inner(),
                    };
                    guard = next;
                }
            }
        };

        let mut batch: Vec<PacketItem> = Vec::with_capacity(SEND_BATCH_SIZE);
        if ready {
            let now = elapsed_ns(base);
            while batch.len() < SEND_BATCH_SIZE {
                let take = match guard.heap.peek() {
                    Some(head) => stop.load(AtomicOrdering::Acquire) || head.due_ns <= now,
                    None => false,
                };
                if !take {
                    break;
                }
                if let Some(queued) = guard.heap.pop() {
                    guard.bytes = guard.bytes.saturating_sub(queued.item.length);
                    batch.push(queued.item);
                }
            }
            stats.set_queue(guard.heap.len());
        }
        drop(guard);

        if batch.is_empty() {
            if stop.load(AtomicOrdering::Acquire) {
                break;
            }
            continue;
        }

        let mut send_failed = false;
        for item in batch {
            if send_failed {
                return_item(pool, item);
                continue;
            }

            match windivert::send(
                handle,
                item.data.as_ptr(),
                item.length as u32,
                item.address.as_ptr(),
            ) {
                Ok(_) => stats.add_sent(),
                Err(code) => {
                    if !stop.load(AtomicOrdering::Acquire) {
                        stats.add_dropped();
                        report_fault(
                            display,
                            events,
                            fault_reported,
                            format!(
                                "{display}发送数据包失败（Win32 错误 {code}）。弱网模拟已停止。"
                            ),
                        );
                        send_failed = true;
                    }
                }
            }

            return_item(pool, item);
        }

        if send_failed {
            break;
        }
    }

    // 退出前清空队列。
    let mut guard = match queue.0.lock() {
        Ok(guard) => guard,
        Err(poisoned) => poisoned.into_inner(),
    };
    while let Some(queued) = guard.heap.pop() {
        guard.bytes = guard.bytes.saturating_sub(queued.item.length);
        return_item(pool, queued.item);
    }
    stats.set_queue(0);
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn burst_loss_state_machine() {
        // 只放行。
        let mut only_allow = BurstLossState::new(100, 0, 0);
        assert!(!only_allow.should_drop(0));

        // 只丢包。
        let mut only_drop = BurstLossState::new(0, 100, 0);
        assert!(only_drop.should_drop(0));

        // 关闭。
        let mut disabled = BurstLossState::new(0, 0, 0);
        assert!(!disabled.should_drop(0));

        // 放行 100ms → 丢包 100ms → 放行…
        let mut cycling = BurstLossState::new(100, 100, 0);
        assert!(!cycling.should_drop(50_000_000));
        assert!(cycling.should_drop(150_000_000));
        assert!(!cycling.should_drop(250_000_000));
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
}
