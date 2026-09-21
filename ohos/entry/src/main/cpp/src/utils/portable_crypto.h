#ifndef PORTABLE_CRYPTO_H
#define PORTABLE_CRYPTO_H

// Self-contained cryptographic primitives for ArkMesh.
//
// These implementations follow the relevant RFCs and have no external
// dependency on OpenSSL. They are used when a prebuilt OpenSSL library is
// not available for the target HarmonyOS architecture.
//
//   * SHA-256                - FIPS 180-4
//   * HMAC-SHA-256           - RFC 2104
//   * HKDF-SHA-256           - RFC 5869
//   * ChaCha20-Poly1305 AEAD - RFC 8439
//   * X25519                 - RFC 7748
//
// All sizes below are in bytes unless stated otherwise.

#include <cstdint>
#include <cstddef>

namespace arkmesh {

// ---------------------------------------------------------------
// SHA-256
// ---------------------------------------------------------------

// Computes the 32-byte SHA-256 digest of `data`.
void sha256(const uint8_t* data, size_t len, uint8_t out[32]);

// ---------------------------------------------------------------
// HMAC-SHA-256
// ---------------------------------------------------------------

// Computes the 32-byte HMAC-SHA-256 tag.
void hmac_sha256(const uint8_t* key, size_t keyLen,
                 const uint8_t* data, size_t dataLen,
                 uint8_t out[32]);

// ---------------------------------------------------------------
// HKDF-SHA-256 (RFC 5869)
// ---------------------------------------------------------------

// Derives `outLen` bytes of key material. `salt` and `info` may be null/empty.
void hkdf_sha256(const uint8_t* salt, size_t saltLen,
                 const uint8_t* ikm, size_t ikmLen,
                 const uint8_t* info, size_t infoLen,
                 uint8_t* out, size_t outLen);

// ---------------------------------------------------------------
// ChaCha20-Poly1305 AEAD (RFC 8439)
// ---------------------------------------------------------------

// key = 32 bytes, nonce = 12 bytes, tag = 16 bytes.
// ciphertext must have room for `plaintextLen` bytes; tag is written separately.
bool chacha20poly1305_encrypt(const uint8_t* key,
                              const uint8_t* nonce,
                              const uint8_t* plaintext, size_t plaintextLen,
                              const uint8_t* aad, size_t aadLen,
                              uint8_t* ciphertext,
                              uint8_t tag[16]);

// Returns false if authentication fails.
bool chacha20poly1305_decrypt(const uint8_t* key,
                              const uint8_t* nonce,
                              const uint8_t* ciphertext, size_t ciphertextLen,
                              const uint8_t* aad, size_t aadLen,
                              const uint8_t tag[16],
                              uint8_t* plaintext);

// ---------------------------------------------------------------
// X25519 (RFC 7748)
// ---------------------------------------------------------------

// Derives the 32-byte public key for a 32-byte private key (scalar).
bool x25519_public_key(const uint8_t privateKey[32], uint8_t publicKey[32]);

// Computes the 32-byte shared secret: privateKey * peerPublicKey.
bool x25519_shared_secret(const uint8_t privateKey[32],
                          const uint8_t peerPublicKey[32],
                          uint8_t sharedSecret[32]);

// ---------------------------------------------------------------
// Secure random
// ---------------------------------------------------------------

// Fills `out` with `len` cryptographically secure random bytes.
// Returns false on failure.
bool secure_random(uint8_t* out, size_t len);

} // namespace arkmesh

#endif // PORTABLE_CRYPTO_H
