//! 管理员模式综合自测：会写入临时文件、加载 WinDivert 并进行真实网络验证。

use std::collections::HashMap;
use std::io::{Read, Write};
use std::net::{
    IpAddr, Ipv4Addr, Ipv6Addr, SocketAddr, TcpListener, TcpStream, ToSocketAddrs, UdpSocket,
};
use std::path::Path;
use std::process::Command;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::Arc;
use std::thread;
use std::time::{Duration, Instant};

use crate::config::{
    self, AppConfig, ConditionProtocol, DirectionProfile, NetworkProfile, ProfileStore,
};
use crate::engine::{EngineEvents, NetworkConditioner};
use crate::hotkey;
use crate::update::{CheckStatus, UpdateService};
use crate::version;
use crate::win;

const LOOPBACK: IpAddr = IpAddr::V4(Ipv4Addr::LOCALHOST);
const LOOPBACK6: IpAddr = IpAddr::V6(Ipv6Addr::LOCALHOST);

#[derive(Default)]
struct Log {
    lines: Vec<String>,
    failures: usize,
}

impl Log {
    fn write(&mut self, text: &str) {
        self.lines.push(format!(
            "[{}] {text}",
            chrono::Local::now().format("%H:%M:%S%.3f")
        ));
    }

    fn pass(&mut self, text: &str) {
        self.write(&format!("PASS  {text}"));
    }

    fn fail(&mut self, text: &str) {
        self.failures += 1;
        self.write(&format!("FAIL  {text}"));
    }

    fn skip(&mut self, text: &str) {
        self.write(&format!("SKIP  {text}"));
    }
}

