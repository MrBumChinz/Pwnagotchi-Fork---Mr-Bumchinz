/*
 * PwnaUI License System Implementation
 * 
 * Hardware-bound license validation using Ed25519 signatures.
 * Uses TweetNaCl for cryptographic operations (public domain).
 * 
 * Author: PwnHub Project
 * License: Proprietary - All Rights Reserved
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <time.h>
#include <sys/stat.h>
#include <errno.h>

#include "license.h"
#include "renderer.h"
#include "font.h"

/* ============================================================================
 * TweetNaCl Ed25519 - Minimal implementation (Public Domain)
 * Only the verification part needed - signing is done in the mobile app
 * ============================================================================ */

typedef uint8_t u8;
typedef uint64_t u64;
typedef int64_t i64;
typedef i64 gf[16];

static const u8 _9[32] = {9};
static const gf gf0 = {0};
static const gf gf1 = {1};
static const gf _121665 = {0xDB41, 1};
static const gf D = {
    0x78a3, 0x1359, 0x4dca, 0x75eb, 0xd8ab, 0x4141, 0x0a4d, 0x0070,
    0xe898, 0x7779, 0x4079, 0x8cc7, 0xfe73, 0x2b6f, 0x6cee, 0x5203
};
static const gf D2 = {
    0xf159, 0x26b2, 0x9b94, 0xebd6, 0xb156, 0x8283, 0x149a, 0x00e0,
    0xd130, 0xeef3, 0x80f2, 0x198e, 0xfce7, 0x56df, 0xd9dc, 0x2406
};
static const gf X = {
    0xd51a, 0x8f25, 0x2d60, 0xc956, 0xa7b2, 0x9525, 0xc760, 0x692c,
    0xdc5c, 0xfdd6, 0xe231, 0xc0a4, 0x53fe, 0xcd6e, 0x36d3, 0x2169
};
static const gf Y = {
    0x6658, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666,
    0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666
};
static const gf I = {
    0xa0b0, 0x4a0e, 0x1b27, 0xc4ee, 0xe478, 0xad2f, 0x1806, 0x2f43,
    0xd7a7, 0x3dfb, 0x0099, 0x2b4d, 0xdf0b, 0x4fc1, 0x2480, 0x2b83
};

/* ============================================================================
 * YOUR PUBLIC KEY - GENERATED ONCE, EMBEDDED HERE
 * The private key stays in your mobile app, NEVER on device
 * ============================================================================ */

static const u8 LICENSE_PUBLIC_KEY[32] = {
    0x6f, 0x19, 0xf8, 0x96, 0x2f, 0x69, 0xb2, 0x11,
    0x7d, 0xd1, 0x1a, 0x80, 0xbc, 0xbd, 0xd6, 0x6f,
    0x63, 0xec, 0xc4, 0x23, 0x3a, 0xe5, 0x2a, 0xa0,
    0x7b, 0xd0, 0x85, 0xaa, 0x6b, 0x4c, 0x1e, 0x88
};

/* ============================================================================
 * TweetNaCl helper functions
 * ============================================================================ */

static u64 dl64(const u8 *x) {
    u64 u = 0;
    for (int i = 0; i < 8; i++) u = (u << 8) | x[i];
    return u;
}

static void ts64(u8 *x, u64 u) {
    for (int i = 7; i >= 0; i--) { x[i] = u & 255; u >>= 8; }
}

static int vn(const u8 *x, const u8 *y, int n) {
    u64 d = 0;
    for (int i = 0; i < n; i++) d |= x[i] ^ y[i];
    return (1 & ((d - 1) >> 8)) - 1;
}

static int crypto_verify_32(const u8 *x, const u8 *y) {
    return vn(x, y, 32);
}

static void set25519(gf r, const gf a) {
    for (int i = 0; i < 16; i++) r[i] = a[i];
}

static void car25519(gf o) {
    i64 c;
    for (int i = 0; i < 16; i++) {
        o[i] += (1LL << 16);
        c = o[i] >> 16;
        o[(i + 1) * (i < 15)] += c - 1 + 37 * (c - 1) * (i == 15);
        o[i] -= c << 16;
    }
}

