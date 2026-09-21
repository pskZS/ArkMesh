package main

import (
	"context"
	"encoding/json"
	"fmt"
	"log"
	"net"
	"net/netip"
	"net/url"
	"os"
	"runtime"
	"strings"
	"sync"
	"time"

	"tailscale.com/envknob"
	"tailscale.com/ipn"
	"tailscale.com/logtail"
	"tailscale.com/net/dnscache"
	"tailscale.com/net/netns"
	"tailscale.com/tailcfg"
	"tailscale.com/tsnet"
)

type controlStatus struct {
	State     string        `json:"state"`
	Backend   string        `json:"backendState,omitempty"`
	NodeIPs   []string      `json:"nodeIPs,omitempty"`
	HostName  string        `json:"hostName,omitempty"`
	DNSName   string        `json:"dnsName,omitempty"`
	MagicDNS  string        `json:"magicDNSSuffix,omitempty"`
	Peers     int           `json:"peers"`
	Online    int           `json:"peersOnline"`
	PeerList  []peerBrief   `json:"peerList,omitempty"`
	ExitWant  string        `json:"exitNodeWant,omitempty"`
	ExitNode  string        `json:"exitNode,omitempty"`
	ExitNodes []exitNodeOpt `json:"exitNodes,omitempty"`
	TunFD     int           `json:"tunFd"`
	TunStats  string        `json:"tunStats,omitempty"`
	PingBusy  bool          `json:"pingBusy"`
	Pings     []pingOutcome `json:"pings,omitempty"`
	PathPings []pingOutcome `json:"pathPings,omitempty"`
	Health    []string      `json:"health,omitempty"`
	Error     string        `json:"error,omitempty"`
	ElapsedMS int64         `json:"elapsedMs"`
	Logs      []string      `json:"logs,omitempty"`
}

const controlLogKeep = 120

// 隧道内 ping 的上限: 每次最长 pingTimeout, 而 tailnet 里可能有十几个节点,
// 全 ping 一遍既慢又没必要 —— 只要有一两个通就证明数据面是好的。
const (
	maxPingPeers = 3
	pingTimeout  = 6 * time.Second
)

// peerBrief 是 status 里回传给 ArkTS 的 peer 摘要, 够画列表也够做连通性诊断。
type peerBrief struct {
	IP     string `json:"ip"`
	Name   string `json:"name,omitempty"`
	Online bool   `json:"online"`
}

// exitNodeOpt 是 status 里回传的"可以当出口节点"的 peer(服务端已批准其广播
// 0.0.0.0/0), ArkTS 用它画选择列表。Active 表示它正是当前生效的那个。
type exitNodeOpt struct {
	IP     string `json:"ip"`
	Name   string `json:"name,omitempty"`
	Online bool   `json:"online"`
	Active bool   `json:"active"`
}

// pingOutcome 是 controlPing 的回传结构, ArkTS 侧直接 JSON.parse。
type pingOutcome struct {
	Target    string `json:"target"`
	Type      string `json:"type"`
	OK        bool   `json:"ok"`
	LatencyMS int64  `json:"latencyMs"`
	NodeName  string `json:"nodeName,omitempty"`
	Via       string `json:"via,omitempty"`
	Error     string `json:"error,omitempty"`
}

// controlParams 记住最近一次 controlUp 的参数。挂接 TUN 时要重建 tsnet,
// 需要原样复用这些参数(节点密钥在 stateDir 里, 重建后身份和 IP 都不变)。
type controlParams struct {
	controlURL string
	authKey    string
	hostname   string
	stateDir   string
	exitNode   string
}

var (
	ctlMu      sync.Mutex
	ctlSrv     *tsnet.Server
	ctlState   = "idle"
	ctlErr     string
	ctlStarted time.Time
	ctlLogs    []string
	ctlParams  controlParams
	ctlTunFD   = -1
	ctlTun     *ohosTun

	ctlPings    []pingOutcome
	ctlPingBusy bool

	// disco ping 的路径结果(直连端点 / DERP 中继)。TSMP 响应不带路径信息,
	// 只有 disco 才给, 所以单独跑一轮、单独存放, 不污染数据面验证用的 ctlPings。
	ctlPathPings []pingOutcome
)