pub fn run() -> i32 {
    if let Ok(port) = std::env::var("NC_SELFTEST_LOOPBACK_SERVER") {
        return port.parse::<u16>().map_or(2, loopback_server_child);
    }

    let log_path = std::env::temp_dir().join("NetConditioner-selftest.log");
    let mut log = Log::default();
    log.write(&format!(
        "NetConditioner self-test, {} {}",
        std::env::consts::OS,
        std::env::consts::ARCH
    ));
    log.write(&format!("Log: {}", log_path.display()));

    if !win::is_elevated() {
        log.fail("需要管理员权限。请以管理员身份运行 --self-test。");
        return finish(log, &log_path);
    }

    // 快捷键校验：单键 F8、组合键、修饰键本身。
    if hotkey::is_valid(0, 0x77)
        && hotkey::is_valid(config::MOD_CONTROL, 0x55)
        && !hotkey::is_valid(0, 0x10)
        && hotkey::format(0, 0x77) == "F8"
    {
        log.pass("快捷键校验：支持单键 F8，修饰键本身被排除");
    } else {
        log.fail("快捷键校验失败。");
    }

    // 配置读写：目标进程列表去重、规范化。
    let config_path = std::env::temp_dir().join("NetConditioner-selftest-config.json");
    {
        let store = ProfileStore::new(config_path.clone());
        let mut test_config = AppConfig::default();
        test_config.settings.process_names = vec![
            "chrome.exe".to_string(),
            "C:\\Tools\\MyApp.EXE".to_string(),
            "chrome.exe".to_string(),
        ];
        let profile = NetworkProfile {
            name: "process-config-test".to_string(),
            ..Default::default()
        };
        test_config.active_profile_id = Some(profile.id.clone());
        test_config.profiles.push(profile);

        match store.save(&test_config) {
            Ok(()) => {
                let loaded = store.load();
                let names = &loaded.config.settings.process_names;
                if names.len() == 2
                    && names.iter().any(|name| name == "chrome.exe")
                    && names.iter().any(|name| name == "myapp.exe")
                {
                    log.pass("配置读写：目标进程列表正确去重并规范化");
                } else {
                    log.fail(&format!(
                        "配置读写：目标进程列表异常（{}）",
                        names.join(",")
                    ));
                }
            }
            Err(error) => log.fail(&format!("配置读写测试异常：{error}")),
        }
    }
    let _ = std::fs::remove_file(&config_path);

    // 云更新：版本号解析与比较。
    let known_sha = UpdateService::compute_sha256_hex(b"abc");
    if version::parse("1.0.1").is_some()
        && version::parse("1.0.0").is_some()
        && version::is_newer(
            version::parse("1.0.0").unwrap(),
            version::parse("1.0.1").unwrap(),
        )
        && !version::is_newer(
            version::parse("1.0.1").unwrap(),
            version::parse("1.0.0").unwrap(),
        )
        && !version::is_newer(
            version::parse("1.0.1").unwrap(),
            version::parse("1.0.1").unwrap(),
        )
        && version::parse("1.0").is_some()
        && version::parse("1.0.0.5").is_some()
        && version::parse("1.0.0-beta").is_none()
        && version::parse("1").is_none()
        && known_sha == "BA7816BF8F01CFEA414140DE5DAE2223B00361A396177A9CB410FF61F20015AD"
    {
        log.pass("云更新：版本号解析/比较/SHA256 计算正确");
    } else {
        log.fail("云更新：版本号解析、比较或 SHA256 计算异常。");
    }

    // 云更新：latest.json 校验规则。
    let sample_sha = "a".repeat(64);
    let valid_manifest = format!(
        "{{\"schemaVersion\":2,\"version\":\"9.9.9\",\"notes\":\"test\",\"installer\":{{\"url\":\"https://example.com/NetConditioner-9.9.9-win-x64.exe\",\"sha256\":\"{sample_sha}\",\"size\":123}}}}"
    );
    let parsed_manifest = UpdateService::try_parse_manifest(&valid_manifest);
    let invalid_manifests = [
        format!("{{\"schemaVersion\":2,\"installer\":{{\"url\":\"https://example.com/a.exe\",\"sha256\":\"{sample_sha}\"}}}}"),
        "{\"schemaVersion\":2,\"version\":\"9.9.9\",\"installer\":{\"url\":\"https://example.com/a.exe\",\"sha256\":\"abc\"}}".to_string(),
        format!("{{\"schemaVersion\":2,\"version\":\"9.9.9\",\"installer\":{{\"url\":\"http://example.com/a.exe\",\"sha256\":\"{sample_sha}\"}}}}"),
            "{\"schemaVersion\":1,\"version\":\"9.9.9\",\"installer\":{\"url\":\"https://example.com/a.exe\",\"sha256\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\"}}".to_string(),
        "not json".to_string(),
    ];
    let mut invalid_rejected = true;
    for invalid in &invalid_manifests {
        if UpdateService::try_parse_manifest(invalid).is_ok() {
            invalid_rejected = false;
        }
    }

    match &parsed_manifest {
        Ok((manifest, parsed_version)) => {
            let sha_ok = manifest
                .installer
                .as_ref()
                .and_then(|installer| installer.sha256.clone())
                .map(|sha| sha == sample_sha.to_uppercase())
                .unwrap_or(false);
            if sha_ok && parsed_version.0 == 9 && parsed_version.1 == 9 && invalid_rejected {
                log.pass("云更新：latest.json 校验规则正确（无效内容全部拒绝）");
            } else {
                log.fail("云更新：latest.json 校验异常。");
            }
        }
        Err(error) => log.fail(&format!("云更新：latest.json 校验异常（{error}）。")),
    }

    // 云更新：配置字段默认值与清洗。
    let update_config_path = std::env::temp_dir().join("NetConditioner-selftest-update.json");
    {
        let store = ProfileStore::new(update_config_path.clone());
        let mut update_config = AppConfig::fresh();
        update_config.settings.update_feed_url = "http://example.com/latest.json".to_string();
        update_config.settings.last_update_check_unix = i64::MAX / 2;
        update_config.settings.last_attempted_version = "1.2.3".to_string();
        match store.save(&update_config) {
            Ok(()) => {
                let reloaded = store.load();
                let now = config::now_unix();
                if reloaded.config.settings.update_feed_url.is_empty()
                    && reloaded.config.settings.last_update_check_unix <= now + 86400
                    && reloaded.config.settings.last_attempted_version == "1.2.3"
                    && reloaded.config.settings.auto_update
                {
                    log.pass("云更新：配置读写与清洗正确");
                } else {
                    log.fail("云更新：配置读写或清洗异常。");
                }

                let mut again = reloaded.config.clone();
                again.settings.update_feed_url = "https://example.com/latest.json".to_string();
                let _ = store.save(&again);
                let reloaded_again = store.load();
                if reloaded_again.config.settings.update_feed_url
                    == "https://example.com/latest.json"
                {
                    log.pass("云更新：https 更新地址可正常保存");
                } else {
                    log.fail("云更新：https 更新地址未被保留。");
                }
            }
            Err(error) => log.fail(&format!("云更新：配置读写测试异常：{error}")),
        }
    }
    let _ = std::fs::remove_file(&update_config_path);

    // 云更新：本地回环 HTTP 服务验证检查、下载与 SHA256 校验。
    let update_payload: Vec<u8> = (0..64 * 1024).map(|_| fastrand::u32(..) as u8).collect();
    let update_payload_sha = UpdateService::compute_sha256_hex(&update_payload);
    let update_port = find_free_tcp_port(LOOPBACK);
    let mut routes: HashMap<String, Vec<u8>> = HashMap::new();
    routes.insert(
        "/latest.json".to_string(),
        format!(
            "{{\"schemaVersion\":2,\"version\":\"9.9.9\",\"installer\":{{\"url\":\"http://127.0.0.1:{update_port}/payload.bin\",\"sha256\":\"{update_payload_sha}\",\"size\":{}}}}}",
            update_payload.len()
        )
        .into_bytes(),
    );
    routes.insert("/payload.bin".to_string(), update_payload.clone());

    match MiniHttpServer::start(LOOPBACK, update_port, routes) {
        Some(_server) => {
            let feed_url = format!("http://127.0.0.1:{update_port}/latest.json");
            match UpdateService::check(&feed_url, version::current()) {
                CheckStatus::UpdateAvailable(manifest, _) => {
                    let sha_ok = manifest
                        .installer
                        .as_ref()
                        .and_then(|installer| installer.sha256.clone())
                        .map(|sha| sha == update_payload_sha)
                        .unwrap_or(false);
                    if sha_ok {
                        log.pass("云更新：本地回环清单解析与版本判断正确");
                    } else {
                        log.fail("云更新：本地回环检查异常（SHA256 不一致）。");
                    }

                    let destination =
                        std::env::temp_dir().join("NetConditioner-selftest-update-setup.exe");
                    let downloaded = UpdateService::download(&manifest, &destination);
                    let downloaded_sha = std::fs::read(&destination)
                        .map(|bytes| UpdateService::compute_sha256_hex(&bytes))
                        .unwrap_or_default();
                    if downloaded && downloaded_sha == update_payload_sha {
                        log.pass("云更新：更新包下载 + SHA256 校验通过");
                    } else {
                        log.fail("云更新：更新包下载或 SHA256 校验失败。");
                    }
                    let _ = std::fs::remove_file(&destination);

                    let mut tampered = manifest.clone();
                    if let Some(installer) = tampered.installer.as_mut() {
                        installer.sha256 = Some("0".repeat(64));
                    }
                    let tampered_destination = std::env::temp_dir()
                        .join("NetConditioner-selftest-update-setup.tampered.exe");
                    let tampered_downloaded =
                        UpdateService::download(&tampered, &tampered_destination);
                    if !tampered_downloaded && !tampered_destination.exists() {
                        log.pass("云更新：SHA256 不匹配时拒绝更新包");
                    } else {
                        log.fail("云更新：SHA256 不匹配未被拒绝。");
                        let _ = std::fs::remove_file(&tampered_destination);
                    }
                }
                other => log.fail(&format!("云更新：本地回环检查异常（{other:?}）。")),
            }
        }
        None => log.skip("云更新：本地回环测试服务无法启动。"),
    }

    // 云更新：自更新命令（dry-run，只记录不执行）。
    {
        let dummy_payload = std::env::temp_dir().join("NetConditioner-selftest-dummy-update.exe");
        let _ = std::fs::write(&dummy_payload, b"dummy-update");
        std::env::set_var("NC_UPDATE_DRY_RUN", "1");
        let applied = UpdateService::apply_self_update(&dummy_payload);
        std::env::remove_var("NC_UPDATE_DRY_RUN");
        let log_text = std::fs::read_to_string(crate::update::log_path()).unwrap_or_default();
        if applied && log_text.contains("DRY RUN 更新包") {
            log.pass("云更新：自更新 dry-run 命令生成正确");
        } else {
            log.fail("云更新：自更新 dry-run 命令生成失败。");
        }
        let _ = std::fs::remove_file(&dummy_payload);
    }

    let port = find_free_tcp_port(LOOPBACK);
    log.write(&format!("测试端口：127.0.0.1:{port}"));

    let echo = match EchoServer::start(LOOPBACK, port) {
        Some(server) => server,
        None => {
            log.fail("本地回环基线连接失败，无法继续。");
            return finish(log, &log_path);
        }
    };

    let baseline = match measure_echo(port, 3, 3000, LOOPBACK) {
        Some(value) => value,
        None => {
            log.fail("本地回环基线连接失败，无法继续。");
            return finish(log, &log_path);
        }
    };
    log.write(&format!("基线 RTT 中位数：{baseline} ms"));

    // 生产过滤器必须对回环 fail closed：客户端与服务器分进程，默认过滤器不应改变 RTT。
    {
        let loopback_port = find_free_tcp_port(LOOPBACK);
        let child = Command::new(std::env::current_exe().unwrap_or_default())
            .arg("--self-test")
            .env("NC_SELFTEST_LOOPBACK_SERVER", loopback_port.to_string())
            .spawn();
        match child {
            Ok(mut child) => {
                thread::sleep(Duration::from_millis(300));
                let conditioner = make_conditioner();
                let profile = NetworkProfile {
                    name: "self-test-production-loopback".to_string(),
                    uplink: DirectionProfile {
                        delay_ms: 300,
                        ..Default::default()
                    },
                    protocol: ConditionProtocol::Tcp,
                    ..Default::default()
                };
                if let Err(error) = conditioner.start(&profile, None, None, 2000, &[]) {
                    log.fail(&format!("生产过滤器回环测试启动失败：{error}"));
                } else {
                    thread::sleep(Duration::from_millis(200));
                    match measure_echo(loopback_port, 3, 3000, LOOPBACK) {
                        Some(value) if value <= baseline + 150 => log.pass(&format!(
                            "生产过滤器排除回环：独立客户端/服务器 RTT {value} ms"
                        )),
                        Some(value) => {
                            log.fail(&format!("生产过滤器错误处理了回环响应：RTT {value} ms"))
                        }
                        None => log.fail("生产过滤器回环测试连接失败。"),
                    }
                    conditioner.stop();
                }
                let _ = child.kill();
                let _ = child.wait();
            }
            Err(error) => log.skip(&format!("无法启动独立回环服务器进程：{error}")),
        }
    }

    let out_filter = format!("outbound and tcp and tcp.DstPort == {port}");
    let in_filter = format!("inbound and tcp and tcp.SrcPort == {port}");
    let loopback_down_filter = format!("tcp and tcp.SrcPort == {port}");

    // 1) 上行延时 300ms。
    {
        let conditioner = make_conditioner();
        let profile = NetworkProfile {
            name: "self-test-delay".to_string(),
            uplink: DirectionProfile {
                delay_ms: 300,
                ..Default::default()
            },
            protocol: ConditionProtocol::Tcp,
            ..Default::default()
        };

        if let Err(error) =
            conditioner.start(&profile, Some(&out_filter), Some(&in_filter), 2000, &[])
        {
            log.fail(&format!("上行延时测试启动失败：{error}"));
        } else {
            thread::sleep(Duration::from_millis(200));
            match measure_echo(port, 3, 5000, LOOPBACK) {
                Some(delayed) if delayed >= baseline + 200 => log.pass(&format!(
                    "上行 300ms 延时生效：{baseline} ms -> {delayed} ms"
                )),
                Some(delayed) => {
                    log.fail(&format!("上行延时未生效：{baseline} ms -> {delayed} ms"))
                }
                None => log.fail("延时测试连接失败。"),
            }

            match conditioner.uplink_stats() {
                Some(stats) if stats.sent > 0 => log.pass(&format!("上行发送计数：{}", stats.sent)),
                _ => log.fail("上行发送计数为 0，WinDivert 可能没有捕获回环流量。"),
            }
            conditioner.stop();
        }
    }

    // 2) 下行延时 300ms。
    {
        let conditioner = make_conditioner();
        let profile = NetworkProfile {
            name: "self-test-down-delay".to_string(),
            downlink: DirectionProfile {
                delay_ms: 300,
                ..Default::default()
            },
            protocol: ConditionProtocol::Tcp,
            ..Default::default()
        };

        if let Err(error) = conditioner.start(
            &profile,
            Some(&out_filter),
            Some(&loopback_down_filter),
            2000,
            &[],
        ) {
            log.fail(&format!("下行延时测试启动失败：{error}"));
        } else {
            thread::sleep(Duration::from_millis(200));
            match measure_echo(port, 3, 5000, LOOPBACK) {
                Some(delayed) if delayed >= baseline + 200 => log.pass(&format!(
                    "下行 300ms 延时生效：{baseline} ms -> {delayed} ms"
                )),
                Some(delayed) => {
                    log.fail(&format!("下行延时未生效：{baseline} ms -> {delayed} ms"))
                }
                None => log.fail("下行延时测试连接失败。"),
            }

            match conditioner.downlink_stats() {
                Some(stats) if stats.sent > 0 => log.pass(&format!("下行发送计数：{}", stats.sent)),
                _ => log.fail("下行发送计数为 0。"),
            }
            conditioner.stop();
        }
    }

    // 3) 进程过滤：选中的进程生效，未选中的进程直通。
    let self_name = std::env::current_exe()
        .ok()
        .and_then(|path| {
            path.file_stem()
                .map(|stem| stem.to_string_lossy().to_string())
        })
        .unwrap_or_else(|| "NetConditioner".to_string());

    {
        let conditioner = make_conditioner();
        let profile = NetworkProfile {
            name: "self-test-process-match".to_string(),
            uplink: DirectionProfile {
                delay_ms: 300,
                ..Default::default()
            },
            protocol: ConditionProtocol::Tcp,
            ..Default::default()
        };
        let names = vec![self_name.clone()];

        if let Err(error) =
            conditioner.start(&profile, Some(&out_filter), Some(&in_filter), 2000, &names)
        {
            log.fail(&format!("进程过滤（命中）测试启动失败：{error}"));
        } else {
            thread::sleep(Duration::from_millis(200));
            match measure_echo(port, 3, 5000, LOOPBACK) {
                Some(delayed) if delayed >= baseline + 200 => log.pass(&format!(
                    "进程过滤生效：{self_name} 的 RTT {baseline} ms -> {delayed} ms"
                )),
                Some(delayed) => log.fail(&format!("进程过滤未生效：RTT 仅 {delayed} ms")),
                None => log.fail("进程过滤（命中）测试连接失败。"),
            }
            conditioner.stop();
        }
    }

    {
        let conditioner = make_conditioner();
        let profile = NetworkProfile {
            name: "self-test-process-miss".to_string(),
            uplink: DirectionProfile {
                delay_ms: 300,
                ..Default::default()
            },
            protocol: ConditionProtocol::Tcp,
            ..Default::default()
        };
        let names = vec!["netconditioner-not-selected.exe".to_string()];

        if let Err(error) =
            conditioner.start(&profile, Some(&out_filter), Some(&in_filter), 2000, &names)
        {
            log.fail(&format!("进程过滤（未命中）测试启动失败：{error}"));
        } else {
            thread::sleep(Duration::from_millis(200));
            match measure_echo(port, 3, 3000, LOOPBACK) {
                Some(pass_through) if pass_through <= baseline + 150 => {
                    log.pass(&format!("未选中的进程直通：RTT {pass_through} ms"))
                }
                Some(pass_through) => {
                    log.fail(&format!("未选中的进程被错误限速：RTT {pass_through} ms"))
                }
                None => log.fail("进程过滤（未命中）测试连接失败。"),
            }
            conditioner.stop();
        }
    }

    // 4) 下行进程过滤：验证入站包也能正确归属到进程。
    {
        let conditioner = make_conditioner();
        let profile = NetworkProfile {
            name: "self-test-process-down".to_string(),
            downlink: DirectionProfile {
                delay_ms: 300,
                ..Default::default()
            },
            protocol: ConditionProtocol::Tcp,
            ..Default::default()
        };
        let names = vec![self_name.clone()];

        if let Err(error) = conditioner.start(
            &profile,
            Some(&out_filter),
            Some(&loopback_down_filter),
            2000,
            &names,
        ) {
            log.fail(&format!("下行进程过滤测试启动失败：{error}"));
        } else {
            thread::sleep(Duration::from_millis(200));
            match measure_echo(port, 3, 5000, LOOPBACK) {
                Some(delayed) if delayed >= baseline + 200 => {
                    log.pass(&format!("下行进程过滤生效：{baseline} ms -> {delayed} ms"))
                }
                Some(delayed) => log.fail(&format!("下行进程过滤未生效：RTT 仅 {delayed} ms")),
                None => log.fail("下行进程过滤测试连接失败。"),
            }
            conditioner.stop();
        }
    }

    // 5) IPv6 回环上的进程过滤（浏览器等可能优先走 IPv6）。
    {
        let port6 = find_free_tcp_port(LOOPBACK6);
        match EchoServer::start(LOOPBACK6, port6) {
            Some(_server) => {
                let out_filter6 = format!("outbound and tcp and tcp.DstPort == {port6}");
                let in_filter6 = format!("tcp and tcp.SrcPort == {port6}");
                let conditioner = make_conditioner();
                let profile = NetworkProfile {
                    name: "self-test-process-ipv6".to_string(),
                    uplink: DirectionProfile {
                        delay_ms: 300,
                        ..Default::default()
                    },
                    protocol: ConditionProtocol::Tcp,
                    ..Default::default()
                };
                let names = vec![self_name.clone()];

                if let Err(error) = conditioner.start(
                    &profile,
                    Some(&out_filter6),
                    Some(&in_filter6),
                    2000,
                    &names,
                ) {
                    log.fail(&format!("IPv6 进程过滤测试启动失败：{error}"));
                } else {
                    thread::sleep(Duration::from_millis(200));
                    match measure_echo(port6, 3, 5000, LOOPBACK6) {
                        Some(value) if value >= 200 => {
                            log.pass(&format!("IPv6 进程过滤生效：RTT {value} ms"))
                        }
                        Some(value) => log.fail(&format!("IPv6 进程过滤未生效：RTT 仅 {value} ms")),
                        None => log.skip("IPv6 回环测试连接失败，跳过。"),
                    }
                    conditioner.stop();
                }
            }
            None => log.skip("IPv6 回环不可用，跳过。"),
        }
    }

    // 6) UDP 进程过滤（QUIC 等使用 UDP）。
    {
        let udp_port = find_free_udp_port(LOOPBACK);
        match UdpEchoServer::start(LOOPBACK, udp_port) {
            Some(_udp_echo) => {
                let udp_out_filter = format!("outbound and udp and udp.DstPort == {udp_port}");
                let udp_in_filter = format!("udp and udp.SrcPort == {udp_port}");
                let conditioner = make_conditioner();
                let profile = NetworkProfile {
                    name: "self-test-process-udp".to_string(),
                    uplink: DirectionProfile {
                        delay_ms: 300,
                        ..Default::default()
                    },
                    protocol: ConditionProtocol::Udp,
                    ..Default::default()
                };
                let names = vec![self_name.clone()];

                if let Err(error) = conditioner.start(
                    &profile,
                    Some(&udp_out_filter),
                    Some(&udp_in_filter),
                    2000,
                    &names,
                ) {
                    log.fail(&format!("UDP 进程过滤测试启动失败：{error}"));
                } else {
                    thread::sleep(Duration::from_millis(200));
                    match measure_udp_echo(udp_port, 3, 5000) {
                        Some(value) if value >= 200 => {
                            log.pass(&format!("UDP 进程过滤生效：RTT {value} ms"))
                        }
                        Some(value) => log.fail(&format!("UDP 进程过滤未生效：RTT 仅 {value} ms")),
                        None => log.fail("UDP 进程过滤测试收发失败。"),
                    }
                    conditioner.stop();
                }
            }
            None => log.skip("UDP 回环不可用，跳过。"),
        }
    }

    // 7) 上行带宽 64 kbps：8KB 至少应耗时约 1 秒。
    {
        let conditioner = make_conditioner();
        let profile = NetworkProfile {
            name: "self-test-bandwidth".to_string(),
            uplink: DirectionProfile {
                bandwidth_kbps: 64,
                ..Default::default()
            },
            protocol: ConditionProtocol::Tcp,
            ..Default::default()
        };

        if let Err(error) =
            conditioner.start(&profile, Some(&out_filter), Some(&in_filter), 2000, &[])
        {
            log.fail(&format!("带宽测试启动失败：{error}"));
        } else {
            thread::sleep(Duration::from_millis(200));
            match measure_transfer(port, 8192, 10000, LOOPBACK) {
                Some(transfer_ms) if transfer_ms >= 600 => {
                    log.pass(&format!("上行 64 kbps 限速生效：8KB 用时 {transfer_ms} ms"))
                }
                Some(transfer_ms) => {
                    log.fail(&format!("上行带宽未生效：8KB 仅用 {transfer_ms} ms"))
                }
                None => log.fail("带宽测试传输失败或超时。"),
            }
            conditioner.stop();
        }
    }

    // 8) 64KB 突发（仅 10ms 延时，不限速）：应无损完成。
    {
        let conditioner = make_conditioner();
        let profile = NetworkProfile {
            name: "self-test-burst".to_string(),
            uplink: DirectionProfile {
                delay_ms: 10,
                ..Default::default()
            },
            protocol: ConditionProtocol::Tcp,
            ..Default::default()
        };

        if let Err(error) =
            conditioner.start(&profile, Some(&out_filter), Some(&in_filter), 2000, &[])
        {
            log.fail(&format!("突发测试启动失败：{error}"));
        } else {
            thread::sleep(Duration::from_millis(200));
            let burst_ms = measure_transfer(port, 64 * 1024, 15000, LOOPBACK);
            match burst_ms {
                Some(value) => log.pass(&format!("64KB 突发传输完成：{value} ms")),
                None => log.fail("64KB 突发传输失败或超时。"),
            }
            let big_burst_ms = measure_transfer(port, 1024 * 1024, 20000, LOOPBACK);
            match big_burst_ms {
                Some(value) => log.pass(&format!("1MB 突发传输完成：{value} ms")),
                None => log.fail("1MB 突发传输失败或超时。"),
            }

            let dropped = conditioner
                .uplink_stats()
                .map(|stats| stats.dropped)
                .unwrap_or(0);
            if dropped == 0 {
                log.pass("突发测试无额外丢包");
            } else {
                log.fail(&format!("突发测试发生额外丢包：{dropped}"));
            }
            conditioner.stop();
        }
    }

    // 9) 上行 100% 丢包。
    {
        let conditioner = make_conditioner();
        let profile = NetworkProfile {
            name: "self-test-loss".to_string(),
            uplink: DirectionProfile {
                random_loss_percent: 100,
                ..Default::default()
            },
            protocol: ConditionProtocol::Tcp,
            ..Default::default()
        };

        if let Err(error) =
            conditioner.start(&profile, Some(&out_filter), Some(&in_filter), 2000, &[])
        {
            log.fail(&format!("丢包测试启动失败：{error}"));
        } else {
            thread::sleep(Duration::from_millis(200));
            if try_connect(port, 1500, LOOPBACK) {
                log.fail("上行 100% 丢包：连接意外成功");
            } else {
                log.pass("上行 100% 丢包：连接按预期超时");
            }

            let dropped = conditioner
                .uplink_stats()
                .map(|stats| stats.dropped)
                .unwrap_or(0);
            if dropped > 0 {
                log.pass(&format!("上行丢弃计数：{dropped}"));
            } else {
                log.fail("上行丢弃计数为 0。");
            }
            conditioner.stop();
        }
    }

    // 10) 停止后恢复。
    match measure_echo(port, 3, 3000, LOOPBACK) {
        Some(restored) if restored <= baseline + 300 => {
            log.pass(&format!("恢复后 RTT：{restored} ms"))
        }
        Some(restored) => log.fail(&format!("停止后未恢复：{restored} ms")),
        None => log.fail("停止后未恢复：连接失败"),
    }

    // 11) 真实网卡方向验证（可选）：访问 1.1.1.1:80。
    const REMOTE_HOST: &str = "1.1.1.1";
    const REMOTE_PORT: u16 = 80;
    match measure_tcp_connect_best(REMOTE_HOST, REMOTE_PORT, 3, 2000) {
        Some(baseline_remote) if baseline_remote <= 1000 => {
            let remote_out_filter = format!("outbound and tcp and tcp.DstPort == {REMOTE_PORT}");
            let remote_in_filter = format!("inbound and tcp and tcp.SrcPort == {REMOTE_PORT}");

            {
                let conditioner = make_conditioner();
                let profile = NetworkProfile {
                    name: "self-test-remote-up".to_string(),
                    uplink: DirectionProfile {
                        delay_ms: 300,
                        ..Default::default()
                    },
                    protocol: ConditionProtocol::Tcp,
                    ..Default::default()
                };
                if let Err(error) = conditioner.start(
                    &profile,
                    Some(&remote_out_filter),
                    Some(&remote_in_filter),
                    2000,
                    &[],
                ) {
                    log.fail(&format!("真实网卡上行延时测试启动失败：{error}"));
                } else {
                    thread::sleep(Duration::from_millis(200));
                    match measure_tcp_connect_best(REMOTE_HOST, REMOTE_PORT, 3, 3000) {
                        Some(value) if value >= baseline_remote + 200 => log.pass(&format!(
                            "真实网卡上行 300ms 延时生效：{baseline_remote} ms -> {value} ms"
                        )),
                        Some(value) => log.fail(&format!(
                            "真实网卡上行延时未生效：{baseline_remote} ms -> {value} ms"
                        )),
                        None => log.fail("真实网卡上行延时测试连接失败。"),
                    }
                    conditioner.stop();
                }
            }

            {
                let conditioner = make_conditioner();
                let profile = NetworkProfile {
                    name: "self-test-remote-down".to_string(),
                    downlink: DirectionProfile {
                        delay_ms: 300,
                        ..Default::default()
                    },
                    protocol: ConditionProtocol::Tcp,
                    ..Default::default()
                };
                if let Err(error) = conditioner.start(
                    &profile,
                    Some(&remote_out_filter),
                    Some(&remote_in_filter),
                    2000,
                    &[],
                ) {
                    log.fail(&format!("真实网卡下行延时测试启动失败：{error}"));
                } else {
                    thread::sleep(Duration::from_millis(200));
                    match measure_tcp_connect_best(REMOTE_HOST, REMOTE_PORT, 3, 3000) {
                        Some(value) if value >= baseline_remote + 200 => log.pass(&format!(
                            "真实网卡下行 300ms 延时生效：{baseline_remote} ms -> {value} ms"
                        )),
                        Some(value) => log.fail(&format!(
                            "真实网卡下行延时未生效：{baseline_remote} ms -> {value} ms"
                        )),
                        None => log.fail("真实网卡下行延时测试连接失败。"),
                    }
                    conditioner.stop();
                }
            }
        }
        _ => log.skip("公网 80 端口不可达或基线过慢，跳过真实网卡方向测试。"),
    }

    drop(echo);
    finish(log, &log_path)
}

