// Tailscale连接状态枚举
export enum ArkMeshStatus {
  DISCONNECTED = 'disconnected',
  CONNECTING = 'connecting',
  CONNECTED = 'connected',
  ERROR = 'error'
}

// 连接状态描述
export const StatusDescriptions: Record<ArkMeshStatus, string> = {
  [ArkMeshStatus.DISCONNECTED]: '未连接',
  [ArkMeshStatus.CONNECTING]: '连接中...',
  [ArkMeshStatus.CONNECTED]: '已连接',
  [ArkMeshStatus.ERROR]: '连接失败'
}
