// VPN 扩展进程 -> UI 进程的状态通道。
//
// 为什么用文件而不是 preferences / emitter: VPN 扩展跑在独立进程里, emitter 和
// EventHub 都只在进程内有效; preferences 虽然落盘, 但每个进程各有一份内存缓存,
// 跨进程读不到对方刚写的值。两个进程同属一个 bundle, 沙箱里的 filesDir 是同一个
// 目录, 直接读写一个 JSON 文件最省事也最可预测。
import { fileIo } from '@kit.CoreFileKit'
import { BusinessError } from '@kit.BasicServicesKit'

// ==== Go 核心 status 的结构 (与 go/arkmesh-core/control.go 的 controlStatus 逐字段对齐) ====

export interface GoPeer {
  ip: string
  name?: string
  online: boolean
}

// 隧道内 ping 的一次结果 (与 control.go 的 pingOutcome 对齐)
export interface GoPing {
  target: string
  type: string
  ok: boolean
  latencyMs: number
  nodeName?: string
  via?: string
  error?: string
}

// 可以当出口节点的 peer (与 control.go 的 exitNodeOpt 对齐)。
// active 表示它正是本机当前选中的那个。
export interface ExitNodeOption {
  ip: string
  name?: string
  online: boolean
  active: boolean
}

export interface GoCoreStatus {
  state: string
  backendState?: string
  nodeIPs?: string[]
  hostName?: string
  dnsName?: string
  magicDNSSuffix?: string
  peers?: number
  peersOnline?: number
  peerList?: GoPeer[]
  exitNodeWant?: string
  exitNode?: string
  exitNodes?: ExitNodeOption[]
  tunFd?: number
  tunStats?: string
  pingBusy?: boolean
  pings?: GoPing[]
  // disco ping 一轮的结果: 只有 disco 响应才带路径(直连端点 / DERP 中继),
  // 节点页据此显示每个 peer 走的是直连还是中继。连接后由 Go 侧自动探一轮。
  pathPings?: GoPing[]
  // MagicDNS 隧道内探针的结果文本, 以及它对应的请求序号(0=连接时自动跑的那次)。
  // 扩展进程写, UI 读; UI 靠 dnsProbeSeq 判断"这条结果是不是我刚请求的那次"。
  dnsProbe?: string
  dnsProbeSeq?: number
  error?: string
}

// UI -> 扩展进程的 MagicDNS 探针请求。name 为空表示让 Go 自动挑目标。
export interface DnsProbeRequest {
  name: string
  seq: number
}

// 扩展进程写、UI 进程读的一行快照。
export interface TunnelSnapshot {
  updatedAt: number
  core: GoCoreStatus
}

const STATUS_FILE = 'arkmesh_status.json'

// UI -> 扩展进程的 MagicDNS 探针请求文件。UI 写 {name, seq}, 扩展在既有的 2s
// 状态发布循环里顺带读一次, 发现新 seq 就跑探针并把结果写回快照。复用现成的
// 循环, 不额外起定时器。
const DNS_PROBE_REQ_FILE = 'arkmesh_dnsprobe_req.json'

// UI 判定"扩展进程已经不在了"的阈值。扩展每 2s 写一次, 连续 4 个周期没更新
// 就说明它被系统回收了或者崩了, 这时不能继续显示"已连接"。
export const SNAPSHOT_STALE_MS = 8000

export class TunnelStatusStore {
  private readonly dir: string

  constructor(filesDir: string) {
    this.dir = filesDir
  }

  private pathOf(): string {
    return `${this.dir}/${STATUS_FILE}`
  }

  private tmpPathOf(): string {
    return `${this.dir}/${STATUS_FILE}.tmp`
  }

  private reqPathOf(): string {
    return `${this.dir}/${DNS_PROBE_REQ_FILE}`
  }

  write(core: GoCoreStatus): void {
    const snap: TunnelSnapshot = { updatedAt: Date.now(), core: core }
    this.writeRaw(JSON.stringify(snap))
  }

  // 扩展进程退出前留一条明确的"已停止", 免得 UI 拿着阈值内的旧快照继续显示已连接。
  writeStopped(): void {
    const stopped: GoCoreStatus = { state: 'stopped' }
    const snap: TunnelSnapshot = { updatedAt: Date.now(), core: stopped }
    this.writeRaw(JSON.stringify(snap))
  }

  // 先写 .tmp 再 rename: UI 是另一个进程, 随时可能来读, 直接覆盖原文件有读到半截
  // JSON 的风险, 而同一文件系统上的 rename 是原子的。
  private writeRaw(text: string): void {
    let fd = -1
    try {
      const f = fileIo.openSync(this.tmpPathOf(),
        fileIo.OpenMode.READ_WRITE | fileIo.OpenMode.CREATE | fileIo.OpenMode.TRUNC)
      fd = f.fd
      fileIo.writeSync(fd, text)
      fileIo.closeSync(fd)
      fd = -1
      fileIo.renameSync(this.tmpPathOf(), this.pathOf())
    } catch (err) {
      const e = err as BusinessError
      console.error(`[TunnelStatus] 写状态文件失败: ${e.message}`)
      if (fd >= 0) {
        try {
          fileIo.closeSync(fd)
        } catch (closeErr) {
          const ce = closeErr as BusinessError
          console.warn(`[TunnelStatus] 关闭临时文件失败: ${ce.message}`)
        }
      }
    }
  }

  // 读不到或解析失败返回 null, 由调用方按"状态未知"处理。
  read(): TunnelSnapshot | null {
    try {
      if (!fileIo.accessSync(this.pathOf())) {
        return null
      }
      const text = fileIo.readTextSync(this.pathOf())
      if (text === '') {
        return null
      }
      return JSON.parse(text) as TunnelSnapshot
    } catch (err) {
      const e = err as BusinessError
      console.warn(`[TunnelStatus] 读状态文件失败: ${e.message}`)
      return null
    }
  }

  // UI 侧: 发起一次 MagicDNS 探针请求。扩展进程会在下一个 2s 周期读到并执行。
  writeDnsProbeRequest(name: string, seq: number): void {
    const req: DnsProbeRequest = { name: name, seq: seq }
    let fd = -1
    try {
      const f = fileIo.openSync(this.reqPathOf(),
        fileIo.OpenMode.READ_WRITE | fileIo.OpenMode.CREATE | fileIo.OpenMode.TRUNC)
      fd = f.fd
      fileIo.writeSync(fd, JSON.stringify(req))
      fileIo.closeSync(fd)
      fd = -1
    } catch (err) {
      const e = err as BusinessError
      console.error(`[TunnelStatus] 写探针请求失败: ${e.message}`)
      if (fd >= 0) {
        try {
          fileIo.closeSync(fd)
        } catch (closeErr) {
          const ce = closeErr as BusinessError
          console.warn(`[TunnelStatus] 关闭探针请求文件失败: ${ce.message}`)
        }
      }
    }
  }

  // 扩展侧: 读取当前待处理的探针请求。读不到或解析失败返回 null。
  readDnsProbeRequest(): DnsProbeRequest | null {
    try {
      if (!fileIo.accessSync(this.reqPathOf())) {
        return null
      }
      const text = fileIo.readTextSync(this.reqPathOf())
      if (text === '') {
        return null
      }
      return JSON.parse(text) as DnsProbeRequest
    } catch (err) {
      return null
    }
  }
}
