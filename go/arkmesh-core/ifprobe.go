package main

// 诊断: 探明 OHOS 沙箱到底允许哪种接口枚举原语, 为 ifgetter.go 的选择提供依据。

/*
#include <ifaddrs.h>
#include <errno.h>

static int cGetIfAddrsCount(int *err) {
	struct ifaddrs *ifa = 0;
	*err = 0;
	if (getifaddrs(&ifa) != 0) {
		*err = errno;
		return -1;
	}
	int n = 0;
	for (struct ifaddrs *p = ifa; p; p = p->ifa_next) n++;
	freeifaddrs(ifa);
	return n;
}
*/
import "C"

import (
	"encoding/binary"
	"fmt"
	"net"
	"syscall"
)

const (
	rtmGetLink   = 18
	rtmNewLink   = 16
	nlmsgError   = 2
	nlmsgDone    = 3
	nlmFRequest  = 0x1
	nlmFRoot     = 0x100
	nlmFMatch    = 0x200
	nlmFDumpFlag = nlmFRoot | nlmFMatch
)

func probeIfPrimitives() []string {
	var out []string
	add := func(f string, a ...any) { out = append(out, fmt.Sprintf(f, a...)) }

	if ifs, err := net.Interfaces(); err != nil {
		// OHOS 沙箱禁止标准库走的 netlink-rib 枚举, 在真机上恒失败, 属预期。
		// 数据面真正依赖的是下面探测的 fallback(ioctl SIOCGIFCONF / ohosInterfaceList),
		// 它们 ok 就够。写成 FAIL 只会误导排查, 这里按"预期不可用"报。
		add("标准库 net.Interfaces: 不可用(OHOS 沙箱预期行为, 以下方 fallback 为准) %v", err)
	} else {
		add("标准库 net.Interfaces: ok (%d 个)", len(ifs))
	}

	if fd, err := syscall.Socket(syscall.AF_NETLINK, syscall.SOCK_RAW|syscall.SOCK_CLOEXEC, syscall.NETLINK_ROUTE); err != nil {
		add("netlink SOCK_RAW socket: FAIL %v", describeErrno(err))
	} else {
		add("netlink SOCK_RAW socket: ok")
		syscall.Close(fd)
	}

	if fd, err := syscall.Socket(syscall.AF_NETLINK, syscall.SOCK_DGRAM|syscall.SOCK_CLOEXEC, syscall.NETLINK_ROUTE); err != nil {
		add("netlink SOCK_DGRAM socket: FAIL %v", describeErrno(err))
	} else {
		n, err := netlinkDumpLinks(fd)
		if err != nil {
			add("netlink SOCK_DGRAM dump RTM_GETLINK: FAIL %v", err)
		} else {
			add("netlink SOCK_DGRAM dump RTM_GETLINK: ok (%d 个接口)", n)
		}
		syscall.Close(fd)
	}

	if fd, err := syscall.Socket(syscall.AF_INET, syscall.SOCK_DGRAM|syscall.SOCK_CLOEXEC, 0); err != nil {
		add("ioctl 用 AF_INET socket: FAIL %v", describeErrno(err))
	} else {
		if names, err := ifconfNames(fd); err != nil {
			add("ioctl SIOCGIFCONF: FAIL %v", err)
		} else {
			add("ioctl SIOCGIFCONF: ok %v", names)
		}
		syscall.Close(fd)
	}

	add("/proc/net/dev: %v", procNetDevNames())
	v6 := procNetIfInet6()
	v6n := 0
	for _, l := range v6 {
		v6n += len(l)
	}
	add("/proc/net/if_inet6: %d 个全局 v6 地址", v6n)

	var cerr C.int
	if n := C.cGetIfAddrsCount(&cerr); n < 0 {
		add("libc getifaddrs: FAIL errno=%d", int(cerr))
	} else {
		add("libc getifaddrs: ok (%d 条)", int(n))
	}

	list, err := ohosInterfaceList()
	if err != nil {
		add("ohosInterfaceList: FAIL %v", err)
	} else {
		detail := ""
		for _, ni := range list {
			addrs, _ := ni.Addrs()
			detail += fmt.Sprintf(" [%s idx=%d flags=%v mtu=%d addrs=%v]",
				ni.Name, ni.Index, ni.Flags, ni.MTU, addrs)
		}
		add("ohosInterfaceList: ok (%d 个)%s", len(list), detail)
	}

	return out
}

func describeErrno(err error) string {
	if e, ok := err.(syscall.Errno); ok {
		return fmt.Sprintf("%d(%v)", int(e), err)
	}
	return err.Error()
}

// 用 SOCK_DGRAM 的 netlink 手写一次 RTM_GETLINK dump, 数出接口个数
func netlinkDumpLinks(fd int) (int, error) {
	req := make([]byte, 32)
	binary.LittleEndian.PutUint32(req[0:4], 32)
	binary.LittleEndian.PutUint16(req[4:6], rtmGetLink)
	binary.LittleEndian.PutUint16(req[6:8], nlmFRequest|nlmFDumpFlag)
	binary.LittleEndian.PutUint32(req[8:12], 1) // seq
	// 后 16 字节是要跟随的 struct ifinfomsg, 全 0 即可

	if err := syscall.Sendto(fd, req, 0, &syscall.SockaddrNetlink{Family: syscall.AF_NETLINK}); err != nil {
		return 0, fmt.Errorf("sendto: %w", err)
	}

	buf := make([]byte, 32768)
	n, from, err := syscall.Recvfrom(fd, buf, 0)
	if err != nil {
		return 0, fmt.Errorf("recvfrom: %w", err)
	}
	if sa, ok := from.(*syscall.SockaddrNetlink); ok && sa.Pid != 0 {
		// 内核回包 pid 恒为 0, 否则是别人发来的
		return 0, fmt.Errorf("非内核回包 pid=%d", sa.Pid)
	}
	if n < 16 {
		return 0, fmt.Errorf("回包过短 %d 字节", n)
	}
	typ := binary.LittleEndian.Uint16(buf[4:6])
	if typ == nlmsgError {
		if n >= 20 {
			return 0, fmt.Errorf("NLMSG_ERROR errno=%d", int32(binary.LittleEndian.Uint32(buf[16:20])))
		}
		return 0, fmt.Errorf("NLMSG_ERROR")
	}
	if typ == nlmsgDone {
		return 0, nil
	}
	// 第一段就是数据即可认为 dump 成功
	return 1, nil
}
