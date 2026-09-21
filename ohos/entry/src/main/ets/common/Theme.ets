// ArkMesh 设计 token 层 —— 全站唯一的颜色/字号/间距/圆角/图标来源。
//
// 颜色一律引用系统语义色 sys.color.*，应用自己不写死任何色值：深色模式由系统
// 自动换一套，强调色也跟随用户在系统设置里挑的个性化主题色。
// 字号/间距/圆角没有对应的系统 token，就集中在这里定义成一套刻度。
// 页面里只准用这里的常量，出现裸数字或裸色值就是漏了 token。

export class Palette {
  // 强调色：跟随系统个性化主题色
  static readonly accent: Resource = $r('sys.color.ohos_id_color_emphasize')
  static readonly accentText: Resource = $r('sys.color.font_emphasize')
  static readonly accentIcon: Resource = $r('sys.color.icon_emphasize')
  static readonly onAccent: Resource = $r('sys.color.font_on_primary')

  // 语义状态色
  static readonly ok: Resource = $r('sys.color.confirm')
  static readonly warn: Resource = $r('sys.color.warning')
  static readonly danger: Resource = $r('sys.color.alert')

  // 文字四级
  static readonly textPrimary: Resource = $r('sys.color.font_primary')
  static readonly textSecondary: Resource = $r('sys.color.font_secondary')
  static readonly textTertiary: Resource = $r('sys.color.font_tertiary')
  static readonly textQuaternary: Resource = $r('sys.color.font_fourth')

  // 图标
  static readonly iconSecondary: Resource = $r('sys.color.icon_secondary')
  static readonly iconTertiary: Resource = $r('sys.color.icon_tertiary')

  // 背景与容器
  static readonly page: Resource = $r('sys.color.ohos_id_color_background')
  static readonly card: Resource = $r('sys.color.ohos_id_color_sub_background')
  static readonly field: Resource = $r('sys.color.comp_background_gray')
  static readonly chip: Resource = $r('sys.color.comp_background_secondary')
  static readonly divider: Resource = $r('sys.color.comp_divider')
}

// 字号刻度（vp）。行高一并给出，避免各处自己凑出七八种字号。
export class Type {
  static readonly title: number = 20
  static readonly section: number = 17
  static readonly body: number = 15
  static readonly caption: number = 13
  static readonly micro: number = 11

  static readonly titleLine: number = 28
  static readonly sectionLine: number = 24
  static readonly bodyLine: number = 21
  static readonly captionLine: number = 18
  static readonly microLine: number = 16
}

// 间距刻度（vp）。page 是页面左右留白，也是卡片之间的垂直间隔。
export class Space {
  static readonly xxs: number = 2
  static readonly xs: number = 4
  static readonly sm: number = 8
  static readonly md: number = 12
  static readonly lg: number = 16
  static readonly xl: number = 20
  static readonly xxl: number = 24
  static readonly hero: number = 32
  static readonly page: number = 16
}

// 圆角刻度（vp）。pill 用于胶囊按钮。
export class Radius {
  static readonly sm: number = 8
  static readonly md: number = 12
  static readonly lg: number = 16
  static readonly pill: number = 999
}

// 尺寸刻度（vp）：控件高度和图标字号，别再各处写 magic number。
export class Size {
  static readonly controlHeight: number = 44
  static readonly buttonHeight: number = 48
  static readonly touchTarget: number = 44
  static readonly iconInline: number = 16
  static readonly iconAction: number = 20
  static readonly iconTab: number = 24
  static readonly iconHero: number = 30
  static readonly iconStatus: number = 40
  // 密码框右侧给眼睛图标留的净空(图标 20 + 左右间距)。
  static readonly fieldIconInset: number = 44
}

// 统一线性图标集：全部取系统 symbol 的描边版本，不用 *_fill 变体。
// 选中/激活状态只改颜色，不改字形，这样一套图标在任何主题下都协调。
export class Icon {
  // 底部导航。节点用方形网格: circle_grid_2x2 是四个小圆点, 字形撑不满字框,
  // 和 house/gearshape 放一起视觉上一号。
  static readonly tabHome: Resource = $r('sys.symbol.house')
  static readonly tabNodes: Resource = $r('sys.symbol.square_grid_2x2')
  static readonly tabSettings: Resource = $r('sys.symbol.gearshape')

  // 状态
  static readonly statusOn: Resource = $r('sys.symbol.checkmark_circle')
  static readonly statusOff: Resource = $r('sys.symbol.power')
  static readonly statusError: Resource = $r('sys.symbol.exclamationmark_triangle')
  static readonly statusInfo: Resource = $r('sys.symbol.info_circle')
  static readonly pass: Resource = $r('sys.symbol.checkmark_circle')
  static readonly fail: Resource = $r('sys.symbol.xmark_circle')
  static readonly warn: Resource = $r('sys.symbol.exclamationmark_circle')

  // 操作
  static readonly copy: Resource = $r('sys.symbol.doc_text_on_doc_text')
  static readonly refresh: Resource = $r('sys.symbol.arrow_clockwise')
  static readonly chevron: Resource = $r('sys.symbol.chevron_right')
  static readonly trash: Resource = $r('sys.symbol.trash')
  static readonly search: Resource = $r('sys.symbol.magnifyingglass')
  static readonly eye: Resource = $r('sys.symbol.eye')
  static readonly eyeSlash: Resource = $r('sys.symbol.eye_slash')

  // 概念
  static readonly server: Resource = $r('sys.symbol.externaldrive')
  static readonly key: Resource = $r('sys.symbol.key')
  static readonly device: Resource = $r('sys.symbol.phone')
  static readonly exitNode: Resource = $r('sys.symbol.arrowshape_turn_up_right')
  static readonly secure: Resource = $r('sys.symbol.checkmark_shield')
  static readonly diagnostics: Resource = $r('sys.symbol.stethoscope')
  static readonly tools: Resource = $r('sys.symbol.wrench_and_screwdriver')
  static readonly dns: Resource = $r('sys.symbol.magnifyingglass_circle')
}

// 语义色调，组件只认 tone 不认具体颜色，避免每个调用点自己配色。
export enum Tone {
  Neutral,
  Accent,
  Ok,
  Warn,
  Danger
}

export function toneColor(tone: Tone): Resource {
  switch (tone) {
    case Tone.Ok:
      return Palette.ok
    case Tone.Warn:
      return Palette.warn
    case Tone.Danger:
      return Palette.danger
    case Tone.Accent:
      return Palette.accentText
    default:
      return Palette.textTertiary
  }
}

export function toneIcon(tone: Tone): Resource {
  switch (tone) {
    case Tone.Ok:
      return Palette.ok
    case Tone.Warn:
      return Palette.warn
    case Tone.Danger:
      return Palette.danger
    case Tone.Accent:
      return Palette.accentIcon
    default:
      return Palette.iconTertiary
  }
}
