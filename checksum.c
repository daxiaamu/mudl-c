#include "checksum.h"
#include <windows.h>
#include <wincrypt.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <stdbool.h>

#ifndef CALG_SHA_256
#define CALG_SHA_256 (ALG_CLASS_HASH | ALG_TYPE_ANY | ALG_SID_SHA_256)
#endif
#ifndef CALG_SHA_384
#define CALG_SHA_384 (ALG_CLASS_HASH | ALG_TYPE_ANY | ALG_SID_SHA_384)
#endif
#ifndef CALG_SHA_512
#define CALG_SHA_512 (ALG_CLASS_HASH | ALG_TYPE_ANY | ALG_SID_SHA_512)
#endif

static int utf8_to_wide(const char* src, wchar_t* dst, int dst_n) {
    int n = MultiByteToWideChar(CP_UTF8, 0, src, -1, dst, dst_n);
    if (n > 0) return 0;
    return MultiByteToWideChar(CP_ACP, 0, src, -1, dst, dst_n) > 0 ? 0 : -1;
}

static void normalize_name(const char* in, char* out, int out_n) {
    int pos = 0;
    for (int i = 0; in[i] && pos < out_n - 1; i++) {
        if (in[i] == '-' || in[i] == '_') continue;
        out[pos++] = (char)tolower((unsigned char)in[i]);
    }
    out[pos] = 0;
}

static bool is_hex_digest(const char* s) {
    if (!s || !s[0]) return false;
    for (int i = 0; s[i]; i++)
        if (!isxdigit((unsigned char)s[i]))
            return false;
    return true;
}

static bool hex_equals_ignore_case(const char* a, const char* b) {
    if (strlen(a) != strlen(b)) return false;
    for (int i = 0; a[i]; i++)
        if (tolower((unsigned char)a[i]) != tolower((unsigned char)b[i]))
            return false;
    return true;
}

static bool alg_from_name(const char* name, ALG_ID* alg, int* hex_len) {
    char norm[32];
    normalize_name(name, norm, sizeof(norm));
    if (strcmp(norm, "md5") == 0) {
        *alg = CALG_MD5; *hex_len = 32; return true;
    }
    if (strcmp(norm, "sha1") == 0) {
        *alg = CALG_SHA1; *hex_len = 40; return true;
    }
    if (strcmp(norm, "sha256") == 0) {
        *alg = CALG_SHA_256; *hex_len = 64; return true;
    }
    if (strcmp(norm, "sha384") == 0) {
        *alg = CALG_SHA_384; *hex_len = 96; return true;
    }
    if (strcmp(norm, "sha512") == 0) {
        *alg = CALG_SHA_512; *hex_len = 128; return true;
    }
    return false;
}

int checksum_verify_file(const char* path, const char* spec,
                         char* actual_hex, int actual_n,
                         char* err, int err_n) {
    const char* eq = strchr(spec, '=');
    if (!eq || eq == spec || !eq[1]) {
        snprintf(err, err_n, "Invalid checksum. Use TYPE=DIGEST");
        return -1;
    }

    char type[32];
    size_t type_len = (size_t)(eq - spec);
    if (type_len >= sizeof(type)) type_len = sizeof(type) - 1;
    memcpy(type, spec, type_len);
    type[type_len] = 0;

    const char* expected = eq + 1;
    ALG_ID alg;
    int expected_hex_len = 0;
    if (!alg_from_name(type, &alg, &expected_hex_len)) {
        snprintf(err, err_n, "Unsupported checksum type: %s", type);
        return -1;
    }
    if (!is_hex_digest(expected) || (int)strlen(expected) != expected_hex_len) {
        snprintf(err, err_n, "Invalid %s digest length", type);
        return -1;
    }

    wchar_t wpath[MAX_PATH * 2];
    if (utf8_to_wide(path, wpath, (int)(sizeof(wpath) / sizeof(wpath[0]))) != 0) {
        snprintf(err, err_n, "Cannot convert path to UTF-16: %s", path);
        return -1;
    }

    HANDLE file = CreateFileW(wpath, GENERIC_READ, FILE_SHARE_READ, NULL,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) {
        snprintf(err, err_n, "Cannot open file for checksum: %s (error %lu)",
                 path, GetLastError());
        return -1;
    }

    HCRYPTPROV prov = 0;
    HCRYPTHASH hash = 0;
    if (!CryptAcquireContext(&prov, NULL, NULL, PROV_RSA_AES, CRYPT_VERIFYCONTEXT)) {
        if (!CryptAcquireContext(&prov, NULL, NULL, PROV_RSA_FULL, CRYPT_VERIFYCONTEXT)) {
            snprintf(err, err_n, "CryptAcquireContext failed: %lu", GetLastError());
            CloseHandle(file);
            return -1;
        }
    }
    if (!CryptCreateHash(prov, alg, 0, 0, &hash)) {
        snprintf(err, err_n, "CryptCreateHash failed for %s: %lu", type, GetLastError());
        CryptReleaseContext(prov, 0);
        CloseHandle(file);
        return -1;
    }

    unsigned char buf[65536];
    DWORD got = 0;
    BOOL read_ok = TRUE;
    while ((read_ok = ReadFile(file, buf, sizeof(buf), &got, NULL)) && got > 0) {
        if (!CryptHashData(hash, buf, got, 0)) {
            snprintf(err, err_n, "CryptHashData failed: %lu", GetLastError());
            CryptDestroyHash(hash);
            CryptReleaseContext(prov, 0);
            CloseHandle(file);
            return -1;
        }
    }
    if (!read_ok) {
        snprintf(err, err_n, "ReadFile failed during checksum: %lu", GetLastError());
        CryptDestroyHash(hash);
        CryptReleaseContext(prov, 0);
        CloseHandle(file);
        return -1;
    }

    DWORD hash_len = 0;
    DWORD hash_len_size = sizeof(hash_len);
    CryptGetHashParam(hash, HP_HASHSIZE, (BYTE*)&hash_len, &hash_len_size, 0);
    unsigned char digest[64];
    if (hash_len > sizeof(digest)) {
        snprintf(err, err_n, "Digest too large");
        CryptDestroyHash(hash);
        CryptReleaseContext(prov, 0);
        CloseHandle(file);
        return -1;
    }
    DWORD digest_len = hash_len;
    if (!CryptGetHashParam(hash, HP_HASHVAL, digest, &digest_len, 0)) {
        snprintf(err, err_n, "CryptGetHashParam failed: %lu", GetLastError());
        CryptDestroyHash(hash);
        CryptReleaseContext(prov, 0);
        CloseHandle(file);
        return -1;
    }

    static const char hex[] = "0123456789abcdef";
    int pos = 0;
    for (DWORD i = 0; i < digest_len && pos < actual_n - 2; i++) {
        actual_hex[pos++] = hex[digest[i] >> 4];
        actual_hex[pos++] = hex[digest[i] & 15];
    }
    actual_hex[pos] = 0;

    CryptDestroyHash(hash);
    CryptReleaseContext(prov, 0);
    CloseHandle(file);

    return hex_equals_ignore_case(actual_hex, expected) ? 0 : 1;
}
