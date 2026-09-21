import { ArkMeshStatus } from '../core/ArkMeshStatus'
import { storageUtils } from '../core/StorageUtils'
import {
  TunnelStatusStore,
  TunnelSnapshot,
  GoCoreStatus,
  ExitNodeOption,
  SNAPSHOT_STALE_MS
} from '../core/TunnelStatusStore'
import { TunnelControl } from '../core/TunnelControl'
import { BusinessError } from '@kit.BasicServicesKit'
import { Card, SectionHead, ActionButton, KeyValueRow, StatusDot, ButtonVariant } from '../common/Components'
import { Palette, Type, Space, Radius, Size, Icon, Tone, toneIcon } from '../common/Theme'
import { copyText } from '../common/Clipboard'
import { requestDiagnostics } from '../common/UiBus'

// 点了"连接"到扩展进程真正写出第一份状态快照之间的容忍窗口。扩展要冷启动 +
// dlopen Go 核心, 慢的时候要好几秒, 这期间读到的还是上一轮的旧快照。
const START_GRACE_MS = 20000

// 概览页: 只回答用户最关心的三件事 —— 连没连上、连的是谁、怎么开关。
// 隧道状态一律以扩展进程写的快照为准, 这里不做乐观翻转。
@Component
export struct HomeView {
  @Link @Watch('onTabChange') activeTab: number
  @State status: ArkMeshStatus = ArkMeshStatus.DISCONNECTED
  @State statusMessage: string = '未连接'
  @State statusDetail: string = ''
  @State meshIP: string = ''
  @State dnsName: string = ''
  @State onlineCount: number = 0
  @State peerTotal: number = 0
  @State exitNodeName: string = ''
  @State serverUrl: string = ''
  @State authKey: string = ''
  private statusTimerId: number = -1
  private pendingStartAt: number = 0
  private store: TunnelStatusStore | null = null

  aboutToAppear() {
    this.loadConfig()
    this.startStatusPolling()
  }

  aboutToDisappear() {
    if (this.statusTimerId !== -1) {
      clearTimeout(this.statusTimerId)
      this.statusTimerId = -1
    }
  }

  loadConfig() {
    this.serverUrl = storageUtils.getString('serverUrl', '')
    this.authKey = storageUtils.getString('authKey', '')
    this.exitNodeName = TunnelControl.exitNodeName()
  }

  private configured(): boolean {
    return this.serverUrl !== '' && this.authKey !== ''
  }

  connect() {
    if (this.status === ArkMeshStatus.CONNECTED || this.status === ArkMeshStatus.CONNECTING) {
      this.stopTunnel()
      return
    }
    if (!this.configured()) {
      this.status = ArkMeshStatus.ERROR
      this.statusMessage = '还没有配置'
      this.statusDetail = '请先在"设置"里填写控制面地址和预授权密钥'
      return
    }
    // 首页不放输入框, 配置只在设置页改; 这里只是把已保存的值交给扩展。
    this.status = ArkMeshStatus.CONNECTING
    this.statusMessage = '正在连接'
    this.statusDetail = '正在与服务器握手, 通常需要十几秒'
    this.pendingStartAt = Date.now()
    TunnelControl.start()
      .then(() => {
        console.info('[GoTunnel] VPN 扩展已启动(goTunnel 模式)')
      })
      .catch((e: Error) => {
        this.status = ArkMeshStatus.ERROR
        this.statusMessage = '连接失败'
        this.statusDetail = e.message
        this.pendingStartAt = 0
        console.error('[GoTunnel] VPN 扩展启动失败: ' + e.message)
      })
  }

  stopTunnel() {
    this.pendingStartAt = 0
    this.statusMessage = '正在断开'
    this.statusDetail = '正在通知扩展进程释放隧道'
    TunnelControl.stop()
      .then(() => {
        console.info('[GoTunnel] 已请求停止 VPN 扩展')
      })
      .catch((e: Error) => {
        this.status = ArkMeshStatus.ERROR
        this.statusMessage = '断开失败'
        this.statusDetail = e.message
        console.error('[GoTunnel] 停止 VPN 扩展失败: ' + e.message)
      })
  }