func coreLog(format string, args ...any) {
	line := fmt.Sprintf(format, args...)
	// stderr 已被 C++ 侧重定向到应用沙箱内的 go_dbg.log
	log.Printf("[core] %s", line)

	ctlMu.Lock()
	ctlLogs = append(ctlLogs, line)
	if len(ctlLogs) > controlLogKeep {
		ctlLogs = ctlLogs[len(ctlLogs)-controlLogKeep:]
	}
	ctlMu.Unlock()
}

// controlUp 启动 tsnet 并返回。真正的连接过程在后台 goroutine 里推进,
// 调用方通过 controlStatus 轮询结果, 避免阻塞 UI 线程。
//
// exitNode 非空时表示"这次要经由该出口节点上网"(IPv4/IPv6 地址字面量), 空串表示
// 只连 tailnet 内部。
func controlUp(controlURL, authKey, hostname, stateDir, exitNode string) error {
	if exitNode != "" {
		if _, err := netip.ParseAddr(exitNode); err != nil {
			return fmt.Errorf("出口节点地址 %q 无效: %w", exitNode, err)
		}
	}

	ctlMu.Lock()
	if ctlState == "starting" || ctlState == "up" || ctlState == "reattaching" {
		ctlMu.Unlock()
		return fmt.Errorf("控制面已在运行 (state=%s)", ctlState)
	}
	ctlState = "starting"
	ctlErr = ""
	ctlStarted = time.Now()
	ctlLogs = nil
	ctlParams = controlParams{
		controlURL: controlURL,
		authKey:    authKey,
		hostname:   hostname,
		stateDir:   stateDir,
		exitNode:   exitNode,
	}
	ctlTunFD = -1
	ctlTun = nil
	ctlPings = nil
	ctlPathPings = nil
	ctlMu.Unlock()

	coreLog("启动 tsnet: control=%s host=%s dir=%s authKey=%s exitNode=%s",
		controlURL, hostname, stateDir, maskSecret(authKey), orNone(exitNode))
	coreLog("GOMAXPROCS=%d NumCPU=%d GODEBUG=%q",
		runtime.GOMAXPROCS(0), runtime.NumCPU(), os.Getenv("GODEBUG"))

	logInterfaceTable()
	prepareDNSAndLogs(stateDir)

	// 沙箱里 netns 的 SO_BINDTODEVICE 会绑错接口导致控制面 connect 超时,
	// 详见 dialdiag.go。netns 必须在这里同步关掉, 对比拨号耗时十几秒,
	// 只能丢到后台(调用方是扩展进程主线程, 阻塞久了会被系统按 lifecycle 超时杀掉)。
	netns.SetEnabled(false)
	coreLog("netns 已禁用: 不再对 socket 设 SO_BINDTODEVICE")
	go func() {
		ctlHost := controlHost(controlURL)
		for _, line := range diagnoseDialPath(ctlHost, "443") {
			coreLog("拨号诊断 %s", line)
		}
		// 拨号诊断最快也要十几秒(每个 5s 超时), 升级探针放在后面, 不抢它的带宽
		for _, line := range probeNoiseUpgrade(ctlHost) {
			coreLog("升级诊断 %s", line)
		}
		coreLog("Noise 诊断 %s", noiseHandshakeProbe(ctlHost))
	}()

	srv := &tsnet.Server{
		Dir:        stateDir,
		ControlURL: controlURL,
		AuthKey:    authKey,
		Hostname:   hostname,
		Logf:       func(format string, args ...any) { coreLog("tsnet: "+format, args...) },
		UserLogf:   func(format string, args ...any) { coreLog("user: "+format, args...) },
	}

	ctlMu.Lock()
	ctlSrv = srv
	ctlMu.Unlock()

	go bringUp(srv, exitNode)

	return nil
}

