// NAPI Native Module Wrapper
// Bridges ArkTS and the Go core (libArkMesh.so -> libarkmesh_core.so).
//
// 只保留生产路径真正调用的 Go 核心方法。旧 C++ 核心(initialize/connect/
// getMeshIP/...)与 S4/S5 spike(goProbe/goUdp*/goTun*)的 wrapper 已随 Stage 4
// 退役删除: 它们没有调用方, 留着只会让人误以为还有第二条数据面。

import nativeModule from 'libArkMesh.so'

// 探针外层兜底超时。Go 侧每个探针自带 8s 超时, 网络检测串行 7 个, 正常最坏
// 也就一分钟出头; 超过这个数说明系统调用被沙箱挂死了, 必须让 Promise 收敛,
// 否则调用方会一直停在"检测中"。
const PROBE_TIMEOUT_MS = 90000
const PROBE_TIMEOUT_MSG = '超时: 探针 90 秒未返回, 系统调用可能被挂起, 详见 hilog'

function withProbeTimeout(task: Promise<string>): Promise<string> {
  return new Promise<string>((resolve) => {
    let settled = false
    const timer: number = setTimeout(() => {
      if (!settled) {
        settled = true
        console.error('arkmeshNative 探针超时(' + PROBE_TIMEOUT_MS + 'ms)')
        resolve(PROBE_TIMEOUT_MSG)
      }
    }, PROBE_TIMEOUT_MS)
    task.then((v: string) => {
      if (!settled) {
        settled = true
        clearTimeout(timer)
        resolve(v)
      }
    }).catch((e: Error) => {
      if (!settled) {
        settled = true
        clearTimeout(timer)
        console.error('arkmeshNative 探针异常:', e)
        resolve('调用异常: ' + e.message)
      }
    })
  })
}

class ArkMeshNativeImpl {
  /**
   * Go 核心自检。原生侧返回 Promise, 探针跑在 NAPI worker 线程上, 不会阻塞 UI。
   * 这里再包一层超时兜底(项目规则: 异步操作必须能超时收敛)。
   */
  coreSelfTest(): Promise<string> {
    try {
      return withProbeTimeout(nativeModule.coreSelfTest())
    } catch (e) {
      console.error('arkmeshNative coreSelfTest error:', e)
      return Promise.resolve('调用异常: ' + JSON.stringify(e))
    }
  }

  /** 控制面网络检测(DNS/TCP/TLS/HTTPS)。异步 + 超时兜底, 语义同上。 */
  coreNetCheck(host: string): Promise<string> {
    try {
      return withProbeTimeout(nativeModule.coreNetCheck(host))
    } catch (e) {
      console.error('arkmeshNative coreNetCheck error:', e)
      return Promise.resolve('调用异常: ' + JSON.stringify(e))
    }
  }

  /** 本机网卡枚举原语探测。异步 + 超时兜底, 语义同上。 */
  coreIfProbe(): Promise<string> {
    try {
      return withProbeTimeout(nativeModule.coreIfProbe())
    } catch (e) {
      console.error('arkmeshNative coreIfProbe error:', e)
      return Promise.resolve('调用异常: ' + JSON.stringify(e))
    }
  }

  // exitNode 传空串表示不用出口节点。C++ 侧固定收 5 个参数, 所以这里给默认值
  // 而不是让调用方省略 —— 省略会让 napi 只拿到 4 个 argv。
  coreControlUp(controlURL: string, authKey: string, hostname: string, stateDir: string,
    exitNode: string = ''): string {
    try {
      return nativeModule.coreControlUp(controlURL, authKey, hostname, stateDir, exitNode)
    } catch (e) {
      console.error('arkmeshNative coreControlUp error:', e)
      return 'ERR ' + JSON.stringify(e)
    }
  }

  coreControlStatus(): string {
    try {
      return nativeModule.coreControlStatus()
    } catch (e) {
      console.error('arkmeshNative coreControlStatus error:', e)
      return '{"state":"error","error":' + JSON.stringify(String(e)) + '}'
    }
  }

  coreControlDown(): string {
    try {
      return nativeModule.coreControlDown()
    } catch (e) {
      console.error('arkmeshNative coreControlDown error:', e)
      return 'ERR ' + JSON.stringify(e)
    }
  }

  /**
   * 让 Go 核心在后台对在线 peer 发隧道内 ping。
   *
   * 为什么不用系统 ping: OHOS 的 VPN 按 UID 抓流量, hdc shell 不在范围内;
   * 扩展进程自己又被 protectProcessNet 整体绕过了 VPN。只有 wgengine 直接注入
   * 加密通道的 TSMP/ICMP ping 才能在本机自证数据面通不通。
   *
   * 立即返回空串表示已受理, 结果从 coreControlStatus() 的 pings / pingBusy 读。
   * type 取 'TSMP' / 'ICMP' / 'disco'。
   */
  corePingPeers(type: string): string {
    try {
      return nativeModule.corePingPeers(type)
    } catch (e) {
      console.error('arkmeshNative corePingPeers error:', e)
      return 'ERR ' + JSON.stringify(e)
    }
  }

  /**
   * 在隧道内注入合成 MagicDNS 查询, 直接验证 mesh 名能否解析。
   *
   * 为什么不用系统 DNS: 扩展进程被 VpnConfig.blockedApplications 整个排除在 VPN
   * 之外, 它发的 DNS 不进隧道, 问不到 MagicDNS(quad-100)。这个探针把查询包从
   * TUN 读端塞进去冒充手机流量, 走的正是用户在浏览器里输 mesh 名时的同一条路径。
   *
   * name 传空串表示自动挑目标(本机 DNSName + 第一个在线 peer)。必须在隧道
   * state=="up" 之后调用。同步返回一行可读结果。
   */
  coreDnsProbe(name: string, timeoutMs: number): string {
    try {
      return nativeModule.coreDnsProbe(name, timeoutMs)
    } catch (e) {
      console.error('arkmeshNative coreDnsProbe error:', e)
      return 'ERR ' + JSON.stringify(e)
    }
  }

  /**
   * 把 VPN fd 挂到 Go 控制面上, 让设备流量真正走隧道。
   * 返回空串表示已受理(重建在 Go 侧后台跑), 之后轮询 coreControlStatus 到 state=="up"。
   */
  coreAttachTun(fd: number, mtu: number): string {
    try {
      return nativeModule.coreAttachTun(fd, mtu)
    } catch (e) {
      console.error('arkmeshNative coreAttachTun error:', e)
      return 'ERR ' + JSON.stringify(e)
    }
  }
}

export const arkmeshNative = new ArkMeshNativeImpl()
