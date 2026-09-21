// 跨 Tab 的一次性意图传递。
//
// 概览页点"查看诊断"要跳到设置页并展开「高级诊断」，这两个 Tab 是兄弟组件，
// 没有父子数据通道。用 AppStorage 放一个递增 token，设置页用
// @StorageProp + @Watch 收到变化就展开 —— 每次都是新值，重复点也有效。
export const DIAG_FOCUS_KEY = 'arkmeshDiagFocus'

export function requestDiagnostics(): void {
  AppStorage.setOrCreate(DIAG_FOCUS_KEY, Date.now())
}
