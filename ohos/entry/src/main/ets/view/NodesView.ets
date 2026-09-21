import {
  TunnelStatusStore,
  TunnelSnapshot,
  GoCoreStatus,
  GoPeer,
  GoPing,
  ExitNodeOption
} from '../core/TunnelStatusStore'
import { TunnelControl } from '../core/TunnelControl'
import { BusinessError } from '@kit.BasicServicesKit'
import { Card, SectionHead, InlineAction, StatusDot, EmptyState } from '../common/Components'
import { Palette, Type, Space, Radius, Size, Icon, Tone, toneColor } from '../common/Theme'
import { copyText } from '../common/Clipboard'

// 同一台设备在 peerList 里会出现两行(IPv4 一行、IPv6 一行), 直接铺出来数量翻倍
// 还看不出是几台机器。按主机名合并成一行, 两个地址都列在下面。
interface MergedPeer {
  name: string
  online: boolean
  ips: string[]
}

// 出口节点的一行。选中态用勾选图标而不是 Radio: 整行都是点击区, 也不用担心
// Radio 分组跨组件失效。
@Component
struct ExitRow {
  @Prop title: string = ''
  @Prop subtitle: string = ''
  @Prop selected: boolean = false
  onSelect: () => void = () => {
  }

  build() {
    Row({ space: Space.md }) {
      Column({ space: Space.xxs }) {
        Text(this.title)
          .fontSize(Type.body)
          .fontColor(Palette.textPrimary)
        if (this.subtitle !== '') {
          Text(this.subtitle)
            .fontSize(Type.micro)
            .fontColor(Palette.textQuaternary)
            .lineHeight(Type.microLine)
        }
      }
      .alignItems(HorizontalAlign.Start)
      .layoutWeight(1)

      if (this.selected) {
        SymbolGlyph(Icon.pass)
          .fontSize(Size.iconAction)
          .fontColor([Palette.accentIcon])
      }
    }
    .width('100%')
    .padding(Space.md)
    .backgroundColor(Palette.card)
    .borderRadius(Radius.md)
    .border({ width: 1, color: this.selected ? Palette.accent : Palette.divider })
    .constraintSize({ minHeight: Size.touchTarget })
    .alignItems(VerticalAlign.Center)
    .accessibilityText(`${this.title} ${this.selected ? '已选中' : '未选中'}`)
    .onClick(() => this.onSelect())
  }
}

// 节点列表的一行。点一下复制主地址 —— IP 基本都是为了粘到别处用的。
@Component
struct PeerRow {
  @Prop name: string = ''
  @Prop online: boolean = false
  @Prop address: string = ''
  @Prop path: string = ''
  onCopy: () => void = () => {
  }

  build() {
    Row({ space: Space.md }) {
      StatusDot({ tone: this.online ? Tone.Ok : Tone.Neutral })
      Column({ space: Space.xxs }) {
        Text(this.name)
          .fontSize(Type.body)
          .fontWeight(FontWeight.Medium)
          .fontColor(Palette.textPrimary)
          .maxLines(1)
          .textOverflow({ overflow: TextOverflow.Ellipsis })
        if (this.address !== '') {
          Text(this.address)
            .fontSize(Type.micro)
            .fontColor(Palette.textTertiary)
            .lineHeight(Type.microLine)
        }
      }
      .alignItems(HorizontalAlign.Start)
      .layoutWeight(1)

      Column({ space: Space.xxs }) {
        Text(this.online ? '在线' : '离线')
          .fontSize(Type.caption)
          .fontColor(toneColor(this.online ? Tone.Ok : Tone.Neutral))
        if (this.path !== '') {
          Text(this.path)
            .fontSize(Type.micro)
            .fontColor(Palette.textTertiary)
            .lineHeight(Type.microLine)
        }
      }
      .alignItems(HorizontalAlign.End)
      SymbolGlyph(Icon.copy)
        .fontSize(Size.iconInline)
        .fontColor([Palette.iconTertiary])
    }
    .width('100%')
    .padding(Space.md)
    .backgroundColor(Palette.field)
    .borderRadius(Radius.md)
    .constraintSize({ minHeight: Size.touchTarget })
    .alignItems(VerticalAlign.Center)
    .accessibilityText(`${this.name} ${this.online ? '在线' : '离线'} ${this.address} ${this.path}`)
    .onClick(() => this.onCopy())
  }
}

