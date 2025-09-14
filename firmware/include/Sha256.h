#pragma once
// Minimal standalone SHA-256 (public domain) tailored for small inputs.
// Not optimized; adequate for short strings like card UID + salt.
#include <Arduino.h>

namespace MiniSHA256
{
    struct Ctx
    {
        uint32_t state[8];
        uint64_t bitlen;
        uint8_t buffer[64];
        uint8_t bufferLen;
    };

    static inline uint32_t rotr(uint32_t x, uint8_t n) { return (x >> n) | (x << (32 - n)); }
    static inline uint32_t ch(uint32_t x, uint32_t y, uint32_t z) { return (x & y) ^ (~x & z); }
    static inline uint32_t maj(uint32_t x, uint32_t y, uint32_t z) { return (x & y) ^ (x & z) ^ (y & z); }
    static inline uint32_t e0(uint32_t x) { return rotr(x, 2) ^ rotr(x, 13) ^ rotr(x, 22); }
    static inline uint32_t e1(uint32_t x) { return rotr(x, 6) ^ rotr(x, 11) ^ rotr(x, 25); }
    static inline uint32_t s0(uint32_t x) { return rotr(x, 7) ^ rotr(x, 18) ^ (x >> 3); }
    static inline uint32_t s1(uint32_t x) { return rotr(x, 17) ^ rotr(x, 19) ^ (x >> 10); }

    static const uint32_t K[64] = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
        0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
        0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
        0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
        0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
        0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
        0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
        0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

    static void init(Ctx &c)
    {
        c.state[0] = 0x6a09e667;
        c.state[1] = 0xbb67ae85;
        c.state[2] = 0x3c6ef372;
        c.state[3] = 0xa54ff53a;
        c.state[4] = 0x510e527f;
        c.state[5] = 0x9b05688c;
        c.state[6] = 0x1f83d9ab;
        c.state[7] = 0x5be0cd19;
        c.bitlen = 0;
        c.bufferLen = 0;
    }

    static void transform(Ctx &c, const uint8_t block[64])
    {
        uint32_t w[64];
        for (int i = 0; i < 16; i++)
        {
            w[i] = (uint32_t)block[i * 4] << 24 | (uint32_t)block[i * 4 + 1] << 16 | (uint32_t)block[i * 4 + 2] << 8 | (uint32_t)block[i * 4 + 3];
        }
        for (int i = 16; i < 64; i++)
            w[i] = s1(w[i - 2]) + w[i - 7] + s0(w[i - 15]) + w[i - 16];
        uint32_t a = c.state[0], b = c.state[1], d = c.state[3], e = c.state[4], f = c.state[5], g = c.state[6], h = c.state[7], cc = c.state[2];
        for (int i = 0; i < 64; i++)
        {
            uint32_t t1 = h + e1(e) + ch(e, f, g) + K[i] + w[i];
            uint32_t t2 = e0(a) + maj(a, b, cc);
            h = g;
            g = f;
            f = e;
            e = d + t1;
            d = cc;
            cc = b;
            b = a;
            a = t1 + t2;
        }
        c.state[0] += a;
        c.state[1] += b;
        c.state[2] += cc;
        c.state[3] += d;
        c.state[4] += e;
        c.state[5] += f;
        c.state[6] += g;
        c.state[7] += h;
    }

    static void update(Ctx &c, const uint8_t *data, size_t len)
    {
        for (size_t i = 0; i < len; i++)
        {
            c.buffer[c.bufferLen++] = data[i];
            if (c.bufferLen == 64)
            {
                transform(c, c.buffer);
                c.bitlen += 512;
                c.bufferLen = 0;
            }
        }
    }

    static void finish(Ctx &c, uint8_t out[32])
    {
        c.bitlen += (uint64_t)c.bufferLen * 8ULL;
        // Append 0x80
        c.buffer[c.bufferLen++] = 0x80;
        if (c.bufferLen > 56)
        {
            while (c.bufferLen < 64)
                c.buffer[c.bufferLen++] = 0;
            transform(c, c.buffer);
            c.bufferLen = 0;
        }
        while (c.bufferLen < 56)
            c.buffer[c.bufferLen++] = 0;
        // Append length big-endian
        for (int i = 7; i >= 0; i--)
            c.buffer[c.bufferLen++] = (uint8_t)(c.bitlen >> (i * 8));
        transform(c, c.buffer);
        for (int i = 0; i < 8; i++)
        {
            out[i * 4] = (uint8_t)(c.state[i] >> 24);
            out[i * 4 + 1] = (uint8_t)(c.state[i] >> 16);
            out[i * 4 + 2] = (uint8_t)(c.state[i] >> 8);
            out[i * 4 + 3] = (uint8_t)(c.state[i]);
        }
    }

    static String hashHex(const String &input)
    {
        Ctx c;
        init(c);
        update(c, (const uint8_t *)input.c_str(), input.length());
        uint8_t h[32];
        finish(c, h);
        char buf[65];
        for (int i = 0; i < 32; i++)
            sprintf(buf + 2 * i, "%02x", h[i]);
        buf[64] = '\0';
        return String(buf);
    }
}

inline String computeSha256Hex(const String &s) { return MiniSHA256::hashHex(s); }
