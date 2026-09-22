import { VpnExtensionAbility, vpnExtension } from '@kit.NetworkKit'
import { Want } from '@kit.AbilityKit'
import { BusinessError } from '@kit.BasicServicesKit'
import { arkmeshNative } from '../core/ArkMeshNative'
import { TunnelStatusStore, GoCoreStatus, GoPeer, GoPing } from '../core/TunnelStatusStore'

// Address class for VPN configuration (matches NetAddress shape)
class Address {
  address: string
  family: number
  port: number

  constructor(address: string, family: number) {
    this.address = address
    this.family = family
    this.port = 0
  }
}

// AddressWithPrefix class for VPN configuration (matches LinkAddress shape)
class AddressWithPrefix {
  address: Address
  prefixLength: number

  constructor(address: Address, prefixLength: number) {
    this.address = address
    this.prefixLength = prefixLength
  }
}

// Route class for VPN routing configuration (matches RouteInfo shape)
class Route {
  interface: string
  destination: AddressWithPrefix
  gateway: Address
  hasGateway: boolean
  isDefaultRoute: boolean

  constructor(iface: string, destination: AddressWithPrefix, gateway: Address, hasGateway: boolean, isDefaultRoute: boolean) {
    this.interface = iface
    this.destination = destination
    this.gateway = gateway
    this.hasGateway = hasGateway
    this.isDefaultRoute = isDefaultRoute
  }
}

// Route B Stage 3: 接口地址/路由都由 Go 核心回传的 netmap 决定, 所以需要一个
// 能接收已构造好的 addresses/routes 的配置类(上面那个 Config 是写死单 IPv4 的)。
class TunnelConfig {
  addresses: AddressWithPrefix[]
  routes: Route[]
  mtu: number
  dnsAddresses: string[]
  searchDomains: string[]
  trustedApplications: string[]
  blockedApplications: string[]

  constructor(addresses: AddressWithPrefix[], routes: Route[], mtu: number, dnsAddresses: string[]) {
    this.addresses = addresses
    this.routes = routes
    this.mtu = mtu
    this.dnsAddresses = dnsAddresses
    this.searchDomains = []
    this.trustedApplications = []
    this.blockedApplications = []
  }
}

export default class ArkMeshVpnAbility extends VpnExtensionAbility {
  private isRunning: boolean = false
  private tunnelFd: number = -1
  private vpnConnection: vpnExtension.VpnConnection | null = null
  private goStatsTimer: number = -1
  private statusTimer: number = -1
  private statusStore: TunnelStatusStore | null = null

  // MagicDNS 隧道内探针: 连接就绪后自动跑一次(seq=0), 之后由 UI 通过请求文件
  // 触发对指定名字的探测(seq=Date.now())。结果连同 seq 一起进快照给 UI 读。
  private lastDnsProbe: string = ''
  private lastDnsProbeSeq: number = 0
  private handledProbeSeq: number = -1

  onCreate(want: Want) {
    console.info('[ArkMeshVpn] onCreate')

    // Route B Stage 2: 在 VPN 扩展进程内把 Go 核心接上 headscale 控制面
    if (want.parameters && want.parameters['goCore'] === 1) {
      console.info('[GoCoreSpike] 进入 Go 核心控制面测试模式')
      this.runGoCoreSpike(want)
      return
    }

    // Route B Stage 3: Go 核心 + 真实 VPN fd, 打通设备流量
    if (want.parameters && want.parameters['goTunnel'] === 1) {
      console.info('[GoTunnel] 进入 Stage 3 隧道模式')
      this.runGoTunnel(want)
      return
    }

    console.warn('[ArkMeshVpn] want 未指定 goCore/goTunnel 模式, 不启动')
  }

  onDestroy() {
    console.info('[ArkMeshVpn] onDestroy')
    this.isRunning = false
    this.stopGoStatsTicker()
    this.stopStatusWriter()
    // 必须主动让 Go 核心下线: 否则 tsnet 只是随进程消失, headscale 要等 keepalive
    // 超时才把节点标成离线, 用户会看到"客户端已断开, 控制台里节点还在线"。
    const rc: string = arkmeshNative.coreControlDown()
    console.info(`[ArkMeshVpn] coreControlDown: ${rc}`)
    this.writeStopped()
    this.cleanupVpn()
  }

