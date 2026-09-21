// Go 探针结果的解析。
//
// 三个探针(ArkMeshSelfTest / ArkMeshNetCheck / ArkMeshIfProbe)都是把若干条
// "名字 + ok/FAIL + 细节" 的字符串用 " | " 拼成一大段返回。原来直接把这段原始
// 文本以 11px 铺在页面上，用户看不出哪条过了哪条没过。这里拆成结构化的行，
// UI 才能给每条配状态图标，也能只把失败的挑出来放最前面。

export enum DiagStatus {
  Pass,
  Fail,
  Warn,
  Info
}

export interface DiagItem {
  name: string
  detail: string
  status: DiagStatus
}

function classify(line: string): DiagStatus {
  if (line.indexOf('FAIL') >= 0 || line.indexOf('ERR') === 0
    || line.indexOf('调用异常') >= 0 || line.indexOf('超时') >= 0) {
    return DiagStatus.Fail
  }
  if (line.indexOf('空结果') >= 0 || line.indexOf('不可读') >= 0
    || line.indexOf('为空') >= 0 || line.indexOf('未发现') >= 0) {
    return DiagStatus.Warn
  }
  if (line.indexOf(' ok') >= 0 || line.indexOf(': ok') >= 0) {
    return DiagStatus.Pass
  }
  return DiagStatus.Info
}

// 细节里再重复一遍 ok/FAIL 没信息量，去掉前缀让行更干净
function trimVerdict(detail: string): string {
  let d = detail
  if (d === 'ok' || d === 'FAIL') {
    return ''
  }
  const prefixes = ['ok: ', 'ok ', 'FAIL: ', 'FAIL ', 'ERR: ', 'ERR ']
  for (const p of prefixes) {
    if (d.startsWith(p)) {
      return d.substring(p.length)
    }
  }
  return d
}

// 探针原始名是给工程师看的(netlink/ohosInterfaceList 这种)。列表里换成一句
// 人话, 细节列仍保留原始文本 —— 排查时复制出去的信息不打折。
const NAME_LABELS: Map<string, string> = new Map([
  ['标准库', '接口枚举(标准库)'],
  ['netlink', 'netlink 通道'],
  ['ioctl', 'ioctl 枚举'],
  ['/proc/net/dev', 'proc 接口清单'],
  ['/proc/net/if_inet6', 'proc IPv6 清单'],
  ['libc', 'libc 枚举'],
  ['ohosInterfaceList', 'OHOS 适配枚举'],
  ['systemCA', '系统证书池'],
  ['resolvConf', 'DNS 配置'],
  ['dns', '域名解析'],
  ['resolvers', '解析器对比'],
  ['tcp', 'TCP 连通'],
  ['tls', 'TLS 握手'],
  ['http', 'HTTPS 响应'],
  ['go=go1.26.8', '运行环境'],
  ['curve25519', 'Curve25519 密钥'],
  ['chacha20poly1305', 'ChaCha20 加密'],
  ['blake2s', 'BLAKE2s 哈希'],
  ['keys', '节点密钥'],
  ['disco', '节点发现'],
  ['tailcfg', '控制协议'],
  ['magicdns', 'MagicDNS']
])

function friendlyName(name: string): string {
  const hit = NAME_LABELS.get(name)
  return hit !== undefined ? hit : name
}

// 把一段探针输出拆成结构化条目。原始文本为空返回空数组，
// 由调用方决定显示"还没跑"还是"没有结果"。
export function parseDiag(text: string): DiagItem[] {
  const trimmed = text.trim()
  if (trimmed === '') {
    return []
  }
  // 整段就是一个错误（原生层没跑起来、超时兜底），别再按 " | " 拆了
  if (trimmed.startsWith('ERR ') || trimmed.startsWith('ERR:')) {
    const bad: DiagItem = {
      name: '探针未能完成',
      detail: trimVerdict(trimmed.substring(3)),
      status: DiagStatus.Fail
    }
    return [bad]
  }
  const out: DiagItem[] = []
  const parts = trimmed.split(' | ')
  for (const raw of parts) {
    const part = raw.trim()
    if (part === '') {
      continue
    }
    const sp = part.indexOf(' ')
    const colon = part.indexOf(':')
    let cut = -1
    if (sp < 0) {
      cut = colon
    } else if (colon >= 0 && colon < sp) {
      cut = colon
    } else {
      cut = sp
    }
    let name = cut > 0 ? part.substring(0, cut) : part
    const detail = cut > 0 ? part.substring(cut + 1).trim() : ''
    while (name.endsWith(':')) {
      name = name.substring(0, name.length - 1)
    }
    const item: DiagItem = { name: friendlyName(name), detail: trimVerdict(detail), status: classify(part) }
    out.push(item)
  }
  return out
}

// 失败排前面：排查时最关心的就是那条红的，不该让人在十几行里翻。
export function sortDiag(items: DiagItem[]): DiagItem[] {
  const rank = (s: DiagStatus): number => {
    switch (s) {
      case DiagStatus.Fail:
        return 0
      case DiagStatus.Warn:
        return 1
      case DiagStatus.Info:
        return 2
      default:
        return 3
    }
  }
  return items.slice().sort((a: DiagItem, b: DiagItem): number => rank(a.status) - rank(b.status))
}

export function countStatus(items: DiagItem[], status: DiagStatus): number {
  let n = 0
  for (const it of items) {
    if (it.status === status) {
      n++
    }
  }
  return n
}
