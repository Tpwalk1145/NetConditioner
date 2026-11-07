using System.Diagnostics;
using System.Net;
using System.Net.Sockets;
using System.Security.Principal;
using System.Text;
using NetConditioner.Models;

namespace NetConditioner.Services;

/// <summary>
/// 自测使用本机回环端口和公网 80 端口，不会影响常规 443 连接。
/// 结果写入 %TEMP%\NetConditioner-selftest.log。
/// </summary>
public static class SelfTest
{
    public static int Run()
    {
        var logPath = Path.Combine(Path.GetTempPath(), "NetConditioner-selftest.log");
        var log = new StringBuilder();
        var failures = 0;

        void Write(string line)
        {
            log.AppendLine($"[{DateTime.Now:HH:mm:ss.fff}] {line}");
        }

        void Pass(string line)
        {
            Write("PASS  " + line);
        }

        void Fail(string line)
        {
            failures++;
            Write("FAIL  " + line);
        }

        try
        {
            Write($"NetConditioner self-test, {Environment.OSVersion}");
            Write($"Log: {logPath}");

            using var identity = WindowsIdentity.GetCurrent();
            if (!new WindowsPrincipal(identity).IsInRole(WindowsBuiltInRole.Administrator))
            {
                Fail("需要管理员权限。请以管理员身份运行 --self-test。");
                return Finish();
            }

            // 快捷键校验：单键 F8、组合键、修饰键本身。
            if (HotkeyFormatter.IsValid(0, 0x77) &&
                HotkeyFormatter.IsValid(HotkeyDefaults.ModControl, 0x55) &&
                !HotkeyFormatter.IsValid(0, 0x10) &&
                HotkeyFormatter.Format(0, 0x77) == "F8")
            {
                Pass("快捷键校验：支持单键 F8，修饰键本身被排除");
            }
            else
            {
                Fail("快捷键校验失败。");
            }

            // 配置读写：目标进程列表去重、规范化。
            var configPath = Path.Combine(
                Path.GetTempPath(), "NetConditioner-selftest-config.json");
            try
            {
                var store = new ProfileStore(configPath);
                var testConfig = new AppConfig();
                testConfig.Profiles.Add(new NetworkProfile
                {
                    Name = "process-config-test",
                    ProcessNames = new List<string>
                    {
                        "chrome.exe", @"C:\Tools\MyApp.EXE", "chrome.exe",
                    },
                });
                testConfig.ActiveProfileId = testConfig.Profiles[0].Id;
                store.Save(testConfig);

                var loaded = store.Load(out _);
                var names = loaded.Profiles[0].ProcessNames;
                if (names.Count == 2 &&
                    names.Contains("chrome.exe") &&
                    names.Contains("myapp.exe"))
                {
                    Pass("配置读写：目标进程列表正确去重并规范化");
                }
                else
                {
                    Fail($"配置读写：目标进程列表异常（{string.Join(",", names)}）");
                }
            }
            catch (Exception ex)
            {
                Fail("配置读写测试异常：" + ex.Message);
            }
            finally
            {
                try
                {
                    File.Delete(configPath);
                }
                catch
                {
                    // ignore
                }
            }

            var port = FindFreeTcpPort();
            Write($"测试端口：127.0.0.1:{port}");

            using var echo = new EchoServer(port);
            echo.Start();

            var baseline = MeasureEcho(port, 3, 3000);
            if (baseline is null)
            {
                Fail("本地回环基线连接失败，无法继续。");
                return Finish();
            }
            Write($"基线 RTT 中位数：{baseline.Value} ms");

            var outFilter = $"outbound and tcp and tcp.DstPort == {port}";
            var inFilter = $"inbound and tcp and tcp.SrcPort == {port}";

            // 1) 上行延时 300ms。
            var delayProfile = new NetworkProfile
            {
                Name = "self-test-delay",
                Uplink = new DirectionProfile { DelayMs = 300 },
                Downlink = new DirectionProfile(),
                Protocol = ConditionProtocol.Tcp,
            };

            using (var conditioner = new NetworkConditioner())
            {
                conditioner.Start(delayProfile, outFilter, inFilter);
                Thread.Sleep(200);
                var delayed = MeasureEcho(port, 3, 5000);
                if (delayed is null)
                {
                    Fail("延时测试连接失败。");
                }
                else if (delayed.Value >= baseline.Value + 200)
                {
                    Pass($"上行 300ms 延时生效：{baseline.Value} ms -> {delayed.Value} ms");
                }
                else
                {
                    Fail($"上行延时未生效：{baseline.Value} ms -> {delayed.Value} ms");
                }

                var sent = conditioner.UplinkStats?.Sent ?? 0;
                if (sent > 0)
                    Pass($"上行发送计数：{sent}");
                else
                    Fail("上行发送计数为 0，WinDivert 可能没有捕获回环流量。");
                conditioner.Stop();
            }

            // 2) 下行延时 300ms。
            var downDelayProfile = new NetworkProfile
            {
                Name = "self-test-down-delay",
                Uplink = new DirectionProfile(),
                Downlink = new DirectionProfile { DelayMs = 300 },
                Protocol = ConditionProtocol.Tcp,
            };

            // 回环流量在 WinDivert 中不保证带 inbound 标志，因此自测用“源端口”过滤响应包；
            // 正式运行使用 inbound 过滤器。
            var loopbackDownFilter = $"tcp and tcp.SrcPort == {port}";

            using (var conditioner = new NetworkConditioner())
            {
                conditioner.Start(downDelayProfile, outFilter, loopbackDownFilter);
                Thread.Sleep(200);
                var delayedDown = MeasureEcho(port, 3, 5000);
                if (delayedDown is null)
                {
                    Fail("下行延时测试连接失败。");
                }
                else if (delayedDown.Value >= baseline.Value + 200)
                {
                    Pass($"下行 300ms 延时生效：{baseline.Value} ms -> {delayedDown.Value} ms");
                }
                else
                {
                    Fail($"下行延时未生效：{baseline.Value} ms -> {delayedDown.Value} ms");
                }

                var downSent = conditioner.DownlinkStats?.Sent ?? 0;
                if (downSent > 0)
                    Pass($"下行发送计数：{downSent}");
                else
                    Fail("下行发送计数为 0。");
                conditioner.Stop();
            }

            // 3) 进程过滤：选中的进程生效，未选中的进程直通。
            var selfName = Process.GetCurrentProcess().ProcessName;
            var matchProcessProfile = new NetworkProfile
            {
                Name = "self-test-process-match",
                Uplink = new DirectionProfile { DelayMs = 300 },
                Downlink = new DirectionProfile(),
                Protocol = ConditionProtocol.Tcp,
                ProcessNames = new List<string> { selfName },
            };

            using (var conditioner = new NetworkConditioner())
            {
                conditioner.Start(matchProcessProfile, outFilter, inFilter);
                Thread.Sleep(200);
                var delayedByProcess = MeasureEcho(port, 3, 5000);
                if (delayedByProcess is null)
                {
                    Fail("进程过滤（命中）测试连接失败。");
                }
                else if (delayedByProcess.Value >= baseline.Value + 200)
                {
                    Pass($"进程过滤生效：{selfName} 的 RTT {baseline.Value} ms -> {delayedByProcess.Value} ms");
                }
                else
                {
                    Fail($"进程过滤未生效：RTT 仅 {delayedByProcess.Value} ms");
                }

                conditioner.Stop();
            }

            var otherProcessProfile = new NetworkProfile
            {
                Name = "self-test-process-miss",
                Uplink = new DirectionProfile { DelayMs = 300 },
                Downlink = new DirectionProfile(),
                Protocol = ConditionProtocol.Tcp,
                ProcessNames = new List<string> { "netconditioner-not-selected.exe" },
            };

            using (var conditioner = new NetworkConditioner())
            {
                conditioner.Start(otherProcessProfile, outFilter, inFilter);
                Thread.Sleep(200);
                var passThrough = MeasureEcho(port, 3, 3000);
                if (passThrough is null)
                {
                    Fail("进程过滤（未命中）测试连接失败。");
                }
                else if (passThrough.Value <= baseline.Value + 150)
                {
                    Pass($"未选中的进程直通：RTT {passThrough.Value} ms");
                }
                else
                {
                    Fail($"未选中的进程被错误限速：RTT {passThrough.Value} ms");
                }

                conditioner.Stop();
            }

            // 4) 下行进程过滤：验证入站包也能正确归属到进程。
            var downProcessProfile = new NetworkProfile
            {
                Name = "self-test-process-down",
                Uplink = new DirectionProfile(),
                Downlink = new DirectionProfile { DelayMs = 300 },
                Protocol = ConditionProtocol.Tcp,
                ProcessNames = new List<string> { selfName },
            };

            using (var conditioner = new NetworkConditioner())
            {
                conditioner.Start(downProcessProfile, outFilter, loopbackDownFilter);
                Thread.Sleep(200);
                var delayedDownByProcess = MeasureEcho(port, 3, 5000);
                if (delayedDownByProcess is null)
                {
                    Fail("下行进程过滤测试连接失败。");
                }
                else if (delayedDownByProcess.Value >= baseline.Value + 200)
                {
                    Pass($"下行进程过滤生效：{baseline.Value} ms -> {delayedDownByProcess.Value} ms");
                }
                else
                {
                    Fail($"下行进程过滤未生效：RTT 仅 {delayedDownByProcess.Value} ms");
                }

                conditioner.Stop();
            }

            // 5) IPv6 回环上的进程过滤（浏览器等可能优先走 IPv6）。
            try
            {
                var port6 = FindFreeTcpPort(IPAddress.IPv6Loopback);
                using var echo6 = new EchoServer(IPAddress.IPv6Loopback, port6);
                echo6.Start();

                var outFilter6 = $"outbound and tcp and tcp.DstPort == {port6}";
                var inFilter6 = $"tcp and tcp.SrcPort == {port6}";
                var ipv6Profile = new NetworkProfile
                {
                    Name = "self-test-process-ipv6",
                    Uplink = new DirectionProfile { DelayMs = 300 },
                    Downlink = new DirectionProfile(),
                    Protocol = ConditionProtocol.Tcp,
                    ProcessNames = new List<string> { selfName },
                };

                using var conditioner = new NetworkConditioner();
                conditioner.Start(ipv6Profile, outFilter6, inFilter6);
                Thread.Sleep(200);
                var ipv6Ms = MeasureEcho(port6, 3, 5000, IPAddress.IPv6Loopback);
                if (ipv6Ms is null)
                {
                    Write("SKIP  IPv6 回环测试连接失败，跳过。");
                }
                else if (ipv6Ms.Value >= 200)
                {
                    Pass($"IPv6 进程过滤生效：RTT {ipv6Ms.Value} ms");
                }
                else
                {
                    Fail($"IPv6 进程过滤未生效：RTT 仅 {ipv6Ms.Value} ms");
                }

                conditioner.Stop();
            }
            catch (Exception ex)
            {
                Write($"SKIP  IPv6 回环不可用：{ex.Message}");
            }

            // 6) UDP 进程过滤（QUIC 等使用 UDP）。
            try
            {
                var udpPort = FindFreeUdpPort(IPAddress.Loopback);
                using var udpEcho = new UdpEchoServer(IPAddress.Loopback, udpPort);
                udpEcho.Start();

                var udpOutFilter = $"outbound and udp and udp.DstPort == {udpPort}";
                var udpInFilter = $"udp and udp.SrcPort == {udpPort}";
                var udpProfile = new NetworkProfile
                {
                    Name = "self-test-process-udp",
                    Uplink = new DirectionProfile { DelayMs = 300 },
                    Downlink = new DirectionProfile(),
                    Protocol = ConditionProtocol.Udp,
                    ProcessNames = new List<string> { selfName },
                };

                using var conditioner = new NetworkConditioner();
                conditioner.Start(udpProfile, udpOutFilter, udpInFilter);
                Thread.Sleep(200);
                var udpMs = MeasureUdpEcho(udpPort, 3, 5000);
                if (udpMs is null)
                {
                    Fail("UDP 进程过滤测试收发失败。");
                }
                else if (udpMs.Value >= 200)
                {
                    Pass($"UDP 进程过滤生效：RTT {udpMs.Value} ms");
                }
                else
                {
                    Fail($"UDP 进程过滤未生效：RTT 仅 {udpMs.Value} ms");
                }

                conditioner.Stop();
            }
            catch (Exception ex)
            {
                Write($"SKIP  UDP 回环不可用：{ex.Message}");
            }

            // 7) 上行带宽 64 kbps：8KB 至少应耗时约 1 秒。
            var bandwidthProfile = new NetworkProfile
            {
                Name = "self-test-bandwidth",
                Uplink = new DirectionProfile { BandwidthKbps = 64 },
                Downlink = new DirectionProfile(),
                Protocol = ConditionProtocol.Tcp,
            };

            using (var conditioner = new NetworkConditioner())
            {
                conditioner.Start(bandwidthProfile, outFilter, inFilter);
                Thread.Sleep(200);
                var transferMs = MeasureTransfer(port, 8192, 10000);
                if (transferMs is null)
                {
                    Fail("带宽测试传输失败或超时。");
                }
                else if (transferMs.Value >= 600)
                {
                    Pass($"上行 64 kbps 限速生效：8KB 用时 {transferMs.Value} ms");
                }
                else
                {
                    Fail($"上行带宽未生效：8KB 仅用 {transferMs.Value} ms");
                }
                conditioner.Stop();
            }

            // 8) 64KB 突发（仅 10ms 延时，不限速）：应无损完成。
            var burstProfile = new NetworkProfile
            {
                Name = "self-test-burst",
                Uplink = new DirectionProfile { DelayMs = 10 },
                Downlink = new DirectionProfile(),
                Protocol = ConditionProtocol.Tcp,
            };

            using (var conditioner = new NetworkConditioner())
            {
                conditioner.Start(burstProfile, outFilter, inFilter);
                Thread.Sleep(200);
                var burstMs = MeasureTransfer(port, 64 * 1024, 15000);
                if (burstMs is null)
                {
                    Fail("64KB 突发传输失败或超时。");
                }
                else
                {
                    Pass($"64KB 突发传输完成：{burstMs.Value} ms");
                }

                var bigBurstMs = MeasureTransfer(port, 1024 * 1024, 20000);
                if (bigBurstMs is null)
                {
                    Fail("1MB 突发传输失败或超时。");
                }
                else
                {
                    Pass($"1MB 突发传输完成：{bigBurstMs.Value} ms");
                }

                var burstDropped = conditioner.UplinkStats?.Dropped ?? 0;
                if (burstDropped == 0)
                    Pass("突发测试无额外丢包");
                else
                    Fail($"突发测试发生额外丢包：{burstDropped}");
                conditioner.Stop();
            }

            // 9) 上行 100% 丢包。
            var lossProfile = new NetworkProfile
            {
                Name = "self-test-loss",
                Uplink = new DirectionProfile { RandomLossPercent = 100 },
                Downlink = new DirectionProfile(),
                Protocol = ConditionProtocol.Tcp,
            };

            using (var conditioner = new NetworkConditioner())
            {
                conditioner.Start(lossProfile, outFilter, inFilter);
                Thread.Sleep(200);
                var connected = TryConnect(port, 1500);
                if (!connected)
                    Pass("上行 100% 丢包：连接按预期超时");
                else
                    Fail("上行 100% 丢包：连接意外成功");

                var dropped = conditioner.UplinkStats?.Dropped ?? 0;
                if (dropped > 0)
                    Pass($"上行丢弃计数：{dropped}");
                else
                    Fail("上行丢弃计数为 0。");
                conditioner.Stop();
            }

            // 10) 停止后恢复。
            var restored = MeasureEcho(port, 3, 3000);
            if (restored is not null && restored.Value <= baseline.Value + 300)
                Pass($"恢复后 RTT：{restored.Value} ms");
            else
                Fail($"停止后未恢复：{restored?.ToString() ?? "连接失败"}");

            // 11) 真实网卡方向验证（可选）：只影响公网 80 端口，不碰 443。
            const string remoteHost = "1.1.1.1";
            const int remotePort = 80;
            var baselineRemote = MeasureTcpConnectBest(remoteHost, remotePort, 3, 2000);
            if (baselineRemote is null || baselineRemote.Value > 1000)
            {
                Write("SKIP  公网 80 端口不可达或基线过慢，跳过真实网卡方向测试。");
            }
            else
            {
                var remoteOutFilter = $"outbound and tcp and tcp.DstPort == {remotePort}";
                var remoteInFilter = $"inbound and tcp and tcp.SrcPort == {remotePort}";

                var remoteUpProfile = new NetworkProfile
                {
                    Name = "self-test-remote-up",
                    Uplink = new DirectionProfile { DelayMs = 300 },
                    Downlink = new DirectionProfile(),
                    Protocol = ConditionProtocol.Tcp,
                };
                using (var conditioner = new NetworkConditioner())
                {
                    conditioner.Start(remoteUpProfile, remoteOutFilter, remoteInFilter);
                    Thread.Sleep(200);
                    var delayedRemote = MeasureTcpConnectBest(remoteHost, remotePort, 3, 3000);
                    if (delayedRemote is null)
                        Fail("真实网卡上行延时测试连接失败。");
                    else if (delayedRemote.Value >= baselineRemote.Value + 200)
                        Pass($"真实网卡上行 300ms 延时生效：{baselineRemote.Value} ms -> {delayedRemote.Value} ms");
                    else
                        Fail($"真实网卡上行延时未生效：{baselineRemote.Value} ms -> {delayedRemote.Value} ms");
                    conditioner.Stop();
                }

                var remoteDownProfile = new NetworkProfile
                {
                    Name = "self-test-remote-down",
                    Uplink = new DirectionProfile(),
                    Downlink = new DirectionProfile { DelayMs = 300 },
                    Protocol = ConditionProtocol.Tcp,
                };
                using (var conditioner = new NetworkConditioner())
                {
                    conditioner.Start(remoteDownProfile, remoteOutFilter, remoteInFilter);
                    Thread.Sleep(200);
                    var delayedRemoteDown = MeasureTcpConnectBest(remoteHost, remotePort, 3, 3000);
                    if (delayedRemoteDown is null)
                        Fail("真实网卡下行延时测试连接失败。");
                    else if (delayedRemoteDown.Value >= baselineRemote.Value + 200)
                        Pass($"真实网卡下行 300ms 延时生效：{baselineRemote.Value} ms -> {delayedRemoteDown.Value} ms");
                    else
                        Fail($"真实网卡下行延时未生效：{baselineRemote.Value} ms -> {delayedRemoteDown.Value} ms");
                    conditioner.Stop();
                }
            }

        }
        catch (Exception ex)
        {
            Fail("自测异常：" + ex);
        }

        return Finish();

        int Finish()
        {
            Write(failures == 0 ? "RESULT: PASS" : $"RESULT: FAIL ({failures})");
            try
            {
                File.WriteAllText(logPath, log.ToString(), Encoding.UTF8);
            }
            catch
            {
                // 日志写失败也不能影响退出码。
            }
            return failures == 0 ? 0 : 1;
        }
    }