  // Route B Stage 2: 在扩展进程内启动 Go 核心的 tsnet, 接入自建 headscale。
  // 参数从 want 传入, 密钥不落盘到源码里。
  private async runGoCoreSpike(want: Want): Promise<void> {
    try {
      const p = want.parameters
      const serverUrl: string = (p && typeof p['serverUrl'] === 'string') ? p['serverUrl'] as string : ''
      const authKey: string = (p && typeof p['authKey'] === 'string') ? p['authKey'] as string : ''
      const hostname: string = (p && typeof p['hostname'] === 'string') ? p['hostname'] as string : 'arkmesh-go'
      if (serverUrl === '' || authKey === '') {
        console.error('[GoCoreSpike] FAIL 缺少 serverUrl/authKey')
        return
      }

      let stateDir = ''
      try {
        stateDir = this.context.filesDir + '/tsnet'
      } catch (err) {
        const e = err as BusinessError
        stateDir = '/data/storage/el2/base/files/tsnet'
        console.warn(`[GoCoreSpike] filesDir 不可用(${e.message}), 回退 ${stateDir}`)
      }

      // 先跑一次自检, 确认核心和网络都正常, 便于把失败面区分开。
      // 这两个探针现在是异步的(原生侧跑 worker 线程), 必须 await, 否则
      // 拿到的是未兑现的 Promise 而不是报告字符串, 扩展进程的启动顺序也乱。
      const self = await arkmeshNative.coreSelfTest()
      console.info(`[GoCoreSpike] selfTest: ${self}`)
      const host = ArkMeshVpnAbility.extractHost(serverUrl)
      const net = await arkmeshNative.coreNetCheck(host)
      console.info(`[GoCoreSpike] netCheck ${host}: ${net}`)

      const rc = arkmeshNative.coreControlUp(serverUrl, authKey, hostname, stateDir)
      if (rc !== '') {
        console.error(`[GoCoreSpike] FAIL coreControlUp: ${rc}`)
        return
      }
      console.info(`[GoCoreSpike] coreControlUp 已启动, stateDir=${stateDir}`)

      // 轮询状态直到 up / failed / 超时
      const deadline = Date.now() + 90000
      while (Date.now() < deadline) {
        await new Promise<void>((resolve) => setTimeout(resolve, 3000))
        const raw = arkmeshNative.coreControlStatus()
        console.info(`[GoCoreSpike] status: ${raw}`)
        if (raw.indexOf('"state":"up"') >= 0) {
          console.info('[GoCoreSpike] PASS 控制面已连接 (见上方 nodeIPs/dnsName)')
          return
        }
        if (raw.indexOf('"state":"failed"') >= 0) {
          console.error('[GoCoreSpike] FAIL 控制面连接失败 (见上方 error/logs)')
          return
        }
      }
      console.error('[GoCoreSpike] FAIL 90s 超时仍未连上')
    } catch (err) {
      const e = err as BusinessError
      console.error(`[GoCoreSpike] FAIL 异常: ${e.message}`)
    }
  }

