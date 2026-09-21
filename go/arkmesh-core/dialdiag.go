package main

import (
	"bufio"
	"context"
	"crypto/tls"
	"encoding/base64"
	"encoding/json"
	"fmt"
	"net"
	"net/http"
	"strings"
	"syscall"
	"time"

	"golang.org/x/sys/unix"
	"tailscale.com/control/controlbase"
	"tailscale.com/net/netmon"
	"tailscale.com/net/netns"
	"tailscale.com/tailcfg"
	"tailscale.com/types/key"
)

// netns_linux.go 会给控制面/日志上传用的每个 socket 设 SO_BINDTODEVICE,
// 设备名取 netmon 的默认路由, 取不到就退回 "lo". 沙箱里 netlink 被禁,
// netmon 拿不到默认路由, 一旦真的绑上 lo/错误接口, SYN 就发不出去,
// 表现为 "connect: connection timed out", 然后 dnscache 才退回 bootstrapDNS.
// 这里把 bind 结果和拨号结果都原样打出来, 不做忽略。
func dialWithBind(ip, port, ifc string, timeout time.Duration) string {
	bindRes := "未绑定"
	d := &net.Dialer{Timeout: timeout}
	if ifc != "" {
		d.Control = func(network, address string, c syscall.RawConn) error {
			cerr := c.Control(func(fd uintptr) {
				if e := unix.SetsockoptString(int(fd), unix.SOL_SOCKET, unix.SO_BINDTODEVICE, ifc); e != nil {
					bindRes = "bind(" + ifc + ") FAIL: " + e.Error()
					return
				}
				bindRes = "bind(" + ifc + ") ok"
			})
			if cerr != nil && bindRes == "未绑定" {
				bindRes = "Control FAIL: " + cerr.Error()
			}
			return nil
		}
	}
	start := time.Now()
	conn, err := d.Dial("tcp", net.JoinHostPort(ip, port))
	el := time.Since(start).Milliseconds()
	if err != nil {
		return fmt.Sprintf("%s -> FAIL(%dms): %v", bindRes, el, err)
	}
	local := conn.LocalAddr().String()
	conn.Close()
	return fmt.Sprintf("%s -> ok(%dms) local=%s", bindRes, el, local)
}

func diagnoseDialPath(host, port string) []string {
	out := []string{fmt.Sprintf("SO_MARK 可用: %v", netns.UseSocketMark())}
	ifc, err := netmon.DefaultRouteInterface()
	if err != nil {
		out = append(out, fmt.Sprintf("netmon 默认路由: FAIL(%v) => netns 会绑 lo", err))
		ifc = "lo"
	} else {
		out = append(out, "netmon 默认路由: "+ifc)
	}
	ips, err := net.LookupHost(host)
	if err != nil {
		return append(out, "解析失败: "+err.Error())
	}
	out = append(out, fmt.Sprintf("解析 %s -> %v", host, ips))
	for _, ip := range ips {
		out = append(out, "裸拨 "+ip+":"+port+": "+dialWithBind(ip, port, "", 5*time.Second))
	}
	for _, ip := range ips {
		out = append(out, "netns 绑 "+ip+":"+port+": "+dialWithBind(ip, port, ifc, 5*time.Second))
	}
	// 控制面 Noise 默认先试 80 端口, 单独看一眼 80 通不通
	if port != "80" {
		for _, ip := range ips {
			out = append(out, "裸拨 "+ip+":80: "+dialWithBind(ip, "80", "", 5*time.Second))
		}
	}
	return out
}

// probeInitB64 是 X-Tailscale-Handshake 那串 base64(真实客户端放的是 Noise msg1)。
// 服务端在跑 Noise 之前就先把 101 写回给客户端, 所以随便给一段合法 base64
// (64 个 'A' 解码成 48 个零字节)就够看清 HTTP 层通不通。
const probeInitB64 = "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"