    private static int FindFreeTcpPort() => FindFreeTcpPort(IPAddress.Loopback);

    private static int FindFreeTcpPort(IPAddress address)
    {
        var listener = new TcpListener(address, 0);
        listener.Start();
        var port = ((IPEndPoint)listener.LocalEndpoint).Port;
        listener.Stop();
        return port;
    }

    private static int FindFreeUdpPort(IPAddress address)
    {
        using var client = new UdpClient(new IPEndPoint(address, 0));
        return ((IPEndPoint)client.Client.LocalEndPoint!).Port;
    }

    private static long? MeasureUdpEcho(int port, int attempts, int timeoutMs)
    {
        var target = new IPEndPoint(IPAddress.Loopback, port);
        var values = new List<long>();
        for (var i = 0; i < attempts; i++)
        {
            try
            {
                using var client = new UdpClient(new IPEndPoint(IPAddress.Loopback, 0));
                client.Client.ReceiveTimeout = timeoutMs;

                var sw = Stopwatch.StartNew();
                client.Send(new byte[] { 0x42 }, 1, target);
                IPEndPoint? remote = null;
                var response = client.Receive(ref remote);
                sw.Stop();

                if (response.Length != 1 || response[0] != 0x42)
                    return null;

                values.Add(sw.ElapsedMilliseconds);
            }
            catch
            {
                return null;
            }
        }

        values.Sort();
        return values[values.Count / 2];
    }

