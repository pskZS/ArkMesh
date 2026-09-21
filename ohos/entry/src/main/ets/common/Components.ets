// ArkMesh 通用组件层。
//
// 页面只负责"摆什么"，长什么样一律由这里的组件决定，颜色/字号/间距全部取自
// Theme 里的 token。新增页面时先看看这里有没有能复用的，别再在页面里手写卡片。
import { Palette, Type, Space, Radius, Size, Icon, Tone, toneColor, toneIcon } from './Theme'
import { DiagItem, DiagStatus, countStatus, parseDiag, sortDiag } from './DiagParser'

@Builder
function EmptySlot() {
}

// 密码框自绘小眼睛点开后, 明文保持多久再自动遮蔽。
const REVEAL_HOLD_MS: number = 5000

export enum ButtonVariant {
  Primary,
  Neutral,
  Danger
}

// 卡片容器：全站统一的白/深色面板。内容通过尾随闭包传进来。
@Component
export struct Card {
  @BuilderParam content: () => void = EmptySlot

  build() {
    Column({ space: Space.md }) {
      this.content()
    }
    .width('100%')
    .padding(Space.lg)
    .backgroundColor(Palette.card)
    .borderRadius(Radius.lg)
    .alignItems(HorizontalAlign.Start)
    .shadow(ShadowStyle.OUTER_DEFAULT_SM)
  }
}

// 卡片标题 + 一句说明。说明可省。
@Component
export struct SectionHead {
  @Prop title: string = ''
  @Prop desc: string = ''

  build() {
    Column({ space: Space.xs }) {
      Text(this.title)
        .fontSize(Type.section)
        .fontWeight(FontWeight.Bold)
        .fontColor(Palette.textPrimary)
        .lineHeight(Type.sectionLine)
      if (this.desc !== '') {
        Text(this.desc)
          .fontSize(Type.caption)
          .fontColor(Palette.textSecondary)
          .lineHeight(Type.captionLine)
      }
    }
    .width('100%')
    .alignItems(HorizontalAlign.Start)
  }
}

// 状态圆点。只认 tone，颜色由 token 决定。
@Component
export struct StatusDot {
  @Prop tone: Tone = Tone.Neutral

  build() {
    Circle()
      .width(Space.sm)
      .height(Space.sm)
      .fill(toneIcon(this.tone))
      .accessibilityText(this.label())
  }

  private label(): string {
    switch (this.tone) {
      case Tone.Ok:
        return '正常'
      case Tone.Warn:
        return '注意'
      case Tone.Danger:
        return '异常'
      default:
        return '未激活'
    }
  }
}

// 小标签：通过/失败/在线/离线 这类短状态词。
@Component
export struct Chip {
  @Prop label: string = ''
  @Prop tone: Tone = Tone.Neutral

  build() {
    Text(this.label)
      .fontSize(Type.micro)
      .lineHeight(Type.microLine)
      .fontColor(toneColor(this.tone))
      .padding({ left: Space.sm, right: Space.sm, top: Space.xs, bottom: Space.xs })
      .backgroundColor(Palette.chip)
      .borderRadius(Radius.sm)
  }
}

// 一行"标签 + 值"。可复制、可点进下一页，两种意图互斥。
@Component
export struct KeyValueRow {
  @Prop label: string = ''
  @Prop value: string = ''
  @Prop valueTone: Tone = Tone.Neutral
  @Prop copyable: boolean = false
  @Prop arrow: boolean = false
  onCopy: () => void = () => {
  }
  onTap: () => void = () => {
  }

  build() {
    Row() {
      Text(this.label)
        .fontSize(Type.body)
        .fontColor(Palette.textSecondary)
        .layoutWeight(1)
      Text(this.value)
        .fontSize(Type.body)
        .fontColor(this.value === '' ? Palette.textQuaternary : toneColor(this.valueTone))
        .textAlign(TextAlign.End)
        .maxLines(2)
        .textOverflow({ overflow: TextOverflow.Ellipsis })
        .constraintSize({ maxWidth: '58%' })
      if (this.copyable) {
        SymbolGlyph(Icon.copy)
          .fontSize(Size.iconInline)
          .fontColor([Palette.iconTertiary])
          .margin({ left: Space.sm })
      }
      if (this.arrow) {
        SymbolGlyph(Icon.chevron)
          .fontSize(Size.iconInline)
          .fontColor([Palette.iconTertiary])
          .margin({ left: Space.xs })
      }
    }
    .width('100%')
    .constraintSize({ minHeight: Size.touchTarget })
    .padding({ top: Space.sm, bottom: Space.sm })
    .alignItems(VerticalAlign.Center)
    .accessibilityText(`${this.label} ${this.value}`)
    .onClick(() => {
      if (this.copyable) {
        this.onCopy()
        return
      }
      if (this.arrow) {
        this.onTap()
      }
    })
  }
}