static void sel25519(gf p, gf q, int b) {
    i64 t, c = ~(b - 1);
    for (int i = 0; i < 16; i++) {
        t = c & (p[i] ^ q[i]);
        p[i] ^= t;
        q[i] ^= t;
    }
}

static void pack25519(u8 *o, const gf n) {
    int i, j, b;
    gf m, t;
    set25519(t, n);
    car25519(t);
    car25519(t);
    car25519(t);
    for (j = 0; j < 2; j++) {
        m[0] = t[0] - 0xffed;
        for (i = 1; i < 15; i++) {
            m[i] = t[i] - 0xffff - ((m[i-1] >> 16) & 1);
            m[i-1] &= 0xffff;
        }
        m[15] = t[15] - 0x7fff - ((m[14] >> 16) & 1);
        b = (m[15] >> 16) & 1;
        m[14] &= 0xffff;
        sel25519(t, m, 1 - b);
    }
    for (i = 0; i < 16; i++) {
        o[2*i] = t[i] & 0xff;
        o[2*i + 1] = t[i] >> 8;
    }
}

static int neq25519(const gf a, const gf b) {
    u8 c[32], d[32];
    pack25519(c, a);
    pack25519(d, b);
    return crypto_verify_32(c, d);
}

static u8 par25519(const gf a) {
    u8 d[32];
    pack25519(d, a);
    return d[0] & 1;
}

static void unpack25519(gf o, const u8 *n) {
    for (int i = 0; i < 16; i++) o[i] = n[2*i] + ((i64)n[2*i + 1] << 8);
    o[15] &= 0x7fff;
}

static void A(gf o, const gf a, const gf b) {
    for (int i = 0; i < 16; i++) o[i] = a[i] + b[i];
}

static void Z(gf o, const gf a, const gf b) {
    for (int i = 0; i < 16; i++) o[i] = a[i] - b[i];
}

static void M(gf o, const gf a, const gf b) {
    i64 t[31] = {0};
    for (int i = 0; i < 16; i++)
        for (int j = 0; j < 16; j++)
            t[i + j] += a[i] * b[j];
    for (int i = 0; i < 15; i++) t[i] += 38 * t[i + 16];
    for (int i = 0; i < 16; i++) o[i] = t[i];
    car25519(o);
    car25519(o);
}

static void S(gf o, const gf a) {
    M(o, a, a);
}

static void inv25519(gf o, const gf i) {
    gf c;
    set25519(c, i);
    for (int a = 253; a >= 0; a--) {
        S(c, c);
        if (a != 2 && a != 4) M(c, c, i);
    }
    set25519(o, c);
}

static void pow2523(gf o, const gf i) {
    gf c;
    set25519(c, i);
    for (int a = 250; a >= 0; a--) {
        S(c, c);
        if (a != 1) M(c, c, i);
    }
    set25519(o, c);
}

