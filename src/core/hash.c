#include "core/hash.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <bcrypt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

uint64_t nc_fnv1a_bytes(const void *data, size_t size) {
    const unsigned char *bytes = (const unsigned char *)data;
    uint64_t hash = 14695981039346656037ull;
    size_t index;
    for (index = 0; index < size; index++) {
        hash ^= bytes[index];
        hash *= 1099511628211ull;
    }
    return hash;
}

uint64_t nc_fnv1a_utf8(const char *text) {
    return text ? nc_fnv1a_bytes(text, strlen(text)) : 14695981039346656037ull;
}

uint64_t nc_fnv1a_wide(const wchar_t *text) {
    return text ? nc_fnv1a_bytes(text, wcslen(text) * sizeof(wchar_t)) : 14695981039346656037ull;
}

void nc_sha256_hex(const void *data, size_t length, char out[65]) {
    BCRYPT_ALG_HANDLE algorithm = NULL;
    BCRYPT_HASH_HANDLE hash = NULL;
    unsigned char digest[32];
    DWORD digest_size = 0;
    DWORD result_size = 0;
    size_t index;
    if (!out) {
        return;
    }
    out[0] = 0;
    if (!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, NULL, 0))) {
        return;
    }
    if (!BCRYPT_SUCCESS(BCryptGetProperty(algorithm, BCRYPT_HASH_LENGTH, (PUCHAR)&digest_size,
                                          sizeof(digest_size), &result_size, 0)) ||
        digest_size != sizeof(digest)) {
        BCryptCloseAlgorithmProvider(algorithm, 0);
        return;
    }
    if (!BCRYPT_SUCCESS(BCryptCreateHash(algorithm, &hash, NULL, 0, NULL, 0, 0))) {
        BCryptCloseAlgorithmProvider(algorithm, 0);
        return;
    }
    if (length > 0) {
        size_t offset = 0;
        while (offset < length) {
            ULONG chunk = (ULONG)((length - offset) > 0x40000000u ? 0x40000000u : (length - offset));
            if (!BCRYPT_SUCCESS(BCryptHashData(hash, (PUCHAR)data + offset, chunk, 0))) {
                BCryptDestroyHash(hash);
                BCryptCloseAlgorithmProvider(algorithm, 0);
                return;
            }
            offset += chunk;
        }
    }
    if (!BCRYPT_SUCCESS(BCryptFinishHash(hash, digest, (ULONG)sizeof(digest), 0))) {
        BCryptDestroyHash(hash);
        BCryptCloseAlgorithmProvider(algorithm, 0);
        return;
    }
    BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(algorithm, 0);
    for (index = 0; index < sizeof(digest); index++) {
        snprintf(out + index * 2, 3, "%02X", digest[index]);
    }
}

bool nc_sha256_file(const wchar_t *path, uint64_t *size, char out[65], NcErr *err) {
    HANDLE file;
    BCRYPT_ALG_HANDLE algorithm = NULL;
    BCRYPT_HASH_HANDLE hash = NULL;
    unsigned char digest[32];
    unsigned char *buffer = NULL;
    const DWORD buffer_size = 64u * 1024u;
    uint64_t total = 0;
    size_t index;
    if (!path || !out) {
        return nc_fail(err, "SHA256 参数无效。");
    }
    out[0] = 0;
    file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                       NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) {
        return nc_fail(err, "打开文件失败（Win32 错误 %lu）。", (unsigned long)GetLastError());
    }
    buffer = (unsigned char *)malloc(buffer_size);
    if (!buffer) {
        CloseHandle(file);
        return nc_fail(err, "内存不足。");
    }
    if (!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, NULL, 0))) {
        free(buffer);
        CloseHandle(file);
        return nc_fail(err, "初始化 SHA256 失败。");
    }
    if (!BCRYPT_SUCCESS(BCryptCreateHash(algorithm, &hash, NULL, 0, NULL, 0, 0))) {
        BCryptCloseAlgorithmProvider(algorithm, 0);
        free(buffer);
        CloseHandle(file);
        return nc_fail(err, "初始化 SHA256 失败。");
    }
    for (;;) {
        DWORD read = 0;
        if (!ReadFile(file, buffer, buffer_size, &read, NULL)) {
            DWORD code = GetLastError();
            BCryptDestroyHash(hash);
            BCryptCloseAlgorithmProvider(algorithm, 0);
            free(buffer);
            CloseHandle(file);
            return nc_fail(err, "读取文件失败（Win32 错误 %lu）。", (unsigned long)code);
        }
        if (read == 0) {
            break;
        }
        if (!BCRYPT_SUCCESS(BCryptHashData(hash, buffer, read, 0))) {
            BCryptDestroyHash(hash);
            BCryptCloseAlgorithmProvider(algorithm, 0);
            free(buffer);
            CloseHandle(file);
            return nc_fail(err, "计算 SHA256 失败。");
        }
        total += read;
    }
    if (!BCRYPT_SUCCESS(BCryptFinishHash(hash, digest, (ULONG)sizeof(digest), 0))) {
        BCryptDestroyHash(hash);
        BCryptCloseAlgorithmProvider(algorithm, 0);
        free(buffer);
        CloseHandle(file);
        return nc_fail(err, "完成 SHA256 失败。");
    }
    BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(algorithm, 0);
    free(buffer);
    CloseHandle(file);
    for (index = 0; index < sizeof(digest); index++) {
        snprintf(out + index * 2, 3, "%02X", digest[index]);
    }
    if (size) {
        *size = total;
    }
    return true;
}
