package main

// OHOS 应用沙箱禁止标准库的 netlink 用法: Go 的 syscall.NetlinkRIB 用
// SOCK_RAW(AF_NETLINK) 发 RTM_GETLINK dump, 内核层面需要 CAP_NET_RAW,
// 应用进程没有该能力 -> EPERM, 于是 net.Interfaces() 直接失败, tsnet.Up 报
// "route ip+net: netlinkrib: permission denied"。
//
// tailscale 为 Android 的同款限制留了逃生门 netmon.RegisterInterfaceGetter
// (见 tailscale issue #2293), 这里用最传统的 SIOCGIF* ioctl + /proc 实现一个
// 替代枚举器, 不依赖 netlink。

import (
	"encoding/binary"
	"fmt"
	"math/bits"
	"net"
	"os"
	"strconv"
	"strings"
	"syscall"
	"unsafe"

	"tailscale.com/net/netmon"
)

// sizeof(struct ifreq) on linux/arm64 = 16(名字) + 24(union)
const ifreqLen = 40

const (
	iffUp        = 0x1
	iffBroadcast = 0x2
	iffLoopback  = 0x8
	iffPoint2P   = 0x10
	iffRunning   = 0x40
	iffMulticast = 0x1000

	arpHrdEther = 1
)

type ifconfHdr struct {
	Len int32
	_   int32
	Buf uintptr
}

func registerInterfaceGetter() {
	netmon.RegisterInterfaceGetter(ohosInterfaceList)
}

// ohosInterfaceList 优先用标准库, 失败(沙箱禁 netlink)时退回 ioctl 实现。
func ohosInterfaceList() ([]netmon.Interface, error) {
	if ifs, err := net.Interfaces(); err == nil {
		out := make([]netmon.Interface, len(ifs))
		for i := range ifs {
			out[i] = netmon.Interface{Interface: &ifs[i]}
		}
		return out, nil
	}

	fd, err := syscall.Socket(syscall.AF_INET, syscall.SOCK_DGRAM|syscall.SOCK_CLOEXEC, 0)
	if err != nil {
		return nil, fmt.Errorf("socket(AF_INET): %w", err)
	}
	defer syscall.Close(fd)

	names, err := ifconfNames(fd)
	if err != nil {
		return nil, fmt.Errorf("SIOCGIFCONF: %w", err)
	}
	names = mergeUnique(names, procNetDevNames())

	v6 := procNetIfInet6()

	var out []netmon.Interface
	var firstErr error
	for _, name := range names {
		ni, err := readIface(fd, name)
		if err != nil {
			if firstErr == nil {
				firstErr = fmt.Errorf("%s: %w", name, err)
			}
			continue
		}
		// AltAddrs 必须非 nil, 否则 netmon 会回落到标准库 Addrs() 再撞 netlink
		addrs := []net.Addr{}
		addrs = append(addrs, ipv4Addrs(fd, name)...)
		addrs = append(addrs, v6[name]...)
		out = append(out, netmon.Interface{Interface: ni, AltAddrs: addrs})
	}
	if len(out) == 0 {
		return nil, fmt.Errorf("未枚举到任何接口 (首个错误: %v)", firstErr)
	}
	return out, nil
}

func ioctlIfreq(fd int, req uintptr, name string) ([]byte, error) {
	buf := make([]byte, ifreqLen)
	copy(buf[:16], name)
	if _, _, errno := syscall.Syscall(syscall.SYS_IOCTL, uintptr(fd), req,
		uintptr(unsafe.Pointer(&buf[0]))); errno != 0 {
		return nil, errno
	}
	return buf, nil
}

func ifconfNames(fd int) ([]string, error) {
	for size := 1024; size <= 32768; size *= 2 {
		buf := make([]byte, size)
		hdr := ifconfHdr{Len: int32(size), Buf: uintptr(unsafe.Pointer(&buf[0]))}
		if _, _, errno := syscall.Syscall(syscall.SYS_IOCTL, uintptr(fd), syscall.SIOCGIFCONF,
			uintptr(unsafe.Pointer(&hdr))); errno != 0 {
			return nil, errno
		}
		used := int(hdr.Len)
		if used <= 0 {
			return nil, nil
		}
		// 填满整个缓冲区说明可能被截断, 换更大的再试
		if used > size-ifreqLen {
			continue
		}
		var names []string
		for off := 0; off+ifreqLen <= used; off += ifreqLen {
			if n := cString(buf[off : off+16]); n != "" {
				names = append(names, n)
			}
		}
		return names, nil
	}
	return nil, fmt.Errorf("接口名列表超出缓冲区")
}

