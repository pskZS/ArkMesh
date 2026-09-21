package main

import (
	"errors"
	"fmt"
	"io"
	"net"
	"net/netip"
	"os"
	"sync"
	"sync/atomic"
	"syscall"
	"time"

	"github.com/tailscale/wireguard-go/tun"
)

// MagicDNS 诊断探针用到的常量。
const (
	// diagDNSPort 是探针查询包的源端口。真实手机流量不会用这个端口发 DNS,
	// 所以 Write 里看到目的端口 == diagDNSPort 的应答就一定是探针的回包。
	diagDNSPort = 53999
	// magicDNSAddr 是 tailscale 的 MagicDNS 解析器地址(quad-100)。
	magicDNSAddr = "100.100.100.100"
	// probePollInterval 是探针激活期间 Read 的轮询周期: 只在探针在跑的那几秒里
	// 生效, 平时 Read 依旧无限阻塞在 fd 上, 不产生任何额外唤醒。
	probePollInterval = 100 * time.Millisecond
)

// ohosTun 把 HarmonyOS VpnExtensionAbility 给的 VPN fd 包装成 wireguard-go 的
// tun.Device。
//
// 为什么不能用 wireguard-go 自带的 CreateTUNFromFile / CreateUnmonitoredTUNFromFD:
// 两者都要对 fd 做 TUNGETIFF 之类的 Linux tun ioctl, 还要建 netlink socket 监听
// 接口事件。OHOS 的 VPN fd 是 netmanager 服务给的普通字符 fd, 不是 /dev/tun 的
// 实例, 那些 ioctl 全部会失败, 沙箱里 netlink 也被禁。所以只能自己实现这 8 个方法。
//
// fd 上的报文是裸 IP 包(无 4 字节 PI 前缀), S5 spike 已实测: 写进去的
// IPv4+UDP 原样被内核接收, 读出来的首字节就是 IP version。
//
// fd 的生命周期归 ArkTS 侧的 vpnConnection.destroy() 所有, Close() 只负责让
// 读写两端停下来, 不关 fd —— 重复 close 一个 fd 号在多线程进程里可能撞上刚被
// 复用的新 fd。
type ohosTun struct {
	file   *os.File
	name   string
	mtu    int
	events chan tun.Event

	closed    atomic.Bool
	closeOnce sync.Once

	// 计数器用于判断"包到底有没有到 fd"。OHOS 的 VPN 是按 UID 抓流量的,
	// hdc shell 之类的进程可能根本不在 VPN 范围内, 光看 ping 通不通分不清是
	// 路由没进来还是隧道里被丢了。
	outPkts  atomic.Uint64 // Read: 手机 -> 隧道
	outBytes atomic.Uint64
	inPkts   atomic.Uint64 // Write: 隧道 -> 手机
	inBytes  atomic.Uint64
	readErrs atomic.Uint64

	// DNS 观测: dnsQ 是读到的 UDP:53 查询包(手机发往解析器), dnsR 是写回的
	// 应答包。MagicDNS 不通时靠这两个数区分"系统根本没把 DNS 送进隧道"和
	// "送进来了但 Go 侧没应答"。lastQ/lastR 记最近一包的域名和应答码, 随
	// stats() 进状态快照, 不用翻沙箱里的 Go 日志就能定位解析失败。
	dnsQ atomic.Uint64
	dnsR atomic.Uint64
	dnsMu sync.Mutex
	lastQ string
	lastR string

	// MagicDNS 诊断探针。probeArmed 只在探针运行的那几秒里为 true, 是 Read 走
	// "轮询 + 优先交付注入包"分支的唯一开关; 平时为 false, Read 的行为和没有探针
	// 时完全一致(无限阻塞在 fd 上, 由 Close 的 deadline 唤醒)。inj 是待注入的
	// 合成查询包队列, probeReply 承接 Write 捕获到的应答。
	probeArmed atomic.Bool
	inj        chan []byte
	probeReply chan dnsProbeResult
}

// dnsProbeResult 是探针捕获到的一条 DNS 应答的解析结果。
type dnsProbeResult struct {
	Rcode int      // DNS 响应码: 0=NOERROR, 3=NXDOMAIN, 2=SERVFAIL...
	AN    int      // 应答记录数
	Addrs []string // 解析出的 A/AAAA 地址
	Err   string   // 非空表示这条结果本身有问题(如应答过短)
}

