#include "portable_crypto.h"

#include <cstring>
#include <vector>

#if defined(__linux__) || defined(__unix__)
#include <fcntl.h>
#include <unistd.h>
#endif

namespace arkmesh {

// =================================================================
// Utility helpers
// =================================================================

static inline uint32_t rotl32(uint32_t v, int n) {
    return (v << n) | (v >> (32 - n));
}

static inline uint32_t load32_le(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) |
           (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}

static inline void store32_le(uint8_t* p, uint32_t v) {
    p[0] = static_cast<uint8_t>(v);
    p[1] = static_cast<uint8_t>(v >> 8);
    p[2] = static_cast<uint8_t>(v >> 16);
    p[3] = static_cast<uint8_t>(v >> 24);
}

static inline uint64_t load64_le(const uint8_t* p) {
    uint64_t v = 0;
    for (int i = 7; i >= 0; --i) {
        v = (v << 8) | p[i];
    }
    return v;
}

static inline void store64_le(uint8_t* p, uint64_t v) {
    for (int i = 0; i < 8; ++i) {
        p[i] = static_cast<uint8_t>(v >> (8 * i));
    }
}

// =================================================================
// Secure random
// =================================================================

bool secure_random(uint8_t* out, size_t len) {
    if (!out) {
        return false;
    }
    if (len == 0) {
        return true;
    }

#if defined(__linux__) || defined(__unix__)
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd >= 0) {
        size_t off = 0;
        while (off < len) {
            ssize_t n = read(fd, out + off, len - off);
            if (n <= 0) {
                close(fd);
                return false;
            }
            off += static_cast<size_t>(n);
        }
        close(fd);
        return true;
    }
#endif

    // Never silently fall back to a non-cryptographic PRNG.
    return false;
}

// =================================================================
// SHA-256 (FIPS 180-4)
// =================================================================

namespace {

struct Sha256 {
    uint32_t h[8];
    uint64_t totalBytes;
    uint8_t buffer[64];
    size_t bufferLen;

    Sha256() {
        reset();
    }

    void reset() {
        h[0] = 0x6a09e667u;
        h[1] = 0xbb67ae85u;
        h[2] = 0x3c6ef372u;
        h[3] = 0xa54ff53au;
        h[4] = 0x510e527fu;
        h[5] = 0x9b05688cu;
        h[6] = 0x1f83d9abu;
        h[7] = 0x5be0cd19u;
        totalBytes = 0;
        bufferLen = 0;
        std::memset(buffer, 0, sizeof(buffer));
    }

