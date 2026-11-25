//! 弱网引擎：组合上下行两个方向的 `DirectionEngine`。

pub mod direction;
pub mod flow;
pub mod process_filter;
pub mod windivert;

use std::sync::mpsc;
use std::sync::{Arc, Mutex};

use crate::config::NetworkProfile;
use direction::{DirectionEngine, DirectionStats};
use process_filter::ProcessFlowFilter;

/// 引擎与 UI 之间的消息。
pub enum EngineEvent {
    StartDone(Result<(), String>),
    StopDone,
    Fault(String),
}

/// 事件通道 + 唤醒 UI 的回调（引擎线程不能直接碰窗口对象）。
pub struct EngineEvents {
    tx: mpsc::Sender<EngineEvent>,
    wake: Box<dyn Fn() + Send + Sync>,
}

impl EngineEvents {
    pub fn new(tx: mpsc::Sender<EngineEvent>, wake: Box<dyn Fn() + Send + Sync>) -> Self {
        Self { tx, wake }
    }

    pub fn send(&self, event: EngineEvent) {
        let _ = self.tx.send(event);
        (self.wake)();
    }
}

/// 统计快照，避免 UI 线程长时间持锁；sent 包含直通流量。
#[derive(Debug, Clone, Copy, Default)]
pub struct StatsSnapshot {
    pub sent: u64,
    pub dropped: u64,
    pub queue: u32,
}

struct Running {
    uplink: DirectionEngine,
    downlink: DirectionEngine,
    #[allow(dead_code)]
    process_filter: Option<Arc<ProcessFlowFilter>>,
}

/// 组合上下行两个方向的弱网引擎。
pub struct NetworkConditioner {
    state: Mutex<Option<Running>>,
    events: Arc<EngineEvents>,
}

impl NetworkConditioner {
    pub const DEFAULT_OUTBOUND_FILTER: &'static str = "outbound and (tcp or udp)";
    pub const DEFAULT_INBOUND_FILTER: &'static str = "inbound and (tcp or udp)";

    pub fn new(events: Arc<EngineEvents>) -> Self {
        Self {
            state: Mutex::new(None),
            events,
        }
    }

    /// 启动弱网模拟。filter 参数用于自测；常规启动使用默认全系统过滤器。
    pub fn start(
        &self,
        profile: &NetworkProfile,
        outbound_filter: Option<&str>,
        inbound_filter: Option<&str>,
        max_queue_delay_ms: i32,
        process_names: &[String],
    ) -> Result<(), String> {
        let mut state = match self.state.lock() {
            Ok(guard) => guard,
            Err(poisoned) => poisoned.into_inner(),
        };

        if state.is_some() {
            return Err("弱网模拟已经在运行。".to_string());
        }

        let snapshot = profile.clone();
        let process_filter = if process_names.is_empty() {
            None
        } else {
            Some(ProcessFlowFilter::new(process_names))
        };

        let filter_owned = process_filter.clone();
        let mut uplink = DirectionEngine::new(
            "上行",
            outbound_filter.unwrap_or(Self::DEFAULT_OUTBOUND_FILTER),
            true,
            &snapshot.uplink,
            snapshot.protocol,
            filter_owned.clone(),
            max_queue_delay_ms,
            self.events.clone(),
        );
        let mut downlink = DirectionEngine::new(
            "下行",
            inbound_filter.unwrap_or(Self::DEFAULT_INBOUND_FILTER),
            false,
            &snapshot.downlink,
            snapshot.protocol,
            filter_owned,
            max_queue_delay_ms,
            self.events.clone(),
        );

        if let Err(error) = uplink.start() {
            uplink.stop();
            return Err(error);
        }

        if let Err(error) = downlink.start() {
            uplink.stop();
            downlink.stop();
            return Err(error);
        }

        *state = Some(Running {
            uplink,
            downlink,
            process_filter,
        });
        Ok(())
    }

    pub fn stop(&self) {
        let running = match self.state.lock() {
            Ok(mut guard) => guard.take(),
            Err(poisoned) => poisoned.into_inner().take(),
        };

        if let Some(mut running) = running {
            running.uplink.stop();
            running.downlink.stop();
        }
    }

    pub fn uplink_stats(&self) -> Option<StatsSnapshot> {
        let guard = self.state.lock().ok()?;
        guard
            .as_ref()
            .map(|running| stats_snapshot(&running.uplink.stats()))
    }

    pub fn downlink_stats(&self) -> Option<StatsSnapshot> {
        let guard = self.state.lock().ok()?;
        guard
            .as_ref()
            .map(|running| stats_snapshot(&running.downlink.stats()))
    }

    pub fn reset_stats(&self) {
        let Ok(guard) = self.state.lock() else {
            return;
        };
        if let Some(running) = guard.as_ref() {
            running.uplink.stats().reset();
            running.downlink.stats().reset();
        }
    }
}

fn stats_snapshot(stats: &DirectionStats) -> StatsSnapshot {
    StatsSnapshot {
        sent: stats.sent(),
        dropped: stats.dropped(),
        queue: stats.queue_count(),
    }
}