// newOHOSTun 接管一个已经打开的 VPN fd。fd 必须是有效的非负整数。
//
// 返回具体类型而不是 tun.Device, 是为了让调用方还能读它的流量计数器。
func newOHOSTun(fd int, name string, mtu int) (*ohosTun, error) {
	if fd < 0 {
		return nil, errors.New("无效的 VPN fd")
	}
	if mtu <= 0 {
		mtu = 1420
	}

	// 显式置非阻塞: os.NewFile 会 fcntl(F_GETFL) 看 fd 当前是不是 O_NONBLOCK,
	// 只有非阻塞时才把它挂进 netpoller(否则每次 Read 都会独占一个 OS 线程死等)。
	// ArkTS 那边 VpnConfig.isBlocking 默认就是 false, 这里再兜一次底。
	if err := syscall.SetNonblock(fd, true); err != nil {
		return nil, err
	}

	f := os.NewFile(uintptr(fd), name)
	if f == nil {
		return nil, errors.New("os.NewFile 返回 nil")
	}

	t := &ohosTun{
		file:       f,
		name:       name,
		mtu:        mtu,
		events:     make(chan tun.Event, 1),
		inj:        make(chan []byte, 4),
		probeReply: make(chan dnsProbeResult, 1),
	}
	// 带缓冲且只发一次, 不会阻塞; 事件读端在 device.Close 时会退出。
	t.events <- tun.EventUp
	return t, nil
}

func (t *ohosTun) File() *os.File { return t.file }

func (t *ohosTun) Read(bufs [][]byte, sizes []int, offset int) (int, error) {
	if len(bufs) == 0 || len(sizes) == 0 {
		return 0, errors.New("Read: 空的 buffers")
	}
	for {
		if t.closed.Load() {
			return 0, os.ErrClosed
		}
		// 探针激活时才走这个分支: 优先把合成的 DNS 查询包当"手机流量"交付给引擎,
		// 并用短周期 deadline 兜底, 保证注入的包一定会被读到。probeArmed 平时为
		// false, 这段整体跳过, 常态读路径和没有探针时一模一样。
		if t.probeArmed.Load() {
			select {
			case pkt := <-t.inj:
				n := copy(bufs[0][offset:], pkt)
				sizes[0] = n
				t.outPkts.Add(1)
				t.outBytes.Add(uint64(n))
				t.observeDNS(bufs[0][offset:offset+n], true)
				return 1, nil
			default:
			}
			_ = t.file.SetReadDeadline(time.Now().Add(probePollInterval))
		}

		n, err := t.file.Read(bufs[0][offset:])
		if err != nil {
			// wireguard-go 的 TUN reader 把任何非 ErrTooManySegments 的错误都当成
			// 致命错误并 go device.Close(), 所以这里不能返回瞬时错误。netpoller 会
			// 把 EAGAIN 挡在下面, 能到这里的只有真关闭、真 IO 错误和 deadline 超时。
			if t.closed.Load() {
				return 0, os.ErrClosed
			}
			if isTimeoutErr(err) {
				// 只可能是探针的轮询 deadline 到期(或探针刚解除时的残留 deadline)。
				// 清掉已过期的 deadline 再重试, 否则会忙等。真关闭在上面已经拦下。
				_ = t.file.SetReadDeadline(time.Time{})
				continue
			}
			t.readErrs.Add(1)
			return 0, err
		}
		if t.probeArmed.Load() {
			_ = t.file.SetReadDeadline(time.Time{})
		}
		sizes[0] = n
		t.outPkts.Add(1)
		t.outBytes.Add(uint64(n))
		t.observeDNS(bufs[0][offset:offset+n], true)
		return 1, nil
	}
}

// isTimeoutErr 判断是不是 SetReadDeadline 触发的超时(而非真正的 IO 错误)。
func isTimeoutErr(err error) bool {
	var ne net.Error
	return errors.As(err, &ne) && ne.Timeout()
}

// Write 把解密后的包交给内核。
//
// offset == -1 是 wireguard-go 的关闭约定(见 tuntest.chTun.Write), 不是真实写入。
func (t *ohosTun) Write(bufs [][]byte, offset int) (int, error) {
	if offset == -1 {
		_ = t.Close()
		return 0, io.EOF
	}
	if t.closed.Load() {
		return 0, os.ErrClosed
	}

	for _, buf := range bufs {
		if len(buf) <= offset {
			continue
		}
		if _, err := t.file.Write(buf[offset:]); err != nil {
			return 0, err
		}
		t.inPkts.Add(1)
		t.inBytes.Add(uint64(len(buf) - offset))
		t.observeDNS(buf[offset:], false)
		t.captureProbeReply(buf[offset:])
	}
	return len(bufs), nil
}

