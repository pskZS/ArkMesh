package main

import (
	"context"
	"crypto/tls"
	"crypto/x509"
	"fmt"
	"io"
	"net"
	"net/http"
	"os"
	"strings"
	"sync"
	"time"
)

func probeCACerts() string {
	pool, err := x509.SystemCertPool()
	if err != nil {
		return "systemCA FAIL: " + err.Error()
	}
	if pool == nil {
		return "systemCA FAIL: nil pool"
	}
	// 用已知的公共站点自签链做一次探测, 判断池子是否真的可用
	paths := []string{
		"/etc/ssl/certs/ca-certificates.crt",
		"/etc/ssl/cert.pem",
		"/system/etc/security/cacerts",
	}
	found := []string{}
	for _, p := range paths {
		if _, err := os.Stat(p); err == nil {
			found = append(found, p)
		}
	}
	if len(found) == 0 {
		// OHOS 不把 CA 放在这些 Linux 惯例路径下, SystemCertPool 读的是系统自己的
		// 信任存储 —— 池子加载成功即可用, 真正验收在后面 TLS 握手那一行。写成
		// "未发现文件路径"会被解析成异常行, 误导排查。
		return "systemCA ok: 池已加载(OHOS 不暴露惯例 CA 文件路径, 属预期)"
	}
	return "systemCA ok 发现: " + strings.Join(found, ",")
}

func probeResolvConf() string {
	b, err := os.ReadFile("/etc/resolv.conf")
	if err != nil {
		return "resolvConf 不可读: " + err.Error()
	}
	lines := []string{}
	for _, l := range strings.Split(string(b), "\n") {
		l = strings.TrimSpace(l)
		if l != "" && !strings.HasPrefix(l, "#") {
			lines = append(lines, l)
		}
	}
	if len(lines) == 0 {
		return "resolvConf 为空"
	}
	return "resolvConf ok: " + strings.Join(lines, "; ")
}

func probeDNS(host string, timeout time.Duration) string {
	start := time.Now()
	ctxDone := make(chan struct{})
	var addrs []string
	var err error
	go func() {
		addrs, err = net.LookupHost(host)
		close(ctxDone)
	}()
	select {
	case <-ctxDone:
	case <-time.After(timeout):
		return fmt.Sprintf("dns FAIL 超时(%s)", timeout)
	}
	el := time.Since(start).Milliseconds()
	if err != nil {
		return fmt.Sprintf("dns FAIL(%dms): %v", el, err)
	}
	return fmt.Sprintf("dns ok(%dms) %s -> %s", el, host, strings.Join(addrs, ","))
}

// probeDNSResolvers 对比两条解析路径: Go 自带解析器(dnscache 默认走的)与
// 系统解析器. OHOS 沙箱里前者会失败, 所以控制面必须切到后者.
func probeDNSResolvers(host string, timeout time.Duration) string {
	type result struct {
		ips []string
		err error
		ms  int64
	}
	lookup := func(r *net.Resolver) result {
		start := time.Now()
		ctx, cancel := context.WithTimeout(context.Background(), timeout)
		defer cancel()
		ips, err := r.LookupNetIP(ctx, "ip", host)
		out := make([]string, 0, len(ips))
		for _, ip := range ips {
			out = append(out, ip.String())
		}
		return result{out, err, time.Since(start).Milliseconds()}
	}

	var goRes, sysRes result
	var wg sync.WaitGroup
	wg.Add(2)
	go func() { defer wg.Done(); goRes = lookup(&net.Resolver{PreferGo: true}) }()
	go func() { defer wg.Done(); sysRes = lookup(&net.Resolver{PreferGo: false}) }()
	wg.Wait()

	format := func(name string, r result) string {
		if r.err != nil {
			return fmt.Sprintf("%s FAIL(%dms): %v", name, r.ms, r.err)
		}
		if len(r.ips) == 0 {
			return fmt.Sprintf("%s 空结果(%dms)", name, r.ms)
		}
		return fmt.Sprintf("%s ok(%dms) %v", name, r.ms, r.ips)
	}
	return "resolvers: " + format("go", goRes) + " | " + format("system", sysRes)
}

func probeTCP(host string, port string, timeout time.Duration) string {
	start := time.Now()
	conn, err := net.DialTimeout("tcp", net.JoinHostPort(host, port), timeout)
	if err != nil {
		return fmt.Sprintf("tcp:%s FAIL: %v", port, err)
	}
	defer conn.Close()
	local := conn.LocalAddr().String()
	return fmt.Sprintf("tcp:%s ok(%dms) local=%s", port, time.Since(start).Milliseconds(), local)
}

func probeTLSHandshake(host string, timeout time.Duration) string {
	d := &net.Dialer{Timeout: timeout}
	conn, err := tls.DialWithDialer(d, "tcp", net.JoinHostPort(host, "443"),
		&tls.Config{ServerName: host, MinVersion: tls.VersionTLS12})
	if err != nil {
		return "tls FAIL: " + err.Error()
	}
	defer conn.Close()
	st := conn.ConnectionState()
	if len(st.PeerCertificates) == 0 {
		return "tls FAIL: 无对端证书"
	}
	leaf := st.PeerCertificates[0]
	return fmt.Sprintf("tls ok %s %s issuer=%s 至%s",
		tls.VersionName(st.Version), tls.CipherSuiteName(st.CipherSuite),
		leaf.Issuer.CommonName, leaf.NotAfter.Format("2006-01-02"))
}

func probeHTTP(host string, timeout time.Duration) string {
	client := &http.Client{Timeout: timeout}
	resp, err := client.Get("https://" + host + "/")
	if err != nil {
		return "http FAIL: " + err.Error()
	}
	defer resp.Body.Close()
	body, _ := io.ReadAll(io.LimitReader(resp.Body, 256))
	snippet := strings.TrimSpace(string(body))
	if len(snippet) > 80 {
		snippet = snippet[:80]
	}
	snippet = strings.ReplaceAll(snippet, "\n", " ")
	return fmt.Sprintf("http ok %d %s body=%q", resp.StatusCode, resp.Header.Get("Server"), snippet)
}

// runNetCheck 只做与 headscale 联通相关的检查, 与密码学自检分开,
// 便于一眼看出失败是网络/信任链问题还是核心问题.
func runNetCheck(host string) []string {
	const t = 8 * time.Second
	return []string{
		probeCACerts(),
		probeResolvConf(),
		probeDNS(host, t),
		probeDNSResolvers(host, t),
		probeTCP(host, "443", t),
		probeTLSHandshake(host, t),
		probeHTTP(host, t),
	}
}