    private static long? MeasureEcho(
        int port, int attempts, int timeoutMs, IPAddress? address = null)
    {
        var target = address ?? IPAddress.Loopback;
        var values = new List<long>();
        for (var i = 0; i < attempts; i++)
        {
            var value = MeasureEchoOnce(target, port, timeoutMs);
            if (value is null)
                return null;
            values.Add(value.Value);
        }
        values.Sort();
        return values[values.Count / 2];
    }

    private static long? MeasureEchoOnce(IPAddress address, int port, int timeoutMs)
    {
        try
        {
            using var client = new TcpClient();
            var connectTask = client.ConnectAsync(address, port);
            if (!connectTask.Wait(timeoutMs))
                return null;

            using var stream = client.GetStream();
            stream.ReadTimeout = timeoutMs;
            stream.WriteTimeout = timeoutMs;

            var sw = Stopwatch.StartNew();
            stream.WriteByte(0x42);
            var response = stream.ReadByte();
            sw.Stop();

            return response == 0x42 ? sw.ElapsedMilliseconds : null;
        }
        catch
        {
            return null;
        }
    }

    private static long? MeasureTransfer(
        int port, int bytes, int timeoutMs, IPAddress? address = null)
    {
        try
        {
            using var client = new TcpClient();
            var connectTask = client.ConnectAsync(address ?? IPAddress.Loopback, port);
            if (!connectTask.Wait(timeoutMs))
                return null;

            using var stream = client.GetStream();
            stream.ReadTimeout = timeoutMs;
            stream.WriteTimeout = timeoutMs;

            var payload = new byte[bytes];
            Random.Shared.NextBytes(payload);

            var sw = Stopwatch.StartNew();
            stream.Write(payload, 0, payload.Length);

            var received = 0;
            var buffer = new byte[4096];
            while (received < bytes)
            {
                var read = stream.Read(buffer, 0, Math.Min(buffer.Length, bytes - received));
                if (read <= 0)
                    return null;
                received += read;
            }
            sw.Stop();
            return sw.ElapsedMilliseconds;
        }
        catch
        {
            return null;
        }
    }