// bringUp 在后台跑 srv.Up, 结果写回 ctlState/ctlErr。
// 调用方(扩展进程主线程)不能阻塞: 阻塞久了会被系统按 lifecycle 超时杀掉。
func bringUp(srv *tsnet.Server, exitNode string) {
	ctx, cancel := context.WithTimeout(context.Background(), 90*time.Second)
	defer cancel()

	st, err := srv.Up(ctx)
	if err != nil {
		coreLog("Up 失败: %v", err)
		ctlMu.Lock()
		if ctlSrv == srv {
			ctlState = "failed"
			ctlErr = err.Error()
		}
		ctlMu.Unlock()
		return
	}

	// 出口节点必须在 state 变 up 之前就位: ArkTS 一看到 up 就会去建 VPN, 而建
	// 不建默认路由完全取决于"这次有没有出口节点"。要是这里只记个警告就放行,
	// OHOS 已经把全部流量灌进 TUN 了, 而 wgengine 里没有任何 peer 的 AllowedIPs
	// 覆盖 0.0.0.0/0, 结果就是整机断网且界面上还显示"已连接"。
	if exitNode != "" {
		if err := applyExitNode(srv, exitNode); err != nil {
			coreLog("设置出口节点 %s 失败: %v", exitNode, err)
			ctlMu.Lock()
			if ctlSrv == srv {
				ctlState = "failed"
				ctlErr = fmt.Sprintf("出口节点 %s 设置失败: %v", exitNode, err)
			}
			ctlMu.Unlock()
			return
		}
	}

	ips := make([]string, 0, len(st.TailscaleIPs))
	for _, ip := range st.TailscaleIPs {
		ips = append(ips, ip.String())
	}
	coreLog("已连接: state=%s ips=%v magicDNS=%s peers=%d exitNode=%s",
		st.BackendState, ips, st.MagicDNSSuffix, len(st.Peer), orNone(exitNode))
	for _, h := range st.Health {
		coreLog("健康告警: %s", h)
	}

	ctlMu.Lock()
	if ctlSrv == srv {
		ctlState = "up"
	}
	ctlMu.Unlock()
}

// applyExitNode 通过 LocalAPI 把出口节点写进当前 profile 的 prefs。
//
// 传 IP 而不是 StableNodeID: ID 是 headscale 内部的编号, ArkTS 那边只看得到 IP。
// ipnlocal 会在 netmap 里按地址查到节点并自动把 ExitNodeIP 升级成 ExitNodeID
// (resolveExitNodeIPLocked), 所以给 IP 就够了。
//
// 每次 srv.Up 之后都得重设一遍: tsnet 启动时用 ipn.NewPrefs() 整体覆盖 profile
// 的 prefs(tsnet.go 的 lb.Start(ipn.Options{UpdatePrefs: prefs})), 上一次设的
// 出口节点不会被保留。
func applyExitNode(srv *tsnet.Server, ip string) error {
	addr, err := netip.ParseAddr(ip)
	if err != nil {
		return fmt.Errorf("地址无效: %w", err)
	}
	ctx, cancel := context.WithTimeout(context.Background(), 20*time.Second)
	defer cancel()
	lc, err := srv.LocalClient()
	if err != nil {
		return err
	}
	prefs, err := lc.EditPrefs(ctx, &ipn.MaskedPrefs{
		Prefs:         ipn.Prefs{ExitNodeIP: addr},
		ExitNodeIPSet: true,
	})
	if err != nil {
		return err
	}
	coreLog("出口节点已写入 prefs: want=%s -> exitID=%q exitIP=%v lanAccess=%v",
		ip, prefs.ExitNodeID, prefs.ExitNodeIP, prefs.ExitNodeAllowLANAccess)
	return nil
}

// orNone 让日志里的空串不至于看起来像漏打了参数。
func orNone(s string) string {
	if s == "" {
		return "(无)"
	}
	return s
}

func firstV4(addrs []netip.Addr) (netip.Addr, bool) {
	for _, a := range addrs {
		if a.Is4() {
			return a, true
		}
	}
	return netip.Addr{}, false
}