/* SHA-512 for Ed25519 */
static u64 K[80] = {
    0x428a2f98d728ae22ULL, 0x7137449123ef65cdULL, 0xb5c0fbcfec4d3b2fULL, 0xe9b5dba58189dbbcULL,
    0x3956c25bf348b538ULL, 0x59f111f1b605d019ULL, 0x923f82a4af194f9bULL, 0xab1c5ed5da6d8118ULL,
    0xd807aa98a3030242ULL, 0x12835b0145706fbeULL, 0x243185be4ee4b28cULL, 0x550c7dc3d5ffb4e2ULL,
    0x72be5d74f27b896fULL, 0x80deb1fe3b1696b1ULL, 0x9bdc06a725c71235ULL, 0xc19bf174cf692694ULL,
    0xe49b69c19ef14ad2ULL, 0xefbe4786384f25e3ULL, 0x0fc19dc68b8cd5b5ULL, 0x240ca1cc77ac9c65ULL,
    0x2de92c6f592b0275ULL, 0x4a7484aa6ea6e483ULL, 0x5cb0a9dcbd41fbd4ULL, 0x76f988da831153b5ULL,
    0x983e5152ee66dfabULL, 0xa831c66d2db43210ULL, 0xb00327c898fb213fULL, 0xbf597fc7beef0ee4ULL,
    0xc6e00bf33da88fc2ULL, 0xd5a79147930aa725ULL, 0x06ca6351e003826fULL, 0x142929670a0e6e70ULL,
    0x27b70a8546d22ffcULL, 0x2e1b21385c26c926ULL, 0x4d2c6dfc5ac42aedULL, 0x53380d139d95b3dfULL,
    0x650a73548baf63deULL, 0x766a0abb3c77b2a8ULL, 0x81c2c92e47edaee6ULL, 0x92722c851482353bULL,
    0xa2bfe8a14cf10364ULL, 0xa81a664bbc423001ULL, 0xc24b8b70d0f89791ULL, 0xc76c51a30654be30ULL,
    0xd192e819d6ef5218ULL, 0xd69906245565a910ULL, 0xf40e35855771202aULL, 0x106aa07032bbd1b8ULL,
    0x19a4c116b8d2d0c8ULL, 0x1e376c085141ab53ULL, 0x2748774cdf8eeb99ULL, 0x34b0bcb5e19b48a8ULL,
    0x391c0cb3c5c95a63ULL, 0x4ed8aa4ae3418acbULL, 0x5b9cca4f7763e373ULL, 0x682e6ff3d6b2b8a3ULL,
    0x748f82ee5defb2fcULL, 0x78a5636f43172f60ULL, 0x84c87814a1f0ab72ULL, 0x8cc702081a6439ecULL,
    0x90befffa23631e28ULL, 0xa4506cebde82bde9ULL, 0xbef9a3f7b2c67915ULL, 0xc67178f2e372532bULL,
    0xca273eceea26619cULL, 0xd186b8c721c0c207ULL, 0xeada7dd6cde0eb1eULL, 0xf57d4f7fee6ed178ULL,
    0x06f067aa72176fbaULL, 0x0a637dc5a2c898a6ULL, 0x113f9804bef90daeULL, 0x1b710b35131c471bULL,
    0x28db77f523047d84ULL, 0x32caab7b40c72493ULL, 0x3c9ebe0a15c9bebcULL, 0x431d67c49c100d4cULL,
    0x4cc5d4becb3e42b6ULL, 0x597f299cfc657e2aULL, 0x5fcb6fab3ad6faecULL, 0x6c44198c4a475817ULL
};

#define R(x, c) ((x) >> (c))
#define Ch(x, y, z) (((x) & (y)) ^ (~(x) & (z)))
#define Maj(x, y, z) (((x) & (y)) ^ ((x) & (z)) ^ ((y) & (z)))
#define Sigma0(x) (((x) >> 28 | (x) << 36) ^ ((x) >> 34 | (x) << 30) ^ ((x) >> 39 | (x) << 25))
#define Sigma1(x) (((x) >> 14 | (x) << 50) ^ ((x) >> 18 | (x) << 46) ^ ((x) >> 41 | (x) << 23))
#define sigma0(x) (((x) >> 1 | (x) << 63) ^ ((x) >> 8 | (x) << 56) ^ ((x) >> 7))
#define sigma1(x) (((x) >> 19 | (x) << 45) ^ ((x) >> 61 | (x) << 3) ^ ((x) >> 6))

static int crypto_hashblocks(u8 *x, const u8 *m, u64 n) {
    u64 z[8], b[8], a[8], w[16], t;
    for (int i = 0; i < 8; i++) z[i] = dl64(x + 8 * i);
    while (n >= 128) {
        for (int i = 0; i < 16; i++) w[i] = dl64(m + 8 * i);
        for (int i = 0; i < 8; i++) a[i] = z[i];
        for (int i = 0; i < 80; i++) {
            b[i % 8] = a[(i + 7) % 8];
            t = a[(i + 7) % 8] + Sigma1(a[(i + 4) % 8]) + Ch(a[(i + 4) % 8], a[(i + 5) % 8], a[(i + 6) % 8]) + K[i] + w[i % 16];
            b[(i + 3) % 8] = t + Sigma0(a[i % 8]) + Maj(a[i % 8], a[(i + 1) % 8], a[(i + 2) % 8]);
            a[(i + 3) % 8] += t;
            for (int j = 0; j < 8; j++) a[j] = b[j];
            if (i % 16 == 15)
                for (int j = 0; j < 16; j++)
                    w[j] += w[(j + 9) % 16] + sigma0(w[(j + 1) % 16]) + sigma1(w[(j + 14) % 16]);
        }
        for (int i = 0; i < 8; i++) z[i] += a[i];
        m += 128;
        n -= 128;
    }
    for (int i = 0; i < 8; i++) ts64(x + 8 * i, z[i]);
    return n;
}