func readIface(fd int, name string) (*net.Interface, error) {
	fb, err := ioctlIfreq(fd, syscall.SIOCGIFFLAGS, name)
	if err != nil {
		return nil, fmt.Errorf("SIOCGIFFLAGS: %w", err)
	}
	ni := &net.Interface{Name: name, Flags: goFlags(binary.LittleEndian.Uint16(fb[16:18]))}

	if b, err := ioctlIfreq(fd, syscall.SIOCGIFMTU, name); err == nil {
		ni.MTU = int(int32(binary.LittleEndian.Uint32(b[16:20])))
	}
	if b, err := ioctlIfreq(fd, syscall.SIOCGIFINDEX, name); err == nil {
		ni.Index = int(int32(binary.LittleEndian.Uint32(b[16:20])))
	}
	if b, err := ioctlIfreq(fd, syscall.SIOCGIFHWADDR, name); err == nil {
		if binary.LittleEndian.Uint16(b[16:18]) == arpHrdEther {
			ni.HardwareAddr = net.HardwareAddr(append([]byte(nil), b[18:24]...))
		}
	}
	return ni, nil
}

func ipv4Addrs(fd int, name string) []net.Addr {
	var out []net.Addr
	ab, err := ioctlIfreq(fd, syscall.SIOCGIFADDR, name)
	if err != nil {
		return out
	}
	// struct sockaddr_in: family(2) port(2) addr(4, 网络序)
	if binary.LittleEndian.Uint16(ab[16:18]) != syscall.AF_INET {
		return out
	}
	ip := net.IPv4(ab[20], ab[21], ab[22], ab[23])
	ones := 32
	if nb, err := ioctlIfreq(fd, syscall.SIOCGIFNETMASK, name); err == nil {
		if binary.LittleEndian.Uint16(nb[16:18]) == syscall.AF_INET {
			ones = 0
			for _, b := range nb[20:24] {
				ones += bits.OnesCount8(b)
			}
		}
	}
	return append(out, &net.IPNet{IP: ip, Mask: net.CIDRMask(ones, 32)})
}

func goFlags(iff uint16) net.Flags {
	var f net.Flags
	if iff&iffUp != 0 {
		f |= net.FlagUp
	}
	if iff&iffRunning != 0 {
		f |= net.FlagRunning
	}
	if iff&iffLoopback != 0 {
		f |= net.FlagLoopback
	}
	if iff&iffBroadcast != 0 {
		f |= net.FlagBroadcast
	}
	if iff&iffPoint2P != 0 {
		f |= net.FlagPointToPoint
	}
	if iff&iffMulticast != 0 {
		f |= net.FlagMulticast
	}
	return f
}

func cString(b []byte) string {
	if i := strings.IndexByte(string(b), 0); i >= 0 {
		return string(b[:i])
	}
	return string(b)
}

func mergeUnique(a, b []string) []string {
	seen := make(map[string]bool, len(a)+len(b))
	out := make([]string, 0, len(a)+len(b))
	for _, s := range append(append([]string(nil), a...), b...) {
		if s != "" && !seen[s] {
			seen[s] = true
			out = append(out, s)
		}
	}
	return out
}

// /proc/net/dev 里只有名字和统计, 用来补齐 SIOCGIFCONF 漏掉的接口
func procNetDevNames() []string {
	b, err := os.ReadFile("/proc/net/dev")
	if err != nil {
		return nil
	}
	var out []string
	for i, line := range strings.Split(string(b), "\n") {
		if i < 2 {
			continue
		}
		name, _, ok := strings.Cut(line, ":")
		if !ok {
			continue
		}
		if name = strings.TrimSpace(name); name != "" {
			out = append(out, name)
		}
	}
	return out
}

// /proc/net/if_inet6: 地址(32 hex) ifindex prefixlen scope flags devname
func procNetIfInet6() map[string][]net.Addr {
	out := map[string][]net.Addr{}
	b, err := os.ReadFile("/proc/net/if_inet6")
	if err != nil {
		return out
	}
	for _, line := range strings.Split(string(b), "\n") {
		f := strings.Fields(line)
		if len(f) < 6 || len(f[0]) != 32 {
			continue
		}
		ip := make(net.IP, 16)
		bad := false
		for i := 0; i < 16; i++ {
			v, err := strconv.ParseUint(f[0][i*2:i*2+2], 16, 8)
			if err != nil {
				bad = true
				break
			}
			ip[i] = byte(v)
		}
		if bad {
			continue
		}
		ones, err := strconv.ParseUint(f[2], 16, 8)
		if err != nil {
			continue
		}
		if ip.IsLinkLocalUnicast() || ip.IsLoopback() {
			// 链路本地/回环 v6 对 tailscale 无用, 且会让 netmon 误判 HaveV6
			continue
		}
		name := f[5]
		out[name] = append(out[name], &net.IPNet{IP: ip, Mask: net.CIDRMask(int(ones), 128)})
	}
	return out
}