// controlAttachTun 把一个已经建好的 HarmonyOS VPN fd 挂到控制面上, 让设备流量
// 真正走隧道。
//
// 为什么要"重建"而不是直接塞进去: tsnet.Server.Tun 的文档明确要求在 Start 之前
// 设置, 没有运行中替换的入口。而 VPN 接口地址必须等于 headscale 分配给我们的
// Tailscale IP(否则隧道里回来的包目的地址不属于本机, 内核直接丢), 那个 IP 只有
// 注册完才知道。所以顺序只能是:
//
//  1. ArkMeshControlUp      —— 无 TUN 的 netstack 模式注册, 拿到 nodeIPs
//  2. ArkTS 用 nodeIPs 建 VPN, 拿到 fd
//  3. ArkMeshAttachTun(fd)  —— 关掉旧的, 带 TUN 重新 Up
//
// 节点密钥在 stateDir 里, 重建后 headscale 认得同一个节点, IP 不变。
func controlAttachTun(fd, mtu int) error {
	if fd < 0 {
		return fmt.Errorf("无效的 VPN fd=%d", fd)
	}

	ctlMu.Lock()
	p := ctlParams
	old := ctlSrv
	state := ctlState
	if p.controlURL == "" {
		ctlMu.Unlock()
		return fmt.Errorf("控制面从未启动过, 先调 ArkMeshControlUp")
	}
	if state != "up" && state != "starting" && state != "reattaching" {
		ctlMu.Unlock()
		return fmt.Errorf("控制面状态为 %q, 无法挂接 TUN", state)
	}
	// 先摘掉 ctlSrv, 免得并发的 status 查询打到正在关闭的 server 上。
	ctlSrv = nil
	ctlState = "reattaching"
	ctlErr = ""
	ctlStarted = time.Now()
	ctlTunFD = fd
	ctlTun = nil
	ctlPings = nil
	ctlPathPings = nil
	ctlMu.Unlock()

	go func() {
		if old != nil {
			coreLog("挂接 TUN fd=%d mtu=%d: 关闭旧的无 TUN tsnet", fd, mtu)
			if err := old.Close(); err != nil {
				coreLog("旧 tsnet 关闭返回错误(继续): %v", err)
			}
		}

		// 必须在旧 server 关完之后才建 tun.Device: 关 engine 会连带关掉它的
		// tundev, 提前建好的话新 engine 拿到的就是个已关闭的设备。
		tdev, err := newOHOSTun(fd, "tun0", mtu)
		if err != nil {
			coreLog("包装 VPN fd 失败: %v", err)
			ctlMu.Lock()
			ctlState = "failed"
			ctlErr = "tun: " + err.Error()
			ctlTunFD = -1
			ctlTun = nil
			ctlMu.Unlock()
			return
		}
		coreLog("VPN fd 已包装成 tun.Device, 带 TUN 重启 tsnet")

		ctlMu.Lock()
		ctlTun = tdev
		ctlMu.Unlock()

		srv := &tsnet.Server{
			Dir:        p.stateDir,
			ControlURL: p.controlURL,
			AuthKey:    p.authKey,
			Hostname:   p.hostname,
			Tun:        tdev,
			Logf:       func(format string, args ...any) { coreLog("tsnet: "+format, args...) },
			UserLogf:   func(format string, args ...any) { coreLog("user: "+format, args...) },
		}

		ctlMu.Lock()
		ctlSrv = srv
		ctlMu.Unlock()

		bringUp(srv, p.exitNode)
	}()

	return nil
}