static const u8 iv[64] = {
    0x6a, 0x09, 0xe6, 0x67, 0xf3, 0xbc, 0xc9, 0x08,
    0xbb, 0x67, 0xae, 0x85, 0x84, 0xca, 0xa7, 0x3b,
    0x3c, 0x6e, 0xf3, 0x72, 0xfe, 0x94, 0xf8, 0x2b,
    0xa5, 0x4f, 0xf5, 0x3a, 0x5f, 0x1d, 0x36, 0xf1,
    0x51, 0x0e, 0x52, 0x7f, 0xad, 0xe6, 0x82, 0xd1,
    0x9b, 0x05, 0x68, 0x8c, 0x2b, 0x3e, 0x6c, 0x1f,
    0x1f, 0x83, 0xd9, 0xab, 0xfb, 0x41, 0xbd, 0x6b,
    0x5b, 0xe0, 0xcd, 0x19, 0x13, 0x7e, 0x21, 0x79
};

static int crypto_hash(u8 *out, const u8 *m, u64 n) {
    u8 h[64], x[256];
    u64 b = n;
    memcpy(h, iv, 64);
    crypto_hashblocks(h, m, n);
    m += n;
    n &= 127;
    m -= n;
    memset(x, 0, 256);
    memcpy(x, m, n);
    x[n] = 128;
    n = 256 - 128 * (n < 112);
    x[n - 9] = b >> 61;
    ts64(x + n - 8, b << 3);
    crypto_hashblocks(h, x, n);
    memcpy(out, h, 64);
    return 0;
}

/* Ed25519 point operations */
static void add(gf p[4], gf q[4]) {
    gf a, b, c, d, t, e, f, g, h;
    Z(a, p[1], p[0]);
    Z(t, q[1], q[0]);
    M(a, a, t);
    A(b, p[0], p[1]);
    A(t, q[0], q[1]);
    M(b, b, t);
    M(c, p[3], q[3]);
    M(c, c, D2);
    M(d, p[2], q[2]);
    A(d, d, d);
    Z(e, b, a);
    Z(f, d, c);
    A(g, d, c);
    A(h, b, a);
    M(p[0], e, f);
    M(p[1], h, g);
    M(p[2], g, f);
    M(p[3], e, h);
}

static void cswap(gf p[4], gf q[4], u8 b) {
    for (int i = 0; i < 4; i++) sel25519(p[i], q[i], b);
}

static void pack(u8 *r, gf p[4]) {
    gf tx, ty, zi;
    inv25519(zi, p[2]);
    M(tx, p[0], zi);
    M(ty, p[1], zi);
    pack25519(r, ty);
    r[31] ^= par25519(tx) << 7;
}

static void scalarmult(gf p[4], gf q[4], const u8 *s) {
    set25519(p[0], gf0);
    set25519(p[1], gf1);
    set25519(p[2], gf1);
    set25519(p[3], gf0);
    for (int i = 255; i >= 0; i--) {
        u8 b = (s[i / 8] >> (i & 7)) & 1;
        cswap(p, q, b);
        add(q, p);
        add(p, p);
        cswap(p, q, b);
    }
}

static void scalarbase(gf p[4], const u8 *s) {
    gf q[4];
    set25519(q[0], X);
    set25519(q[1], Y);
    set25519(q[2], gf1);
    M(q[3], X, Y);
    scalarmult(p, q, s);
}

static int unpackneg(gf r[4], const u8 p[32]) {
    gf t, chk, num, den, den2, den4, den6;
    set25519(r[2], gf1);
    unpack25519(r[1], p);
    S(num, r[1]);
    M(den, num, D);
    Z(num, num, r[2]);
    A(den, r[2], den);
    S(den2, den);
    S(den4, den2);
    M(den6, den4, den2);
    M(t, den6, num);
    M(t, t, den);
    pow2523(t, t);
    M(t, t, num);
    M(t, t, den);
    M(t, t, den);
    M(r[0], t, den);
    S(chk, r[0]);
    M(chk, chk, den);
    if (neq25519(chk, num)) M(r[0], r[0], I);
    S(chk, r[0]);
    M(chk, chk, den);
    if (neq25519(chk, num)) return -1;
    if (par25519(r[0]) == (p[31] >> 7)) Z(r[0], gf0, r[0]);
    M(r[3], r[0], r[1]);
    return 0;
}