fn make_conditioner() -> NetworkConditioner {
    let (sender, _receiver) = std::sync::mpsc::channel();
    let events = Arc::new(EngineEvents::new(sender, Box::new(|| {})));
    NetworkConditioner::new(events)
}

fn finish(mut log: Log, path: &Path) -> i32 {
    if log.failures == 0 {
        log.write("RESULT: PASS");
    } else {
        log.write(&format!("RESULT: FAIL ({})", log.failures));
    }
    let text = log.lines.join("\r\n") + "\r\n";
    let _ = std::fs::write(path, text);
    if log.failures == 0 {
        0
    } else {
        1
    }
}

fn find_free_tcp_port(address: IpAddr) -> u16 {
    match TcpListener::bind(SocketAddr::new(address, 0)) {
        Ok(listener) => {
            let port = listener.local_addr().map(|addr| addr.port()).unwrap_or(0);
            drop(listener);
            port
        }
        Err(_) => 0,
    }
}

fn find_free_udp_port(address: IpAddr) -> u16 {
    match UdpSocket::bind(SocketAddr::new(address, 0)) {
        Ok(socket) => {
            let port = socket.local_addr().map(|addr| addr.port()).unwrap_or(0);
            drop(socket);
            port
        }
        Err(_) => 0,
    }
}