  private statusStore(): TunnelStatusStore | null {
    if (this.store !== null) {
      return this.store
    }
    try {
      const ctx = this.getUIContext().getHostContext()
      if (ctx === undefined) {
        return null
      }
      this.store = new TunnelStatusStore(ctx.filesDir)
    } catch (err) {
      const e = err as Error
      console.warn('[TunnelStatus] 拿不到 filesDir: ' + e.message)
      return null
    }
    return this.store
  }

  private startStatusPolling(): void {
    this.refreshTunnelStatus()
    this.scheduleNext()
  }

  // 概览不在前台时不读快照(节点/设置页各自管自己的数据); 连接过程中 2s 一档
  // 跟状态, 稳定后放慢到 4s —— 状态文件本来也是扩展 2s 才写一次。
  private scheduleNext(): void {
    if (this.statusTimerId !== -1) {
      return
    }
    const delay: number = this.status === ArkMeshStatus.CONNECTING ? 2000 : 4000
    this.statusTimerId = setTimeout(() => {
      this.statusTimerId = -1
      if (this.activeTab === 0) {
        this.refreshTunnelStatus()
      }
      this.scheduleNext()
    }, delay)
  }

  onTabChange() {
    if (this.activeTab === 0) {
      // 切回来先同步一次: 离开期间配置可能在设置页改过, 状态也可能变过
      this.loadConfig()
      this.refreshTunnelStatus()
    }
  }

  refreshTunnelStatus(): void {
    const store = this.statusStore()
    if (store === null) {
      return
    }
    const snap: TunnelSnapshot | null = store.read()
    if (snap === null) {
      this.showNoTunnel('未连接')
      return
    }
    // stopped 是扩展 onDestroy 亲手写的最后一句话, 不参与过期判定 —— 否则正常断开
    // 8s 后会被下面的 stale 分支改写成"隧道已停止", 听起来像出了故障。
    if (snap.core.state === 'stopped') {
      this.showNoTunnel('已断开')
      return
    }
    // 快照过期说明扩展进程已经不在了(被系统回收或崩溃), 这时绝不能继续显示"已连接"。
    if (Date.now() - snap.updatedAt > SNAPSHOT_STALE_MS) {
      this.showNoTunnel('隧道已停止')
      return
    }

    this.pendingStartAt = 0
    const core: GoCoreStatus = snap.core
    if (core.state === 'up') {
      this.status = ArkMeshStatus.CONNECTED
      this.statusMessage = '已连接'
      this.statusDetail = '流量已进入你的私有网络'
      const ips: string[] = core.nodeIPs !== undefined ? core.nodeIPs : []
      this.meshIP = ips.length > 0 ? ips[0] : ''
      const rawName: string = core.dnsName !== undefined ? core.dnsName : ''
      // tailscale 给的 DNSName 带结尾点(绝对域名), 展示时去掉
      this.dnsName = rawName.endsWith('.') ? rawName.substring(0, rawName.length - 1) : rawName
      this.onlineCount = core.peersOnline !== undefined ? core.peersOnline : 0
      this.peerTotal = core.peers !== undefined ? core.peers : 0
      // 首页显示"隧道真正在用的出口", 不是设置里选的那个: 选完要重连才生效,
      // 两者不一致时照设置显示会让用户以为已经走出口了。
      const activeIP: string = core.exitNode !== undefined ? core.exitNode : ''
      if (activeIP === '') {
        this.exitNodeName = ''
      } else {
        const opts: ExitNodeOption[] = core.exitNodes !== undefined ? core.exitNodes : []
        let nm: string = ''
        for (const o of opts) {
          if (o.ip === activeIP) {
            nm = o.name !== undefined ? o.name : ''
            break
          }
        }
        this.exitNodeName = nm !== '' ? nm : activeIP
      }
      return
    }
    if (core.state === 'failed') {
      this.status = ArkMeshStatus.ERROR
      this.statusMessage = '连接失败'
      this.statusDetail = core.error !== undefined && core.error !== '' ? core.error : '隧道未能建立'
      return
    }
    // idle / starting / reattaching: 扩展已经起来了但隧道还没就绪
    this.status = ArkMeshStatus.CONNECTING
    this.statusMessage = core.state === 'reattaching' ? '正在重建隧道' : '正在连接'
    this.statusDetail = '扩展进程已就绪, 等待隧道建立'
  }