func controlStatusJSON() string {
	ctlMu.Lock()
	srv := ctlSrv
	state := ctlState
	errMsg := ctlErr
	started := ctlStarted
	logs := append([]string(nil), ctlLogs...)
	tunFD := ctlTunFD
	tunDev := ctlTun
	pings := append([]pingOutcome(nil), ctlPings...)
	pathPings := append([]pingOutcome(nil), ctlPathPings...)
	pingBusy := ctlPingBusy
	exitWant := ctlParams.exitNode
	ctlMu.Unlock()

	out := controlStatus{
		State:     state,
		Error:     errMsg,
		Logs:      logs,
		TunFD:     tunFD,
		PingBusy:  pingBusy,
		Pings:     pings,
		PathPings: pathPings,
		ExitWant:  exitWant,
	}
	if tunDev != nil {
		out.TunStats = tunDev.stats()
	}
	if !started.IsZero() {
		out.ElapsedMS = time.Since(started).Milliseconds()
	}

	if srv != nil && state == "up" {
		ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
		defer cancel()
		if lc, err := srv.LocalClient(); err == nil {
			if st, err := lc.Status(ctx); err == nil {
				out.Backend = st.BackendState
				out.MagicDNS = st.MagicDNSSuffix
				out.Health = st.Health
				out.Peers = len(st.Peer)
				for _, p := range st.Peer {
					if p.Online {
						out.Online++
					}
					for _, ip := range p.TailscaleIPs {
						out.PeerList = append(out.PeerList, peerBrief{
							IP:     ip.String(),
							Name:   p.HostName,
							Online: p.Online,
						})
					}
					// ExitNodeOption 是"服务端已批准它当出口节点", ExitNode 是
					// "它正是本机当前选中的那个"。候选列表只收 v4 地址: OHOS 的
					// 默认路由要按族分别配, 而 ArkTS 那边只拿一个 IP 去选。
					if p.ExitNodeOption {
						if v4, ok := firstV4(p.TailscaleIPs); ok {
							out.ExitNodes = append(out.ExitNodes, exitNodeOpt{
								IP:     v4.String(),
								Name:   p.HostName,
								Online: p.Online,
								Active: p.ExitNode,
							})
							if p.ExitNode {
								out.ExitNode = v4.String()
							}
						}
					}
				}
				if st.Self != nil {
					out.HostName = st.Self.HostName
					out.DNSName = st.Self.DNSName
					for _, ip := range st.Self.TailscaleIPs {
						out.NodeIPs = append(out.NodeIPs, ip.String())
					}
				}
			} else {
				out.Error = "Status: " + err.Error()
			}
		} else {
			out.Error = "LocalClient: " + err.Error()
		}
	}

	b, err := json.Marshal(&out)
	if err != nil {
		return `{"state":"error","error":"marshal 失败"}`
	}
	return string(b)
}

// pingType 把 ArkTS 传进来的字符串翻成 tailcfg.PingType, 认不出的一律按 TSMP。
func pingType(typ string) tailcfg.PingType {
	switch typ {
	case "disco":
		return tailcfg.PingDisco
	case "ICMP":
		return tailcfg.PingICMP
	default:
		return tailcfg.PingTSMP
	}
}

// pingOne 向单个 peer 发一次隧道内 ping。
//
// 为什么不能用系统 ping: OHOS 的 VPN 按 UID 抓流量, hdc shell 根本不在范围内;
// 而扩展进程自己又被 protectProcessNet 整体绕过了 VPN。两头都发不出"会进隧道"
// 的包。TSMP/ICMP ping 由 wgengine 直接注入加密通道(magicsock -> DERP 或直连
// -> 对端), 走的正是真实数据面, 所以能在本机独立验证, 不依赖第二台机器。
func pingOne(srv *tsnet.Server, ip netip.Addr, pt tailcfg.PingType, timeout time.Duration) pingOutcome {
	out := pingOutcome{Target: ip.String(), Type: string(pt)}

	ctx, cancel := context.WithTimeout(context.Background(), timeout)
	defer cancel()
	lc, err := srv.LocalClient()
	if err != nil {
		out.Error = "LocalClient: " + err.Error()
		return out
	}
	res, err := lc.Ping(ctx, ip, pt)
	if err != nil {
		out.Error = err.Error()
		return out
	}
	if res == nil {
		out.Error = "空响应"
		return out
	}

	out.NodeName = res.NodeName
	out.LatencyMS = int64(res.LatencySeconds * 1000)
	// TSMP ping 的响应里 Endpoint 和 DERPRegionID 都不填(tailscale 上游如此),
	// 所以走 TSMP 时 Via 一般是空的, 只有 disco/ICMP 才能看出走直连还是 DERP。
	switch {
	case res.Err != "":
		out.Error = res.Err
	case res.Endpoint != "":
		out.OK = true
		out.Via = "direct " + res.Endpoint
	case res.DERPRegionID != 0:
		out.OK = true
		out.Via = "derp " + res.DERPRegionCode
	default:
		out.OK = true
	}
	return out
}