  // Route B Stage 3: 真正把设备流量接进隧道。
  //
  // 分三步走的顺序是被两个硬约束逼出来的:
  //   a) VPN 接口地址必须等于 headscale 分配给我们的 Tailscale IP —— 否则隧道里
  //      回来的包目的地址不属于本机, 内核会直接丢掉;
  //   b) tsnet.Server.Tun 只能在 Start 之前设置, 没有运行中热替换的入口。
  // 而 IP 只有注册完才知道, 所以只能: 先无 TUN 注册拿 IP -> 建 VPN -> 带 TUN
  // 重建 tsnet。节点密钥存在 stateDir 里, 重建后 headscale 认得同一个节点, IP 不变。
  private async runGoTunnel(want: Want): Promise<void> {
    try {
      // 状态通道要最先起来: 后面的每一步失败都得让 UI 立刻看到, 否则它会一直
      // 停在"连接中"。
      this.initStatusStore()

      const p = want.parameters
      const serverUrl: string = (p && typeof p['serverUrl'] === 'string') ? p['serverUrl'] as string : ''
      const authKey: string = (p && typeof p['authKey'] === 'string') ? p['authKey'] as string : ''
      const hostname: string = (p && typeof p['hostname'] === 'string') ? p['hostname'] as string : 'arkmesh-harmony'
      // 出口节点只在启动时定: OHOS 的 VpnConfig 建完就不能改, 加默认路由必须整个
      // 重建 VPN, 所以切换出口节点等于重启一次隧道(UI 侧就是 stop + start)。
      const exitNode: string = (p && typeof p['exitNode'] === 'string') ? p['exitNode'] as string : ''
      if (serverUrl === '' || authKey === '') {
        console.error('[GoTunnel] FAIL 缺少 serverUrl/authKey')
        this.publishFailure('缺少控制面地址或预授权密钥')
        return
      }

      let stateDir = ''
      try {
        stateDir = this.context.filesDir + '/tsnet'
      } catch (err) {
        const e = err as BusinessError
        stateDir = '/data/storage/el2/base/files/tsnet'
        console.warn(`[GoTunnel] filesDir 不可用(${e.message}), 回退 ${stateDir}`)
      }

      // 第 1 步: 无 TUN 注册, 只为拿到 headscale 分配的 IP。
      // 出口节点也在这一步就交给 Go: 它会在 srv.Up 成功后写进 prefs, 失败就让
      // state 直接变 failed —— 那时 VPN 还没建, 不会因为多出来的默认路由把整机断网。
      const rc = arkmeshNative.coreControlUp(serverUrl, authKey, hostname, stateDir, exitNode)
      if (rc !== '') {
        console.error(`[GoTunnel] FAIL coreControlUp: ${rc}`)
        this.publishFailure(`控制面启动失败: ${rc}`)
        return
      }
      console.info(`[GoTunnel] 第1步 无 TUN 注册已启动 exitNode=${exitNode !== '' ? exitNode : '(无)'}`)
      const st1 = await this.pollGoCore('注册', 90000)
      if (st1 === null) {
        return // 具体失败原因已由 pollGoCore 发布到状态文件
      }
      const nodeIPs: string[] = st1.nodeIPs !== undefined ? st1.nodeIPs : []
      if (nodeIPs.length === 0) {
        console.error('[GoTunnel] FAIL 控制面已连上但没拿到 nodeIPs')
        this.publishFailure('控制面已连上但未分配到 mesh IP')
        return
      }
      const magicDns: string = st1.magicDNSSuffix !== undefined ? st1.magicDNSSuffix : ''
      console.info(`[GoTunnel] 第1步 PASS ips=${nodeIPs.join(',')} magicDNS=${magicDns}`)

      // 第 2 步: 用这些 IP 建 VPN, 拿到 fd。
      // cfg.blockedApplications 里带上自己这个包, 是本进程之后还能上网的前提,
      // 原因详见 buildTunnelConfig。
      const ownBundle: string = (typeof want.bundleName === 'string' && want.bundleName !== '')
        ? want.bundleName : 'com.arkmesh.client'
      const cfg = ArkMeshVpnAbility.buildTunnelConfig(nodeIPs, magicDns, exitNode !== '', ownBundle)
      this.vpnConnection = vpnExtension.createVpnConnection(this.context)
      const fd: number = await this.vpnConnection.create(cfg)
      if (fd < 0) {
        console.error(`[GoTunnel] FAIL 创建 VPN 失败 fd=${fd}`)
        this.publishFailure(`创建 VPN 通道失败 fd=${fd}`)
        return
      }
      this.tunnelFd = fd
      console.info(`[GoTunnel] 第2步 PASS VPN fd=${fd} mtu=${cfg.mtu} dns=${cfg.dnsAddresses.join(',')} ` +
        `search=${cfg.searchDomains.join(',')} ` +
        `routes=${cfg.routes.length}(含全流量路由=${exitNode !== ''}) ` +
        `blocked=${cfg.blockedApplications.join(',')}`)

      // 第 3 步: 带 TUN 重建 tsnet
      const rc2 = arkmeshNative.coreAttachTun(fd, cfg.mtu)
      if (rc2 !== '') {
        console.error(`[GoTunnel] FAIL coreAttachTun: ${rc2}`)
        this.publishFailure(`挂载 TUN 失败: ${rc2}`)
        return
      }
      console.info('[GoTunnel] 第3步 带 TUN 重建 tsnet 已受理')
      const st2 = await this.pollGoCore('重建', 90000)
      if (st2 === null) {
        return // 具体失败原因已由 pollGoCore 发布到状态文件
      }
      this.isRunning = true
      this.startGoStatsTicker()
      const st3 = await this.awaitPeers(20000)
      const ips2: string[] = st3.nodeIPs !== undefined ? st3.nodeIPs : []
      const peerList: GoPeer[] = st3.peerList !== undefined ? st3.peerList : []
      const peerDesc: string = peerList.map((p: GoPeer): string =>
        `${p.ip}(${p.name !== undefined ? p.name : '?'},${p.online ? 'online' : 'offline'})`).join(' ')
      console.info(`[GoTunnel] PASS 隧道就绪 tunFd=${st3.tunFd} ips=${ips2.join(',')} ` +
        `dns=${st3.dnsName !== undefined ? st3.dnsName : ''} ` +
        `peers=${st3.peers !== undefined ? st3.peers : 0} online=${st3.peersOnline !== undefined ? st3.peersOnline : 0} ` +
        `exitNode=${st3.exitNode !== undefined && st3.exitNode !== '' ? st3.exitNode : '(无)'}`)
      console.info(`[GoTunnel] peer 列表: ${peerDesc !== '' ? peerDesc : '(空)'}`)

      // 第 4 步: 数据面自证。
      // hdc shell 的流量不进 VPN(按 UID 抓), 扩展进程自己又在 blockedApplications
      // 里被整个排除了, 两头都发不出"会进隧道"的包。所以让 Go 核心直接往
      // wgengine 注入 TSMP ping —— 走的是和真实流量同一条加密路径, 通不通
      // 当场就知道, 不用等第二台机器配合。
      const rc3 = arkmeshNative.corePingPeers('TSMP')
      if (rc3 !== '') {
        console.error(`[GoTunnel] WARN 隧道内 ping 未能启动: ${rc3}`)
      } else {
        await this.awaitPings(40000)
      }

      // 第 5 步: MagicDNS 探针。隧道就绪后自动验证 mesh 名能否解析, 结果进快照,
      // 设置页可直接看到, 不用等用户去浏览器里试。
      this.runAutoDnsProbe()
    } catch (err) {
      const e = err as BusinessError
      console.error(`[GoTunnel] FAIL 异常: ${e.message}`)
      this.publishFailure(`隧道异常: ${e.message}`)
    }
  }