// observeDNS 统计 UDP:53 包。query=true 表示手机发出的查询(看目的端口),
// 否则是写回手机的应答(看源端口)。顺带解析查询域名和应答码记进 lastQ/lastR,
// 够判断"系统把 DNS 发给了谁、解析的是哪个名字、应答是成功还是 NXDOMAIN/SERVFAIL"。
func (t *ohosTun) observeDNS(pkt []byte, query bool) {
	udp, ok := udpPayload(pkt)
	if !ok {
		return
	}
	port := uint16(udp[2])<<8 | uint16(udp[3]) // 目的端口
	if !query {
		port = uint16(udp[0])<<8 | uint16(udp[1]) // 应答包看源端口
	}
	if port != 53 {
		return
	}
	if query {
		t.dnsQ.Add(1)
		if len(udp) >= 20 {
			t.setLastQ(dnsQueryName(udp[20:]))
		}
	} else {
		t.dnsR.Add(1)
		if len(udp) >= 8+12 {
			hdr := udp[8:]
			t.setLastR(fmt.Sprintf("rc=%d an=%d", uint16(hdr[3])&0x0f,
				uint16(hdr[6])<<8|uint16(hdr[7])))
		}
	}
}

// udpPayload 从一个裸 IP 包里取出 UDP 段(含 8 字节 UDP 头)。支持 IPv4/IPv6,
// 非 UDP 或长度不足时返回 false。observeDNS 和探针回包捕获共用它。
func udpPayload(pkt []byte) ([]byte, bool) {
	switch {
	case len(pkt) >= 20 && pkt[0]>>4 == 4 && pkt[9] == 17:
		ihl := int(pkt[0]&0x0f) * 4
		if ihl < 20 || len(pkt) < ihl+8 {
			return nil, false
		}
		return pkt[ihl:], true
	case len(pkt) >= 48 && pkt[0]>>4 == 6 && pkt[6] == 17:
		if len(pkt) < 40+8 {
			return nil, false
		}
		return pkt[40:], true
	}
	return nil, false
}

func (t *ohosTun) setLastQ(q string) {
	t.dnsMu.Lock()
	t.lastQ = q
	t.dnsMu.Unlock()
}

func (t *ohosTun) setLastR(r string) {
	t.dnsMu.Lock()
	t.lastR = r
	t.dnsMu.Unlock()
}

// dnsQueryName 解 DNS 报文里的 QNAME(label 序列)。只取前 63 字节, 够认名字。
func dnsQueryName(p []byte) string {
	name := ""
	i := 0
	for i < len(p) {
		n := int(p[i])
		if n == 0 {
			break
		}
		if n > 63 || i+1+n > len(p) {
			break
		}
		if name != "" {
			name += "."
		}
		name += string(p[i+1 : i+1+n])
		i += 1 + n
		if len(name) > 63 {
			break
		}
	}
	return name
}

// captureProbeReply 在探针激活期间, 从写回手机的包里挑出目的端口 == diagDNSPort
// 的那条 DNS 应答, 解析后送进 probeReply。非探针期间直接返回, 不影响常态写路径。
func (t *ohosTun) captureProbeReply(pkt []byte) {
	if !t.probeArmed.Load() {
		return
	}
	udp, ok := udpPayload(pkt)
	if !ok || len(udp) < 8+12 {
		return
	}
	if uint16(udp[2])<<8|uint16(udp[3]) != diagDNSPort {
		return
	}
	res := parseDNSResponse(udp[8:])
	select {
	case t.probeReply <- res:
	default: // 已有一条待取的结果, 丢弃后来的
	}
}

// runDNSProbe 注入一条合成的 MagicDNS 查询并等待引擎的应答。
//
// srcIP 必须是本机在 tailnet 里的地址: 引擎的入站包过滤只认本机地址做源, 用别的
// 源地址会被 packet filter 丢掉, 探针就永远等不到回包。查询发往 quad-100:53,
// 由 tstun 里的 dnsmanager 就地应答(和真实手机 DNS 走的是同一条路径)。
func (t *ohosTun) runDNSProbe(srcIP netip.Addr, name string, timeout time.Duration) dnsProbeResult {
	pkt, err := buildDNSQuery(srcIP, name)
	if err != nil {
		return dnsProbeResult{Err: err.Error()}
	}
	// 清掉上一轮可能残留的应答
	select {
	case <-t.probeReply:
	default:
	}

	t.probeArmed.Store(true)
	defer t.probeArmed.Store(false)

	select {
	case t.inj <- pkt:
	default:
		return dnsProbeResult{Err: "注入队列满"}
	}
	// 唤醒可能正阻塞在 fd 上的 reader; 它醒来后会看到 probeArmed 并取走注入包。
	_ = t.file.SetReadDeadline(time.Now())

	select {
	case r := <-t.probeReply:
		return r
	case <-time.After(timeout):
		return dnsProbeResult{Err: "等待应答超时"}
	}
}

