#include "core/time.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <stdio.h>

int64_t nc_now_unix(void) {
    FILETIME file_time;
    ULARGE_INTEGER value;
    const uint64_t epoch = 116444736000000000ull;
    GetSystemTimeAsFileTime(&file_time);
    value.LowPart = file_time.dwLowDateTime;
    value.HighPart = file_time.dwHighDateTime;
    if (value.QuadPart < epoch) {
        return 0;
    }
    return (int64_t)((value.QuadPart - epoch) / 10000000ull);
}

void nc_format_local_iso_ms(char *buf, size_t cap) {
    SYSTEMTIME st;
    if (!buf || cap == 0) {
        return;
    }
    GetLocalTime(&st);
    snprintf(buf, cap, "%04u-%02u-%02u %02u:%02u:%02u.%03u",
             (unsigned)st.wYear, (unsigned)st.wMonth, (unsigned)st.wDay,
             (unsigned)st.wHour, (unsigned)st.wMinute, (unsigned)st.wSecond, (unsigned)st.wMilliseconds);
}

void nc_format_local_hms_ms(char *buf, size_t cap) {
    SYSTEMTIME st;
    if (!buf || cap == 0) {
        return;
    }
    GetLocalTime(&st);
    snprintf(buf, cap, "%02u:%02u:%02u.%03u",
             (unsigned)st.wHour, (unsigned)st.wMinute, (unsigned)st.wSecond, (unsigned)st.wMilliseconds);
}

void nc_format_local_hms(char *buf, size_t cap) {
    SYSTEMTIME st;
    if (!buf || cap == 0) {
        return;
    }
    GetLocalTime(&st);
    snprintf(buf, cap, "%02u:%02u:%02u",
             (unsigned)st.wHour, (unsigned)st.wMinute, (unsigned)st.wSecond);
}

static BOOL CALLBACK monotonic_init([[maybe_unused]] PINIT_ONCE once, PVOID parameter,
                                    [[maybe_unused]] PVOID *context) {
    LARGE_INTEGER *state = (LARGE_INTEGER *)parameter;
    QueryPerformanceFrequency(&state[0]);
    QueryPerformanceCounter(&state[1]);
    return TRUE;
}

uint64_t nc_monotonic_ns(void) {
    static INIT_ONCE once = INIT_ONCE_STATIC_INIT;
    static LARGE_INTEGER state[2];
    LARGE_INTEGER now;
    uint64_t delta;
    uint64_t freq;
    InitOnceExecuteOnce(&once, monotonic_init, state, NULL);
    if (state[0].QuadPart <= 0) {
        return 0;
    }
    QueryPerformanceCounter(&now);
    if (now.QuadPart < state[1].QuadPart) {
        return 0;
    }
    delta = (uint64_t)(now.QuadPart - state[1].QuadPart);
    freq = (uint64_t)state[0].QuadPart;
    return (delta / freq) * 1000000000ull + (delta % freq) * 1000000000ull / freq;
}

uint64_t nc_random_seed(void) {
    LARGE_INTEGER counter;
    QueryPerformanceCounter(&counter);
    return (uint64_t)counter.QuadPart ^ ((uint64_t)GetCurrentProcessId() << 32) ^
           ((uint64_t)GetCurrentThreadId() << 16) ^ GetTickCount64();
}

uint64_t nc_rand_next(uint64_t *state) {
    uint64_t value;
    if (!state || *state == 0) {
        uint64_t seed = nc_random_seed();
        if (state) {
            *state = seed ? seed : 0x9E3779B97F4A7C15ull;
        }
        if (!state) {
            return seed;
        }
    }
    value = *state;
    value ^= value >> 12;
    value ^= value << 25;
    value ^= value >> 27;
    *state = value;
    return value * 2685821657736338717ull;
}

uint32_t nc_rand_below(uint64_t *state, uint32_t bound) {
    if (bound == 0) {
        return 0;
    }
    return (uint32_t)(nc_rand_next(state) % bound);
}

uint64_t nc_rand_inclusive(uint64_t *state, uint64_t max) {
    if (max == UINT64_MAX) {
        return nc_rand_next(state);
    }
    return nc_rand_next(state) % (max + 1);
}

void nc_new_id(char out[33]) {
    static SRWLOCK lock = SRWLOCK_INIT;
    static uint64_t state = 0;
    int index;
    if (!out) {
        return;
    }
    AcquireSRWLockExclusive(&lock);
    if (state == 0) {
        state = nc_random_seed();
        if (state == 0) {
            state = 0x9E3779B97F4A7C15ull;
        }
    }
    for (index = 0; index < 4; index++) {
        uint32_t value = (uint32_t)(nc_rand_next(&state) >> 16);
        snprintf(out + index * 8, 9, "%08x", value);
    }
    ReleaseSRWLockExclusive(&lock);
}