  // 轮询 Go 核心状态直到 up / failed / 超时。成功返回解析后的状态, 否则 null。
  // 失败/超时时直接把真实原因发布到状态文件, 首页能看到"为什么连不上",
  // 而不是只有一句笼统的"接入控制面失败"。
  private async pollGoCore(label: string, timeoutMs: number): Promise<GoCoreStatus | null> {
    const deadline = Date.now() + timeoutMs
    while (Date.now() < deadline) {
      await new Promise<void>((resolve) => setTimeout(resolve, 2000))
      const raw = arkmeshNative.coreControlStatus()
      let st: GoCoreStatus
      try {
        st = JSON.parse(raw) as GoCoreStatus
      } catch (err) {
        const e = err as BusinessError
        console.error(`[GoTunnel] ${label} status 解析失败(${e.message}): ${raw}`)
        this.publishFailure(`${label}状态解析失败: ${e.message}`)
        return null
      }
      if (st.state === 'up') {
        return st
      }
      if (st.state === 'failed') {
        const rawErr: string = st.error !== undefined && st.error !== '' ? st.error : '(无 error 字段)'
        console.error(`[GoTunnel] FAIL ${label}失败: ${rawErr}`)
        this.publishFailure(ArkMeshVpnAbility.describeFailure(rawErr))
        return null
      }
      console.info(`[GoTunnel] ${label}中... state=${st.state}`)
    }
    console.error(`[GoTunnel] FAIL ${label} ${timeoutMs}ms 超时仍未就绪`)
    this.publishFailure(`${label}超时(${Math.round(timeoutMs / 1000)}s 未就绪), 请检查网络后重试`)
    return null
  }