// controlPingPeers 后台对最多 maxPingPeers 个在线 peer 各发一次隧道内 ping,
// 结果攒进 ctlPings, 由 controlStatusJSON 一起回传。
//
// 必须异步: 每次 ping 最长阻塞 pingTimeout, 而调用方是 VPN 扩展进程的主线程,
// 同步等待几个 peer 就是十几秒, 会被系统按 lifecycle 超时杀掉。
func controlPingPeers(typ string) error {
	pt := pingType(typ)

	ctlMu.Lock()
	srv := ctlSrv
	state := ctlState
	if state != "up" || srv == nil {
		ctlMu.Unlock()
		return fmt.Errorf("控制面状态为 %q, 无法 ping", state)
	}
	if ctlPingBusy {
		ctlMu.Unlock()
		return fmt.Errorf("上一轮 ping 还在跑")
	}
	ctlPingBusy = true
	ctlPings = nil
	ctlPathPings = nil
	ctlMu.Unlock()

	go func() {
		defer func() {
			ctlMu.Lock()
			ctlPingBusy = false
			ctlMu.Unlock()
		}()

		ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
		defer cancel()
		lc, err := srv.LocalClient()
		if err != nil {
			recordPing(pingOutcome{Type: string(pt), Error: "LocalClient: " + err.Error()})
			return
		}
		st, err := lc.Status(ctx)
		if err != nil {
			recordPing(pingOutcome{Type: string(pt), Error: "Status: " + err.Error()})
			return
		}

		n := 0
		for _, p := range st.Peer {
			if n >= maxPingPeers {
				break
			}
			if !p.Online || len(p.TailscaleIPs) == 0 {
				continue
			}
			n++
			out := pingOne(srv, p.TailscaleIPs[0], pt, pingTimeout)
			out.NodeName = p.HostName
			recordPing(out)
		}
		if n == 0 {
			recordPing(pingOutcome{Type: string(pt), Error: "没有在线 peer 可 ping"})
		}

		// 数据面验证完顺手探一轮路径: 直连端点 / DERP 中继只有 disco ping 才给
		// (见 pingOne 注释), 节点页靠它显示"直连还是中继"。独立 goroutine +
		// 独立存储, 不拖慢也不污染上面的连接判定。
		if pt == tailcfg.PingTSMP {
			go probePaths(srv)
		}
	}()

	return nil
}

// probePaths 对在线 peer 并发发 disco ping, 结果攒进 ctlPathPings。
// 并发而非串行: 每次最长 pingTimeout, 串行跑满几个 peer 要半分钟, 而路径信息
// 只是展示用, 连接后几秒内齐了最好。
func probePaths(srv *tsnet.Server) {
	ctx, cancel := context.WithTimeout(context.Background(), 12*time.Second)
	defer cancel()
	lc, err := srv.LocalClient()
	if err != nil {
		recordPathPing(pingOutcome{Type: "disco", Error: "LocalClient: " + err.Error()})
		return
	}
	st, err := lc.Status(ctx)
	if err != nil {
		recordPathPing(pingOutcome{Type: "disco", Error: "Status: " + err.Error()})
		return
	}

	ctlMu.Lock()
	ctlPathPings = nil
	ctlMu.Unlock()

	const maxPathPeers = 8
	var wg sync.WaitGroup
	n := 0
	for _, p := range st.Peer {
		if n >= maxPathPeers {
			break
		}
		if !p.Online || len(p.TailscaleIPs) == 0 {
			continue
		}
		n++
		wg.Add(1)
		go func(ip netip.Addr, name string) {
			defer wg.Done()
			out := pingOne(srv, ip, tailcfg.PingDisco, pingTimeout)
			out.NodeName = name
			recordPathPing(out)
		}(p.TailscaleIPs[0], p.HostName)
	}
	wg.Wait()
	if n == 0 {
		recordPathPing(pingOutcome{Type: "disco", Error: "没有在线 peer 可探路径"})
	}
}