// 节点列表和出口节点候选都只能从扩展进程写的快照里拿: UI 进程自己那份
// libarkmesh_core.so 里没有 tsnet 实例, 直接查永远是空的。
@Component
export struct NodesView {
  // 只有真正显示着才需要读快照。Tab 切走还继续刷列表既费电又会让列表在
  // 后台跳来跳去, 所以由 Index 把"是否可见"传进来, 切回来时主动刷一次。
  @Prop @Watch('onTabVisible') tabVisible: boolean = false
  @State peers: GoPeer[] = []
  @State pathPings: GoPing[] = []
  @State exitOptions: ExitNodeOption[] = []
  @State activeExit: string = ''
  @State tunnelUp: boolean = false
  @State chosenIP: string = ''
  @State chosenName: string = ''
  @State busy: boolean = false
  @State searchQuery: string = ''
  private busyTimerId: number = -1
  private store: TunnelStatusStore | null = null

  aboutToAppear() {
    this.chosenIP = TunnelControl.exitNodeIP()
    this.chosenName = TunnelControl.exitNodeName()
    this.refresh()
  }

  aboutToDisappear() {
    this.stopBusyPoll()
  }

  onTabVisible() {
    if (this.tabVisible) {
      this.refresh()
      this.chosenIP = TunnelControl.exitNodeIP()
      this.chosenName = TunnelControl.exitNodeName()
    }
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
      console.warn('[Nodes] 拿不到 filesDir: ' + e.message)
      return null
    }
    return this.store
  }

  refresh() {
    const store = this.statusStore()
    if (store === null) {
      return
    }
    const snap: TunnelSnapshot | null = store.read()
    if (snap === null || snap.core.state !== 'up') {
      this.tunnelUp = false
      this.peers = []
      this.pathPings = []
      this.exitOptions = []
      this.activeExit = ''
      return
    }
    const core: GoCoreStatus = snap.core
    this.tunnelUp = true
    this.peers = core.peerList !== undefined ? core.peerList : []
    this.pathPings = core.pathPings !== undefined ? core.pathPings : []
    this.exitOptions = core.exitNodes !== undefined ? core.exitNodes : []
    this.activeExit = core.exitNode !== undefined ? core.exitNode : ''
  }

  // 把 disco ping 的路径结果翻译成一句话。via 的取值见 go/arkmesh-core 的
  // pingOne: "direct <端点>" 或 "derp <区域码>", 空表示响应没带路径信息。
  private pathText(m: MergedPeer): string {
    for (const g of this.pathPings) {
      if (m.ips.indexOf(g.target) < 0) {
        continue
      }
      if (!g.ok) {
        return '链路未测通'
      }
      const via: string = g.via !== undefined ? g.via : ''
      if (via.indexOf('direct') === 0) {
        return `直连 ${g.latencyMs}ms`
      }
      if (via.indexOf('derp') === 0) {
        return `DERP 中继 ${g.latencyMs}ms`
      }
      return `可达 ${g.latencyMs}ms`
    }
    return ''
  }

  private mergedPeers(): MergedPeer[] {
    const out: MergedPeer[] = []
    for (const p of this.peers) {
      const key: string = (p.name !== undefined && p.name !== '') ? p.name : p.ip
      let hit: MergedPeer | null = null
      for (const m of out) {
        if (m.name === key) {
          hit = m
          break
        }
      }
      if (hit === null) {
        const m: MergedPeer = { name: key, online: p.online, ips: [p.ip] }
        out.push(m)
      } else {
        if (hit.ips.indexOf(p.ip) < 0) {
          hit.ips.push(p.ip)
        }
        hit.online = hit.online || p.online
      }
    }
    // IPv4 排前面: 用户认的是 100.64.x.x, v6 地址只是补充信息
    for (const m of out) {
      m.ips.sort((a: string, b: string): number =>
        (a.indexOf(':') >= 0 ? 1 : 0) - (b.indexOf(':') >= 0 ? 1 : 0))
    }
    return out
  }

  private filteredPeers(): MergedPeer[] {
    const all: MergedPeer[] = this.mergedPeers()
    const q = this.searchQuery.trim().toLowerCase()
    if (q === '') {
      return all
    }
    return all.filter((m: MergedPeer): boolean =>
      m.name.toLowerCase().indexOf(q) >= 0 || m.ips.join(' ').toLowerCase().indexOf(q) >= 0)
  }

  private onlineCount(): number {
    let n = 0
    for (const m of this.mergedPeers()) {
      if (m.online) {
        n++
      }
    }
    return n
  }

  // 切换出口节点后隧道要重连十几秒, 这段时间临时轮询快照看它起来没有;
  // 平时不轮询, 列表靠进 Tab 和手动刷新更新。
  private startBusyPoll() {
    this.stopBusyPoll()
    this.busyTimerId = setInterval(() => {
      this.refresh()
    }, 3000)
  }

  private stopBusyPoll() {
    if (this.busyTimerId !== -1) {
      clearInterval(this.busyTimerId)
      this.busyTimerId = -1
    }
  }

  // 选中/取消出口节点都要把整条隧道重启一遍: OHOS 的 VpnConfig 建完就不能改,
  // 而"全流量默认路由"只能在 create() 时一次性给全。
  private chooseExit(ip: string, name: string) {
    if (this.busy || ip === this.chosenIP) {
      return
    }
    TunnelControl.setExitNode(ip, name)
    this.chosenIP = ip
    this.chosenName = name

    if (!this.tunnelUp) {
      this.toast(ip === '' ? '已取消出口节点, 下次连接生效' : `已选择 ${name !== '' ? name : ip}, 下次连接生效`)
      return
    }

    this.busy = true
    this.startBusyPoll()
    this.toast(ip === '' ? '正在取消出口节点, 隧道重连中' : `正在切换到 ${name !== '' ? name : ip}, 隧道重连中`)
    TunnelControl.start()
      .then(() => {
        console.info(`[Nodes] 隧道已按出口节点 ${ip !== '' ? ip : '(无)'} 重启`)
      })
      .catch((e: Error) => {
        this.toast('重启隧道失败: ' + e.message)
        console.error('[Nodes] 重启隧道失败: ' + e.message)
      })
      .finally(() => {
        this.stopBusyPoll()
        this.refresh()
        this.busy = false
      })
  }

  private copyPeer(m: MergedPeer): void {
    if (m.ips.length === 0) {
      return
    }
    copyText(m.ips[0])
      .then(() => this.toast(`${m.name} 的地址已复制`))
      .catch((e: Error) => this.toast('复制失败: ' + e.message))
  }

  private toast(msg: string): void {
    try {
      this.getUIContext().getPromptAction().showToast({ message: msg, duration: 2500 })
    } catch (err) {
      const e = err as BusinessError
      console.error('[Nodes] toast 失败: ' + e.message)
    }
  }

  build() {
    Scroll() {
      Column({ space: Space.lg }) {
        // 顶栏
        Row() {
          Text('节点')
            .fontSize(Type.title)
            .fontWeight(FontWeight.Bold)
            .fontColor(Palette.textPrimary)
            .layoutWeight(1)
          InlineAction({
            label: '刷新',
            symbol: Icon.refresh,
            busy: this.busy,
            action: (): void => this.refresh()
          })
        }
        .width('100%')
        .padding({ top: Space.md })
        .alignItems(VerticalAlign.Center)

        // ==== 出口节点 ====
        Card() {
          SectionHead({
            title: '出口节点',
            desc: '选定后所有上网流量都从该节点出去, 相当于换了个出口 IP。切换会重连隧道, 约需十几秒。'
          })

          Row({ space: Space.sm }) {
            Text('当前生效')
              .fontSize(Type.caption)
              .fontColor(Palette.textSecondary)
            Blank()
            StatusDot({ tone: this.activeExit !== '' ? Tone.Ok : Tone.Neutral })
            Text(this.activeExit !== '' ? this.activeExit : '未启用')
              .fontSize(Type.caption)
              .fontColor(this.activeExit !== '' ? Palette.textPrimary : Palette.textTertiary)
          }
          .width('100%')
          .padding(Space.md)
          .backgroundColor(Palette.field)
          .borderRadius(Radius.md)
          .alignItems(VerticalAlign.Center)

          ExitRow({
            title: '不使用出口节点',
            subtitle: '流量直连, 只在私有网络内部互通',
            selected: this.chosenIP === '',
            onSelect: (): void => this.chooseExit('', '')
          })

          if (!this.tunnelUp && this.exitOptions.length === 0) {
            Text('候选列表要连上隧道之后才能拿到, 先回"概览"点连接。')
              .fontSize(Type.micro)
              .fontColor(Palette.textQuaternary)
              .lineHeight(Type.microLine)
          }

          ForEach(this.exitOptions, (o: ExitNodeOption) => {
            ExitRow({
              title: o.name !== undefined && o.name !== '' ? o.name : o.ip,
              subtitle: o.ip,
              selected: this.chosenIP === o.ip,
              onSelect: (): void => this.chooseExit(o.ip, o.name !== undefined ? o.name : '')
            })
          }, (o: ExitNodeOption) => o.ip)

          if (this.busy) {
            Row({ space: Space.sm }) {
              LoadingProgress()
                .width(Size.iconAction)
                .height(Size.iconAction)
                .color(Palette.accentIcon)
              Text('隧道正在重连, 稍等十几秒')
                .fontSize(Type.caption)
                .fontColor(Palette.textSecondary)
            }
            .width('100%')
            .alignItems(VerticalAlign.Center)
          }
        }

        // ==== 节点列表 ====
        Card() {
          SectionHead({
            title: '网络节点',
            desc: this.tunnelUp ? `${this.onlineCount()} / ${this.mergedPeers().length} 台在线` : '未连接'
          })

          if (!this.tunnelUp) {
            EmptyState({
              title: '隧道未连接',
              desc: '连上之后这里会列出私有网络里的所有设备。',
              symbol: Icon.tabNodes
            })
          } else {
            Search({ value: this.searchQuery, placeholder: '搜索节点名称或地址' })
              .width('100%')
              .height(Size.controlHeight)
              .textFont({ size: Type.body })
              .fontColor(Palette.textPrimary)
              .backgroundColor(Palette.field)
              .placeholderColor(Palette.textQuaternary)
              .onChange((value: string) => {
                this.searchQuery = value
              })

            if (this.filteredPeers().length === 0) {
              EmptyState({
                title: this.searchQuery.trim() === '' ? '没有节点' : '没有匹配的节点',
                desc: this.searchQuery.trim() === ''
                  ? '私有网络里目前只有这台设备。'
                  : '换个关键词试试, 支持名称和 IP。',
                symbol: Icon.search
              })
            } else {
              ForEach(this.filteredPeers(), (m: MergedPeer) => {
                PeerRow({
                  name: m.name,
                  online: m.online,
                  address: m.ips.join('  ·  '),
                  path: this.pathText(m),
                  onCopy: (): void => this.copyPeer(m)
                })
              }, (m: MergedPeer) => m.name)
            }
          }
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