// rawHTTPHead 手写一个 HTTP/1.1 请求发出去, 只读响应头, 用来观察链路上谁把请求掐了。
func rawHTTPHead(ip, sni, port, method, path string, hdr [][2]string) string {
	conn, err := tls.DialWithDialer(&net.Dialer{Timeout: 8 * time.Second},
		"tcp", net.JoinHostPort(ip, port), &tls.Config{
			ServerName: sni,
			// controlhttp 就是这么关 HTTP/2 的: 101 升级只在 HTTP/1.1 上有意义
			NextProtos: []string{},
			// 真实客户端对证书错误也是"只记日志、照常继续", 探针也不因它失败
			InsecureSkipVerify: true,
		})
	if err != nil {
		return "TLS FAIL: " + err.Error()
	}
	defer conn.Close()

	req := method + " " + path + " HTTP/1.1\r\n" +
		"Host: " + sni + "\r\n" +
		"User-Agent: Go-http-client/1.1\r\n"
	for _, kv := range hdr {
		req += kv[0] + ": " + kv[1] + "\r\n"
	}
	req += "\r\n"

	_ = conn.SetDeadline(time.Now().Add(12 * time.Second))
	if _, err := conn.Write([]byte(req)); err != nil {
		return "写请求 FAIL: " + err.Error()
	}

	br := bufio.NewReader(conn)
	line, err := br.ReadString('\n')
	if err != nil {
		return fmt.Sprintf("无响应(连状态行都没读到): %v [TLS=0x%x]", err, conn.ConnectionState().Version)
	}
	out := []string{strings.TrimRight(line, "\r\n")}
	for i := 0; i < 6; i++ {
		l, err := br.ReadString('\n')
		if err != nil {
			out = append(out, "…"+err.Error())
			break
		}
		l = strings.TrimRight(l, "\r\n")
		if l == "" {
			break
		}
		out = append(out, l)
	}
	return strings.Join(out, " | ")
}

// probeNoiseUpgrade 把控制面 Noise 升级那一步拆成几个可对比的请求, 尽量都打到同一个
// IP, 用来定位 "reading response header: EOF" 是谁造成的:
//   - GET /key:               同一台机器的普通 HTTPS(已知是通的)
//   - POST /ts2021 Upgrade=tailscale-control-protocol: 真实客户端走的那条
//   - POST /ts2021 Upgrade=websocket: 只换 Upgrade 值, 看中间层是否只放行 WebSocket
//   - 再来一发打到 baidu, 看本地网络是否对所有升级请求都动手
func probeNoiseUpgrade(host string) []string {
	ips, err := net.LookupHost(host)
	if err != nil || len(ips) == 0 {
		return []string{"解析 " + host + " 失败: " + err.Error()}
	}
	ip := ips[0]
	tsUpgrade := [][2]string{
		{"Content-Length", "0"},
		{"Upgrade", "tailscale-control-protocol"},
		{"Connection", "upgrade"},
		{"X-Tailscale-Handshake", probeInitB64},
	}
	wsUpgrade := [][2]string{
		{"Content-Length", "0"},
		{"Upgrade", "websocket"},
		{"Connection", "upgrade"},
	}
	return []string{
		fmt.Sprintf("GET /key %s(%s) -> %s", host, ip,
			rawHTTPHead(ip, host, "443", "GET", "/key", nil)),
		fmt.Sprintf("POST /ts2021 Upgrade=tailscale %s(%s) -> %s", host, ip,
			rawHTTPHead(ip, host, "443", "POST", "/ts2021", tsUpgrade)),
		fmt.Sprintf("POST /ts2021 Upgrade=websocket %s(%s) -> %s", host, ip,
			rawHTTPHead(ip, host, "443", "POST", "/ts2021", wsUpgrade)),
		"POST /ts2021 Upgrade=tailscale www.baidu.com -> " +
			rawHTTPHead("www.baidu.com", "www.baidu.com", "443", "POST", "/ts2021", tsUpgrade),
	}
}

// readHTTPHead 按字节读到响应头结束, 不能用 bufio: 紧随其后的 Noise 响应
// 可能已经被一并读进缓冲区, 那样后面交给 Noise 层就少字节了。
func readHTTPHead(conn net.Conn) (string, error) {
	var sb strings.Builder
	var buf [1]byte
	for sb.Len() < 8192 {
		if _, err := conn.Read(buf[:]); err != nil {
			return sb.String(), err
		}
		sb.WriteByte(buf[0])
		if strings.HasSuffix(sb.String(), "\r\n\r\n") {
			return sb.String(), nil
		}
	}
	return sb.String(), fmt.Errorf("响应头超过 8KB")
}