// buildDNSQuery 造一个 IPv4+UDP 的 DNS A/AAAA 查询包: srcIP:diagDNSPort ->
// 100.100.100.100:53。IPv4 UDP 校验和置 0(RFC 768 允许, 表示不校验), IP 头校验
// 和必须算对, 否则包会在进引擎前被丢。
func buildDNSQuery(srcIP netip.Addr, name string) ([]byte, error) {
	if !srcIP.Is4() {
		return nil, fmt.Errorf("探针只支持 IPv4 源地址, 得到 %s", srcIP)
	}
	dstIP, err := netip.ParseAddr(magicDNSAddr)
	if err != nil {
		return nil, err
	}
	msg := buildDNSMessage(name)

	udpLen := 8 + len(msg)
	udp := make([]byte, udpLen)
	putU16(udp[0:], diagDNSPort)
	putU16(udp[2:], 53)
	putU16(udp[4:], uint16(udpLen))
	putU16(udp[6:], 0) // checksum 占位, 稍后填
	copy(udp[8:], msg)
	putU16(udp[6:], udpChecksum(srcIP, dstIP, udp))

	ip := make([]byte, 20, 20+len(udp))
	ip[0] = 0x45 // version=4, IHL=5
	ip[1] = 0
	putU16(ip[2:], uint16(20+len(udp)))           // total length
	putU16(ip[4:], uint16(time.Now().UnixNano())) // identification
	putU16(ip[6:], 0)                             // flags + fragment offset
	ip[8] = 64                                    // TTL
	ip[9] = 17                                    // protocol = UDP
	putU16(ip[10:], 0) // checksum, 稍后填
	copy(ip[12:16], srcIP.AsSlice())
	copy(ip[16:20], dstIP.AsSlice())
	putU16(ip[10:], ipChecksum(ip))

	return append(ip, udp...), nil
}

// udpChecksum 按 RFC 768 计算带伪头部的 UDP 校验和。
// 不能省: 校验和置 0 在 IPv4 里虽合法, 但引擎的入站解析会按"校验和错误"丢掉,
// 真机实测置 0 时 dnsmanager 完全不回包。
func udpChecksum(src, dst netip.Addr, udp []byte) uint16 {
	var sum uint32
	sb := src.AsSlice()
	db := dst.AsSlice()
	for i := 0; i+1 < len(sb); i += 2 {
		sum += uint32(sb[i])<<8 | uint32(sb[i+1])
		sum += uint32(db[i])<<8 | uint32(db[i+1])
	}
	sum += 17                    // protocol
	sum += uint32(len(udp))      // udp length
	for i := 0; i+1 < len(udp); i += 2 {
		sum += uint32(udp[i])<<8 | uint32(udp[i+1])
	}
	if len(udp)%2 == 1 {
		sum += uint32(udp[len(udp)-1]) << 8
	}
	for sum>>16 != 0 {
		sum = (sum & 0xffff) + sum>>16
	}
	c := ^uint16(sum)
	if c == 0 {
		c = 0xffff // 0 表示"无校验和", 这里要避开
	}
	return c
}

// buildDNSMessage 造 DNS 报文: 12 字节头(RD=1, QDCOUNT=1) + QNAME + A/IN。
// 只查 A 记录就够判断"能不能解析到地址"; MagicDNS 对 mesh 名会回 A。
func buildDNSMessage(name string) []byte {
	var m []byte
	id := uint16(time.Now().UnixNano())
	m = append(m, byte(id>>8), byte(id))
	m = append(m, 0x01, 0x00) // flags: RD=1
	m = append(m, 0x00, 0x01) // QDCOUNT=1
	m = append(m, 0x00, 0x00) // ANCOUNT
	m = append(m, 0x00, 0x00) // NSCOUNT
	m = append(m, 0x00, 0x00) // ARCOUNT
	for _, label := range splitDNSName(name) {
		m = append(m, byte(len(label)))
		m = append(m, label...)
	}
	m = append(m, 0x00)       // QNAME 结束
	m = append(m, 0x00, 0x01) // QTYPE = A
	m = append(m, 0x00, 0x01) // QCLASS = IN
	return m
}