fn measure_echo(port: u16, attempts: usize, timeout_ms: u64, address: IpAddr) -> Option<u64> {
    let mut values = Vec::with_capacity(attempts);
    for _ in 0..attempts {
        values.push(measure_echo_once(port, timeout_ms, address)?);
    }
    values.sort_unstable();
    values.get(values.len() / 2).copied()
}

fn measure_echo_once(port: u16, timeout_ms: u64, address: IpAddr) -> Option<u64> {
    let timeout = Duration::from_millis(timeout_ms);
    let mut stream = TcpStream::connect_timeout(&SocketAddr::new(address, port), timeout).ok()?;
    stream.set_read_timeout(Some(timeout)).ok()?;
    stream.set_write_timeout(Some(timeout)).ok()?;

    let started = Instant::now();
    stream.write_all(&[0x42]).ok()?;
    let mut response = [0u8; 1];
    stream.read_exact(&mut response).ok()?;
    let elapsed = started.elapsed().as_millis() as u64;
    if response[0] == 0x42 {
        Some(elapsed)
    } else {
        None
    }
}

fn measure_transfer(port: u16, bytes: usize, timeout_ms: u64, address: IpAddr) -> Option<u64> {
    let timeout = Duration::from_millis(timeout_ms);
    let mut stream = TcpStream::connect_timeout(&SocketAddr::new(address, port), timeout).ok()?;
    stream.set_read_timeout(Some(timeout)).ok()?;
    stream.set_write_timeout(Some(timeout)).ok()?;

    let payload: Vec<u8> = (0..bytes).map(|_| fastrand::u32(..) as u8).collect();
    let started = Instant::now();
    stream.write_all(&payload).ok()?;

    let mut received = 0usize;
    let mut buffer = [0u8; 4096];
    while received < bytes {
        let read = stream.read(&mut buffer).ok()?;
        if read == 0 {
            return None;
        }
        received += read;
    }

    Some(started.elapsed().as_millis() as u64)
}