func recordPathPing(out pingOutcome) {
	coreLog("path %s(%s): ok=%v %dms node=%s via=%s err=%s",
		out.Target, out.Type, out.OK, out.LatencyMS, out.NodeName, out.Via, out.Error)
	ctlMu.Lock()
	ctlPathPings = append(ctlPathPings, out)
	ctlMu.Unlock()
}

func recordPing(out pingOutcome) {
	coreLog("ping %s(%s): ok=%v %dms node=%s via=%s err=%s",
		out.Target, out.Type, out.OK, out.LatencyMS, out.NodeName, out.Via, out.Error)
	ctlMu.Lock()
	ctlPings = append(ctlPings, out)
	ctlMu.Unlock()
}

// controlDnsProbe 在隧道内注入合成的 MagicDNS 查询, 直接验证"mesh 名能不能解析"。
//
// 为什么不能用系统 DNS / Go 自己的解析器: 扩展进程整个被 VpnConfig.blockedApplications
// 排除在 VPN 之外(见 buildTunnelConfig), 它发的 DNS 根本不进隧道, 也就永远问不到
// MagicDNS(quad-100)。只有把查询包从 TUN 的读端"塞进去", 让它冒充手机流量, 才会
// 被引擎的 dnsmanager 当作真实查询处理并回包。这条路径和用户在浏览器里输 mesh 名
// 时走的是同一条, 所以结果可信。
//
// name 为空时自动挑目标: 本机 DNSName + 第一个在线 peer 的 DNSName。返回多行文本,
// 每行一个目标的解析结果(rc / A 记录 / 超时 / 错误)。
func controlDnsProbe(name string, timeout time.Duration) string {
	ctlMu.Lock()
	tun := ctlTun
	state := ctlState
	srv := ctlSrv
	ctlMu.Unlock()

	if tun == nil || state != "up" || srv == nil {
		return fmt.Sprintf("ERR 隧道未就绪(state=%s), 无法探针", state)
	}
	if timeout <= 0 {
		timeout = 3 * time.Second
	}

	ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
	defer cancel()
	lc, err := srv.LocalClient()
	if err != nil {
		return "ERR LocalClient: " + err.Error()
	}
	st, err := lc.Status(ctx)
	if err != nil {
		return "ERR Status: " + err.Error()
	}

	// 源地址必须是本机在 tailnet 里的 IPv4: 引擎入站过滤只认本机地址做源。
	var selfV4 netip.Addr
	var selfDNS string
	if st.Self != nil {
		selfDNS = st.Self.DNSName
		for _, ip := range st.Self.TailscaleIPs {
			if ip.Is4() {
				selfV4 = ip
				break
			}
		}
	}
	if !selfV4.IsValid() {
		return "ERR 本机没有 tailnet IPv4 地址, 探针无法构造源地址"
	}

	targets := []string{}
	if name != "" {
		targets = append(targets, name)
	} else {
		if selfDNS != "" {
			targets = append(targets, selfDNS)
		}
		for _, p := range st.Peer {
			if p.Online && p.DNSName != "" {
				targets = append(targets, p.DNSName)
				break
			}
		}
	}
	if len(targets) == 0 {
		return "ERR 没有可探测的名字(本机 DNSName 为空且无在线 peer)"
	}

	lines := []string{fmt.Sprintf("源=%s MagicDNS=%s", selfV4, magicDNSAddr)}
	for _, tgt := range targets {
		res := tun.runDNSProbe(selfV4, tgt, timeout)
		lines = append(lines, formatDNSProbe(tgt, res))
	}
	out := strings.Join(lines, " | ")
	coreLog("DNS 探针: %s", out)
	return out
}

