import { HomeView } from '../view/HomeView'
import { NodesView } from '../view/NodesView'
import { SettingsView } from '../view/SettingsView'
import { Palette, Type, Space, Size, Icon } from '../common/Theme'

// 底部导航用的是系统 SymbolGlyph 线性图标，不再依赖自带的一套 svg。
// 选中态只改颜色，不换字形，深色模式和系统强调色都自动跟随。
@Entry
@Component
struct Index {
  @State activeTab: number = 0

  private symbolOf(index: number): Resource {
    if (index === 1) {
      return Icon.tabNodes
    }
    if (index === 2) {
      return Icon.tabSettings
    }
    return Icon.tabHome
  }

  private labelOf(index: number): string {
    if (index === 1) {
      return '节点'
    }
    if (index === 2) {
      return '设置'
    }
    return '概览'
  }

  @Builder
  tabBar(index: number) {
    Column({ space: Space.xxs }) {
      SymbolGlyph(this.symbolOf(index))
        .fontSize(Size.iconTab)
        .fontColor([this.activeTab === index ? Palette.accentIcon : Palette.iconTertiary])
      Text(this.labelOf(index))
        .fontSize(Type.micro)
        .fontColor(this.activeTab === index ? Palette.accentText : Palette.textTertiary)
    }
    .width('100%')
    .height('100%')
    .justifyContent(FlexAlign.Center)
    .accessibilityText(this.labelOf(index))
  }

  build() {
    Tabs({ barPosition: BarPosition.End, index: this.activeTab }) {
      TabContent() {
        HomeView({ activeTab: $activeTab })
      }
      .tabBar(this.tabBar(0))

      TabContent() {
        NodesView({ tabVisible: this.activeTab === 1 })
      }
      .tabBar(this.tabBar(1))

      TabContent() {
        SettingsView({ tabVisible: this.activeTab === 2 })
      }
      .tabBar(this.tabBar(2))
    }
    .scrollable(false)
    .animationDuration(200)
    .barBackgroundColor(Palette.card)
    .divider({ strokeWidth: 0.5, color: Palette.divider })
    .backgroundColor(Palette.page)
    .onChange((index: number) => {
      this.activeTab = index
    })
  }
}
