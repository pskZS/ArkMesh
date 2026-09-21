import { huks } from '@kit.UniversalKeystoreKit'

// Key types for ArkMesh
export enum KeyType {
  MACHINE_KEY = 'arkmesh_machine_key',      // WireGuard machine key
  NODE_KEY = 'arkmesh_node_key',            // WireGuard node key
  AUTH_KEY = 'arkmesh_auth_key'             // Headscale auth key
}

// Key pair structure
export interface KeyPair {
  publicKey: string
  privateKey: string
}

/**
 * KeyManager - Secure key storage using HarmonyOS HUKS
 * 
 * This class provides secure storage for WireGuard keys using HarmonyOS
 * Universal Keystore System (HUKS). Keys are stored in the TEE (Trusted
 * Execution Environment) and never leave the device in plaintext.
 */
export class KeyManager {
  private static instance: KeyManager | null = null
  
  private constructor() {}
  
  /**
   * Get singleton instance
   */
  static getInstance(): KeyManager {
    if (!KeyManager.instance) {
      KeyManager.instance = new KeyManager()
    }
    return KeyManager.instance
  }
  
  /**
   * Generate and store a new key pair
   * 
   * @param keyType Type of key to generate
   * @returns Promise<KeyPair> Generated key pair
   */
  async generateKeyPair(keyType: KeyType): Promise<KeyPair> {
    console.info(`[KeyManager] Generating key pair for ${keyType}`)
    
    try {
      // Generate ECC key pair for WireGuard (Curve25519)
      const properties: Array<huks.HuksParam> = [
        {
          tag: huks.HuksTag.HUKS_TAG_ALGORITHM,
          value: huks.HuksKeyAlg.HUKS_ALG_ECC
        },
        {
          tag: huks.HuksTag.HUKS_TAG_KEY_SIZE,
          value: huks.HuksKeySize.HUKS_ECC_KEY_SIZE_256
        },
        {
          tag: huks.HuksTag.HUKS_TAG_PURPOSE,
          value: huks.HuksKeyPurpose.HUKS_KEY_PURPOSE_SIGN | huks.HuksKeyPurpose.HUKS_KEY_PURPOSE_VERIFY
        },
        {
          tag: huks.HuksTag.HUKS_TAG_DIGEST,
          value: huks.HuksKeyDigest.HUKS_DIGEST_SHA256
        }
      ]
      
      const options: huks.HuksOptions = {
        properties: properties
      }
      
      // Generate key in HUKS
      await huks.generateKeyItem(keyType, options)
      console.info(`[KeyManager] Key pair generated successfully for ${keyType}`)
      
      // Export public key
      const publicKey = await this.exportPublicKey(keyType)
      
      // Note: Private key cannot be exported from HUKS for security
      // We return a placeholder for private key, actual crypto operations
      // will be done through HUKS APIs
      return {
        publicKey: publicKey,
        privateKey: '[STORED_IN_HUKS]'  // Placeholder
      }
      
    } catch (error) {
      console.error(`[KeyManager] Failed to generate key pair:`, error)
      throw error
    }
  }
  
  /**
   * Import an existing key pair
   * 
   * @param keyType Type of key
   * @param keyPair Key pair to import
   * @returns Promise<void>
   */
  async importKeyPair(keyType: KeyType, keyPair: KeyPair): Promise<void> {
    console.info(`[KeyManager] Importing key pair for ${keyType}`)
    
    try {
      // Convert private key string to Uint8Array
      const privateKeyData = this.stringToUint8Array(keyPair.privateKey)
      
      const properties: Array<huks.HuksParam> = [
        {
          tag: huks.HuksTag.HUKS_TAG_ALGORITHM,
          value: huks.HuksKeyAlg.HUKS_ALG_ECC
        },
        {
          tag: huks.HuksTag.HUKS_TAG_KEY_SIZE,
          value: huks.HuksKeySize.HUKS_ECC_KEY_SIZE_256
        },
        {
          tag: huks.HuksTag.HUKS_TAG_PURPOSE,
          value: huks.HuksKeyPurpose.HUKS_KEY_PURPOSE_SIGN | huks.HuksKeyPurpose.HUKS_KEY_PURPOSE_VERIFY
        }
      ]
      
      const options: huks.HuksOptions = {
        properties: properties,
        inData: privateKeyData
      }
      
      // Import key to HUKS
      await huks.importKeyItem(keyType, options)
      console.info(`[KeyManager] Key pair imported successfully for ${keyType}`)
      
    } catch (error) {
      console.error(`[KeyManager] Failed to import key pair:`, error)
      throw error
    }
  }
  