// formatDNSProbe 把一条探针结果压成人能直接看懂的一行。
func formatDNSProbe(name string, r dnsProbeResult) string {
	if r.Err != "" {
		return fmt.Sprintf("%s -> %s", name, r.Err)
	}
	switch {
	case r.Rcode == 0 && len(r.Addrs) > 0:
		return fmt.Sprintf("%s -> OK %s", name, strings.Join(r.Addrs, ","))
	case r.Rcode == 0:
		return fmt.Sprintf("%s -> NOERROR 但无地址(an=%d)", name, r.AN)
	case r.Rcode == 3:
		return fmt.Sprintf("%s -> NXDOMAIN(名字不在 netmap, 多半是 ACL/未注册)", name)
	default:
		return fmt.Sprintf("%s -> rcode=%d an=%d", name, r.Rcode, r.AN)
	}
}

func controlDown() {
	ctlMu.Lock()
	srv := ctlSrv
	ctlSrv = nil
	ctlState = "idle"
	ctlErr = ""
	ctlTunFD = -1
	ctlTun = nil
	ctlPings = nil
	ctlPathPings = nil
	ctlMu.Unlock()
	if srv != nil {
		coreLog("关闭 tsnet")
		_ = srv.Close()
	}
}

// prepareDNSAndLogs 调整几处默认行为:
//  1. 控制面 DNS 改用系统解析器(cgo/getaddrinfo). tailscale 在 linux 默认用
//     Go 自带解析器, Android 端同样是关掉的, 这里保持一致.
//  2. logtail.Disable(): 自建 headscale 场景不需要往 log.tailscale.io 传日志.
//  3. TS_FORCE_NOISE_443: 控制面 Noise 通道默认先拨 80 端口(明文 HTTP 升级),
//     手机网络里 80 常被劫持/丢弃, 一慢就吃掉整个 10s 注册超时; 自建 headscale
//     一般走 443, 直接快路径.
//  4. TS_LOGS_DIR: release 签名的应用进程读不到 /proc/self/environ, 环境变量
//     视为空 → HOME 缺失会让 logpolicy.LogsDir 一路兜底到 MkdirTemp(/tmp 不可写)
//     然后 panic("no safe place found to store log state"). 显式指到 stateDir 绕开。
func prepareDNSAndLogs(stateDir string) {
	dnscache.Get().Forward = &net.Resolver{PreferGo: false}
	dnscache.SetDebugLoggingEnabled(true)
	logtail.Disable()
	envknob.Setenv("TS_FORCE_NOISE_443", "1")
	envknob.Setenv("TS_DEBUG_NOISE_DIAL", "1")
	envknob.Setenv("TS_LOGS_DIR", stateDir)
	coreLog("DNS 解析器已切换为系统解析器, logtail 已关闭, Noise 只走 443")
}

// controlHost 从控制面 URL 里取主机名, 用于拨号诊断
func controlHost(controlURL string) string {
	u, err := url.Parse(controlURL)
	if err != nil || u.Hostname() == "" {
		return controlURL
	}
	return u.Hostname()
}

// logInterfaceTable 注册 OHOS 版接口枚举器(沙箱禁 netlink, 标准库会失败),
// 并把结果写进状态日志, 方便真机上确认 magicsock 能拿到哪些本机地址。
func logInterfaceTable() {
	registerInterfaceGetter()
	list, err := ohosInterfaceList()
	if err != nil {
		coreLog("接口枚举失败: %v", err)
		return
	}
	coreLog("本机接口 %d 个:", len(list))
	for i, ni := range list {
		if i >= 8 {
			break
		}
		addrs, _ := ni.Addrs()
		coreLog("  %s idx=%d mtu=%d flags=%v addrs=%v",
			ni.Name, ni.Index, ni.MTU, ni.Flags, addrs)
	}
}

// maskSecret 保证密钥不会出现在日志/回传里
func maskSecret(s string) string {
	if s == "" {
		return "(空)"
	}
	if len(s) <= 8 {
		return "***"
	}
	return s[:4] + "***" + s[len(s)-4:]
}
