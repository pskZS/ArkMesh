package main

/*
#include <stdlib.h>
*/
import "C"

import (
	"os"
	"runtime"
	"strings"
	"time"
	"unsafe"
)

// init 在 dlopen 时执行, 用来确认 C++ 侧设的 GODEBUG 被 Go 运行时看到了,
// 以及启动时的 GOMAXPROCS(Go 1.26 会按 cgroup 配额动态改它)。
func init() {
	coreLog("核心加载: GOMAXPROCS=%d NumCPU=%d GODEBUG=%q",
		runtime.GOMAXPROCS(0), runtime.NumCPU(), os.Getenv("GODEBUG"))
}

//export ArkMeshSelfTest
func ArkMeshSelfTest() *C.char {
	return C.CString(strings.Join(runAllProbes(), " | "))
}

//export ArkMeshNetCheck
func ArkMeshNetCheck(host *C.char) *C.char {
	h := C.GoString(host)
	if h == "" {
		return C.CString("ERR 需要 host")
	}
	return C.CString(strings.Join(runNetCheck(h), " | "))
}

//export ArkMeshIfProbe
func ArkMeshIfProbe() *C.char {
	return C.CString(strings.Join(probeIfPrimitives(), " | "))
}

// ArkMeshControlUp 启动控制面。exitNode 非空表示这次要经由该出口节点上网
// (地址字面量), 空串表示只连 tailnet 内部。
//
//export ArkMeshControlUp
func ArkMeshControlUp(controlURL, authKey, hostname, stateDir, exitNode *C.char) *C.char {
	if err := controlUp(C.GoString(controlURL), C.GoString(authKey),
		C.GoString(hostname), C.GoString(stateDir), C.GoString(exitNode)); err != nil {
		return C.CString("ERR " + err.Error())
	}
	return C.CString("")
}

//export ArkMeshControlStatus
func ArkMeshControlStatus() *C.char {
	return C.CString(controlStatusJSON())
}

// ArkMeshPingPeers 后台对最多 maxPingPeers 个在线 peer 各发一次隧道内 ping。
// typ 取 "TSMP" / "ICMP" / "disco", 空串按 TSMP。返回空串表示已受理;
// 结果通过 ArkMeshControlStatus 的 pings / pingBusy 字段读取。
//
//export ArkMeshPingPeers
func ArkMeshPingPeers(typ *C.char) *C.char {
	if err := controlPingPeers(C.GoString(typ)); err != nil {
		return C.CString("ERR " + err.Error())
	}
	return C.CString("")
}

//export ArkMeshControlDown
func ArkMeshControlDown() {
	controlDown()
}

// ArkMeshDnsProbe 在隧道内注入合成 MagicDNS 查询, 验证 mesh 名能否解析。
// name 为空表示自动挑目标(本机 DNSName + 第一个在线 peer); timeoutMs<=0 按 3s。
// 同步返回一行可读结果, 调用方(扩展进程)应在隧道 state=="up" 之后再调。
//
//export ArkMeshDnsProbe
func ArkMeshDnsProbe(name *C.char, timeoutMs C.int) *C.char {
	return C.CString(controlDnsProbe(C.GoString(name), time.Duration(timeoutMs)*time.Millisecond))
}

// ArkMeshAttachTun 把 VpnExtensionAbility 建好的 VPN fd 挂到控制面上。
// 必须在 ArkMeshControlUp 之后、status 变成 "up" 拿到 nodeIPs 之后调用;
// 返回空串表示已受理(重建在后台跑), "ERR ..." 表示参数或状态不对。
//
//export ArkMeshAttachTun
func ArkMeshAttachTun(fd, mtu C.int) *C.char {
	if err := controlAttachTun(int(fd), int(mtu)); err != nil {
		return C.CString("ERR " + err.Error())
	}
	return C.CString("")
}

//export ArkMeshFree
func ArkMeshFree(p *C.char) { C.free(unsafe.Pointer(p)) }

func main() {}