  // 把 Go 侧的原始报错翻译成用户能看懂的中文提示, 原始信息截断后附在后面,
  // 方便截图反馈时定位问题。模式取自真机上实际见过的失败场景:
  // 密钥无效/过期(最常见)、地址写错、网络不通、证书问题。
  private static describeFailure(raw: string): string {
    const lower = raw.toLowerCase()
    let hint = ''
    if (lower.includes('authkey') || lower.includes('preauth') || lower.includes('401')
      || lower.includes('403') || lower.includes('unauthorized') || lower.includes('invalid key')) {
      hint = '预授权密钥无效或已过期, 请在设置中更换新密钥'
    } else if (lower.includes('no such host') || lower.includes('server misbehaving')) {
      hint = '服务器域名解析失败, 请检查控制面地址是否写错'
    } else if (lower.includes('connection refused')) {
      hint = '服务器拒绝连接, 请检查控制面地址和端口'
    } else if (lower.includes('x509') || lower.includes('certificate') || lower.includes('tls')) {
      hint = 'TLS 证书校验失败, 请检查服务器证书配置'
    } else if (lower.includes('timeout') || lower.includes('unreachable')) {
      hint = '网络连接超时或不可达, 请检查当前网络能否访问服务器'
    } else if (lower.includes('404')) {
      hint = '地址可访问但不是 Headscale 服务, 请检查 URL 路径'
    }
    const brief: string = raw.length > 120 ? raw.substring(0, 120) + '…' : raw
    return hint !== '' ? `${hint} (${brief})` : `连接失败: ${brief}`
  }

  // state 变 up 的那一刻, netmap 往往还是从磁盘缓存加载的(peers=0), 真 netmap
  // 要再过几秒才从控制面下来。这里补等一会儿, 免得把中间的 peers=0 当成最终结果。
  // 等不到不算失败, 只警告 —— tailnet 里确实可能没有别的在线节点。
  private async awaitPeers(timeoutMs: number): Promise<GoCoreStatus> {
    const deadline = Date.now() + timeoutMs
    let last: GoCoreStatus = { state: 'up' }
    while (Date.now() < deadline) {
      const raw = arkmeshNative.coreControlStatus()
      try {
        last = JSON.parse(raw) as GoCoreStatus
      } catch (err) {
        const e = err as BusinessError
        console.warn(`[GoTunnel] awaitPeers status 解析失败: ${e.message}`)
        return last
      }
      if ((last.peers !== undefined ? last.peers : 0) > 0) {
        return last
      }
      await new Promise<void>((resolve) => setTimeout(resolve, 2000))
    }
    console.warn(`[GoTunnel] WARN ${timeoutMs}ms 内 peers 仍为 0`)
    return last
  }

  // 等 Go 侧那轮隧道内 ping 跑完并把结果打出来。等不到只警告, 不算隧道失败
  // —— ping 失败说明数据面有问题, 但控制面和 TUN 本身已经是好的了。
  private async awaitPings(timeoutMs: number): Promise<void> {
    const deadline = Date.now() + timeoutMs
    let last: GoCoreStatus = { state: 'up' }
    while (Date.now() < deadline) {
      await new Promise<void>((resolve) => setTimeout(resolve, 2000))
      try {
        last = JSON.parse(arkmeshNative.coreControlStatus()) as GoCoreStatus
      } catch (err) {
        const e = err as BusinessError
        console.warn(`[GoTunnel] awaitPings status 解析失败: ${e.message}`)
        return
      }
      const done: number = last.pings !== undefined ? last.pings.length : 0
      if (done > 0 && last.pingBusy !== true) {
        break
      }
    }

    const pings: GoPing[] = last.pings !== undefined ? last.pings : []
    if (pings.length === 0) {
      console.warn(`[GoTunnel] WARN ${timeoutMs}ms 内没有拿到 ping 结果`)
      return
    }
    let ok = 0
    for (const p of pings) {
      if (p.ok) {
        ok++
      }
      const detail: string = p.ok
        ? `PASS ${p.latencyMs}ms${p.via !== undefined ? ' via=' + p.via : ''}`
        : `FAIL ${p.error !== undefined ? p.error : '(无 error)'}`
      console.info(`[GoTunnel] 隧道内 ping ${p.target}(${p.nodeName !== undefined ? p.nodeName : '?'}) ` +
        `${p.type} => ${detail}`)
    }
    if (ok > 0) {
      console.info(`[GoTunnel] PASS 数据面打通: ${ok}/${pings.length} 个在线 peer 在隧道内可达`)
    } else {
      console.error(`[GoTunnel] FAIL 数据面不通: ${pings.length} 个在线 peer 全部 ping 失败`)
    }
  }

