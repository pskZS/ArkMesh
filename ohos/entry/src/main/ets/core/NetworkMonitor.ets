import { connection } from '@kit.NetworkKit'
import { BusinessError } from '@kit.BasicServicesKit'

/**
 * ArkMesh 网络状态监听器
 * 
 * 监听网络状态变化，实现自动重连机制
 */
export interface NetworkStatus {
  isConnected: boolean
  netType: string
  isWifi: boolean
  isCellular: boolean
}

interface NetCap {
  bearerTypes: number[]
}

interface NetCapabilityInfo {
  netCap: NetCap
}

class NetworkMonitorImpl {
  private connection: connection.NetConnection | null = null
  private isMonitoring = false
  private currentStatus: NetworkStatus = {
    isConnected: false,
    netType: 'none',
    isWifi: false,
    isCellular: false
  }

  private statusChangeListeners: Array<(status: NetworkStatus) => void> = []
  private disconnectListeners: Array<() => void> = []
  private reconnectListeners: Array<() => void> = []

  /**
   * 开始监听网络状态
   */
  startMonitoring(): void {
    if (this.isMonitoring) {
      return
    }

    try {
      this.connection = connection.createNetConnection()

      // 监听网络可用性变化
      this.connection.on('netAvailable', (data: object) => {
        console.info('ArkMesh: 网络可用:', JSON.stringify(data))
        this.currentStatus.isConnected = true
        this.notifyStatusChange()
      })

      // 监听网络断开
      this.connection.on('netUnavailable', () => {
        console.info('ArkMesh: 网络断开')
        this.currentStatus.isConnected = false
        this.currentStatus.netType = 'none'
        this.currentStatus.isWifi = false
        this.currentStatus.isCellular = false
        this.notifyStatusChange()
        this.notifyDisconnect()
      })

      // 监听网络类型变化
      this.connection.on('netCapabilitiesChange', (data: NetCapabilityInfo) => {
        console.info('ArkMesh: 网络能力变化:', JSON.stringify(data))
        if (data && data.netCap) {
          const bearerTypes = data.netCap.bearerTypes
          this.currentStatus.isWifi = bearerTypes?.includes(0) ?? false
          this.currentStatus.isCellular = bearerTypes?.includes(1) ?? false
          this.currentStatus.netType = this.currentStatus.isWifi ? 'wifi' : 
                                      this.currentStatus.isCellular ? 'cellular' : 'unknown'
          this.notifyStatusChange()

          // 如果之前断网现在恢复，触发重连
          if (this.currentStatus.isConnected && !this.currentStatus.isConnected) {
            this.currentStatus.isConnected = true
            this.notifyReconnect()
          }
        }
      })

      this.connection.register((err: BusinessError) => {
        if (err) {
          console.error('ArkMesh: 网络监听注册失败:', err.message)
          return
        }
        console.info('ArkMesh: 网络监听已注册')
      })

      this.isMonitoring = true
      console.info('ArkMesh: 网络状态监听已启动')
    } catch (err) {
      console.error('ArkMesh: 启动网络监听失败:', err)
    }
  }

  /**
   * 停止监听网络状态
   */
  stopMonitoring(): void {
    if (!this.isMonitoring || !this.connection) {
      return
    }

    try {
      this.connection.unregister((err: BusinessError) => {
        if (err) {
          console.error('ArkMesh: 网络监听注销失败:', err.message)
        } else {
          console.info('ArkMesh: 网络监听已注销')
        }
      })
      this.connection = null
      this.isMonitoring = false
    } catch (err) {
      console.error('ArkMesh: 停止网络监听失败:', err)
    }
  }

  /**
   * 获取当前网络状态
   */
  getCurrentStatus(): NetworkStatus {
    return this.currentStatus
  }

  /**
   * 注册状态变化监听器
   */
  onStatusChange(listener: (status: NetworkStatus) => void): void {
    this.statusChangeListeners.push(listener)
  }

  /**
   * 注册断网监听器
   */
  onDisconnect(listener: () => void): void {
    this.disconnectListeners.push(listener)
  }

  /**
   * 注册重连监听器
   */
  onReconnect(listener: () => void): void {
    this.reconnectListeners.push(listener)
  }

  private notifyStatusChange(): void {
    this.statusChangeListeners.forEach(listener => {
      try {
        listener(this.currentStatus)
      } catch (err) {
        console.error('ArkMesh: 网络状态监听回调异常:', err)
      }
    })
  }

  private notifyDisconnect(): void {
    this.disconnectListeners.forEach(listener => {
      try {
        listener()
      } catch (err) {
        console.error('ArkMesh: 断网监听回调异常:', err)
      }
    })
  }

  private notifyReconnect(): void {
    this.reconnectListeners.forEach(listener => {
      try {
        listener()
      } catch (err) {
        console.error('ArkMesh: 重连监听回调异常:', err)
      }
    })
  }
}

export const networkMonitor = new NetworkMonitorImpl()