  // 扩展进程不在或已停止。刚点了"连接"的那一小段时间里要显示"正在启动":
  // 扩展冷启动 + dlopen Go 核心要好几秒, 这期间读到的还是上一轮的旧快照,
  // 直接按"未连接"渲染会让按钮闪一下。
  private showNoTunnel(msg: string): void {
    if (this.pendingStartAt !== 0 && Date.now() - this.pendingStartAt < START_GRACE_MS) {
      this.status = ArkMeshStatus.CONNECTING
      this.statusMessage = '正在启动隧道'
      this.statusDetail = '正在拉起后台扩展进程'
      return
    }
    this.status = ArkMeshStatus.DISCONNECTED
    this.statusMessage = msg
    this.statusDetail = '连接后可以访问私有网络里的其他设备'
    this.meshIP = ''
    this.dnsName = ''
    this.onlineCount = 0
    this.peerTotal = 0
  }

  // "未连接"是常态而不是故障, 所以用中性色; 只有真失败才标红。
  private statusTone(): Tone {
    switch (this.status) {
      case ArkMeshStatus.CONNECTED:
        return Tone.Ok
      case ArkMeshStatus.CONNECTING:
        return Tone.Accent
      case ArkMeshStatus.ERROR:
        return Tone.Danger
      default:
        return Tone.Neutral
    }
  }

  private statusGlyph(): Resource {
    switch (this.status) {
      case ArkMeshStatus.CONNECTED:
        return Icon.statusOn
      case ArkMeshStatus.ERROR:
        return Icon.statusError
      default:
        return Icon.statusOff
    }
  }

  private connectLabel(): string {
    if (this.status === ArkMeshStatus.CONNECTED) {
      return '断开连接'
    }
    if (this.status === ArkMeshStatus.CONNECTING) {
      return '取消连接'
    }
    return '连接'
  }

  // 断开不是破坏性操作(随时能再连上), 所以不用红色实心, 免得用户不敢点。
  private connectVariant(): ButtonVariant {
    return this.status === ArkMeshStatus.DISCONNECTED || this.status === ArkMeshStatus.ERROR
      ? ButtonVariant.Primary
      : ButtonVariant.Neutral
  }

  private copy(value: string, what: string): void {
    if (value === '') {
      return
    }
    copyText(value)
      .then(() => this.toast(`${what}已复制`))
      .catch((e: Error) => this.toast('复制失败: ' + e.message))
  }

  private toast(msg: string): void {
    try {
      this.getUIContext().getPromptAction().showToast({ message: msg, duration: 2000 })
    } catch (err) {
      const e = err as BusinessError
      console.error('[Home] toast 失败: ' + e.message)
    }
  }