    private static long? MeasureTcpConnect(string host, int port, int timeoutMs)
    {
        try
        {
            using var client = new TcpClient();
            var sw = Stopwatch.StartNew();
            var task = client.ConnectAsync(host, port);
            if (!task.Wait(timeoutMs))
                return null;
            sw.Stop();
            return client.Connected ? sw.ElapsedMilliseconds : null;
        }
        catch
        {
            return null;
        }
    }

    private static long? MeasureTcpConnectBest(string host, int port, int attempts, int timeoutMs)
    {
        long? best = null;
        for (var i = 0; i < attempts; i++)
        {
            var value = MeasureTcpConnect(host, port, timeoutMs);
            if (value is null)
                return null;
            if (best is null || value.Value < best.Value)
                best = value;
        }
        return best;
    }

    private static bool TryConnect(int port, int timeoutMs)
    {
        try
        {
            using var client = new TcpClient();
            var task = client.ConnectAsync(IPAddress.Loopback, port);
            return task.Wait(timeoutMs) && client.Connected;
        }
        catch
        {
            return false;
        }
    }

    private sealed class UdpEchoServer : IDisposable
    {
        private readonly UdpClient _client;
        private readonly CancellationTokenSource _cts = new();
        private Task? _loop;

        public UdpEchoServer(IPAddress address, int port)
        {
            _client = new UdpClient(new IPEndPoint(address, port));
        }