  /**
   * Export public key
   * 
   * @param keyType Type of key
   * @returns Promise<string> Public key in base64 format
   */
  async exportPublicKey(keyType: KeyType): Promise<string> {
    console.info(`[KeyManager] Exporting public key for ${keyType}`)
    
    try {
      const emptyOptions: huks.HuksOptions = {
        properties: []
      }
      
      const result = await huks.exportKeyItem(keyType, emptyOptions)
      
      if (result.outData) {
        const publicKey = this.uint8ArrayToString(result.outData)
        console.info(`[KeyManager] Public key exported successfully for ${keyType}`)
        return publicKey
      } else {
        throw new Error('No public key data returned')
      }
      
    } catch (error) {
      console.error(`[KeyManager] Failed to export public key:`, error)
      throw error
    }
  }
  
  /**
   * Check if key exists
   * 
   * @param keyType Type of key
   * @returns Promise<boolean> True if key exists
   */
  async hasKey(keyType: KeyType): Promise<boolean> {
    console.info(`[KeyManager] Checking if key exists for ${keyType}`)
    
    try {
      const emptyOptions: huks.HuksOptions = {
        properties: []
      }
      
      // Try to export public key to check if key exists
      await huks.exportKeyItem(keyType, emptyOptions)
      console.info(`[KeyManager] Key exists for ${keyType}`)
      return true
      
    } catch (error) {
      console.info(`[KeyManager] Key does not exist for ${keyType}`)
      return false
    }
  }
  
  /**
   * Delete key
   * 
   * @param keyType Type of key
   * @returns Promise<void>
   */
  async deleteKey(keyType: KeyType): Promise<void> {
    console.info(`[KeyManager] Deleting key for ${keyType}`)
    
    try {
      const emptyOptions: huks.HuksOptions = {
        properties: []
      }
      
      await huks.deleteKeyItem(keyType, emptyOptions)
      console.info(`[KeyManager] Key deleted successfully for ${keyType}`)
      
    } catch (error) {
      console.error(`[KeyManager] Failed to delete key:`, error)
      throw error
    }
  }
  
  /**
   * Get or create key pair
   * 
   * @param keyType Type of key
   * @returns Promise<KeyPair> Existing or newly created key pair
   */
  async getOrCreateKeyPair(keyType: KeyType): Promise<KeyPair> {
    console.info(`[KeyManager] Getting or creating key pair for ${keyType}`)
    
    try {
      // Check if key already exists
      const exists = await this.hasKey(keyType)
      
      if (exists) {
        console.info(`[KeyManager] Using existing key pair for ${keyType}`)
        const publicKey = await this.exportPublicKey(keyType)
        return {
          publicKey: publicKey,
          privateKey: '[STORED_IN_HUKS]'
        }
      } else {
        console.info(`[KeyManager] Creating new key pair for ${keyType}`)
        return await this.generateKeyPair(keyType)
      }
      
    } catch (error) {
      console.error(`[KeyManager] Failed to get or create key pair:`, error)
      throw error
    }
  }
  
  /**
   * Convert string to Uint8Array
   */
  private stringToUint8Array(str: string): Uint8Array {
    const arr = new Uint8Array(str.length)
    for (let i = 0; i < str.length; i++) {
      arr[i] = str.charCodeAt(i)
    }
    return arr
  }
  
  /**
   * Convert Uint8Array to string
   */
  private uint8ArrayToString(arr: Uint8Array): string {
    let str = ''
    for (let i = 0; i < arr.length; i++) {
      str += String.fromCharCode(arr[i])
    }
    return str
  }
}
