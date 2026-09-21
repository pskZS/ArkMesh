// UI 侧对 VPN 扩展的启停封装。
//
// 为什么要抽出来: 出口节点的选择在"网络节点"页, 连接/断开按钮在首页, 两边都要
// 拉起/停掉同一个扩展。Want 的构造(尤其是 serverUrl/authKey/exitNode 这几个只
// 在运行时从 Preferences 读、绝不写进源码的参数)只能有一份, 否则会各自漂移。
import { vpnExtension } from '@kit.NetworkKit'
import { Want } from '@kit.AbilityKit'
import { deviceInfo } from '@kit.BasicServicesKit'
import { storageUtils } from './StorageUtils'

const BUNDLE_NAME = 'com.arkmesh.client'
const ABILITY_NAME = 'ArkMeshVpnAbility'

// stop 之后要等扩展进程真的走完 onDestroy(里面会 coreControlDown + 销毁 VPN),
// 立刻 start 会撞上"上一个实例还在"。
const RESTART_DELAY_MS = 2000

export class TunnelStartError extends Error {
}

export class TunnelControl {
  // 设备名称的默认值: 真机的市场名(如 "HUAWEI Mate 60"), 压成 DNS 安全的小写
  // 连字符形式。headscale 控制台里一眼能认出是哪台手机, 比写死的占位名强。
  // 市场名缺失(或纯中文被滤空)时退到型号, 再不行才用兜底名。
  static defaultMachineName(): string {
    const raw: string = deviceInfo.marketName !== '' ? deviceInfo.marketName : deviceInfo.productModel
    let out: string = ''
    for (let i = 0; i < raw.length; i++) {
      const c: string = raw.charAt(i).toLowerCase()
      if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) {
        out += c
      } else if (c === ' ' || c === '-' || c === '_') {
        out += '-'
      }
    }
    while (out.startsWith('-')) {
      out = out.substring(1)
    }
    while (out.endsWith('-')) {
      out = out.substring(0, out.length - 1)
    }
    return out !== '' ? out : 'arkmesh-device'
  }

  // 读设备名称, 顺带把旧占位值迁移成真机名。设置页是懒加载的 Tab, 它的
  // aboutToAppear 可能晚于第一次"连接", 所以迁移必须也放在连接路径上,
  // 否则首连还会拿占位名去 headscale 注册。
  static machineName(): string {
    let stored: string = storageUtils.getString('machineName', '')
    if (stored === '' || stored === 'harmony-device' || stored === 'arkmesh-harmony') {
      stored = TunnelControl.defaultMachineName()
      storageUtils.setString('machineName', stored)
      storageUtils.flush()
    }
    return stored
  }

  // 出口节点 IP, 空串表示不经过出口节点。
  static exitNodeIP(): string {
    const ip = storageUtils.getString('exitNodeIP', '')
    if (ip !== '') {
      return ip
    }
    // 早期版本把 IP 存在 'exitNode' 键下。不迁移的话升级后读不到老选择,
    // 出口节点会静默失效(首页还显示着名字, 隧道却不再走出口)。
    const legacy = storageUtils.getString('exitNode', '')
    if (legacy !== '') {
      storageUtils.setString('exitNodeIP', legacy)
      storageUtils.delete('exitNode')
      storageUtils.flush()
      return legacy
    }
    return ''
  }

  static exitNodeName(): string {
    return storageUtils.getString('exitNodeName', '')
  }

  static setExitNode(ip: string, name: string): void {
    storageUtils.setString('exitNodeIP', ip)
    storageUtils.setString('exitNodeName', name)
    storageUtils.flush()
  }

  // 从 https://host[:port]/path 里取出 host。
  static extractHost(url: string): string {
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

  // host 形状校验: 只放行 ASCII 域名/IP。
  // 上架测试里用户往地址栏填了 "Hello恭喜发财", 底层 cgo 的 getaddrinfo 走进
  // IDN 路径长时间阻塞, 直接把主线程焊死触发 APP_INPUT_BLOCK。连接路径和诊断
  // 探针都在入口把它拦下, 不再让垃圾输入流到解析器。
  static isPlausibleHost(host: string): boolean {
    if (host === '' || host.length > 253) {
      return false
    }
    for (let i = 0; i < host.length; i++) {
      const c = host.charCodeAt(i)
      const isAlnum = (c >= 0x30 && c <= 0x39) || (c >= 0x41 && c <= 0x5A) || (c >= 0x61 && c <= 0x7A)
      const isPunct = c === 0x2D || c === 0x2E || c === 0x5F || c === 0x3A || c === 0x5B || c === 0x5D
      if (!isAlnum && !isPunct) {
        return false
      }
    }
    return true
  }

  // 启动隧道 = 先 stop 再 start, 无条件。
  //
  // 为什么不能直接 start: VpnExtensionAbility 只声明了 onCreate/onDestroy, 没有
  // onRequest。扩展进程一旦还活着(比如上一次跑失败了, 没人调 stop), 再调
  // startVpnExtensionAbility 只会让框架打一行 "onRequest, startId:2" 就完事,
  // onCreate 不会再跑 —— 界面上"连接"点了没任何反应, 只能杀掉 App 才能恢复。
  // 真机上已经复现过: 隧道失败后扩展进程 59271 一直挂着, 连接按钮彻底失灵。
  // 先 stop 让 onDestroy 走完(里面会 coreControlDown + 销毁 VPN + 进程退出),
  // 再 start 才能保证走到 onCreate。本来就没连着时 stop 会失败, 忽略即可。
  //
  // 改出口节点也走这里: OHOS 的 VpnConfig 建完不能改, 默认路由只能在 create()
  // 时一次性给全, 所以切换出口节点等于重启一次隧道。
  static async start(): Promise<void> {
    const serverUrl = storageUtils.getString('serverUrl', '')
    const authKey = storageUtils.getString('authKey', '')
    if (serverUrl === '' || authKey === '') {
      throw new TunnelStartError('请先在设置里填写控制面地址和预授权密钥')
    }
    const host = TunnelControl.extractHost(serverUrl)
    if (!TunnelControl.isPlausibleHost(host)) {
      throw new TunnelStartError('控制面地址格式不对: 只接受 ASCII 域名或 IP(如 headscale.example.com)')
    }
    try {
      await TunnelControl.stop()
    } catch (err) {
      // 本来就没连着, stop 失败不影响后面的 start
      console.warn('[TunnelControl] stop 失败(继续启动): ' + JSON.stringify(err))
    }
    await new Promise<void>((resolve) => setTimeout(resolve, RESTART_DELAY_MS))

    const want: Want = {
      bundleName: BUNDLE_NAME,
      abilityName: ABILITY_NAME,
      parameters: {
        serverUrl: serverUrl,
        authKey: authKey,
        // 设备名称在设置页可改, 改完下次连接生效(headscale 里会跟着改名)
        hostname: TunnelControl.machineName(),
        exitNode: TunnelControl.exitNodeIP(),
        goTunnel: 1
      }
    }
    try {
      return await vpnExtension.startVpnExtensionAbility(want)
    } catch (err) {
      throw new Error('启动扩展异常: ' + JSON.stringify(err))
    }
  }

  static stop(): Promise<void> {
    const want: Want = {
      bundleName: BUNDLE_NAME,
      abilityName: ABILITY_NAME
    }
    try {
      return vpnExtension.stopVpnExtensionAbility(want)
    } catch (err) {
      return Promise.reject(new Error('停止扩展异常: ' + JSON.stringify(err)))
    }
  }
}
