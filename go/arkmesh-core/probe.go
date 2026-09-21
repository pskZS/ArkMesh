package main

import (
	"crypto/rand"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"runtime"

	"golang.org/x/crypto/blake2s"
	"golang.org/x/crypto/chacha20poly1305"
	"golang.org/x/crypto/curve25519"

	"tailscale.com/tailcfg"
	"tailscale.com/tsnet"
	"tailscale.com/types/key"
	"tailscale.com/util/dnsname"
)

func probeRuntime() string {
	return fmt.Sprintf("go=%s os=%s arch=%s cpus=%d tsnet=%T",
		runtime.Version(), runtime.GOOS, runtime.GOARCH, runtime.NumCPU(), &tsnet.Server{})
}

// curve25519 在 arm64 上走汇编实现, 是 WireGuard 密钥交换的基础.
func probeCurve25519() string {
	var priv, base [32]byte
	priv[0] = 7
	base[0] = 9
	pub, err := curve25519.X25519(priv[:], base[:])
	if err != nil {
		return "curve25519 FAIL: " + err.Error()
	}
	if _, err := curve25519.X25519(priv[:], pub); err != nil {
		return "curve25519 FAIL(2): " + err.Error()
	}
	return fmt.Sprintf("curve25519 ok pub=%s", hex.EncodeToString(pub[:4]))
}

// ChaCha20-Poly1305: WireGuard 的载荷加密.
func probeAEAD() string {
	keyBytes := make([]byte, chacha20poly1305.KeySize)
	if _, err := rand.Read(keyBytes); err != nil {
		return "aead FAIL: " + err.Error()
	}
	aead, err := chacha20poly1305.New(keyBytes)
	if err != nil {
		return "aead FAIL(new): " + err.Error()
	}
	nonce := make([]byte, aead.NonceSize())
	plain := []byte("arkmesh-route-b")
	opened, err := aead.Open(nil, nonce, aead.Seal(nil, nonce, plain, nil), nil)
	if err != nil {
		return "aead FAIL(open): " + err.Error()
	}
	if string(opened) != string(plain) {
		return "aead FAIL(roundtrip)"
	}
	return "chacha20poly1305 ok"
}

// BLAKE2s: WireGuard 的 KDF.
func probeBlake2s() string {
	h, err := blake2s.New256(nil)
	if err != nil {
		return "blake2s FAIL: " + err.Error()
	}
	h.Write([]byte("arkmesh"))
	return fmt.Sprintf("blake2s ok %s", hex.EncodeToString(h.Sum(nil)[:4]))
}

// tailscale 的节点/机器密钥: curve25519 + nacl box 密封解封.
func probeNodeKeys() string {
	nk := key.NewNode()
	npub := nk.Public()
	opened, ok := nk.OpenFrom(npub, nk.SealTo(npub, []byte("arkmesh")))
	if !ok || string(opened) != "arkmesh" {
		return "nodekey FAIL(seal/open)"
	}
	mk := key.NewMachine()
	mpub := mk.Public()
	opened, ok = mk.OpenFrom(mpub, mk.SealTo(mpub, []byte("arkmesh")))
	if !ok || string(opened) != "arkmesh" {
		return "machinekey FAIL(seal/open)"
	}
	return fmt.Sprintf("keys ok n=%s m=%s", npub.ShortString(), mpub.ShortString())
}

// disco 密钥: 节点间直连打洞时的身份与共享密钥.
func probeDisco() string {
	d1 := key.NewDisco()
	d2 := key.NewDisco()
	if d1.Public() == d2.Public() {
		return "disco FAIL(duplicate key)"
	}
	// DiscoShared 是 structs.Incomparable, 只验证计算不 panic
	_ = d1.Shared(d2.Public())
	return fmt.Sprintf("disco ok %s", d1.Public().ShortString())
}

// MapRequest 是控制平面的核心线上格式, 这里验证其 JSON 往返.
func probeControlJSON() string {
	req := tailcfg.MapRequest{
		Version:   tailcfg.CurrentCapabilityVersion,
		NodeKey:   key.NewNode().Public(),
		DiscoKey:  key.NewDisco().Public(),
		Stream:    true,
		KeepAlive: true,
	}
	b, err := json.Marshal(&req)
	if err != nil {
		return "tailcfg FAIL(marshal): " + err.Error()
	}
	var back tailcfg.MapRequest
	if err := json.Unmarshal(b, &back); err != nil {
		return "tailcfg FAIL(unmarshal): " + err.Error()
	}
	if back.NodeKey != req.NodeKey || back.DiscoKey != req.DiscoKey {
		return "tailcfg FAIL(roundtrip)"
	}
	return fmt.Sprintf("tailcfg ok %dB v%d", len(b), int(back.Version))
}

// MagicDNS 名称规范化.
func probeMagicDNS() string {
	fqdn, err := dnsname.ToFQDN("ArkMesh-Node")
	if err != nil {
		return "magicdns FAIL: " + err.Error()
	}
	return fmt.Sprintf("magicdns ok %s", fqdn.WithTrailingDot())
}

func runAllProbes() []string {
	return []string{
		probeRuntime(),
		probeCurve25519(),
		probeAEAD(),
		probeBlake2s(),
		probeNodeKeys(),
		probeDisco(),
		probeControlJSON(),
		probeMagicDNS(),
	}
}
