// Copyright 2023 Niels Martignène <niels.martignene@protonmail.com>
//
// Permission is hereby granted, free of charge, to any person obtaining a copy of
// this software and associated documentation files (the “Software”), to deal in
// the Software without restriction, including without limitation the rights to use,
// copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the
// Software, and to permit persons to whom the Software is furnished to do so,
// subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED “AS IS”, WITHOUT WARRANTY OF ANY KIND,
// EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES
// OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
// NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT
// HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
// WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
// FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
// OTHER DEALINGS IN THE SOFTWARE.

#include "src/core/base/base.hh"
#include "otp.hh"
#include "vendor/libsodium/src/libsodium/include/sodium.h"
#include "vendor/sha1/sha1.h"
#include "vendor/qrcodegen/qrcodegen.h"
#define MINIZ_NO_ZLIB_COMPATIBLE_NAMES
#include "vendor/miniz/miniz.h"

namespace RG {

static inline Size GetBase32DecodedLength(Size len)
{
    // This may overestimate because of padding characters
    return 5 * (len / 8) + 5;
}

static inline uint8_t DecodeBase32Char(int c)
{
    if (c >= 'A' && c <= 'Z') {
        return (uint8_t)(c - 'A');
    } else if (c >= '2' && c <= '7') {
        return (uint8_t)(c - '2' + 26);
    } else {
        return 0xFF;
    }
}

static Size DecodeBase32(Span<const char> b32, Span<uint8_t> out_buf)
{
    if (!b32.len) {
        LogError("Empty secret is not allowed");
        return -1;
    }
    if (GetBase32DecodedLength(b32.len) > out_buf.len) {
        LogError("Secret is too long");
        return -1;
    }

    Size len = 0;

    for (Size i = 0, j = 0; i < b32.len && b32[i] != '='; i++, j = (j + 1) & 0x7) {
        uint8_t value = DecodeBase32Char(b32[i]);

        if (value == 0xFF) [[unlikely]] {
            LogError("Unexpected Base32 character '%1'", b32[i]);
            return -1;
        }

        switch (j) {
            case 0: { out_buf[len] = ((value << 3) & 0xF8); } break;
            case 1: {
                out_buf[len++] |= ((value >> 2) & 0x7);
                out_buf[len] = ((value << 6) & 0xC0);
            } break;
            case 2: { out_buf[len] |= ((value << 1) & 0x3E); } break;
            case 3: {
                out_buf[len++] |= ((value >> 4) & 0x1);
                out_buf[len] = ((value << 4) & 0xF0);
            } break;
            case 4: {
                out_buf[len++] |= ((value >> 1) & 0xF);
                out_buf[len] = ((value << 7) & 0x80);
            } break;
            case 5: { out_buf[len] |= ((value << 2) & 0x7C); } break;
            case 6: {
                out_buf[len++] |= ((value >> 3) & 0x3);
                out_buf[len] = ((value << 5) & 0xE0);
            } break;
            case 7: { out_buf[len++] |= (value & 0x1F); } break;
        }
    }

    return len;
}

// XXX: Kind of duplicated in libnet (but changed to allow @). Clean this up!
static void EncodeUrlSafe(const char *str, HeapArray<char> *out_buf)
{
    for (Size i = 0; str[i]; i++) {
        int c = str[i];

        if (IsAsciiAlphaOrDigit(c) || c == '-' || c == '.' || c == '_' || c == '~' || c == '@') {
            out_buf->Append((char)c);
        } else {
            Fmt(out_buf, "%%%1", FmtHex((uint8_t)c).Pad0(-2));
        }
    }

    out_buf->Grow(1);
    out_buf->ptr[out_buf->len] = 0;
}

void pwd_GenerateSecret(Span<char> out_buf)
{
    RG_ASSERT(out_buf.len > 0);

    static const char *chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567";

    FillRandomSafe(out_buf.ptr, out_buf.len);
    for (Size i = 0; i < out_buf.len - 1; i++) {
        out_buf[i] = chars[(uint8_t)out_buf[i] % 32];
    }
    out_buf[out_buf.len - 1] = 0;
}

bool pwd_CheckSecret(const char *secret)
{
    if (!secret[0]) {
        LogError("Empty secret is not allowed");
        return false;
    }

    for (Size i = 0; secret[i]; i++) {
        if (DecodeBase32Char(secret[i]) == 0xFF) {
            LogError("Invalid Base32 secret");
            return false;
        }
    }

    return true;
}

const char *pwd_GenerateHotpUrl(const char *label, const char *username, const char *issuer,
                                pwd_HotpAlgorithm algo, const char *secret, int digits, Allocator *alloc)
{
    HeapArray<char> buf(alloc);

    if (!pwd_CheckSecret(secret))
        return nullptr;

    Fmt(&buf, "otpauth://totp/"); EncodeUrlSafe(label, &buf);
    if (username) {
        Fmt(&buf, ":"); EncodeUrlSafe(username, &buf);
    }
    Fmt(&buf, "?algorithm=%1&secret=%2&digits=%3", pwd_HotpAlgorithmNames[(int)algo], secret, digits);
    if (issuer) {
        Fmt(&buf, "&issuer="); EncodeUrlSafe(issuer, &buf);
    }

    const char *url = buf.TrimAndLeak(1).ptr;
    return url;
}

static void GenerateUtf8(const uint8_t qr[qrcodegen_BUFFER_LEN_MAX], bool ansi, int border, HeapArray<char> *out_utf8)
{
    int size = qrcodegen_getSize(qr) + 2 * border;

    for (int y = 0; y < size; y += 2) {
        out_utf8->Append(ansi ? "\e[40;37m" : "");

        for (int x = 0; x < size; x++) {
            int combined = (qrcodegen_getModule(qr, x - border, y - border) << 0) |
                           (qrcodegen_getModule(qr, x - border, y - border + 1) << 1);

            switch (combined) {
                case 0: { out_utf8->Append("\u2588"); } break;
                case 1: { out_utf8->Append("\u2584"); } break;
                case 2: { out_utf8->Append("\u2580"); } break;
                case 3: { out_utf8->Append(' '); } break;
            }
        }

        out_utf8->Append(ansi ? "\e[0m\n" : "\n");
    }
}

bool pwd_GenerateHotpUtf8(const char *url, bool ansi, int border, HeapArray<char> *out_buf)
{
    RG_ASSERT(!out_buf->len);
    RG_ASSERT(border % 2 == 0);

    uint8_t qr[qrcodegen_BUFFER_LEN_MAX];
    uint8_t tmp[qrcodegen_BUFFER_LEN_MAX];
    static_assert(qrcodegen_BUFFER_LEN_MAX < Kibibytes(8));

    bool success = qrcodegen_encodeText(url, tmp, qr, qrcodegen_Ecc_MEDIUM,
                                        qrcodegen_VERSION_MIN, qrcodegen_VERSION_MAX, qrcodegen_Mask_AUTO, true);
    if (!success) {
        LogError("QR code encoding failed");
        return false;
    }

    GenerateUtf8(qr, ansi, border, out_buf);

    return true;
}

static bool GeneratePNG(const uint8_t qr[qrcodegen_BUFFER_LEN_MAX], int border, HeapArray<uint8_t> *out_png)
{
    // Account for scanline byte
    static const int MaxSize = Kibibytes(2) - 1;

    static const uint8_t PngHeader[] = { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A };
    static const uint8_t PngFooter[] = { 0, 0, 0, 0, 'I', 'E', 'N', 'D', 0xAE, 0x42, 0x60, 0x82};

    int size = qrcodegen_getSize(qr) + 2 * border / 4;
    int size4 = qrcodegen_getSize(qr) * 4 + 2 * border;

    if (size > MaxSize) [[unlikely]] {
        LogError("Excessive QR code image size");
        return false;
    }

    out_png->Append(PngHeader);

#pragma pack(push, 1)
    struct ChunkHeader {
        uint32_t len;
        uint8_t type[4];
    };
    struct IHDR {
        uint32_t width;
        uint32_t height;
        uint8_t bit_depth;
        uint8_t color_type;
        uint8_t compression;
        uint8_t filter;
        uint8_t interlace;
    };
#pragma pack(pop)

    // Write IHDR chunk
    {
        Size chunk_pos = out_png->len;

        ChunkHeader chunk = {};
        IHDR ihdr = {};

        chunk.len = BigEndian((uint32_t)RG_SIZE(ihdr));
        MemCpy(chunk.type, "IHDR", 4);
        ihdr.width = BigEndian(size4);
        ihdr.height = BigEndian(size4);
        ihdr.bit_depth = 1;
        ihdr.color_type = 0;
        ihdr.compression = 0;
        ihdr.filter = 0;
        ihdr.interlace = 0;

        out_png->Append(MakeSpan((const uint8_t *)&chunk, RG_SIZE(chunk)));
        out_png->Append(MakeSpan((const uint8_t *)&ihdr, RG_SIZE(ihdr)));

        // Chunk CRC-32
        uint32_t crc = BigEndian((uint32_t)mz_crc32(MZ_CRC32_INIT, out_png->ptr + chunk_pos + 4, RG_SIZE(ihdr) + 4));
        out_png->Append(MakeSpan((const uint8_t *)&crc, 4));
    }

    // Write image data (IDAT)
    {
        Size chunk_pos = out_png->len;

        ChunkHeader chunk = {};
        chunk.len = 0; // Unknown for now
        MemCpy(chunk.type, "IDAT", 4);
        out_png->Append(MakeSpan((const uint8_t *)&chunk, RG_SIZE(chunk)));

        StreamWriter writer(out_png, "<png>", CompressionType::Zlib);
        for (int y = 0; y < size4; y++) {
            LocalArray<uint8_t, MaxSize + 1> buf;
            buf.Append((uint8_t)0); // Scanline filter

            for (int x = 0; x < size; x += 2) {
                uint8_t byte = (qrcodegen_getModule(qr, x + 0 - border / 4, y / 4 - border / 4) * 0xF0) |
                               (qrcodegen_getModule(qr, x + 1 - border / 4, y / 4 - border / 4) * 0x0F);
                buf.Append(~byte);
            }

            writer.Write(buf);
        }
        bool success = writer.Close();
        RG_ASSERT(success);

        // Fix length
        {
            uint32_t len = BigEndian((uint32_t)(out_png->len - chunk_pos - 8));
            uint32_t *ptr = (uint32_t *)(out_png->ptr + chunk_pos);
            MemCpy(ptr, &len, RG_SIZE(len));
        }

        // Chunk CRC-32
        uint32_t crc = BigEndian((uint32_t)mz_crc32(MZ_CRC32_INIT, out_png->ptr + chunk_pos + 4, out_png->len - chunk_pos - 4));
        out_png->Append(MakeSpan((const uint8_t *)&crc, 4));
    }

    // End image (IEND)
    out_png->Append(PngFooter);

    return true;
}

bool pwd_GenerateHotpPng(const char *url, int border, HeapArray<uint8_t> *out_buf)
{
    RG_ASSERT(!out_buf->len);

    uint8_t qr[qrcodegen_BUFFER_LEN_MAX];
    uint8_t tmp[qrcodegen_BUFFER_LEN_MAX];
    static_assert(qrcodegen_BUFFER_LEN_MAX < Kibibytes(8));

    bool success = qrcodegen_encodeText(url, tmp, qr, qrcodegen_Ecc_MEDIUM,
                                        qrcodegen_VERSION_MIN, qrcodegen_VERSION_MAX, qrcodegen_Mask_AUTO, true);
    if (!success) {
        LogError("QR code encoding failed");
        return false;
    }

    if (!GeneratePNG(qr, border, out_buf))
        return false;

    return true;
}

static Size HmacSha1(Span<const uint8_t> key, Span<const uint8_t> message, uint8_t out_digest[20])
{
    RG_ASSERT(message.len <= UINT32_MAX);

    uint8_t padded_key[64];

    // Hash and/or pad key
    if (key.len > RG_SIZE(padded_key)) {
        SHA1((char *)padded_key, (const char *)key.ptr, (uint32_t)key.len);
        MemSet(padded_key + 20, 0, RG_SIZE(padded_key) - 20);
    } else {
        MemCpy(padded_key, key.ptr, key.len);
        MemSet(padded_key + key.len, 0, RG_SIZE(padded_key) - key.len);
    }

    // Inner hash
    uint8_t inner_hash[20];
    {
        SHA1_CTX ctx;
        SHA1Init(&ctx);

        for (Size i = 0; i < RG_SIZE(padded_key); i++) {
            padded_key[i] ^= 0x36;
        }

        SHA1Update(&ctx, padded_key, RG_SIZE(padded_key));
        SHA1Update(&ctx, message.ptr, (uint32_t)message.len);
        SHA1Final((unsigned char *)inner_hash, &ctx);
    }

    // Outer hash
    {
        SHA1_CTX ctx;
        SHA1Init(&ctx);

        for (Size i = 0; i < RG_SIZE(padded_key); i++) {
            padded_key[i] ^= 0x36; // IPAD is still there
            padded_key[i] ^= 0x5C;
        }

        SHA1Update(&ctx, padded_key, RG_SIZE(padded_key));
        SHA1Update(&ctx, inner_hash, RG_SIZE(inner_hash));
        SHA1Final((unsigned char *)out_digest, &ctx);
    }

    return 20;
}

static Size HmacSha256(Span<const uint8_t> key, Span<const uint8_t> message, uint8_t out_digest[32])
{
    static_assert(crypto_hash_sha256_BYTES == 32);

    uint8_t padded_key[64];

    // Hash and/or pad key
    if (key.len > RG_SIZE(padded_key)) {
        crypto_hash_sha256(padded_key, key.ptr, (size_t)key.len);
        MemSet(padded_key + 32, 0, RG_SIZE(padded_key) - 32);
    } else {
        MemCpy(padded_key, key.ptr, key.len);
        MemSet(padded_key + key.len, 0, RG_SIZE(padded_key) - key.len);
    }

    // Inner hash
    uint8_t inner_hash[32];
    {
        crypto_hash_sha256_state state;
        crypto_hash_sha256_init(&state);

        for (Size i = 0; i < RG_SIZE(padded_key); i++) {
            padded_key[i] ^= 0x36;
        }

        crypto_hash_sha256_update(&state, padded_key, RG_SIZE(padded_key));
        crypto_hash_sha256_update(&state, message.ptr, (size_t)message.len);
        crypto_hash_sha256_final(&state, inner_hash);
    }

    // Outer hash
    {
        crypto_hash_sha256_state state;
        crypto_hash_sha256_init(&state);

        for (Size i = 0; i < RG_SIZE(padded_key); i++) {
            padded_key[i] ^= 0x36; // IPAD is still there
            padded_key[i] ^= 0x5C;
        }

        crypto_hash_sha256_update(&state, padded_key, RG_SIZE(padded_key));
        crypto_hash_sha256_update(&state, inner_hash, RG_SIZE(inner_hash));
        crypto_hash_sha256_final(&state, out_digest);
    }

    return 32;
}

static Size HmacSha512(Span<const uint8_t> key, Span<const uint8_t> message, uint8_t out_digest[64])
{
    static_assert(crypto_hash_sha512_BYTES == 64);

    uint8_t padded_key[128];

    // Hash and/or pad key
    if (key.len > RG_SIZE(padded_key)) {
        crypto_hash_sha512(padded_key, key.ptr, (size_t)key.len);
        MemSet(padded_key + 64, 0, RG_SIZE(padded_key) - 64);
    } else {
        MemCpy(padded_key, key.ptr, key.len);
        MemSet(padded_key + key.len, 0, RG_SIZE(padded_key) - key.len);
    }

    // Inner hash
    uint8_t inner_hash[64];
    {
        crypto_hash_sha512_state state;
        crypto_hash_sha512_init(&state);

        for (Size i = 0; i < RG_SIZE(padded_key); i++) {
            padded_key[i] ^= 0x36;
        }

        crypto_hash_sha512_update(&state, padded_key, RG_SIZE(padded_key));
        crypto_hash_sha512_update(&state, message.ptr, (size_t)message.len);
        crypto_hash_sha512_final(&state, inner_hash);
    }

    // Outer hash
    {
        crypto_hash_sha512_state state;
        crypto_hash_sha512_init(&state);

        for (Size i = 0; i < RG_SIZE(padded_key); i++) {
            padded_key[i] ^= 0x36; // IPAD is still there
            padded_key[i] ^= 0x5C;
        }

        crypto_hash_sha512_update(&state, padded_key, RG_SIZE(padded_key));
        crypto_hash_sha512_update(&state, inner_hash, RG_SIZE(inner_hash));
        crypto_hash_sha512_final(&state, out_digest);
    }

    return 64;
}

static int ComputeHotp(Span<const uint8_t> key, pwd_HotpAlgorithm algo, int64_t counter, int digits)
{
    union { int64_t i; uint8_t raw[8]; } message;
    message.i = BigEndian(counter);

    // HMAC-SHA1
    LocalArray<uint8_t, 64> digest;
    switch (algo) {
        case pwd_HotpAlgorithm::SHA1: { digest.len = HmacSha1(key, message.raw, digest.data); } break;
        case pwd_HotpAlgorithm::SHA256: { digest.len = HmacSha256(key, message.raw, digest.data); } break;
        case pwd_HotpAlgorithm::SHA512: { digest.len = HmacSha512(key, message.raw, digest.data); } break;
    }

    // Dynamic truncation
    int offset = digest[digest.len - 1] & 0xF;
    uint32_t sbits = (((uint32_t)digest[offset + 0] & 0x7F) << 24) |
                     (((uint32_t)digest[offset + 1] & 0xFF) << 16) |
                     (((uint32_t)digest[offset + 2] & 0xFF) << 8) |
                     (((uint32_t)digest[offset + 3] & 0xFF) << 0);

    // Return just enough digits
    switch (digits) {
        case 6: return sbits % 1000000;
        case 7: return sbits % 10000000;
        case 8: return sbits % 100000000;

        default: {
            LogError("Invalid number of digits");
            return -1;
        } break;
    }
}

int pwd_ComputeHotp(const char *secret, pwd_HotpAlgorithm algo, int64_t counter, int digits)
{
    LocalArray<uint8_t, 128> key;
    key.len = DecodeBase32(secret, key.data);
    if (key.len < 0)
        return -1;

    return ComputeHotp(key, algo, counter, digits);
}

bool pwd_CheckHotp(const char *secret, pwd_HotpAlgorithm algo, int64_t min, int64_t max, int digits, const char *code)
{
    LocalArray<uint8_t, 128> key;
    key.len = DecodeBase32(secret, key.data);
    if (key.len < 0)
        return false;

    for (int64_t counter = min; counter <= max; counter++) {
        int ret = ComputeHotp(key, algo, counter, digits);
        if (ret < 0)
            return false;

        char buf[16];
        Fmt(buf, "%1", FmtArg(ret).Pad0(-digits));

        if (TestStr(buf, code))
            return true;
    }

    return false;
}

}
