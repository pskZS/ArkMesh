#ifndef ARKMESH_GO_PROBE_H
#define ARKMESH_GO_PROBE_H

#include <napi/native_api.h>

namespace gobridge {

// 自检: dlopen Go 运行时库并调用 GoHello, 返回诊断报告字符串
napi_value Probe(napi_env env, napi_callback_info info);

// 启动 Go 侧 goroutine 计数器
napi_value StartTicker(napi_env env, napi_callback_info info);

// 读取计数器当前值
napi_value Ticks(napi_env env, napi_callback_info info);

// TLS 布局探针: 测量 TP 相对偏移的可得性
napi_value TlsProbe(napi_env env, napi_callback_info info);

// ==== S5: Go 侧网络能力 ====

// 起 UDP 回显服务+客户端, 返回监听的 addr
napi_value UdpStart(napi_env env, napi_callback_info info);

// UDP 往返统计
napi_value UdpStats(napi_env env, napi_callback_info info);

// (hostPort, payload) 发一个 UDP 包并等回包
napi_value UdpSendTo(napi_env env, napi_callback_info info);

// (host) 用 Go 的解析器查 DNS
napi_value DnsLookup(napi_env env, napi_callback_info info);

// (fd) 把 VPN 的 TUN fd 交给 Go 侧 reader
napi_value TunStart(napi_env env, napi_callback_info info);

// (fd, hexPkt) 往 TUN 写一个 IP 包, 返回写入字节数
napi_value TunWrite(napi_env env, napi_callback_info info);

// TUN 读写统计
napi_value TunStats(napi_env env, napi_callback_info info);

// (fd, dst, marker) 发标记包并看能否从 TUN 读到
napi_value TunCapture(napi_env env, napi_callback_info info);

// (srcIp, dstIp, sport, dport, payload) 构造 IPv4+UDP 包的 hex
napi_value TunBuildUdp(napi_env env, napi_callback_info info);

// ==== Route B: ArkMesh Go 核心 (libarkmesh_core.so) ====

// 独立 dlopen libarkmesh_core.so 并跑一遍密码学/协议自检。
// 异步返回 Promise<string>, 探针跑在 NAPI worker 线程, 不阻塞 UI
napi_value CoreSelfTest(napi_env env, napi_callback_info info);

// (host) 检测 DNS / TCP / TLS / HTTPS 到控制面的联通性。
// 异步返回 Promise<string>, 探针跑在 NAPI worker 线程, 不阻塞 UI
napi_value CoreNetCheck(napi_env env, napi_callback_info info);

// 探测本机接口枚举的可用原语 (netlink/ioctl/proc/libc), 用于诊断沙箱限制。
// 异步返回 Promise<string>, 探针跑在 NAPI worker 线程, 不阻塞 UI
napi_value CoreIfProbe(napi_env env, napi_callback_info info);

// (controlURL, authKey, hostname, stateDir) 在后台启动 tsnet 接入控制面; 返回空串表示已启动
napi_value CoreControlUp(napi_env env, napi_callback_info info);

// 返回 JSON: {state, backendState, nodeIPs, dnsName, peers, peersOnline, health, error, elapsedMs, logs}
napi_value CoreControlStatus(napi_env env, napi_callback_info info);

// (type) 后台对在线 peer 发隧道内 ping; type 取 "TSMP"/"ICMP"/"disco"。
// 立即返回, 结果从 CoreControlStatus 的 pings / pingBusy 字段读
napi_value CorePingPeers(napi_env env, napi_callback_info info);

// (name, timeoutMs) 在隧道内注入合成 MagicDNS 查询, 同步返回一行解析结果。
// name 为空表示自动挑目标(本机 DNSName + 第一个在线 peer)。必须在 state=="up" 后调。
napi_value CoreDnsProbe(napi_env env, napi_callback_info info);

// 关闭控制面连接
napi_value CoreControlDown(napi_env env, napi_callback_info info);

// (fd, mtu) 把 VpnExtensionAbility 建好的 VPN fd 挂到控制面上, 带 TUN 重建 tsnet;
// 返回空串表示已受理, 之后用 CoreControlStatus 轮询到 state=="up"
napi_value CoreAttachTun(napi_env env, napi_callback_info info);

} // namespace gobridge

#endif // ARKMESH_GO_PROBE_H
