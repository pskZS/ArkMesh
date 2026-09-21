import { storageUtils } from '../core/StorageUtils'
import { arkmeshNative } from '../core/ArkMeshNative'
import { TunnelControl } from '../core/TunnelControl'
import { TunnelStatusStore, TunnelSnapshot } from '../core/TunnelStatusStore'
import { bundleManager, Want } from '@kit.AbilityKit'
import { vpnExtension } from '@kit.NetworkKit'
import { BusinessError } from '@kit.BasicServicesKit'
import { promptAction } from '@kit.ArkUI'
import {
  Card,
  SectionHead,
  ActionButton,
  FormField,
  KeyValueRow,
  DiagSection,
  InlineAction,
  ButtonVariant,
  hasDiag
} from '../common/Components'
import { Palette, Type, Space, Radius, Size, Icon, Tone } from '../common/Theme'
import { copyText } from '../common/Clipboard'

// 设置页: 配置 + 高级诊断 + 关于。
//
// 只保留真正生效的项。之前的"DNS 服务器 / MTU / 日志级别"三栏在实现里从来没
// 被读过(MTU 固定取 tailscale 的 safeTUNMTU=1280, DNS 固定是 MagicDNS 的
// 100.100.100.100), 留着只会让用户以为改了有用, 所以删掉。
//
// 诊断默认折叠: 它是排查问题用的工具, 不该在正常使用时占满一屏工程味十足的文本。
@Component
export struct SettingsView {
  @State serverUrl: string = ''
  @State authKey: string = ''
  @State machineName: string = ''
  @State versionName: string = ''

  // 诊断。coreResult/netResult/ifResult 是三个探针的原始输出(用 " | " 拼接),
  // 交给 DiagSection 解析成带通过/失败标记的逐条明细。
  @State coreResult: string = ''
  @State netResult: string = ''
  @State ifResult: string = ''
  @State controlResult: string = ''
  @State diagStage: string = ''
  @State diagBusy: boolean = false
  @State diagExpanded: boolean = false

  // MagicDNS 隧道内解析测试。结果来自扩展进程(隧道在那边), 经快照文件回传:
  // UI 写请求文件 -> 扩展在 2s 循环里跑探针 -> 结果连 seq 一起进快照 -> UI 轮询取回。
  @Prop @Watch('onTabVisible') tabVisible: boolean = false
  @State dnsProbeName: string = ''
  @State dnsProbeResult: string = ''
  @State dnsProbePending: boolean = false

  // 概览页点"查看诊断"时写一个新 token 进来, 这里收到变化就把高级诊断展开。
  // 键名必须和 common/UiBus.ets 里的 DIAG_FOCUS_KEY 一致(装饰器只吃字面量)。
  @StorageProp('arkmeshDiagFocus') @Watch('onDiagFocus') diagFocus: number = 0

  private savedServerUrl: string = ''
  private savedAuthKey: string = ''
  private savedMachineName: string = ''
  private store: TunnelStatusStore | null = null
  private probeSeq: number = 0
  private probeTimerId: number = -1

  aboutToAppear() {
    this.loadSettings()
    // 概览页可能在设置页被创建之前就发过"查看诊断", 那时 @Watch 不会触发,
    // 只能靠初值补一次。
    if (this.diagFocus > 0) {
      this.diagExpanded = true
    }
    try {
      const info = bundleManager.getBundleInfoForSelfSync(bundleManager.BundleFlag.GET_BUNDLE_INFO_DEFAULT)
      this.versionName = info.versionName
    } catch (err) {
      const e = err as BusinessError
      console.warn('[Settings] 读版本号失败: ' + e.message)
    }
  }

  aboutToDisappear() {
    this.stopProbePoll()
  }

  // 切到设置页时, 把扩展自动跑过的那次探针结果显示出来(不覆盖正在进行的手动测试)。
  onTabVisible() {
    if (this.tabVisible && !this.dnsProbePending) {
      this.loadLatestProbe()
    }
  }