static void modL(u8 *r, i64 x[64]) {
    static const i64 L[32] = {
        0xed, 0xd3, 0xf5, 0x5c, 0x1a, 0x63, 0x12, 0x58,
        0xd6, 0x9c, 0xf7, 0xa2, 0xde, 0xf9, 0xde, 0x14,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10
    };
    i64 carry;
    for (int i = 63; i >= 32; i--) {
        carry = 0;
        for (int j = i - 32; j < i - 12; j++) {
            x[j] += carry - 16 * x[i] * L[j - (i - 32)];
            carry = (x[j] + 128) >> 8;
            x[j] -= carry << 8;
        }
        x[i - 12] += carry;
        x[i] = 0;
    }
    carry = 0;
    for (int j = 0; j < 32; j++) {
        x[j] += carry - (x[31] >> 4) * L[j];
        carry = x[j] >> 8;
        x[j] &= 255;
    }
    for (int j = 0; j < 32; j++) x[j] -= carry * L[j];
    for (int i = 0; i < 32; i++) {
        x[i + 1] += x[i] >> 8;
        r[i] = x[i] & 255;
    }
}

static void reduce(u8 *r) {
    i64 x[64];
    for (int i = 0; i < 64; i++) x[i] = (u64)r[i];
    for (int i = 0; i < 64; i++) r[i] = 0;
    modL(r, x);
}

/* Ed25519 signature verification */
static int crypto_sign_open(u8 *m, u64 *mlen, const u8 *sm, u64 n, const u8 *pk) {
    u8 t[32], h[64];
    gf p[4], q[4];
    if (n < 64) return -1;
    if (unpackneg(q, pk)) return -1;
    memcpy(m, sm, n);
    memcpy(m + 32, pk, 32);
    crypto_hash(h, m, n);
    reduce(h);
    scalarmult(p, q, h);
    scalarbase(q, sm + 32);
    add(p, q);
    pack(t, p);
    n -= 64;
    if (crypto_verify_32(sm, t)) {
        memset(m, 0, n);
        return -1;
    }
    memmove(m, m + 64, n);
    *mlen = n;
    return 0;
}

/* ============================================================================
 * Base64 decoding
 * ============================================================================ */

static const char b64_table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static int b64_decode_char(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

static int base64_decode(const char *in, u8 *out, size_t *out_len) {
    size_t in_len = strlen(in);
    size_t i, j = 0;
    u8 sextet[4];
    
    for (i = 0; i < in_len; ) {
        int valid = 0;
        for (int k = 0; k < 4 && i < in_len; k++) {
            while (i < in_len && (in[i] == '\n' || in[i] == '\r' || in[i] == ' ')) i++;
            if (i >= in_len) break;
            if (in[i] == '=') { sextet[k] = 0; i++; }
            else {
                int v = b64_decode_char(in[i++]);
                if (v < 0) return -1;
                sextet[k] = v;
                valid++;
            }
        }
        if (valid > 1) out[j++] = (sextet[0] << 2) | (sextet[1] >> 4);
        if (valid > 2) out[j++] = (sextet[1] << 4) | (sextet[2] >> 2);
        if (valid > 3) out[j++] = (sextet[2] << 6) | sextet[3];
    }
    *out_len = j;
    return 0;
}

/* ============================================================================
 * License System Implementation
 * ============================================================================ */

static char g_device_serial[32] = {0};
static license_status_t g_license_status = LICENSE_MISSING;
static license_data_t g_current_license = {0};
static bool g_license_checked = false;
static bool g_license_ignore = false;  /* When true, bypass license checks */

/*
 * Check if license checks should be ignored.
 * Enabled if either:
 *   - Environment variable PWNAUI_LICENSE_IGNORE is set to "1"
 *   - File /etc/pwnaui/license.ignore exists
 */
static void license_check_ignore_flag(void) {
    const char *env = getenv("PWNAUI_LICENSE_IGNORE");
    if (env && env[0] == '1') {
        g_license_ignore = true;
        return;
    }

    struct stat st;
    if (stat("/etc/pwnaui/license.ignore", &st) == 0) {
        g_license_ignore = true;
    }
}

/*
 * Read Raspberry Pi CPU serial number
 */
static int read_cpu_serial(char *serial, size_t len) {
    FILE *f = fopen("/proc/cpuinfo", "r");
    if (!f) {
        /* Fallback for non-Pi systems (testing) */
        snprintf(serial, len, "0000000000000000");
        return 0;
    }
    
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "Serial", 6) == 0) {
            char *colon = strchr(line, ':');
            if (colon) {
                /* Skip ": " and trim whitespace */
                colon += 2;
                while (*colon == ' ' || *colon == '\t') colon++;
                char *end = colon + strlen(colon) - 1;
                while (end > colon && (*end == '\n' || *end == '\r' || *end == ' ')) *end-- = '\0';
                strncpy(serial, colon, len - 1);
                serial[len - 1] = '\0';
                fclose(f);
                return 0;
            }
        }
    }
    fclose(f);
    snprintf(serial, len, "0000000000000000");
    return -1;
}