fn measure_udp_echo(port: u16, attempts: usize, timeout_ms: u64) -> Option<u64> {
    let mut values = Vec::with_capacity(attempts);
    for _ in 0..attempts {
        let socket = UdpSocket::bind(SocketAddr::new(LOOPBACK, 0)).ok()?;
        socket
            .set_read_timeout(Some(Duration::from_millis(timeout_ms)))
            .ok()?;
        let target = SocketAddr::new(LOOPBACK, port);

        let started = Instant::now();
        socket.send_to(&[0x42], target).ok()?;
        let mut response = [0u8; 16];
        let (read, _) = socket.recv_from(&mut response).ok()?;
        let elapsed = started.elapsed().as_millis() as u64;
        if read != 1 || response[0] != 0x42 {
            return None;
        }
        values.push(elapsed);
    }
    values.sort_unstable();
    values.get(values.len() / 2).copied()
}

fn measure_tcp_connect(host: &str, port: u16, timeout_ms: u64) -> Option<u64> {
    let timeout = Duration::from_millis(timeout_ms);
    let address = (host, port)
        .to_socket_addrs()
        .ok()?
        .find(|candidate| candidate.is_ipv4())
        .or_else(|| (host, port).to_socket_addrs().ok()?.next())?;

    let started = Instant::now();
    let _stream = TcpStream::connect_timeout(&address, timeout).ok()?;
    Some(started.elapsed().as_millis() as u64)
}