// 主按钮：整行宽的胶囊。四种语义变体，颜色全部来自 token。
@Component
export struct ActionButton {
  @Prop label: string = ''
  @Prop variant: ButtonVariant = ButtonVariant.Primary
  @Prop loading: boolean = false
  @Prop disabled: boolean = false
  // 图标可能随状态变（连接/断开是同一个按钮），所以也得是 @Prop；
  // 普通成员在父组件刷新时不会跟着更新。
  @Prop symbol: Resource | null = null
  action: () => void = () => {
  }

  private bg(): ResourceColor {
    // 禁用态不做整控件透明: 底色和文字一起淡化会掉到 1.3:1 过不了可用性检测, 改无底+主色文字。
    if (this.disabled) {
      return Color.Transparent
    }
    switch (this.variant) {
      case ButtonVariant.Neutral:
        return Palette.field
      case ButtonVariant.Danger:
        return Palette.danger
      default:
        return Palette.accent
    }
  }

  private fg(): ResourceColor {
    if (this.disabled) {
      return Palette.textPrimary
    }
    switch (this.variant) {
      case ButtonVariant.Neutral:
        return Palette.textPrimary
      default:
        return Palette.onAccent
    }
  }

  build() {
    Button() {
      Row({ space: Space.sm }) {
        if (this.loading) {
          LoadingProgress()
            .width(Size.iconAction)
            .height(Size.iconAction)
            .color(this.fg())
        } else if (this.symbol !== null) {
          SymbolGlyph(this.symbol)
            .fontSize(Size.iconAction)
            .fontColor([this.fg()])
        }
        Text(this.label)
          .fontSize(Type.body)
          .fontWeight(FontWeight.Medium)
          .fontColor(this.fg())
      }
      .justifyContent(FlexAlign.Center)
    }
    .type(ButtonType.Capsule)
    .width('100%')
    .height(Size.buttonHeight)
    .backgroundColor(this.bg())
    .enabled(!this.disabled && !this.loading)
    .accessibilityText(this.label)
    .onClick(() => this.action())
  }
}

// 行内动作：图标 + 文字，没有按钮底色。用在卡片标题栏这种次要操作上。
@Component
export struct InlineAction {
  @Prop label: string = ''
  @Prop disabled: boolean = false
  @Prop busy: boolean = false
  symbol: Resource = Icon.chevron
  action: () => void = () => {
  }

  build() {
    Row({ space: Space.xs }) {
      if (this.busy) {
        LoadingProgress()
          .width(Size.iconAction)
          .height(Size.iconAction)
          .color(Palette.accentIcon)
      } else {
        SymbolGlyph(this.symbol)
          .fontSize(Size.iconAction)
          .fontColor([this.disabled ? Palette.iconSecondary : Palette.accentIcon])
      }
      Text(this.label)
        .fontSize(Type.caption)
        .fontColor(this.disabled ? Palette.textSecondary : Palette.accentText)
    }
    .constraintSize({ minHeight: Space.hero })
    .padding({ left: Space.sm, right: Space.sm })
    .enabled(!this.disabled)
    .accessibilityText(this.label)
    .onClick(() => this.action())
  }
}

// 表单字段：标签 + 输入框 + 说明/错误。错误文案非空时输入框描红边。
//
// 密码框不用系统自带的小眼睛：它只在"按住期间"显示明文, 一松手就收 —— 对着
// 长密钥逐字符核对时根本来不及。改成自绘图标: 点开明文, 保持 REVEAL_HOLD_MS
// 后自动遮蔽; 显示期间再点一下可立即遮蔽。
@Component
export struct FormField {
  @Prop label: string = ''
  @Prop placeholder: string = ''
  @Prop hint: string = ''
  @Prop error: string = ''
  @Prop password: boolean = false
  @Link value: string
  @State reveal: boolean = false
  private revealTimerId: number = -1

  aboutToDisappear() {
    this.stopRevealTimer()
  }