  // 扩展进程 -> UI 进程的状态通道。
  //
  // 为什么不能直接让 UI 调 coreControlStatus: UI 进程 dlopen 的是它自己那份
  // libarkmesh_core.so, 里面没有 tsnet 实例, 永远返回 idle。真实状态只活在扩展
  // 进程里, 所以落到共享沙箱的一个 JSON 文件, 让 UI 轮询。
  private initStatusStore(): void {
    let dir = ''
    try {
      dir = this.context.filesDir
    } catch (err) {
      const e = err as BusinessError
      console.warn(`[GoTunnel] filesDir 不可用(${e.message}), 状态通道不启用`)
      return
    }
    this.statusStore = new TunnelStatusStore(dir)
    this.publishStatus()
    if (this.statusTimer === -1) {
      this.statusTimer = setInterval(() => this.publishStatus(), 2000)
    }
  }

  private publishStatus(): void {
    const store = this.statusStore
    if (store === null) {
      return
    }
    let st: GoCoreStatus
    try {
      st = JSON.parse(arkmeshNative.coreControlStatus()) as GoCoreStatus
    } catch (err) {
      return
    }
    // 顺带处理 UI 发来的探针请求, 再把最近一次探针结果塞进快照。放在这里是因为
    // 这个 2s 循环本来就在跑, 复用它读一个小文件比另起定时器省资源。
    this.handleDnsProbeRequest(st)
    if (this.lastDnsProbe !== '') {
      st.dnsProbe = this.lastDnsProbe
      st.dnsProbeSeq = this.lastDnsProbeSeq
    }
    store.write(st)
  }

  // 读 UI 的探针请求文件; 有新的 seq 就同步跑一次探针(隧道内注入合成查询)。
  // 只有连上(state=="up")时才有意义, 否则 Go 侧会直接返回未就绪。
  private handleDnsProbeRequest(st: GoCoreStatus): void {
    const store = this.statusStore
    if (store === null) {
      return
    }
    const req = store.readDnsProbeRequest()
    if (req === null || req.seq <= this.handledProbeSeq) {
      return
    }
    this.handledProbeSeq = req.seq
    if (st.state !== 'up') {
      this.lastDnsProbe = 'ERR 隧道未连接, 无法探针'
      this.lastDnsProbeSeq = req.seq
      return
    }
    const name: string = req.name !== undefined ? req.name : ''
    const result: string = arkmeshNative.coreDnsProbe(name, 3000)
    this.lastDnsProbe = result
    this.lastDnsProbeSeq = req.seq
    console.info(`[GoTunnel] MagicDNS 探针(手动 name=${name !== '' ? name : '(auto)'}): ${result}`)
  }

  // 连接就绪后自动跑一次 MagicDNS 探针(本机 DNSName + 第一个在线 peer),
  // 让用户不点任何按钮也能在设置页看到 mesh 名到底能不能解析。
  private runAutoDnsProbe(): void {
    const result: string = arkmeshNative.coreDnsProbe('', 3000)
    this.lastDnsProbe = result
    this.lastDnsProbeSeq = 0
    console.info(`[GoTunnel] MagicDNS 探针(自动): ${result}`)
  }

  // 隧道压根没建起来时(参数缺失、VPN create 失败), Go 侧不一定进入 failed,
  // 这里补一条明确的失败, 免得 UI 一直显示"连接中"。
  private publishFailure(msg: string): void {
    this.stopStatusWriter()
    const store = this.statusStore
    if (store === null) {
      return
    }
    const failed: GoCoreStatus = { state: 'failed', error: msg }
    store.write(failed)
  }

  private writeStopped(): void {
    this.stopStatusWriter()
    const store = this.statusStore
    if (store === null) {
      return
    }
    store.writeStopped()
  }

  private stopStatusWriter(): void {
    if (this.statusTimer !== -1) {
      clearInterval(this.statusTimer)
      this.statusTimer = -1
    }
  }

  // 隧道建好后每 10s 打一次 Go 侧的 TUN 读写计数。
  //
  // 为什么要这个: OHOS 的 VPN 按 UID 抓流量, hdc shell 里的 ping 未必会进 VPN,
  // 单看 ping 通不通分不清是"包没到 fd"还是"到了 fd 被隧道丢了"。计数器能分开这两种情况。
  private startGoStatsTicker(): void {
    if (this.goStatsTimer !== -1) {
      return
    }
    this.goStatsTimer = setInterval(() => {
      const raw = arkmeshNative.coreControlStatus()
      let st: GoCoreStatus
      try {
        st = JSON.parse(raw) as GoCoreStatus
      } catch (err) {
        return
      }
      console.info(`[GoTunnel] tick state=${st.state} peers=${st.peers !== undefined ? st.peers : 0} ` +
        `online=${st.peersOnline !== undefined ? st.peersOnline : 0} ` +
        `tun=${st.tunStats !== undefined ? st.tunStats : '(无)'}`)
    }, 10000)
  }