fn measure_tcp_connect_best(
    host: &str,
    port: u16,
    attempts: usize,
    timeout_ms: u64,
) -> Option<u64> {
    let mut best: Option<u64> = None;
    for _ in 0..attempts {
        let value = measure_tcp_connect(host, port, timeout_ms)?;
        best = Some(match best {
            Some(current) => current.min(value),
            None => value,
        });
    }
    best
}

fn try_connect(port: u16, timeout_ms: u64, address: IpAddr) -> bool {
    TcpStream::connect_timeout(
        &SocketAddr::new(address, port),
        Duration::from_millis(timeout_ms),
    )
    .is_ok()
}

fn echo_connection(mut stream: TcpStream) {
    let mut buffer = [0u8; 4096];
    loop {
        match stream.read(&mut buffer) {
            Ok(0) => break,
            Ok(read) => {
                if stream.write_all(&buffer[..read]).is_err() {
                    break;
                }
            }
            Err(_) => break,
        }
    }
}

fn loopback_server_child(port: u16) -> i32 {
    let Some(server) = EchoServer::start(LOOPBACK, port) else {
        return 2;
    };
    thread::sleep(Duration::from_secs(30));
    drop(server);
    0
}

struct EchoServer {
    stop: Arc<AtomicBool>,
    handle: Option<thread::JoinHandle<()>>,
}