/*
 * Load license from file
 */
static int load_license_file(license_data_t *license) {
    FILE *f = fopen(LICENSE_FILE_PATH, "rb");
    if (!f) return -1;
    
    /* Read base64 encoded license */
    char b64_buf[512];
    size_t n = fread(b64_buf, 1, sizeof(b64_buf) - 1, f);
    fclose(f);
    
    if (n == 0) return -1;
    b64_buf[n] = '\0';
    
    /* Decode base64 */
    u8 raw[256];
    size_t raw_len;
    if (base64_decode(b64_buf, raw, &raw_len) != 0) return -1;
    
    /* Parse license structure */
    /* Format: serial(16) + issued(8) + expiry(8) + features(1) + signature(64) = 97 bytes */
    if (raw_len < 97) return -1;
    
    memcpy(license->device_serial, raw, 16);
    license->device_serial[16] = '\0';
    
    license->issued_timestamp = 0;
    for (int i = 0; i < 8; i++) {
        license->issued_timestamp = (license->issued_timestamp << 8) | raw[16 + i];
    }
    
    license->expiry_timestamp = 0;
    for (int i = 0; i < 8; i++) {
        license->expiry_timestamp = (license->expiry_timestamp << 8) | raw[24 + i];
    }
    
    license->features = raw[32];
    memcpy(license->signature, raw + 33, 64);
    
    return 0;
}

/*
 * Save license to file
 */
static int save_license_file(const u8 *license_data, size_t len) {
    /* Create directory if needed */
    mkdir(LICENSE_DIR_PATH, 0755);
    
    FILE *f = fopen(LICENSE_FILE_PATH, "wb");
    if (!f) return -1;
    
    fwrite(license_data, 1, len, f);
    fclose(f);
    return 0;
}

/*
 * Verify license signature
 */
license_status_t license_verify(const license_data_t *license) {
    /* Build message: serial + timestamps + features */
    u8 msg[33];
    memcpy(msg, license->device_serial, 16);
    
    u64 ts = license->issued_timestamp;
    for (int i = 7; i >= 0; i--) { msg[16 + i] = ts & 0xFF; ts >>= 8; }
    
    ts = license->expiry_timestamp;
    for (int i = 7; i >= 0; i--) { msg[24 + i] = ts & 0xFF; ts >>= 8; }
    
    msg[32] = license->features;
    
    /* Build signed message format for verification */
    u8 sm[64 + 33];
    memcpy(sm, license->signature, 64);
    memcpy(sm + 64, msg, 33);
    
    u8 m[33];
    u64 mlen;
    
    if (crypto_sign_open(m, &mlen, sm, sizeof(sm), LICENSE_PUBLIC_KEY) != 0) {
        return LICENSE_INVALID;
    }
    
    /* Check device serial matches */
    if (strncmp(license->device_serial, g_device_serial, 16) != 0) {
        return LICENSE_WRONG_DEVICE;
    }
    
    /* Check expiry (0 = never expires) */
    if (license->expiry_timestamp != 0) {
        time_t now = time(NULL);
        if ((u64)now > license->expiry_timestamp) {
            return LICENSE_EXPIRED;
        }
    }
    
    return LICENSE_VALID;
}