    void compress(const uint8_t block[64]) {
        static const uint32_t K[64] = {
            0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5,
            0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
            0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
            0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
            0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
            0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
            0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
            0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
            0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
            0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
            0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3,
            0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
            0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5,
            0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
            0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
            0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
        };

        uint32_t w[64];
        for (int i = 0; i < 16; ++i) {
            w[i] = load32_le(block + 4 * i);
        }
        for (int i = 16; i < 64; ++i) {
            uint32_t s0 = rotl32(w[i - 15], 7) ^ rotl32(w[i - 15], 18) ^ (w[i - 15] >> 3);
            uint32_t s1 = rotl32(w[i - 2], 17) ^ rotl32(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }

        uint32_t a = h[0], b = h[1], c = h[2], d = h[3];
        uint32_t e = h[4], f = h[5], g = h[6], hh = h[7];

        for (int i = 0; i < 64; ++i) {
            uint32_t S1 = rotl32(e, 6) ^ rotl32(e, 11) ^ rotl32(e, 25);
            uint32_t ch = (e & f) ^ ((~e) & g);
            uint32_t t1 = hh + S1 + ch + K[i] + w[i];
            uint32_t S0 = rotl32(a, 2) ^ rotl32(a, 13) ^ rotl32(a, 22);
            uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
            uint32_t t2 = S0 + maj;

            hh = g;
            g = f;
            f = e;
            e = d + t1;
            d = c;
            c = b;
            b = a;
            a = t1 + t2;
        }

        h[0] += a;
        h[1] += b;
        h[2] += c;
        h[3] += d;
        h[4] += e;
        h[5] += f;
        h[6] += g;
        h[7] += hh;
    }

    void update(const uint8_t* data, size_t len) {
        totalBytes += len;
        while (len > 0) {
            size_t take = 64 - bufferLen;
            if (take > len) {
                take = len;
            }
            std::memcpy(buffer + bufferLen, data, take);
            bufferLen += take;
            data += take;
            len -= take;
            if (bufferLen == 64) {
                compress(buffer);
                bufferLen = 0;
            }
        }
    }

    void final(uint8_t out[32]) {
        uint64_t bitLen = totalBytes * 8;
        uint8_t pad = 0x80;
        update(&pad, 1);
        uint8_t zero = 0;
        while (bufferLen != 56) {
            update(&zero, 1);
        }
        uint8_t lenBytes[8];
        for (int i = 0; i < 8; ++i) {
            lenBytes[i] = static_cast<uint8_t>(bitLen >> (8 * (7 - i)));
        }
        update(lenBytes, 8);

        for (int i = 0; i < 8; ++i) {
            store32_le(out + 4 * i, h[i]);
        }
        reset();
    }
};

} // namespace

void sha256(const uint8_t* data, size_t len, uint8_t out[32]) {
    Sha256 ctx;
    ctx.update(data, len);
    ctx.final(out);
}

// =================================================================
// HMAC-SHA-256 (RFC 2104)
// =================================================================

void hmac_sha256(const uint8_t* key, size_t keyLen,
                 const uint8_t* data, size_t dataLen,
                 uint8_t out[32]) {
    uint8_t k[64];
    std::memset(k, 0, sizeof(k));
    if (keyLen > 64) {
        sha256(key, keyLen, k);
    } else {
        std::memcpy(k, key, keyLen);
    }

    uint8_t ipad[64];
    uint8_t opad[64];
    for (int i = 0; i < 64; ++i) {
        ipad[i] = k[i] ^ 0x36;
        opad[i] = k[i] ^ 0x5c;
    }

    Sha256 inner;
    inner.update(ipad, 64);
    inner.update(data, dataLen);
    uint8_t innerDigest[32];
    inner.final(innerDigest);

    Sha256 outer;
    outer.update(opad, 64);
    outer.update(innerDigest, 32);
    outer.final(out);
}

// =================================================================
// HKDF-SHA-256 (RFC 5869)
// =================================================================

void hkdf_sha256(const uint8_t* salt, size_t saltLen,
                 const uint8_t* ikm, size_t ikmLen,
                 const uint8_t* info, size_t infoLen,
                 uint8_t* out, size_t outLen) {
    uint8_t prk[32];
    if (salt && saltLen > 0) {
        hmac_sha256(salt, saltLen, ikm, ikmLen, prk);
    } else {
        uint8_t zeros[32] = {0};
        hmac_sha256(zeros, 32, ikm, ikmLen, prk);
    }

    std::vector<uint8_t> t;
    uint8_t counter = 1;
    size_t produced = 0;
    while (produced < outLen) {
        std::vector<uint8_t> block;
        block.insert(block.end(), t.begin(), t.end());
        if (info && infoLen > 0) {
            block.insert(block.end(), info, info + infoLen);
        }
        block.push_back(counter);

        uint8_t digest[32];
        hmac_sha256(prk, 32, block.data(), block.size(), digest);

        size_t take = outLen - produced;
        if (take > 32) {
            take = 32;
        }
        std::memcpy(out + produced, digest, take);
        produced += take;

        t.assign(digest, digest + 32);
        counter++;
    }
}

// =================================================================
// ChaCha20 core (RFC 8439)
// =================================================================

namespace {

struct ChaChaState {
    uint32_t s[16];