impl EchoServer {
    fn start(address: IpAddr, port: u16) -> Option<Self> {
        let listener = TcpListener::bind(SocketAddr::new(address, port)).ok()?;
        listener.set_nonblocking(true).ok()?;
        let stop = Arc::new(AtomicBool::new(false));
        let stop_thread = stop.clone();
        let handle = thread::spawn(move || {
            while !stop_thread.load(Ordering::Relaxed) {
                match listener.accept() {
                    Ok((stream, _)) => {
                        let _ = stream.set_nonblocking(false);
                        thread::spawn(move || echo_connection(stream));
                    }
                    Err(error) if error.kind() == std::io::ErrorKind::WouldBlock => {
                        thread::sleep(Duration::from_millis(5));
                    }
                    Err(_) => break,
                }
            }
        });
        Some(Self {
            stop,
            handle: Some(handle),
        })
    }
}

impl Drop for EchoServer {
    fn drop(&mut self) {
        self.stop.store(true, Ordering::Relaxed);
        if let Some(handle) = self.handle.take() {
            let _ = handle.join();
        }
    }
}

struct UdpEchoServer {
    stop: Arc<AtomicBool>,
    handle: Option<thread::JoinHandle<()>>,
}

impl UdpEchoServer {
    fn start(address: IpAddr, port: u16) -> Option<Self> {
        let socket = UdpSocket::bind(SocketAddr::new(address, port)).ok()?;
        socket
            .set_read_timeout(Some(Duration::from_millis(100)))
            .ok()?;
        let stop = Arc::new(AtomicBool::new(false));
        let stop_thread = stop.clone();
        let handle = thread::spawn(move || {
            let mut buffer = [0u8; 2048];
            while !stop_thread.load(Ordering::Relaxed) {
                if let Ok((read, remote)) = socket.recv_from(&mut buffer) {
                    let _ = socket.send_to(&buffer[..read], remote);
                }
            }
        });
        Some(Self {
            stop,
            handle: Some(handle),
        })
    }
}