  onDiagFocus() {
    this.diagExpanded = true
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
      console.warn('[Settings] 拿不到 filesDir: ' + e.message)
      return null
    }
    return this.store
  }

  private loadLatestProbe(): void {
    const store = this.statusStore()
    if (store === null) {
      return
    }
    const snap: TunnelSnapshot | null = store.read()
    if (snap === null) {
      return
    }
    const probe: string = snap.core.dnsProbe !== undefined ? snap.core.dnsProbe : ''
    if (probe !== '') {
      const seq: number = snap.core.dnsProbeSeq !== undefined ? snap.core.dnsProbeSeq : 0
      this.dnsProbeResult = (seq === 0 ? '[连接时自动] ' : '') + probe
    }
  }

  // 发起一次针对指定名字的隧道内解析测试。留空则让 Go 自动挑目标。
  runDnsProbe(): void {
    const store = this.statusStore()
    if (store === null) {
      this.dnsProbeResult = '无法访问状态文件'
      return
    }
    const snap: TunnelSnapshot | null = store.read()
    if (snap === null || snap.core.state !== 'up') {
      this.dnsProbeResult = '请先在"概览"页连接后再测试'
      return
    }
    this.probeSeq = Date.now()
    store.writeDnsProbeRequest(this.dnsProbeName.trim(), this.probeSeq)
    this.dnsProbePending = true
    this.dnsProbeResult = '测试中...(隧道内注入查询, 最多几秒)'
    this.pollProbeResult(0)
  }

  // 有界轮询: 每 700ms 读一次快照, 直到拿到本次 seq 的结果或超时(~12s)。
  // 只在等结果时跑, 拿到或超时就停, 不做常驻轮询。
  private pollProbeResult(elapsed: number): void {
    this.stopProbePoll()
    if (elapsed > 12000) {
      this.dnsProbePending = false
      this.dnsProbeResult = '等待超时: 扩展进程未回传结果(可能已断开)'
      return
    }
    this.probeTimerId = setTimeout(() => {
      this.probeTimerId = -1
      const store = this.statusStore()
      if (store !== null) {
        const snap: TunnelSnapshot | null = store.read()
        if (snap !== null && snap.core.dnsProbeSeq === this.probeSeq
          && snap.core.dnsProbe !== undefined) {
          this.dnsProbePending = false
          this.dnsProbeResult = snap.core.dnsProbe
          return
        }
      }
      this.pollProbeResult(elapsed + 700)
    }, 700)
  }

  private stopProbePoll(): void {
    if (this.probeTimerId !== -1) {
      clearTimeout(this.probeTimerId)
      this.probeTimerId = -1
    }
  }

  loadSettings() {
    this.serverUrl = storageUtils.getString('serverUrl', '')
    this.authKey = storageUtils.getString('authKey', '')
    // 默认真机名 + 旧占位值迁移都在 TunnelControl.machineName() 里,
    // 连接路径也走它, 两边不会漂移。
    this.machineName = TunnelControl.machineName()
    this.savedServerUrl = this.serverUrl
    this.savedAuthKey = this.authKey
    this.savedMachineName = this.machineName
  }

  private dirty(): boolean {
    return this.serverUrl !== this.savedServerUrl
      || this.authKey !== this.savedAuthKey
      || this.machineName !== this.savedMachineName
  }

  saveSettings() {
    if (!this.dirty()) {
      return
    }
    const url = this.serverUrl.trim()
    const key = this.authKey.trim()
    const name = this.machineName.trim()
    storageUtils.setString('serverUrl', url)
    storageUtils.setString('authKey', key)
    storageUtils.setString('machineName', name)
    storageUtils.flush()
    this.savedServerUrl = url
    this.savedAuthKey = key
    this.savedMachineName = name
    this.serverUrl = url
    this.authKey = key
    this.machineName = name
    this.toast('设置已保存, 下次连接生效')
  }

  // 地址栏的行内校验。空值不算错(由概览页的"还没有配置"接管), 但一旦填了就必须
  // 是能解析的形状 —— 上架测试里填进去的中文串会把底层 cgo 的 getaddrinfo 卡死。
  private urlError(): string {
    const url = this.serverUrl.trim()
    if (url === '') {
      return ''
    }
    const schemeAt = url.indexOf('://')
    if (schemeAt > 0) {
      const scheme = url.substring(0, schemeAt).toLowerCase()
      if (scheme !== 'https' && scheme !== 'http') {
        return '协议要写 https:// 或 http://'
      }
    }
    const host = TunnelControl.extractHost(url)
    if (host === '') {
      return '缺少主机名, 例如 https://headscale.example.com'
    }
    if (!TunnelControl.isPlausibleHost(host)) {
      return '只接受 ASCII 域名或 IP, 不能包含空格或中文'
    }
    return ''
  }

  // 诊断: 加载 Go 核心跑自检, 并单独测到控制面的网络可达性。
  // 排查"连不上"时先看这里, 能分清是核心没跑起来还是网络不通。
  // 三个探针都是异步的(原生侧跑在 NAPI worker 线程), 主线程只等结果 ——
  // 同步调用会阻塞 UI 超过 8s, 被系统按 APP_INPUT_BLOCK 杀进程。
  async runDiagnostics() {
    if (this.diagBusy) {
      return
    }
    this.diagBusy = true
    try {
      this.coreResult = ''
      this.netResult = ''
      this.ifResult = ''

      this.diagStage = '正在自检 Go 核心'
      this.coreResult = await arkmeshNative.coreSelfTest()
      console.info('[TsCore] ' + this.coreResult)

      this.diagStage = '正在探测网络接口'
      this.ifResult = await arkmeshNative.coreIfProbe()
      console.info('[TsCore] ifprobe: ' + this.ifResult)

      const host = TunnelControl.extractHost(this.serverUrl)
      if (host === '') {
        this.netResult = '未配置控制面地址: 已跳过网络检测'
        console.warn('[TsCore] ' + this.netResult)
        return
      }
      if (!TunnelControl.isPlausibleHost(host)) {
        this.netResult = '地址格式不对 FAIL: 只接受 ASCII 域名或 IP(如 headscale.example.com)'
        console.warn('[TsCore] 非法 host, 跳过 netcheck')
        return
      }
      this.diagStage = '正在检测到服务器的网络(DNS/TCP/TLS/HTTPS, 最坏约 1 分钟)'
      this.netResult = await arkmeshNative.coreNetCheck(host)
      console.info('[TsCore] netcheck ' + host + ': ' + this.netResult)
    } catch (err) {
      const e = err as BusinessError
      console.error('[TsCore] 自检异常: ' + e.message)
      if (this.coreResult === '') {
        this.coreResult = 'ERR 自检异常: ' + e.message
      }
    } finally {
      this.diagStage = ''
      this.diagBusy = false
    }
  }

  // 诊断: 单独把扩展进程的 Go 核心接上控制面, 不建 VPN。
  // 用来区分"控制面连不上"和"VPN 建不起来"两种故障。
  startControlTest() {
    if (this.diagBusy) {
      return
    }
    if (this.serverUrl.trim() === '' || this.authKey.trim() === '') {
      this.controlResult = '控制面 FAIL: 请先填写服务器地址和预授权密钥'
      return
    }
    const host = TunnelControl.extractHost(this.serverUrl)
    if (host === '' || !TunnelControl.isPlausibleHost(host)) {
      this.controlResult = '控制面 FAIL: 服务器地址格式不对, 只接受 ASCII 域名或 IP'
      return
    }
    try {
      const want: Want = {
        bundleName: 'com.arkmesh.client',
        abilityName: 'ArkMeshVpnAbility',
        parameters: {
          serverUrl: this.serverUrl.trim(),
          authKey: this.authKey.trim(),
          hostname: this.machineName !== '' ? this.machineName : TunnelControl.defaultMachineName(),
          goCore: 1
        }
      }
      vpnExtension.startVpnExtensionAbility(want)
        .then(() => {
          this.controlResult = '控制面 ok: 已请求启动扩展进程, 详细结果见 hilog 的 [GoCoreSpike]'
          console.info('[GoCore] VPN 扩展已启动(goCore 模式)')
        })
        .catch((e: Error) => {
          this.controlResult = '控制面 FAIL: 启动扩展失败 ' + e.message
          console.error('[GoCore] VPN 扩展启动失败: ' + e.message)
        })
    } catch (err) {
      const e = err as BusinessError
      this.controlResult = '控制面 FAIL: 启动扩展异常 ' + e.message
      console.error('[GoCore] startVpnExtensionAbility 异常: ' + e.message)
    }
  }

  private allDiagText(): string {
    const blocks: string[] = []
    if (this.coreResult !== '') {
      blocks.push('[核心自检] ' + this.coreResult)
    }
    if (this.ifResult !== '') {
      blocks.push('[接口探测] ' + this.ifResult)
    }
    if (this.netResult !== '') {
      blocks.push('[网络检测] ' + this.netResult)
    }
    if (this.controlResult !== '') {
      blocks.push('[控制面] ' + this.controlResult)
    }
    if (this.dnsProbeResult !== '') {
      blocks.push('[MagicDNS] ' + this.dnsProbeResult)
    }
    return blocks.join('\n\n')
  }

  private copyAll(): void {
    const text = this.allDiagText()
    if (text === '') {
      this.toast('还没有诊断结果')
      return
    }
    copyText(text)
      .then(() => this.toast('诊断结果已复制'))
      .catch((e: Error) => this.toast('复制失败: ' + e.message))
  }

  // 将被清除的配置数据大小(字节)。清除只作用于 preferences 里的配置项,
  // 所以按这些配置项的 UTF-8 字节数估算, 让弹窗里的"大小"是真实数字而非空话。
  private clearSizeText(): string {
    const parts: string[] = [
      this.serverUrl, this.authKey, this.machineName,
      TunnelControl.exitNodeIP(), TunnelControl.exitNodeName()
    ]
    let bytes = 0
    for (const p of parts) {
      for (let i = 0; i < p.length; i++) {
        bytes += p.charCodeAt(i) > 127 ? 3 : 1
      }
    }
    if (bytes <= 0) {
      return '当前没有已保存的配置'
    }
    return bytes < 1024 ? `约 ${bytes} B` : `约 ${(bytes / 1024).toFixed(1)} KB`
  }

  confirmClear() {
    try {
      this.getUIContext().getPromptAction().showDialog({
        title: '清除所有数据',
        message: `服务器地址、预授权密钥和出口节点选择都会被清空, 且无法恢复。` +
          `当前配置数据${this.clearSizeText()}。确定继续?`,
        buttons: [
          { text: '取消', color: Palette.textSecondary },
          { text: '清除', color: Palette.danger }
        ]
      }).then((resp: promptAction.ShowDialogSuccessResponse) => {
        if (resp.index !== 1) {
          return
        }
        storageUtils.clear()
        storageUtils.flush()
        this.loadSettings()
        this.toast('数据已清除')
      })
    } catch (err) {
      const e = err as BusinessError
      console.error('[Settings] 弹窗失败: ' + e.message)
    }
  }

  private toast(msg: string): void {
    try {
      this.getUIContext().getPromptAction().showToast({ message: msg, duration: 2000 })
    } catch (err) {
      const e = err as BusinessError
      console.error('[Settings] toast 失败: ' + e.message)
    }
  }

  build() {
    Scroll() {
      Column({ space: Space.lg }) {
        Text('设置')
          .fontSize(Type.title)
          .fontWeight(FontWeight.Bold)
          .fontColor(Palette.textPrimary)
          .width('100%')
          .padding({ top: Space.md })

        // ==== 服务器 ====
        Card() {
          SectionHead({ title: '服务器' })
          FormField({
            label: '控制面地址',
            placeholder: 'https://headscale.example.com',
            hint: '自建 Headscale 的 HTTPS 地址。',
            error: this.urlError(),
            value: $serverUrl
          })
          FormField({
            label: '预授权密钥',
            placeholder: '粘贴预认证密钥',
            hint: '在 Headscale 控制台生成, 注册时用一次。',
            password: true,
            value: $authKey
          })
        }

        // ==== 设备 ====
        Card() {
          SectionHead({ title: '设备' })
          FormField({
            label: '设备名称',
            placeholder: '设备名称',
            hint: '在 Headscale 控制台里显示的名字, 改名后下次连接生效。',
            value: $machineName
          })
        }

        ActionButton({
          label: this.dirty() ? '保存设置' : '已保存',
          variant: ButtonVariant.Primary,
          disabled: !this.dirty(),
          action: (): void => this.saveSettings()
        })

        // ==== 高级诊断(默认折叠) ====
        Card() {
          Row({ space: Space.md }) {
            SymbolGlyph(Icon.tools)
              .fontSize(Size.iconAction)
              .fontColor([Palette.iconSecondary])
            Column({ space: Space.xxs }) {
              Text('高级诊断')
                .fontSize(Type.section)
                .fontWeight(FontWeight.Bold)
                .fontColor(Palette.textPrimary)
              Text(this.diagExpanded ? '收起诊断工具' : '连不上时用来定位问题')
                .fontSize(Type.caption)
                .fontColor(Palette.textSecondary)
            }
            .alignItems(HorizontalAlign.Start)
            .layoutWeight(1)

            SymbolGlyph(Icon.chevron)
              .fontSize(Size.iconAction)
              .fontColor([Palette.iconTertiary])
              .rotate({ angle: this.diagExpanded ? 90 : 0 })
              .animation({ duration: 200, curve: Curve.EaseOut })
          }
          .width('100%')
          .constraintSize({ minHeight: Size.touchTarget })
          .alignItems(VerticalAlign.Center)
          .accessibilityText(`高级诊断 ${this.diagExpanded ? '已展开' : '已收起'}`)
          .onClick(() => {
            this.diagExpanded = !this.diagExpanded
          })

          if (this.diagExpanded) {
            Divider().color(Palette.divider).strokeWidth(0.5)

            SectionHead({
              title: '本机检测',
              desc: '依次跑核心自检、接口探测和到服务器的网络检测。网络检测最坏约 1 分钟, 期间界面不会卡住。'
            })
            ActionButton({
              label: '运行本机检测',
              variant: ButtonVariant.Neutral,
              symbol: Icon.diagnostics,
              loading: this.diagBusy,
              disabled: this.diagBusy,
              action: (): void => {
                this.runDiagnostics()
              }
            })
            ActionButton({
              label: '控制面连通测试',
              variant: ButtonVariant.Neutral,
              symbol: Icon.server,
              disabled: this.diagBusy,
              action: (): void => this.startControlTest()
            })

            if (this.diagBusy) {
              Row({ space: Space.sm }) {
                LoadingProgress()
                  .width(Size.iconAction)
                  .height(Size.iconAction)
                  .color(Palette.accentIcon)
                Text(this.diagStage)
                  .fontSize(Type.caption)
                  .fontColor(Palette.textSecondary)
                  .layoutWeight(1)
              }
              .width('100%')
              .alignItems(VerticalAlign.Center)
            }

            if (hasDiag(this.coreResult)) {
              DiagSection({ title: '核心与加密', raw: this.coreResult })
            }
            if (hasDiag(this.netResult)) {
              DiagSection({ title: '到服务器的网络', raw: this.netResult })
            }
            if (hasDiag(this.ifResult)) {
              DiagSection({ title: '网络接口读取', raw: this.ifResult })
            }
            if (hasDiag(this.controlResult)) {
              DiagSection({ title: '控制面启动', raw: this.controlResult })
            }
            if (hasDiag(this.allDiagText())) {
              Row() {
                Blank()
                InlineAction({
                  label: '复制全部结果',
                  symbol: Icon.copy,
                  action: (): void => this.copyAll()
                })
              }
              .width('100%')
            }

            Divider().color(Palette.divider).strokeWidth(0.5)

            SectionHead({
              title: 'MagicDNS 解析测试',
              desc: '连接后测试 mesh 主机名能否解析。留空则自动测本机名和一个在线节点, 走的是和浏览器访问同一条路径。'
            })
            FormField({
              label: '要解析的名字',
              placeholder: 'xxx.mesh.example.com(留空 = 自动)',
              value: $dnsProbeName
            })
            ActionButton({
              label: this.dnsProbePending ? '测试中' : '测试解析',
              variant: ButtonVariant.Neutral,
              symbol: Icon.dns,
              loading: this.dnsProbePending,
              disabled: this.dnsProbePending,
              action: (): void => this.runDnsProbe()
            })
            if (this.dnsProbeResult !== '') {
              Text(this.dnsProbeResult)
                .fontSize(Type.micro)
                .fontColor(Palette.textTertiary)
                .lineHeight(Type.microLine)
                .width('100%')
                .copyOption(CopyOptions.LocalDevice)
            }
          }
        }

        // ==== 关于 ====
        Card() {
          SectionHead({ title: '关于' })
          Text('ArkMesh 是面向 HarmonyOS 的私有组网客户端。它通过 WireGuard 加密隧道与 Noise 控制面协议, 把本机接入你自建的 Headscale 网络, 实现设备间点对点互通、MagicDNS 内网解析与出口节点切换。')
            .fontSize(Type.caption)
            .fontColor(Palette.textSecondary)
            .lineHeight(Type.captionLine)
            .width('100%')
          Divider().color(Palette.divider).strokeWidth(0.5)
          SectionHead({ title: '快速开始' })
          this.howTo(1, '在"服务器"里填写控制面地址和预授权密钥')
          this.howTo(2, '回到"概览"点连接, 加入你的私有网络')
          this.howTo(3, '需要换出口 IP 时, 在"节点"里选一个出口节点')
          Divider().color(Palette.divider).strokeWidth(0.5)
          KeyValueRow({
            label: '版本',
            value: this.versionName !== '' ? this.versionName : '0.9.0',
            valueTone: Tone.Neutral
          })
          KeyValueRow({
            label: '组网协议',
            value: 'WireGuard / Noise / MagicDNS',
            valueTone: Tone.Neutral
          })
          Row({ space: Space.sm }) {
            SymbolGlyph(Icon.secure)
              .fontSize(Size.iconInline)
              .fontColor([Palette.iconSecondary])
            Text('配置与密钥仅保存在本机, 不上传至任何第三方服务器。')
              .fontSize(Type.micro)
              .fontColor(Palette.textQuaternary)
              .lineHeight(Type.microLine)
              .layoutWeight(1)
          }
          .width('100%')
          .alignItems(VerticalAlign.Top)
        }

        ActionButton({
          label: '清除所有数据',
          variant: ButtonVariant.Danger,
          symbol: Icon.trash,
          action: (): void => this.confirmClear()
        })
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

  // "关于"里的一行使用说明
  @Builder
  howTo(index: number, text: string) {
    Row({ space: Space.sm }) {
      Text(`${index}`)
        .fontSize(Type.micro)
        .fontColor(Palette.onAccent)
        .width(Size.iconAction)
        .height(Size.iconAction)
        .textAlign(TextAlign.Center)
        .borderRadius(Radius.pill)
        .backgroundColor(Palette.accent)
      Text(text)
        .fontSize(Type.caption)
        .fontColor(Palette.textSecondary)
        .lineHeight(Type.captionLine)
        .layoutWeight(1)
    }
    .width('100%')
    .alignItems(VerticalAlign.Top)
  }
}