    void init(const uint8_t key[32], const uint8_t nonce[12], uint32_t counter) {
        s[0] = 0x61707865;
        s[1] = 0x3320646e;
        s[2] = 0x79622d32;
        s[3] = 0x6b206574;
        for (int i = 0; i < 8; ++i) {
            s[4 + i] = load32_le(key + 4 * i);
        }
        s[12] = counter;
        for (int i = 0; i < 3; ++i) {
            s[13 + i] = load32_le(nonce + 4 * i);
        }
    }

    void quarterRound(int a, int b, int c, int d) {
        s[a] += s[b]; s[d] ^= s[a]; s[d] = rotl32(s[d], 16);
        s[c] += s[d]; s[b] ^= s[c]; s[b] = rotl32(s[b], 12);
        s[a] += s[b]; s[d] ^= s[a]; s[d] = rotl32(s[d], 8);
        s[c] += s[d]; s[b] ^= s[c]; s[b] = rotl32(s[b], 7);
    }

    void block(uint8_t out[64]) {
        ChaChaState work = *this;
        for (int i = 0; i < 10; ++i) {
            work.quarterRound(0, 4, 8, 12);
            work.quarterRound(1, 5, 9, 13);
            work.quarterRound(2, 6, 10, 14);
            work.quarterRound(3, 7, 11, 15);
            work.quarterRound(0, 5, 10, 15);
            work.quarterRound(1, 6, 11, 12);
            work.quarterRound(2, 7, 8, 13);
            work.quarterRound(3, 4, 9, 14);
        }
        for (int i = 0; i < 16; ++i) {
            store32_le(out + 4 * i, work.s[i] + s[i]);
        }
    }
};

// Poly1305 (RFC 8439) using 26-bit limbs.
struct Poly1305 {
    uint32_t r[5];
    uint32_t h[5];
    uint32_t pad[4];

    void init(const uint8_t key[32]) {
        uint32_t t0 = load32_le(key + 0);
        uint32_t t1 = load32_le(key + 4);
        uint32_t t2 = load32_le(key + 8);
        uint32_t t3 = load32_le(key + 12);

        r[0] = t0 & 0x3ffffff;
        r[1] = ((t0 >> 26) | (t1 << 6)) & 0x3ffff03;
        r[2] = ((t1 >> 20) | (t2 << 12)) & 0x3ffc0ff;
        r[3] = ((t2 >> 14) | (t3 << 18)) & 0x3f03fff;
        r[4] = (t3 >> 8) & 0x00fffff;

        h[0] = h[1] = h[2] = h[3] = h[4] = 0;

        pad[0] = load32_le(key + 16);
        pad[1] = load32_le(key + 20);
        pad[2] = load32_le(key + 24);
        pad[3] = load32_le(key + 28);
    }