impl Drop for UdpEchoServer {
    fn drop(&mut self) {
        self.stop.store(true, Ordering::Relaxed);
        if let Some(handle) = self.handle.take() {
            let _ = handle.join();
        }
    }
}

struct MiniHttpServer {
    stop: Arc<AtomicBool>,
    handle: Option<thread::JoinHandle<()>>,
}

impl MiniHttpServer {
    fn start(address: IpAddr, port: u16, routes: HashMap<String, Vec<u8>>) -> Option<Self> {
        let listener = TcpListener::bind(SocketAddr::new(address, port)).ok()?;
        listener.set_nonblocking(true).ok()?;
        let stop = Arc::new(AtomicBool::new(false));
        let stop_thread = stop.clone();
        let routes = Arc::new(routes);

        let handle = thread::spawn(move || {
            while !stop_thread.load(Ordering::Relaxed) {
                match listener.accept() {
                    Ok((mut stream, _)) => {
                        let _ = stream.set_nonblocking(false);
                        let routes = routes.clone();
                        thread::spawn(move || serve_http(&mut stream, &routes));
                    }
                    Err(error) if error.kind() == std::io::ErrorKind::WouldBlock => {
                        thread::sleep(Duration::from_millis(5));
                    }
                    Err(_) => break,
                }
            }
        });

        Some(Self {
            stop,
            handle: Some(handle),
        })
    }
}

impl Drop for MiniHttpServer {
    fn drop(&mut self) {
        self.stop.store(true, Ordering::Relaxed);
        if let Some(handle) = self.handle.take() {
            let _ = handle.join();
        }
    }
}

fn serve_http(stream: &mut TcpStream, routes: &HashMap<String, Vec<u8>>) {
    let _ = stream.set_read_timeout(Some(Duration::from_secs(2)));
    let mut request = [0u8; 4096];
    let read = match stream.read(&mut request) {
        Ok(0) | Err(_) => return,
        Ok(read) => read,
    };

    let text = String::from_utf8_lossy(&request[..read]);
    let request_line = text.lines().next().unwrap_or("");
    let path = request_line.split(' ').nth(1).unwrap_or("/");

    let (status, body) = match routes.get(path) {
        Some(body) => ("200 OK", body.clone()),
        None => ("404 Not Found", Vec::new()),
    };

    let header = format!(
        "HTTP/1.1 {status}\r\nContent-Length: {}\r\nConnection: close\r\n\r\n",
        body.len()
    );
    let _ = stream.write_all(header.as_bytes());
    let _ = stream.write_all(&body);
    let _ = stream.flush();
}
