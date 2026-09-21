import { preferences } from '@kit.ArkData'

class StorageUtilsImpl {
  private pref: preferences.Preferences | null = null
  private initialized = false

  init(context: any) {
    if (this.initialized) return
    try {
      this.pref = preferences.getPreferencesSync(context, { name: 'ArkMesh' })
      this.initialized = true
    } catch (e) {
      console.error('StorageUtils init failed:', e)
    }
  }

  private ensureInit(): preferences.Preferences | null {
    return this.pref
  }

  setString(key: string, value: string): void {
    const pref = this.ensureInit()
    if (!pref) return
    try {
      pref.putSync(key, value)
    } catch (e) {
      console.error('StorageUtils setString error:', e)
    }
  }

  getString(key: string, defaultValue: string = ''): string {
    const pref = this.ensureInit()
    if (!pref) return defaultValue
    try {
      return pref.getSync(key, defaultValue) as string
    } catch (e) {
      console.error('StorageUtils getString error:', e)
      return defaultValue
    }
  }

  setNumber(key: string, value: number): void {
    const pref = this.ensureInit()
    if (!pref) return
    try {
      pref.putSync(key, value)
    } catch (e) {
      console.error('StorageUtils setNumber error:', e)
    }
  }

  getNumber(key: string, defaultValue: number = 0): number {
    const pref = this.ensureInit()
    if (!pref) return defaultValue
    try {
      return pref.getSync(key, defaultValue) as number
    } catch (e) {
      console.error('StorageUtils getNumber error:', e)
      return defaultValue
    }
  }

  setBoolean(key: string, value: boolean): void {
    const pref = this.ensureInit()
    if (!pref) return
    try {
      pref.putSync(key, value)
    } catch (e) {
      console.error('StorageUtils setBoolean error:', e)
    }
  }

  getBoolean(key: string, defaultValue: boolean = false): boolean {
    const pref = this.ensureInit()
    if (!pref) return defaultValue
    try {
      return pref.getSync(key, defaultValue) as boolean
    } catch (e) {
      console.error('StorageUtils getBoolean error:', e)
      return defaultValue
    }
  }

  delete(key: string): void {
    const pref = this.ensureInit()
    if (!pref) return
    try {
      pref.deleteSync(key)
    } catch (e) {
      console.error('StorageUtils delete error:', e)
    }
  }

  clear(): void {
    const pref = this.ensureInit()
    if (!pref) return
    try {
      pref.clearSync()
    } catch (e) {
      console.error('StorageUtils clear error:', e)
    }
  }

  flush(): void {
    const pref = this.ensureInit()
    if (!pref) return
    try {
      pref.flushSync()
    } catch (e) {
      console.error('StorageUtils flush error:', e)
    }
  }
}

export const storageUtils = new StorageUtilsImpl()