  private stopGoStatsTicker(): void {
    if (this.goStatsTimer !== -1) {
      clearInterval(this.goStatsTimer)
      this.goStatsTimer = -1
    }
  }

  // 用 headscale 分配到的 IP 构造 VPN 配置。
  //
  // 不开出口节点时, 路由只覆盖 tailnet 自己的地址段: IPv4 CGNAT 100.64.0.0/10
  // (含 MagicDNS 的 100.100.100.100)和 IPv6 ULA fd7a:115c:a1e0::/104, 公网流量
  // 照旧走 Wi-Fi/蜂窝。
  //
  // 开了出口节点就得再加"全流量"路由, 否则 OHOS 只会把 tailnet 网段的包送进 TUN,
  // Go 侧那条 0.0.0.0/0 的 AllowedIPs 根本等不到包。这里用 0.0.0.0/1 + 128.0.0.0/1
  // 两半拼出全覆盖, 而不是直接下 0.0.0.0/0 并把 isDefaultRoute 置真:
  // 两条 /1 比底层网络的 /0 更具体, 一样能吃掉所有目的地址, 但不需要系统把 VPN
  // 提成全局默认网络(WireGuard/Tailscale 的 Android、iOS 客户端都是这么做的)。
  // subnet router 广播的网段仍然没接进来 —— 那要按 netmap 逐个加, 是另一件事。
  private static buildTunnelConfig(nodeIPs: string[], magicSuffix: string, useExitNode: boolean,
    ownBundle: string): TunnelConfig {
    const magicDns: boolean = magicSuffix !== ''
    const addresses: AddressWithPrefix[] = []
    let hasV6 = false
    for (const ip of nodeIPs) {
      if (ip.indexOf(':') >= 0) {
        hasV6 = true
        addresses.push(new AddressWithPrefix(new Address(ip, 2), 128))
      } else {
        addresses.push(new AddressWithPrefix(new Address(ip, 1), 32))
      }
    }

    const routes: Route[] = [
      new Route('', new AddressWithPrefix(new Address('100.64.0.0', 1), 10),
        new Address('', 1), false, false)
    ]
    if (hasV6) {
      routes.push(new Route('', new AddressWithPrefix(new Address('fd7a:115c:a1e0::', 2), 104),
        new Address('', 2), false, false))
    }
    if (useExitNode) {
      routes.push(new Route('', new AddressWithPrefix(new Address('0.0.0.0', 1), 1),
        new Address('', 1), false, false))
      routes.push(new Route('', new AddressWithPrefix(new Address('128.0.0.0', 1), 1),
        new Address('', 1), false, false))
      if (hasV6) {
        routes.push(new Route('', new AddressWithPrefix(new Address('::', 2), 1),
          new Address('', 2), false, false))
        routes.push(new Route('', new AddressWithPrefix(new Address('8000::', 2), 1),
          new Address('', 2), false, false))
      }
    }

    // MagicDNS 服务器就是 100.100.100.100, 落在上面的 100.64.0.0/10 里, 包会进
    // 隧道并被 Go 侧 netstack 就地应答(netstack 无条件吸收 quad-100)。没开
    // MagicDNS 就不下发 DNS, 让系统沿用原来的解析器。
    //
    // 开出口节点时这条 DNS 还会顺带从 SERVFAIL 变成可用: tailscale 只在"用了出口
    // 节点"等少数几种情况下才把 headscale 下发的 FallbackResolvers 提为默认解析器
    // (ipnlocal 的 setDNSConfig), 我们的 headscale 没开 override_local_dns,
    // DefaultResolvers 是空的, 不提就没有上游可转发。
    const dns: string[] = magicDns ? ['100.100.100.100', '223.5.5.5'] : []

    // 给 MagicDNS 加一条显式 /32 主机路由。虽然 100.64.0.0/10 已经覆盖了
    // 100.100.100.100, 但部分 OHOS 版本在判断"DNS 查询要不要走 VPN"时, 只看
    // DNS 服务器本身有没有精确路由, 不看更大的超网。这条路由确保系统不会把 quad-100
    // 的 DNS 包绕过隧道直接发往物理网卡。
    if (magicDns) {
      routes.push(new Route('', new AddressWithPrefix(new Address('100.100.100.100', 1), 32),
        new Address('', 1), false, false))
    }

    // MTU 取 tailscale 的 safeTUNMTU: WireGuard 封装 + 可能走 DERP(TLS/TCP) +
    // 下层可能是 IPv6, 1280 是不需要分片的安全值。
    const cfg = new TunnelConfig(addresses, routes, 1280, dns)

    // 把 MagicDNS 后缀(如 mesh.example.com)作为 DNS 搜索域下发。
    //
    // 这是给第三方 App 解析 mesh 名唯一的 SDK 抓手: OHOS 的 VpnConfig 只暴露
    // dnsAddresses + searchDomains, 没有"按域把查询路由到指定 DNS"(split-DNS/NRPT)
    // 的接口。分割隧道下, 别的 App(如 RDP)默认走物理网络的 DNS, 问不到内网主机名。
    // 下发 searchDomains 有两个作用: (1) 裸名 gientech 能被补全成 FQDN;
    // (2) 期望平台据此把该后缀的查询交给 VPN 的 quad-100 —— 后者能否生效取决于
    // OHOS 内部实现, 只能真机验证。不加父域: 那会把控制面自己的
    // 公网域名也引到 quad-100, 而它对内网外的名字可能 SERVFAIL。
    cfg.searchDomains = magicDns ? [magicSuffix] : []

    // 把自己这个包列进 VPN 黑名单, 让本进程的 socket 一律绕过 VPN 直连底层网络。
    //
    // 这是出口节点能成立的前提: 上面那两条 /1 会把扩展进程自己建的控制面 HTTPS、
    // DERP、netcheck socket 一并抓进 TUN, 包源地址变成 TUN 的 100.64.0.10,
    // netstack 报 "open-conn-track: timeout opening ...; no associated peer node",
    // netmap 永远下不来, peers 恒为 0, 整机断网 —— 经典的 VPN 自举死锁。
    //
    // 不能用 protectProcessNet() 代替: 真机(API 24)实测它对 Go 经 libc 建的 fd
    // 完全无效, 调用成功但 SO_MARK 仍是 0, 连接照样超时。怀疑系统只认
    // @ohos.net.socket 登记过的 fd。
    // 也不必逐个 protect(fd): 那个 API 对裸 fd 确实有效(会把 SO_MARK 设成 0x20000),
    // 但 Go 自己 setsockopt(SO_MARK) 是 EPERM, 只能由 ArkTS 代劳, 而 tailscale 的
    // socket 都在 Go 内部创建, 要接进去得 vendor 一份 tailscale + cgo 回调 +
    // napi_threadsafe_function, 代价大得多。黑名单一行就够, 且两种模式都无害:
    // 不开出口节点时路由只有 100.64.0.0/10, 本进程压根不会往那个段发包。
    //
    // 代价: ArkMesh 自己这个 App 访问不了 tailnet 内的地址。它只跟公网上的
    // headscale 说话, 不需要。
    cfg.blockedApplications = [ownBundle]
    return cfg
  }

  private static extractHost(url: string): string {
    let s = url.trim()
    const scheme = s.indexOf('://')
    if (scheme >= 0) {
      s = s.substring(scheme + 3)
    }
    const slash = s.indexOf('/')
    if (slash >= 0) {
      s = s.substring(0, slash)
    }
    const colon = s.lastIndexOf(':')
    if (colon > 0) {
      s = s.substring(0, colon)
    }
    return s
  }

  private async cleanupVpn(): Promise<void> {
    try {
      console.info('[ArkMeshVpn] Cleaning up VPN...')

      // Destroy VPN connection
      if (this.vpnConnection) {
        await this.vpnConnection.destroy()
        console.info('[ArkMeshVpn] VPN connection destroyed')
      }
      
      this.tunnelFd = -1
      this.vpnConnection = null
      
      console.info('[ArkMeshVpn] VPN cleanup completed')
    } catch (err) {
      console.error('[ArkMeshVpn] VPN cleanup failed:', err)
    }
  }

  private getPrefixLength(cidr: string): number {
    const parts = cidr.split('/')
    if (parts.length > 1) {
      return parseInt(parts[1], 10)
    }
    return 32 // Default to /32
  }
}