// splitDNSName 把域名切成 label, 忽略末尾的点。
func splitDNSName(name string) []string {
	if len(name) > 0 && name[len(name)-1] == '.' {
		name = name[:len(name)-1]
	}
	if name == "" {
		return nil
	}
	out := []string{}
	cur := ""
	for i := 0; i < len(name); i++ {
		if name[i] == '.' {
			out = append(out, cur)
			cur = ""
			continue
		}
		cur += string(name[i])
	}
	out = append(out, cur)
	return out
}

// parseDNSResponse 解析 DNS 应答: 取 rcode/ancount, 跳过问题段, 再从应答段里
// 收集 A(type=1)/AAAA(type=28) 地址。能处理名字压缩指针。
func parseDNSResponse(msg []byte) dnsProbeResult {
	if len(msg) < 12 {
		return dnsProbeResult{Err: "应答过短"}
	}
	res := dnsProbeResult{
		Rcode: int(msg[3] & 0x0f),
		AN:    int(msg[6])<<8 | int(msg[7]),
	}
	off := 12
	off = dnsSkipName(msg, off) // 问题段 QNAME
	off += 4                    // QTYPE + QCLASS
	for i := 0; i < res.AN && off < len(msg); i++ {
		off = dnsSkipName(msg, off) // 应答记录 NAME
		if off+10 > len(msg) {
			break
		}
		rtype := int(msg[off])<<8 | int(msg[off+1])
		rdlen := int(msg[off+8])<<8 | int(msg[off+9])
		off += 10
		if off+rdlen > len(msg) {
			break
		}
		rd := msg[off : off+rdlen]
		switch {
		case rtype == 1 && rdlen == 4:
			res.Addrs = append(res.Addrs, fmt.Sprintf("%d.%d.%d.%d", rd[0], rd[1], rd[2], rd[3]))
		case rtype == 28 && rdlen == 16:
			var b [16]byte
			copy(b[:], rd)
			res.Addrs = append(res.Addrs, netip.AddrFrom16(b).Unmap().String())
		}
		off += rdlen
	}
	return res
}

// dnsSkipName 跳过一个 DNS 名字(可能是 label 序列, 也可能是 2 字节压缩指针),
// 返回名字之后的偏移。
func dnsSkipName(msg []byte, off int) int {
	for off < len(msg) {
		n := int(msg[off])
		if n == 0 {
			return off + 1
		}
		if n&0xc0 == 0xc0 { // 压缩指针
			return off + 2
		}
		off += 1 + n
	}
	return off
}

// ipChecksum 算 IPv4 头校验和(RFC 1071 反码求和)。
func ipChecksum(b []byte) uint16 {
	var sum uint32
	for i := 0; i+1 < len(b); i += 2 {
		sum += uint32(b[i])<<8 | uint32(b[i+1])
	}
	if len(b)%2 == 1 {
		sum += uint32(b[len(b)-1]) << 8
	}
	for sum>>16 != 0 {
		sum = (sum & 0xffff) + sum>>16
	}
	return ^uint16(sum)
}

func putU16(b []byte, v uint16) {
	b[0] = byte(v >> 8)
	b[1] = byte(v)
}

func (t *ohosTun) MTU() (int, error) { return t.mtu, nil }

func (t *ohosTun) Name() (string, error) { return t.name, nil }

func (t *ohosTun) Events() <-chan tun.Event { return t.events }

// BatchSize 返回 1: 一次 Read 只交付一个包, 不做 readv 批量。
// 代价是没有 GRO, 高吞吐场景 syscall 数偏多; 换来的是不依赖任何 OHOS 上没有的
// 批量 IO 原语, 先保证正确性。
func (t *ohosTun) BatchSize() int { return 1 }

// stats 返回一行可读的流量计数, 供 controlStatus 回传给 ArkTS。
func (t *ohosTun) stats() string {
	q := ""
	r := ""
	t.dnsMu.Lock()
	q = t.lastQ
	r = t.lastR
	t.dnsMu.Unlock()
	return fmt.Sprintf("out=%d包/%dB in=%d包/%dB readErr=%d dnsQ=%d dnsR=%d lastQ=%s lastR=%s",
		t.outPkts.Load(), t.outBytes.Load(),
		t.inPkts.Load(), t.inBytes.Load(),
		t.readErrs.Load(), t.dnsQ.Load(), t.dnsR.Load(), q, r)
}

func (t *ohosTun) Close() error {
	var err error
	t.closeOnce.Do(func() {
		t.closed.Store(true)
		close(t.events)
		// 用读超时把阻塞在 Read 里的 goroutine 唤醒; 它醒来后会看到 closed
		// 并返回 os.ErrClosed。这里不关 fd, 理由见类型注释。
		err = t.file.SetReadDeadline(time.Now())
	})
	return err
}