    void processBlock(const uint8_t block[16], bool last) {
        uint32_t n0 = load32_le(block + 0);
        uint32_t n1 = load32_le(block + 4);
        uint32_t n2 = load32_le(block + 8);
        uint32_t n3 = load32_le(block + 12);

        h[0] += n0 & 0x3ffffff;
        h[1] += ((n0 >> 26) | (n1 << 6)) & 0x3ffffff;
        h[2] += ((n1 >> 20) | (n2 << 12)) & 0x3ffffff;
        h[3] += ((n2 >> 14) | (n3 << 18)) & 0x3ffffff;
        h[4] += (n3 >> 8) & 0x3ffffff;

        // For the final partial block add 2^128 (the "hibit"). In radix 2^26
        // limb 4 represents bits 104..129, so 2^128 == 1 << 24.
        if (last) {
            h[4] += 1 << 24;
        }

        // Multiply by r.
        uint64_t d0 = (uint64_t)h[0] * r[0] + (uint64_t)h[1] * (5 * r[4]) +
                      (uint64_t)h[2] * (5 * r[3]) + (uint64_t)h[3] * (5 * r[2]) +
                      (uint64_t)h[4] * (5 * r[1]);
        uint64_t d1 = (uint64_t)h[0] * r[1] + (uint64_t)h[1] * r[0] +
                      (uint64_t)h[2] * (5 * r[4]) + (uint64_t)h[3] * (5 * r[3]) +
                      (uint64_t)h[4] * (5 * r[2]);
        uint64_t d2 = (uint64_t)h[0] * r[2] + (uint64_t)h[1] * r[1] +
                      (uint64_t)h[2] * r[0] + (uint64_t)h[3] * (5 * r[4]) +
                      (uint64_t)h[4] * (5 * r[3]);
        uint64_t d3 = (uint64_t)h[0] * r[3] + (uint64_t)h[1] * r[2] +
                      (uint64_t)h[2] * r[1] + (uint64_t)h[3] * r[0] +
                      (uint64_t)h[4] * (5 * r[4]);
        uint64_t d4 = (uint64_t)h[0] * r[4] + (uint64_t)h[1] * r[3] +
                      (uint64_t)h[2] * r[2] + (uint64_t)h[3] * r[1] +
                      (uint64_t)h[4] * r[0];

        uint32_t c;
        c = (uint32_t)(d0 >> 26); h[0] = (uint32_t)d0 & 0x3ffffff; d1 += c;
        c = (uint32_t)(d1 >> 26); h[1] = (uint32_t)d1 & 0x3ffffff; d2 += c;
        c = (uint32_t)(d2 >> 26); h[2] = (uint32_t)d2 & 0x3ffffff; d3 += c;
        c = (uint32_t)(d3 >> 26); h[3] = (uint32_t)d3 & 0x3ffffff; d4 += c;
        c = (uint32_t)(d4 >> 26); h[4] = (uint32_t)d4 & 0x3ffffff; h[0] += c * 5;
        c = h[0] >> 26; h[0] &= 0x3ffffff; h[1] += c;
    }

