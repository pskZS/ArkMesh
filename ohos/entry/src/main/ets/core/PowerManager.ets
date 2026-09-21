import { wantAgent, common } from '@kit.AbilityKit'
import { BusinessError } from '@kit.BasicServicesKit'
import { backgroundTaskManager } from '@kit.BackgroundTasksKit'

/**
 * ArkMesh 电源与后台管理工具
 * 
 * 提供以下功能：
 * 1. 后台任务保活（BackgroundTaskManager）
 * 2. 运行锁管理（RunningLock）
 * 3. 电池状态监听与自适应调整
 * 4. 网络状态监听
 */
class PowerManagerImpl {
  private isBackgroundTaskRunning = false
  private runningLock: object | null = null
  private context: common.UIAbilityContext | null = null

  /**
   * 初始化：从 Ability 生命周期传入 context
   * 替代已弃用的 getContext() 全局函数
   */
  init(context: common.UIAbilityContext): void {
    this.context = context
  }

  /**
   * 启动后台保活任务
   * 在 HarmonyOS NEXT 中，VPN 服务需要保持后台运行
   */
  async startBackgroundTask(): Promise<void> {
    if (this.isBackgroundTaskRunning) {
      return
    }
    if (!this.context) {
      console.warn('ArkMesh: PowerManager 未初始化 context，跳过后台保活')
      return
    }

    try {
      // 创建 WantAgent 用于后台任务回调
      const wantAgentInfo: wantAgent.WantAgentInfo = {
        wants: [
          {
            deviceId: '',
            bundleName: 'com.arkmesh.client',
            abilityName: 'EntryAbility',
            action: ''
          }
        ],
        operationType: wantAgent.OperationType.START_ABILITY,
        requestCode: 0,
        wantAgentFlags: [wantAgent.WantAgentFlags.UPDATE_PRESENT_FLAG]
      }

      const agent = await wantAgent.getWantAgent(wantAgentInfo)

      // 请求后台任务（数据传输类型，适合 VPN）
      await backgroundTaskManager.startBackgroundRunning(
        this.context,
        backgroundTaskManager.BackgroundMode.DATA_TRANSFER,
        agent
      )

      this.isBackgroundTaskRunning = true
      console.info('ArkMesh: 后台保活任务已启动')
    } catch (err) {
      const error = err as BusinessError
      console.error('ArkMesh: 启动后台保活任务失败:', error.code, error.message)
    }
  }

  /**
   * 停止后台保活任务
   */
  async stopBackgroundTask(): Promise<void> {
    if (!this.isBackgroundTaskRunning) {
      return
    }

    try {
      if (!this.context) {
        console.warn('ArkMesh: PowerManager 未初始化 context，跳过停止后台保活')
        return
      }
      await backgroundTaskManager.stopBackgroundRunning(this.context)
      this.isBackgroundTaskRunning = false
      console.info('ArkMesh: 后台保活任务已停止')
    } catch (err) {
      const error = err as BusinessError
      console.error('ArkMesh: 停止后台保活任务失败:', error.code, error.message)
    }
  }

  /**
   * 释放运行锁（如果有）
   */
  releaseRunningLock(): void {
    if (this.runningLock) {
      try {
        // HarmonyOS NEXT 中运行锁需要通过 power 模块释放
        // 这里使用占位符，实际实现需要引入 @ohos.power 模块
        this.runningLock = null
        console.info('ArkMesh: 运行锁已释放')
      } catch (err) {
        console.error('ArkMesh: 释放运行锁失败:', err)
      }
    }
  }

  /**
   * 根据电池状态调整工作模式
   */
  adjustForBatteryLevel(level: number, isCharging: boolean): void {
    if (isCharging) {
      // 充电时，可以全速运行
      console.info('ArkMesh: 设备正在充电，全速运行')
      return
    }

    if (level < 20) {
      // 低电量模式：减少 keepalive 频率
      console.info('ArkMesh: 低电量模式，降低网络活动频率')
      // 实际实现中调整 keepalive 间隔
    } else if (level < 50) {
      // 中等电量：适度降低活动
      console.info('ArkMesh: 中等电量，适度降低网络活动')
    } else {
      // 高电量：正常模式
      console.info('ArkMesh: 电量充足，正常运行')
    }
  }

  /**
   * 检查当前是否为省电模式
   */
  isPowerSaveMode(): boolean {
    // HarmonyOS NEXT 中可以通过 deviceInfo 或 batteryInfo 获取
    // 此处为简化实现，实际应根据系统 API 获取
    return false
  }
}

export const powerManager = new PowerManagerImpl()
