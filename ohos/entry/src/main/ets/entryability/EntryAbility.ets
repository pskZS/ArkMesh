import { UIAbility, Want, AbilityConstant } from '@kit.AbilityKit'
import { window } from '@kit.ArkUI'
import { storageUtils } from '../core/StorageUtils'
import { powerManager } from '../core/PowerManager'

export default class EntryAbility extends UIAbility {
  onCreate(want: Want, launchParam: AbilityConstant.LaunchParam) {
    console.info('EntryAbility onCreate')
    // 将 Ability context 传给 PowerManager（替代已弃用的 getContext() 全局函数）
    powerManager.init(this.context)
    // Initialize persistent storage
    try {
      storageUtils.init(this.context)
    } catch (err) {
      console.error('Failed to init storage:', err)
    }
  }

  onWindowStageCreate(windowStage: window.WindowStage) {
    console.info('EntryAbility onWindowStageCreate')

    // Configure window to avoid full-screen mode and ensure proper layout
    try {
      const mainWindow = windowStage.getMainWindowSync()
      if (mainWindow) {
        mainWindow.setWindowLayoutFullScreen(false)
      }
    } catch (err) {
      console.error('Failed to configure window:', err)
    }

    // Set main window
    windowStage.loadContent('pages/Index', (err, data) => {
      if (err.code) {
        console.error('Failed to load content:', JSON.stringify(err))
        return
      }
      console.info('Content loaded successfully')
    })
  }

  onWindowStageDestroy() {
    console.info('EntryAbility onWindowStageDestroy')
  }

  onForeground() {
    console.info('EntryAbility onForeground')
  }

  onBackground() {
    console.info('EntryAbility onBackground')
  }
}