// noiseHandshakeProbe 是决定性的探针: 用真实客户端一模一样的方式(init 塞在
// X-Tailscale-Handshake 头里)跟控制面做一次完整的 Noise IK 握手。HTTP 层已经
// 验证能拿到 101, 所以这一步失败, 问题就在 Noise 层(密钥不匹配 / 服务端版本)。
func noiseHandshakeProbe(host string) string {
	keys, err := fetchServerKeys(host)
	if err != nil {
		return "取 /key FAIL: " + err.Error()
	}
	machineKey := key.NewMachine()
	init, cont, err := controlbase.ClientDeferred(machineKey, keys.PublicKey,
		uint16(tailcfg.CurrentCapabilityVersion))
	if err != nil {
		return "生成 init FAIL: " + err.Error()
	}

	ips, err := net.LookupHost(host)
	if err != nil || len(ips) == 0 {
		return "解析 FAIL: " + err.Error()
	}
	conn, err := tls.DialWithDialer(&net.Dialer{Timeout: 8 * time.Second},
		"tcp", net.JoinHostPort(ips[0], "443"), &tls.Config{
			ServerName:         host,
			NextProtos:         []string{},
			InsecureSkipVerify: true,
		})
	if err != nil {
		return "TLS FAIL: " + err.Error()
	}
	defer conn.Close()

	req := "POST /ts2021 HTTP/1.1\r\n" +
		"Host: " + host + "\r\n" +
		"User-Agent: Go-http-client/1.1\r\n" +
		"Content-Length: 0\r\n" +
		"Upgrade: tailscale-control-protocol\r\n" +
		"Connection: upgrade\r\n" +
		"X-Tailscale-Handshake: " + base64.StdEncoding.EncodeToString(init) + "\r\n\r\n"
	_ = conn.SetDeadline(time.Now().Add(20 * time.Second))
	if _, err := conn.Write([]byte(req)); err != nil {
		return "写升级请求 FAIL: " + err.Error()
	}

	head, err := readHTTPHead(conn)
	flat := strings.Join(strings.Fields(head), " ")
	if err != nil {
		return "读 101 FAIL: " + err.Error() + " | 已读到: " + flat
	}
	if !strings.Contains(head, "101") {
		return "升级被拒: " + flat
	}

	nc, err := cont(context.Background(), conn)
	if err != nil {
		return fmt.Sprintf("Noise 握手 FAIL: %v | 控制面密钥=%s | 升级响应: %s",
			err, keys.PublicKey.ShortString(), flat)
	}
	ver := nc.ProtocolVersion()
	nc.Close()
	return fmt.Sprintf("Noise 握手 ok, 协议版本=%d | 控制面密钥=%s | 升级响应: %s",
		ver, keys.PublicKey.ShortString(), flat)
}

// fetchServerKeys 复刻 controlclient.loadServerPubKeys: GET /key?v=<版本>。
// 走 CF 的话这里有可能拿到缓存住的旧密钥, 而 Noise 用的却是服务端当前密钥,
// 两边对不上握手就会失败 —— 所以把密钥本体也打出来比对。
func fetchServerKeys(host string) (*tailcfg.OverTLSPublicKeyResponse, error) {
	ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
	defer cancel()
	url := fmt.Sprintf("https://%s/key?v=%d", host, tailcfg.CurrentCapabilityVersion)
	req, err := http.NewRequestWithContext(ctx, "GET", url, nil)
	if err != nil {
		return nil, err
	}
	res, err := http.DefaultClient.Do(req)
	if err != nil {
		return nil, err
	}
	defer res.Body.Close()
	if res.StatusCode != 200 {
		return nil, fmt.Errorf("HTTP %d", res.StatusCode)
	}
	var out tailcfg.OverTLSPublicKeyResponse
	if err := json.NewDecoder(res.Body).Decode(&out); err != nil {
		return nil, err
	}
	return &out, nil
}