  build() {
    Scroll() {
      Column({ space: Space.lg }) {
        // 顶栏: 品牌标识, 导航交给底部 Tab
        Row({ space: Space.sm }) {
          Image($r('app.media.startIcon'))
            .width(Size.iconHero)
            .height(Size.iconHero)
            .borderRadius(Radius.sm)
          Text('ArkMesh')
            .fontSize(Type.title)
            .fontWeight(FontWeight.Bold)
            .fontColor(Palette.textPrimary)
        }
        .width('100%')
        .padding({ top: Space.md, bottom: Space.xs })

        // ==== 状态主卡片 ====
        Card() {
          Column({ space: Space.lg }) {
            Column({ space: Space.md }) {
              if (this.status === ArkMeshStatus.CONNECTING) {
                LoadingProgress()
                  .width(Size.iconStatus)
                  .height(Size.iconStatus)
                  .color(Palette.accentIcon)
              } else {
                SymbolGlyph(this.statusGlyph())
                  .fontSize(Size.iconStatus)
                  .fontColor([toneIcon(this.statusTone())])
              }
              Column({ space: Space.xs }) {
                Text(this.statusMessage)
                  .fontSize(Type.title)
                  .fontWeight(FontWeight.Bold)
                  .fontColor(Palette.textPrimary)
                  .textAlign(TextAlign.Center)
                if (this.statusDetail !== '') {
                  Text(this.statusDetail)
                    .fontSize(Type.caption)
                    .fontColor(Palette.textSecondary)
                    .textAlign(TextAlign.Center)
                    .lineHeight(Type.captionLine)
                    .copyOption(this.status === ArkMeshStatus.ERROR ? CopyOptions.LocalDevice : CopyOptions.None)
                }
              }
              .width('100%')
            }
            .width('100%')
            .alignItems(HorizontalAlign.Center)

            ActionButton({
              label: this.connectLabel(),
              variant: this.connectVariant(),
              symbol: this.status === ArkMeshStatus.DISCONNECTED || this.status === ArkMeshStatus.ERROR
                ? Icon.statusOn : Icon.statusOff,
              action: (): void => this.connect()
            })
          }
          .width('100%')
        }

        // ==== 连接失败: 给一条明确的出路, 别只丢一句错误 ====
        if (this.status === ArkMeshStatus.ERROR) {
          Card() {
            Row({ space: Space.sm }) {
              SymbolGlyph(Icon.statusError)
                .fontSize(Size.iconAction)
                .fontColor([Palette.danger])
              Column({ space: Space.xxs }) {
                Text('连接没有成功')
                  .fontSize(Type.body)
                  .fontWeight(FontWeight.Medium)
                  .fontColor(Palette.textPrimary)
                Text('先跑一遍诊断, 能分清是地址填错了、网络不通, 还是核心没跑起来。')
                  .fontSize(Type.caption)
                  .fontColor(Palette.textSecondary)
                  .lineHeight(Type.captionLine)
              }
              .alignItems(HorizontalAlign.Start)
              .layoutWeight(1)
            }
            .width('100%')
            .alignItems(VerticalAlign.Top)

            ActionButton({
              label: '查看诊断',
              variant: ButtonVariant.Neutral,
              symbol: Icon.diagnostics,
              action: (): void => {
                requestDiagnostics()
                this.activeTab = 2
              }
            })
          }
        }

        // ==== 连接信息 ====
        if (this.status === ArkMeshStatus.CONNECTED) {
          Card() {
            SectionHead({ title: '连接信息', desc: '点一下带复制图标的行可以直接复制。' })
            KeyValueRow({
              label: '本机 IP',
              value: this.meshIP !== '' ? this.meshIP : '未分配',
              copyable: this.meshIP !== '',
              onCopy: (): void => this.copy(this.meshIP, '本机 IP ')
            })
            if (this.dnsName !== '') {
              KeyValueRow({
                label: '设备域名',
                value: this.dnsName,
                copyable: true,
                onCopy: (): void => this.copy(this.dnsName, '设备域名')
              })
            }
            Divider().color(Palette.divider).strokeWidth(0.5)
            KeyValueRow({
              label: '在线节点',
              value: `${this.onlineCount} / ${this.peerTotal}`,
              arrow: true,
              onTap: (): void => {
                this.activeTab = 1
              }
            })
            KeyValueRow({
              label: '出口节点',
              value: this.exitNodeName !== '' ? this.exitNodeName : '未启用',
              valueTone: this.exitNodeName !== '' ? Tone.Accent : Tone.Neutral,
              arrow: true,
              onTap: (): void => {
                this.activeTab = 1
              }
            })
          }
        }

        // ==== 未配置: 一条明确的出路 ====
        if (!this.configured()) {
          Card() {
            SectionHead({
              title: '还没有配置服务器',
              desc: '填写自建控制面的地址和预授权密钥之后才能连接。'
            })
            ActionButton({
              label: '去设置',
              variant: ButtonVariant.Primary,
              symbol: Icon.tabSettings,
              action: (): void => {
                this.activeTab = 2
              }
            })
          }
        }

        // ==== 已连接时的一行安全提示 ====
        if (this.status === ArkMeshStatus.CONNECTED) {
          Row({ space: Space.sm }) {
            SymbolGlyph(Icon.secure)
              .fontSize(Size.iconInline)
              .fontColor([toneIcon(Tone.Ok)])
            Text('端到端加密已启用, 流量只在你的设备之间直连')
              .fontSize(Type.micro)
              .fontColor(Palette.textTertiary)
              .layoutWeight(1)
            StatusDot({ tone: Tone.Ok })
          }
          .width('100%')
          .padding({ left: Space.xs, right: Space.xs })
          .alignItems(VerticalAlign.Center)
        }
      }
      .width('100%')
      .padding({ left: Space.page, right: Space.page, bottom: Space.xxl })
    }
    .width('100%')
    .height('100%')
    .scrollBar(BarState.Off)
    .align(Alignment.Top)
    .backgroundColor(Palette.page)
    .edgeEffect(EdgeEffect.Spring)
  }
}