        public void Start()
        {
            _loop = Task.Run(LoopAsync);
        }

        private async Task LoopAsync()
        {
            try
            {
                while (!_cts.IsCancellationRequested)
                {
                    var result = await _client.ReceiveAsync(_cts.Token);
                    await _client.SendAsync(
                        result.Buffer.AsMemory(), result.RemoteEndPoint, _cts.Token);
                }
            }
            catch
            {
                // 关闭时正常退出。
            }
        }

        public void Dispose()
        {
            _cts.Cancel();
            _client.Dispose();
            try
            {
                _loop?.Wait(1000);
            }
            catch
            {
                // ignore
            }
            _cts.Dispose();
        }
    }

    private sealed class EchoServer : IDisposable
    {
        private readonly TcpListener _listener;
        private readonly CancellationTokenSource _cts = new();
        private Task? _acceptTask;

        public EchoServer(int port) : this(IPAddress.Loopback, port)
        {
        }

        public EchoServer(IPAddress address, int port)
        {
            _listener = new TcpListener(address, port);
        }

        public void Start()
        {
            _listener.Start();
            _acceptTask = Task.Run(AcceptLoopAsync);
        }

        private async Task AcceptLoopAsync()
        {
            try
            {
                while (!_cts.IsCancellationRequested)
                {
                    var client = await _listener.AcceptTcpClientAsync(_cts.Token);
                    _ = Task.Run(() => HandleAsync(client));
                }
            }
            catch
            {
                // 关闭监听时正常退出。
            }
        }

        private static async Task HandleAsync(TcpClient client)
        {
            try
            {
                using (client)
                await using (var stream = client.GetStream())
                {
                    var buffer = new byte[4096];
                    while (true)
                    {
                        var read = await stream.ReadAsync(buffer);
                        if (read <= 0)
                            break;
                        await stream.WriteAsync(buffer.AsMemory(0, read));
                    }
                }
            }
            catch
            {
                // 客户端断开。
            }
        }

        public void Dispose()
        {
            _cts.Cancel();
            _listener.Stop();
            try
            {
                _acceptTask?.Wait(1000);
            }
            catch
            {
                // ignore
            }
            _cts.Dispose();
        }
    }
}
