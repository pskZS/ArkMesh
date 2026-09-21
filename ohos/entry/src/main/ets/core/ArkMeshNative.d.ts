declare module 'libArkMesh.so' {
  interface ArkMeshNativeConfig {
    serverUrl: string
    authKey: string
    userName: string
    machineName: string
  }

  interface ArkMeshNativeModule {
    initialize(): number
    connect(): number
    disconnect(): number
    getStatus(): string
    isConnected(): boolean
    getMeshIP(): string
    getNodes(): string
    generateMachineKey(): string
    registerWithAuthKey(authKey: string): boolean
    initializeWithConfig(config: ArkMeshNativeConfig): boolean
    goProbe(): string
    goStartTicker(): number
    goTicks(): number
    goTlsProbe(): string
    goUdpStart(): string
    goUdpStats(): string
    goUdpSendTo(hostPort: string, payload: string): string
    goDnsLookup(host: string): string
    goTunStart(fd: number): string
    goTunWrite(fd: number, hexPkt: string): number
    goTunStats(): string
    goTunCapture(fd: number, dst: string, marker: string): string
    goTunBuildUdp(srcIp: string, dstIp: string, sport: number, dport: number, payload: string): string
    coreSelfTest(): Promise<string>
    coreNetCheck(host: string): Promise<string>
    coreIfProbe(): Promise<string>
    coreControlUp(controlURL: string, authKey: string, hostname: string, stateDir: string, exitNode?: string): string
    coreControlStatus(): string
    corePingPeers(type: string): string
    coreDnsProbe(name: string, timeoutMs: number): string
    coreControlDown(): string
    coreAttachTun(fd: number, mtu: number): string
  }

  const module: ArkMeshNativeModule
  export default module
}