  private stopRevealTimer(): void {
    if (this.revealTimerId !== -1) {
      clearTimeout(this.revealTimerId)
      this.revealTimerId = -1
    }
  }

  private toggleReveal(): void {
    this.stopRevealTimer()
    if (this.reveal) {
      this.reveal = false
      return
    }
    this.reveal = true
    this.revealTimerId = setTimeout(() => {
      this.revealTimerId = -1
      this.reveal = false
    }, REVEAL_HOLD_MS)
  }

  build() {
    Column({ space: Space.xs }) {
      Text(this.label)
        .fontSize(Type.caption)
        .fontColor(Palette.textSecondary)
      Stack({ alignContent: Alignment.End }) {
        TextInput({ placeholder: this.placeholder, text: this.value })
          .width('100%')
          .height(Size.controlHeight)
          .fontSize(Type.body)
          .fontColor(Palette.textPrimary)
          .placeholderColor(Palette.textQuaternary)
          .backgroundColor(Palette.field)
          .borderRadius(Radius.sm)
          .padding({ left: Space.lg, right: this.password ? Size.fieldIconInset : Space.lg })
          .type(this.password && !this.reveal ? InputType.Password : InputType.Normal)
          .showPasswordIcon(false)
          .border({ width: 1, color: this.error !== '' ? Palette.danger : Color.Transparent })
          .accessibilityText(this.label)
          .onChange((v: string) => {
            this.value = v
          })
        if (this.password) {
          SymbolGlyph(this.reveal ? Icon.eye : Icon.eyeSlash)
            .fontSize(Size.iconAction)
            .fontColor([Palette.iconSecondary])
            .margin({ right: Space.md })
            .accessibilityText(this.reveal ? '隐藏密码' : '显示密码')
            .onClick(() => this.toggleReveal())
        }
      }
      .width('100%')
      if (this.error !== '') {
        Row({ space: Space.xs }) {
          SymbolGlyph(Icon.warn)
            .fontSize(Size.iconInline)
            .fontColor([Palette.danger])
          Text(this.error)
            .fontSize(Type.micro)
            .fontColor(Palette.danger)
            .layoutWeight(1)
        }
        .width('100%')
        .alignItems(VerticalAlign.Top)
      } else if (this.hint !== '') {
        Text(this.hint)
          .fontSize(Type.micro)
          .fontColor(Palette.textQuaternary)
          .lineHeight(Type.microLine)
      }
    }
    .width('100%')
    .alignItems(HorizontalAlign.Start)
  }
}

// 空状态：图标 + 标题 + 一句解释。别再让页面写一行灰字了事。
@Component
export struct EmptyState {
  @Prop title: string = ''
  @Prop desc: string = ''
  symbol: Resource = Icon.statusInfo

  build() {
    Column({ space: Space.sm }) {
      SymbolGlyph(this.symbol)
        .fontSize(Size.iconHero)
        .fontColor([Palette.iconTertiary])
      Text(this.title)
        .fontSize(Type.body)
        .fontColor(Palette.textSecondary)
      if (this.desc !== '') {
        Text(this.desc)
          .fontSize(Type.caption)
          .fontColor(Palette.textQuaternary)
          .textAlign(TextAlign.Center)
          .lineHeight(Type.captionLine)
      }
    }
    .width('100%')
    .padding({ top: Space.xxl, bottom: Space.xxl })
    .justifyContent(FlexAlign.Center)
    .alignItems(HorizontalAlign.Center)
  }
}

// 诊断结果的一行：状态图标 + 项目名 + 细节。
@Component
struct DiagRow {
  @Prop name: string = ''
  @Prop detail: string = ''
  @Prop status: DiagStatus = DiagStatus.Info

  private tone(): Tone {
    switch (this.status) {
      case DiagStatus.Pass:
        return Tone.Ok
      case DiagStatus.Fail:
        return Tone.Danger
      case DiagStatus.Warn:
        return Tone.Warn
      default:
        return Tone.Neutral
    }
  }

  private glyph(): Resource {
    switch (this.status) {
      case DiagStatus.Pass:
        return Icon.pass
      case DiagStatus.Fail:
        return Icon.fail
      case DiagStatus.Warn:
        return Icon.warn
      default:
        return Icon.statusInfo
    }
  }