/*
 * Initialize license system
 */
license_status_t license_init(void) {
    /* Allow temporary bypass via env/file flag */
    license_check_ignore_flag();
    if (g_license_ignore) {
        g_license_status = LICENSE_VALID;
        g_license_checked = true;
        return g_license_status;
    }

    /* Read device serial */
    read_cpu_serial(g_device_serial, sizeof(g_device_serial));
    
    /* Try to load existing license */
    if (load_license_file(&g_current_license) != 0) {
        g_license_status = LICENSE_MISSING;
        g_license_checked = true;
        return g_license_status;
    }
    
    /* Verify the license */
    g_license_status = license_verify(&g_current_license);
    g_license_checked = true;
    
    return g_license_status;
}

bool license_is_valid(void) {
    /* Re-evaluate ignore flag at runtime so creating the file/env takes effect without rebuild */
    if (!g_license_ignore) {
        license_check_ignore_flag();
    }
    if (g_license_ignore) return true;
    if (!g_license_checked) {
        license_init();
    }
    return g_license_status == LICENSE_VALID;
}

license_status_t license_get_status(void) {
    if (!g_license_ignore) {
        license_check_ignore_flag();
    }
    if (g_license_ignore) return LICENSE_VALID;
    if (!g_license_checked) {
        license_init();
    }
    return g_license_status;
}

const char* license_get_device_serial(void) {
    if (g_device_serial[0] == '\0') {
        read_cpu_serial(g_device_serial, sizeof(g_device_serial));
    }
    return g_device_serial;
}

license_status_t license_install(const char *license_b64) {
    /* Save the raw base64 to file */
    size_t len = strlen(license_b64);
    if (save_license_file((const u8*)license_b64, len) != 0) {
        return LICENSE_CORRUPTED;
    }
    
    /* Reload and verify */
    if (load_license_file(&g_current_license) != 0) {
        g_license_status = LICENSE_CORRUPTED;
        return g_license_status;
    }
    
    g_license_status = license_verify(&g_current_license);
    return g_license_status;
}

const char* license_status_string(license_status_t status) {
    if (g_license_ignore) return "Licensed (ignore flag)";
    switch (status) {
        case LICENSE_VALID:        return "Licensed";
        case LICENSE_MISSING:      return "Not Activated";
        case LICENSE_INVALID:      return "Invalid License";
        case LICENSE_EXPIRED:      return "License Expired";
        case LICENSE_WRONG_DEVICE: return "Wrong Device";
        case LICENSE_CORRUPTED:    return "License Corrupted";
        default:                   return "Unknown";
    }
}

bool license_has_feature(uint8_t feature) {
    if (!license_is_valid()) return false;
    return (g_current_license.features & feature) != 0;
}

/*
 * Render locked screen - BLACK background, WHITE text
 * Shows: "SCREEN LOCKED" and "Download PwnHub to unlock"
 */
void license_render_locked_screen(uint8_t *framebuffer, int width, int height) {
    /* Fill with BLACK (0x00 = all pixels black for e-ink) */
    int fb_size = (width * height + 7) / 8;
    memset(framebuffer, 0x00, fb_size);
    
    /* Simplified layout - just center each line individually */
    const char *line1 = "LOCKED";
    const char *line2 = "Get PwnHub App";
    const char *line3 = "to unlock";
    
    /* Calculate centered positions */
    int w1 = font_text_width(line1, FONT_BOLD);
    int w2 = font_text_width(line2, FONT_MEDIUM);
    int w3 = font_text_width(line3, FONT_SMALL);
    
    int x1 = (width - w1) / 2;
    int x2 = (width - w2) / 2;
    int x3 = (width - w3) / 2;
    
    /* Vertical spacing from top */
    int y1 = 35;  /* Line 1 */
    int y2 = 60;  /* Line 2 */
    int y3 = 80;  /* Line 3 */
    
    renderer_draw_text_simple(framebuffer, width, height, x1, y1, line1, FONT_BOLD, 1);
    renderer_draw_text_simple(framebuffer, width, height, x2, y2, line2, FONT_MEDIUM, 1);
    renderer_draw_text_simple(framebuffer, width, height, x3, y3, line3, FONT_SMALL, 1);
}