    void final(uint8_t tag[16]) {
        uint32_t c;
        c = h[1] >> 26; h[1] &= 0x3ffffff; h[2] += c;
        c = h[2] >> 26; h[2] &= 0x3ffffff; h[3] += c;
        c = h[3] >> 26; h[3] &= 0x3ffffff; h[4] += c;
        c = h[4] >> 26; h[4] &= 0x3ffffff; h[0] += c * 5;
        c = h[0] >> 26; h[0] &= 0x3ffffff; h[1] += c;

        // Compute h + -p.
        uint32_t g0 = h[0] + 5;
        c = g0 >> 26; g0 &= 0x3ffffff;
        uint32_t g1 = h[1] + c;
        c = g1 >> 26; g1 &= 0x3ffffff;
        uint32_t g2 = h[2] + c;
        c = g2 >> 26; g2 &= 0x3ffffff;
        uint32_t g3 = h[3] + c;
        c = g3 >> 26; g3 &= 0x3ffffff;
        uint32_t g4 = h[4] + c - (1 << 26);

        // Select h if h < p, else h - p.
        uint32_t mask = (g4 >> 31) - 1;
        g0 &= mask;
        g1 &= mask;
        g2 &= mask;
        g3 &= mask;
        g4 &= mask;
        mask = ~mask;
        h[0] = (h[0] & mask) | g0;
        h[1] = (h[1] & mask) | g1;
        h[2] = (h[2] & mask) | g2;
        h[3] = (h[3] & mask) | g3;
        h[4] = (h[4] & mask) | g4;

        h[0] |= h[1] << 26;
        h[1] = (h[1] >> 6) | (h[2] << 20);
        h[2] = (h[2] >> 12) | (h[3] << 14);
        h[3] = (h[3] >> 18) | (h[4] << 8);

        uint64_t f0 = (uint64_t)h[0] + pad[0];
        uint64_t f1 = (uint64_t)h[1] + pad[1] + (f0 >> 32);
        f0 &= 0xffffffff;
        uint64_t f2 = (uint64_t)h[2] + pad[2] + (f1 >> 32);
        f1 &= 0xffffffff;
        uint64_t f3 = (uint64_t)h[3] + pad[3] + (f2 >> 32);
        f2 &= 0xffffffff;

        store32_le(tag, (uint32_t)f0);
        store32_le(tag + 4, (uint32_t)f1);
        store32_le(tag + 8, (uint32_t)f2);
        store32_le(tag + 12, (uint32_t)f3);
    }
};

} // namespace

// =================================================================
// ChaCha20-Poly1305 AEAD (RFC 8439)
// =================================================================

static void poly1305_authenticate(Poly1305& mac, const uint8_t* data, size_t len) {
    // RFC 8439 AEAD applies pad16 to AAD and ciphertext independently:
    // each chunk is zero-padded to a 16-byte boundary. The padded block is a
    // full block (hibit = 0), so `last` is always false here.
    size_t off = 0;
    while (off + 16 <= len) {
        mac.processBlock(data + off, false);
        off += 16;
    }
    if (off < len) {
        uint8_t buf[16] = {0};
        std::memcpy(buf, data + off, len - off);
        mac.processBlock(buf, false);
    }
}

bool chacha20poly1305_encrypt(const uint8_t* key,
                              const uint8_t* nonce,
                              const uint8_t* plaintext, size_t plaintextLen,
                              const uint8_t* aad, size_t aadLen,
                              uint8_t* ciphertext,
                              uint8_t tag[16]) {
    // Derive the one-time Poly1305 key from ChaCha20 block 0.
    ChaChaState polyState;
    polyState.init(key, nonce, 0);
    uint8_t polyBlock[64];
    polyState.block(polyBlock);

    Poly1305 mac;
    mac.init(polyBlock);

    poly1305_authenticate(mac, aad, aadLen);

    // Encrypt the plaintext in one pass.
    ChaChaState encState;
    encState.init(key, nonce, 1);
    uint32_t counter = 1;
    size_t off = 0;
    while (off < plaintextLen) {
        uint8_t stream[64];
        encState.block(stream);
        size_t take = plaintextLen - off;
        if (take > 64) {
            take = 64;
        }
        for (size_t i = 0; i < take; ++i) {
            ciphertext[off + i] = plaintext[off + i] ^ stream[i];
        }
        off += take;
        counter++;
        encState.s[12] = counter;
    }

    // Authenticate the complete ciphertext (pad16 applied once).
    poly1305_authenticate(mac, ciphertext, plaintextLen);

    uint8_t lenBlock[16];
    store64_le(lenBlock, aadLen);
    store64_le(lenBlock + 8, plaintextLen);
    mac.processBlock(lenBlock, false);

    mac.final(tag);
    return true;
}

bool chacha20poly1305_decrypt(const uint8_t* key,
                              const uint8_t* nonce,
                              const uint8_t* ciphertext, size_t ciphertextLen,
                              const uint8_t* aad, size_t aadLen,
                              const uint8_t tag[16],
                              uint8_t* plaintext) {
    ChaChaState polyState;
    polyState.init(key, nonce, 0);
    uint8_t polyBlock[64];
    polyState.block(polyBlock);

    Poly1305 mac;
    mac.init(polyBlock);

    poly1305_authenticate(mac, aad, aadLen);
    poly1305_authenticate(mac, ciphertext, ciphertextLen);

    uint8_t lenBlock[16];
    store64_le(lenBlock, aadLen);
    store64_le(lenBlock + 8, ciphertextLen);
    mac.processBlock(lenBlock, false);

    uint8_t computed[16];
    mac.final(computed);

    uint8_t diff = 0;
    for (int i = 0; i < 16; ++i) {
        diff |= computed[i] ^ tag[i];
    }
    if (diff != 0) {
        return false;
    }

    ChaChaState decState;
    decState.init(key, nonce, 1);
    uint32_t counter = 1;
    size_t off = 0;
    while (off < ciphertextLen) {
        uint8_t stream[64];
        decState.block(stream);
        size_t take = ciphertextLen - off;
        if (take > 64) {
            take = 64;
        }
        for (size_t i = 0; i < take; ++i) {
            plaintext[off + i] = ciphertext[off + i] ^ stream[i];
        }
        off += take;
        counter++;
        decState.s[12] = counter;
    }
    return true;
}

// =================================================================
// X25519 (RFC 7748, Appendix A) - 5-limb radix 2^51 representation
// =================================================================

namespace {

typedef int64_t fe[5];

static void fe_0(fe h) {
    h[0] = 0; h[1] = 0; h[2] = 0; h[3] = 0; h[4] = 0;
}

static void fe_1(fe h) {
    h[0] = 1; h[1] = 0; h[2] = 0; h[3] = 0; h[4] = 0;
}

static void fe_copy(fe h, const fe f) {
    h[0] = f[0]; h[1] = f[1]; h[2] = f[2]; h[3] = f[3]; h[4] = f[4];
}

static void fe_frombytes(fe h, const uint8_t* s) {
    uint64_t c0 = load64_le(s);
    uint64_t c1 = load64_le(s + 8);
    uint64_t c2 = load64_le(s + 16);
    uint64_t c3 = load64_le(s + 24) & 0x7fffffffffffffffULL;

    const uint64_t M51 = 0x7ffffffffffffULL;
    h[0] = (int64_t)(c0 & M51);
    h[1] = (int64_t)(((c0 >> 51) | (c1 << 13)) & M51);
    h[2] = (int64_t)(((c1 >> 38) | (c2 << 26)) & M51);
    h[3] = (int64_t)(((c2 >> 25) | (c3 << 39)) & M51);
    h[4] = (int64_t)((c3 >> 12) & M51);
}

static void fe_tobytes(uint8_t* s, const fe h) {
    int64_t h0 = h[0], h1 = h[1], h2 = h[2], h3 = h[3], h4 = h[4];
    int64_t q;

    q = (h0 + (1LL << 25)) >> 26; h0 -= q << 26; h1 += q;
    q = (h1 + (1LL << 24)) >> 25; h1 -= q << 25; h2 += q;
    q = (h2 + (1LL << 25)) >> 26; h2 -= q << 26; h3 += q;
    q = (h3 + (1LL << 24)) >> 25; h3 -= q << 25; h4 += q;
    q = (h4 + (1LL << 25)) >> 26; h4 -= q << 26; h0 += q * 19;
    q = (h0 + (1LL << 25)) >> 26; h0 -= q << 26; h1 += q;

    q = (h1 + (1LL << 24)) >> 25; h1 -= q << 25; h2 += q;
    q = (h2 + (1LL << 25)) >> 26; h2 -= q << 26; h3 += q;
    q = (h3 + (1LL << 24)) >> 25; h3 -= q << 25; h4 += q;
    q = (h4 + (1LL << 25)) >> 26; h4 -= q << 26; h0 += q * 19;
    q = (h0 + (1LL << 25)) >> 26; h0 -= q << 26; h1 += q;

    uint64_t b0 = (uint64_t)h0;
    uint64_t b1 = (uint64_t)h1;
    uint64_t b2 = (uint64_t)h2;
    uint64_t b3 = (uint64_t)h3;
    uint64_t b4 = (uint64_t)h4;

    store64_le(s, b0 | (b1 << 51));
    store64_le(s + 8, (b1 >> 13) | (b2 << 38));
    store64_le(s + 16, (b2 >> 26) | (b3 << 25));
    store64_le(s + 24, (b3 >> 39) | (b4 << 12));
    s[31] = static_cast<uint8_t>(b4 >> 44) & 0x7f;
}

static void fe_add(fe h, const fe f, const fe g) {
    h[0] = f[0] + g[0];
    h[1] = f[1] + g[1];
    h[2] = f[2] + g[2];
    h[3] = f[3] + g[3];
    h[4] = f[4] + g[4];
}

static void fe_sub(fe h, const fe f, const fe g) {
    h[0] = f[0] - g[0];
    h[1] = f[1] - g[1];
    h[2] = f[2] - g[2];
    h[3] = f[3] - g[3];
    h[4] = f[4] - g[4];
}

static void fe_cswap(fe f, fe g, unsigned int b) {
    int64_t mask = (int64_t)b - 1;
    for (int i = 0; i < 5; ++i) {
        int64_t t = mask & (f[i] ^ g[i]);
        f[i] ^= t;
        g[i] ^= t;
    }
}

static void fe_mul(fe h, const fe f, const fe g) {
    __int128 t[5] = {0, 0, 0, 0, 0};
    for (int i = 0; i < 5; ++i) {
        for (int j = 0; j < 5; ++j) {
            __int128 prod = (__int128)f[i] * g[j];
            int idx = i + j;
            if (idx < 5) {
                t[idx] += prod;
            } else {
                t[idx - 5] += prod * 19;
            }
        }
    }

    for (int i = 0; i < 4; ++i) {
        int64_t carry = (int64_t)(t[i] >> 51);
        t[i] &= ((__int128)1 << 51) - 1;
        t[i + 1] += carry;
    }
    int64_t carry = (int64_t)(t[4] >> 51);
    t[4] &= ((__int128)1 << 51) - 1;
    t[0] += (__int128)carry * 19;
    carry = (int64_t)(t[0] >> 51);
    t[0] &= ((__int128)1 << 51) - 1;
    t[1] += carry;

    for (int i = 0; i < 5; ++i) {
        h[i] = (int64_t)t[i];
    }
}

static void fe_sq(fe h, const fe f) {
    fe_mul(h, f, f);
}

static void fe_mul121666(fe h, fe f) {
    __int128 t[5] = {0, 0, 0, 0, 0};
    for (int i = 0; i < 5; ++i) {
        __int128 prod = (__int128)f[i] * 121666;
        int idx = i;
        if (idx < 5) {
            t[idx] += prod;
        } else {
            t[idx - 5] += prod * 19;
        }
    }

    for (int i = 0; i < 4; ++i) {
        int64_t carry = (int64_t)(t[i] >> 51);
        t[i] &= ((__int128)1 << 51) - 1;
        t[i + 1] += carry;
    }
    int64_t carry = (int64_t)(t[4] >> 51);
    t[4] &= ((__int128)1 << 51) - 1;
    t[0] += (__int128)carry * 19;
    carry = (int64_t)(t[0] >> 51);
    t[0] &= ((__int128)1 << 51) - 1;
    t[1] += carry;

    for (int i = 0; i < 5; ++i) {
        h[i] = (int64_t)t[i];
    }
}

static void fe_invert(fe out, const fe z) {
    fe t0, t1, t2, t3;
    int i;

    fe_sq(t0, z);          // 2
    fe_sq(t1, t0);         // 4
    fe_sq(t1, t1);         // 8
    fe_mul(t1, z, t1);     // 9
    fe_mul(t0, t0, t1);    // 11
    fe_sq(t2, t0);         // 22
    fe_mul(t1, t1, t2);    // 31
    fe_sq(t2, t1);         // 62
    for (i = 1; i < 5; ++i) {
        fe_sq(t2, t2);     // 992
    }
    fe_mul(t1, t2, t1);    // 1023
    fe_sq(t2, t1);         // 2046
    for (i = 1; i < 10; ++i) {
        fe_sq(t2, t2);     // 2095104
    }
    fe_mul(t2, t2, t1);    // 2096127
    fe_sq(t3, t2);         // 4192254
    for (i = 1; i < 20; ++i) {
        fe_sq(t3, t3);     // 2199023243264
    }
    fe_mul(t2, t3, t2);    // 2199023243295
    fe_sq(t2, t2);         // 4398046486590
    for (i = 1; i < 10; ++i) {
        fe_sq(t2, t2);     // 2251799813685248
    }
    fe_mul(t1, t2, t1);    // 2251799813686271
    fe_sq(t2, t1);         // 4503599627372542
    for (i = 1; i < 50; ++i) {
        fe_sq(t2, t2);     // 5070602400912917605986812821504
    }
    fe_mul(t2, t2, t1);    // 5070602400912917605986812822527
    fe_sq(t3, t2);         // 10141204801825835211973625645054
    for (i = 1; i < 100; ++i) {
        fe_sq(t3, t3);     // 1286583448999599... (z^((2^100)*...))
    }
    fe_mul(t2, t3, t2);    // ...
    fe_sq(t2, t2);
    for (i = 1; i < 50; ++i) {
        fe_sq(t2, t2);
    }
    fe_mul(t1, t2, t1);
    fe_sq(t1, t1);
    for (i = 1; i < 5; ++i) {
        fe_sq(t1, t1);
    }
    fe_mul(out, t1, t0);
}

static void x25519_scalar_mult(uint8_t out[32],
                               const uint8_t scalar[32],
                               const uint8_t point[32]) {
    fe x1, x2, z2, x3, z3, tmp0, tmp1;
    uint8_t e[32];
    std::memcpy(e, scalar, 32);
    e[0] &= 248;
    e[31] &= 127;
    e[31] |= 64;

    fe_frombytes(x1, point);
    fe_1(x2);
    fe_0(z2);
    fe_copy(x3, x1);
    fe_1(z3);

    unsigned int swap = 0;
    for (int pos = 254; pos >= 0; --pos) {
        unsigned int b = (e[pos >> 3] >> (pos & 7)) & 1;
        swap ^= b;
        fe_cswap(x2, x3, swap);
        fe_cswap(z2, z3, swap);
        swap = b;

        fe_sub(tmp0, x3, z3);
        fe_sub(tmp1, x2, z2);
        fe_add(x2, x2, z2);
        fe_add(z2, x3, z3);
        fe_mul(z3, tmp0, x2);
        fe_mul(z2, z2, tmp1);
        fe_sq(tmp0, tmp1);
        fe_sq(tmp1, x2);
        fe_add(x3, z3, z2);
        fe_sub(z2, z3, z2);
        fe_mul(x2, tmp1, tmp0);
        fe_sub(tmp1, tmp1, tmp0);
        fe_sq(z2, z2);
        fe_mul121666(z3, tmp1);
        fe_sq(x3, x3);
        fe_add(tmp0, tmp0, z3);
        fe_mul(z3, x1, z2);
        fe_mul(z2, tmp1, tmp0);
    }
    fe_cswap(x2, x3, swap);
    fe_cswap(z2, z3, swap);

    fe_invert(z2, z2);
    fe_mul(x2, x2, z2);
    fe_tobytes(out, x2);
}

} // namespace

bool x25519_public_key(const uint8_t privateKey[32], uint8_t publicKey[32]) {
    static const uint8_t basePoint[32] = {9};
    uint8_t scalar[32];
    std::memcpy(scalar, privateKey, 32);
    x25519_scalar_mult(publicKey, scalar, basePoint);
    return true;
}

bool x25519_shared_secret(const uint8_t privateKey[32],
                          const uint8_t peerPublicKey[32],
                          uint8_t sharedSecret[32]) {
    x25519_scalar_mult(sharedSecret, privateKey, peerPublicKey);
    return true;
}

} // namespace arkmesh