  build() {
    Row({ space: Space.sm }) {
      SymbolGlyph(this.glyph())
        .fontSize(Size.iconAction)
        .fontColor([toneIcon(this.tone())])
        .margin({ top: Space.xxs })
      Column({ space: Space.xxs }) {
        Text(this.name)
          .fontSize(Type.caption)
          .fontColor(Palette.textPrimary)
          .width('100%')
        if (this.detail !== '') {
          Text(this.detail)
            .fontSize(Type.micro)
            .fontColor(Palette.textTertiary)
            .lineHeight(Type.microLine)
            .width('100%')
            .copyOption(CopyOptions.LocalDevice)
        }
      }
      .alignItems(HorizontalAlign.Start)
      .layoutWeight(1)
    }
    .width('100%')
    .alignItems(VerticalAlign.Top)
    .accessibilityText(`${this.name} ${this.detail}`)
  }
}

// 一组诊断结果：标题 + 结论 + 明细。
//
// 收原始文本而不是收解析好的数组：@Prop 对字符串是稳的，对接口数组要深拷贝，
// 解析本身只有几十行字符串切分，放在这里每次渲染重算也不费事。
//
// 通过的条目默认折叠：十几行绿色对勾对排查毫无价值，还会把真正要看的异常
// 挤出视野。异常(含预期内的信息行)始终展开。
@Component
export struct DiagSection {
  @Prop title: string = ''
  @Prop raw: string = ''
  @Prop note: string = ''
  @State showPass: boolean = false

  private items(): DiagItem[] {
    return sortDiag(parseDiag(this.raw))
  }

  private passItems(): DiagItem[] {
    return this.items().filter((i: DiagItem): boolean => i.status === DiagStatus.Pass)
  }

  private issueItems(): DiagItem[] {
    return this.items().filter((i: DiagItem): boolean => i.status !== DiagStatus.Pass)
  }

  private failCount(): number {
    return countStatus(this.items(), DiagStatus.Fail) + countStatus(this.items(), DiagStatus.Warn)
  }

  build() {
    Column({ space: Space.sm }) {
      Row({ space: Space.sm }) {
        Text(this.title)
          .fontSize(Type.caption)
          .fontWeight(FontWeight.Medium)
          .fontColor(Palette.textPrimary)
          .layoutWeight(1)
        if (this.failCount() > 0) {
          Chip({ label: `${this.failCount()} 项异常`, tone: Tone.Danger })
        } else {
          Chip({ label: `通过 ${this.passItems().length} 项`, tone: Tone.Ok })
        }
      }
      .width('100%')
      .alignItems(VerticalAlign.Center)

      if (this.note !== '') {
        Text(this.note)
          .fontSize(Type.micro)
          .fontColor(Palette.textQuaternary)
          .lineHeight(Type.microLine)
          .width('100%')
      }

      ForEach(this.issueItems(), (it: DiagItem) => {
        DiagRow({ name: it.name, detail: it.detail, status: it.status })
      }, (it: DiagItem) => `${it.name}:${it.status}:${it.detail}`)

      if (this.passItems().length > 0) {
        Row({ space: Space.xs }) {
          SymbolGlyph(Icon.pass)
            .fontSize(Size.iconInline)
            .fontColor([toneIcon(Tone.Ok)])
          Text(this.showPass ? '收起通过项' : `查看通过的 ${this.passItems().length} 项`)
            .fontSize(Type.micro)
            .fontColor(Palette.accentText)
          SymbolGlyph(Icon.chevron)
            .fontSize(Size.iconInline)
            .fontColor([Palette.iconTertiary])
            .rotate({ angle: this.showPass ? 90 : 0 })
            .animation({ duration: 200, curve: Curve.EaseOut })
        }
        .constraintSize({ minHeight: Size.touchTarget })
        .alignItems(VerticalAlign.Center)
        .accessibilityText(this.showPass ? '收起通过项' : '查看通过项')
        .onClick(() => {
          this.showPass = !this.showPass
        })
      }

      if (this.showPass) {
        ForEach(this.passItems(), (it: DiagItem) => {
          DiagRow({ name: it.name, detail: it.detail, status: it.status })
        }, (it: DiagItem) => `${it.name}:${it.status}:${it.detail}`)
      }
    }
    .width('100%')
    .alignItems(HorizontalAlign.Start)
  }
}

// 有没有可展示的诊断结果。空结果不渲染 DiagSection 本身，而不是让它渲染成空壳 ——
// Card 内部按 space 给子项留间距，空壳会白白撑出一段空白。
export function hasDiag(raw: string): boolean {
  return raw.trim() !== ''
}
