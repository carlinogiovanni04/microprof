/*
 *   COPYRIGHT (C) - Giovanni Carlino - 2026 - ALL RIGHTS RESERVED
 *
 *   general purpose instrumentation profiler (single header)
 *
 * Usage:
 *   #define MICROPROF_IMPLEMENTATION   // in one .c file
 *   #include "profiler.h"
 *
 *   void my_frame(void) {
 *       PROF_SCOPE("frame") {
 *           PROF_SCOPE("update") { // Do your logic update }
 *           PROF_SCOPE("render") { // Do your rendering }
 *       }
 *       prof_frame_end();
 *   }
 *
 *   prof_report();                    // text  to stdout
 *   prof_write_trace("trace.json");   // chrome://tracing / perfetto
 *
 * Threading: one call stack per thread (allocated lazy from a fixed slot
 * table). Aggregated counters and event buffer are safe to hit from multiple
 * threads. Zone registration is serialized with a spin lock.
 */

#ifndef MICROPROF_H
#define MICROPROF_H

/* sizeof(ProfEvent) == 24, so 8 MiB holds
 * roughly 349k events. Override before including, or hand in memory with
 * prof_init() to avoid the static buffer entirely. */
#ifndef PROF_ARENA_BYTES
#define PROF_ARENA_BYTES (8ull * 1024ull * 1024ull)
#endif

/* Max active zones on one thread at the same time. */
#ifndef PROF_MAX_DEPTH
#define PROF_MAX_DEPTH 256
#endif

/* Max distinct call sites that can be tracked. */
#ifndef PROF_MAX_ZONES
#define PROF_MAX_ZONES 1024
#endif

/* Power of two, must be >= 2 * PROF_MAX_ZONES. */
#ifndef PROF_ZONE_SLOTS
#define PROF_ZONE_SLOTS 4096
#endif

/* Max threads issuing profiled work. */
#ifndef PROF_MAX_THREADS
#define PROF_MAX_THREADS 16
#endif

/* Name shown for the process in the trace metadata. */
//TODO(Giovanni): auto-retrieve?
#ifndef PROF_PROCESS_NAME
#define PROF_PROCESS_NAME "process"
#endif

/* Define PROF_DISABLE to compile every macro down to nothing. */
/* #define PROF_DISABLE */

#if !defined(BASE_H)

#if defined(_MSC_VER)
typedef signed char        i8;
typedef short              i16;
typedef int                i32;
typedef long long          i64;
typedef unsigned char      u8;
typedef unsigned short     u16;
typedef unsigned int       u32;
typedef unsigned long long u64;
typedef unsigned long long usize;
typedef unsigned long long uptr;
#else
typedef __INT8_TYPE__   i8;
typedef __INT16_TYPE__  i16;
typedef __INT32_TYPE__  i32;
typedef __INT64_TYPE__  i64;
typedef __UINT8_TYPE__  u8;
typedef __UINT16_TYPE__ u16;
typedef __UINT32_TYPE__ u32;
typedef __UINT64_TYPE__ u64;
typedef __SIZE_TYPE__   usize;
typedef __SIZE_TYPE__   uptr;
#endif

typedef float  f32;
typedef double f64;
typedef i32    b32;

#endif /* !BASE_H */

#ifndef true
#define true  1
#endif
#ifndef false
#define false 0
#endif

typedef struct ProfZone {
    const char *name;
    const char *file;
    u64         hash;
    u64         self_ns;   /* exclusive time */
    u64         incl_ns;   /* inclusive time */
    u64         hits;
    u32         line;
    u32         id;
} ProfZone;

typedef struct ProfEvent {
    u64 ts;        /* nanoseconds, monotonic */
    u32 zone_id;
    u32 tid;
    u8  phase;     /* 0 = begin, 1 = end */
    u8  pad[7];
} ProfEvent;

typedef struct ProfThread {
    ProfZone *stack[PROF_MAX_DEPTH];
    u64       start_ns[PROF_MAX_DEPTH];
    u64       child_ns[PROF_MAX_DEPTH];
    u32       depth;
    u32       overflow;
    u32       tid;
    u32       pad;
} ProfThread;

/* Optional: supply your own arena (must outlive ALL profiling). Returns
 * false if the buffer is too small to be useful. */
b32  prof_init(void *memory, usize size);

/* Boundary for one logical frame; only affects the reported frame average. */
void prof_frame_end(void);

/* Clear all accumulated counters (zones stay registered). */
void prof_reset(void);

/* Toggle the per instance event stream (chrome trace source). Cheap to leave
 * on; turn it off for long captures where you just want the aggregate. */
void prof_set_recording(b32 enabled);

/* Print the aggregate table to stdout. */
void prof_report(void);

/* Write a chrome trace JSON file. Returns false on I/O failure. */
b32  prof_write_trace(const char *path);

u64  prof_frame_count(void);
u64  prof_event_count(void);
u64  prof_event_dropped(void);

/* Internal entry points used by the macros below; DO NOT CALL THESE, call the macros instead. */
void prof__begin(const char *name, const char *file, u32 line);
void prof__end(void);

#if !defined(PROF_DISABLE)

#define PROF_BEGIN(name) prof__begin((name), __FILE__, (u32)__LINE__)
#define PROF_END()       prof__end()

/* Scopes a block. Note: an early `return`/`break` out of the body skips the
 * matching end; use PROF_BEGIN/PROF_END if the body can leave early. */
#define PROF_SCOPE(name)                                                      \
    for (i32 prof__scope_once_ =                                              \
             (prof__begin((name), __FILE__, (u32)__LINE__), 1);               \
         prof__scope_once_;                                                   \
         prof__scope_once_ = (prof__end(), 0))

#define PROF_FUNCTION()                                                       \
    for (i32 prof__scope_once_ =                                              \
             (prof__begin(__FUNCTION__, __FILE__, (u32)__LINE__), 1);         \
         prof__scope_once_;                                                   \
         prof__scope_once_ = (prof__end(), 0))

#else /* PROF_DISABLE */

#define PROF_BEGIN(name) ((void)0)
#define PROF_END()       ((void)0)
#define PROF_SCOPE(name)
#define PROF_FUNCTION()

#endif /* PROF_DISABLE */


#if defined(MICROPROF_IMPLEMENTATION)

#if defined(_MSC_VER)
#define PROF_TLS __declspec(thread)
#else
#define PROF_TLS _Thread_local
#endif

/* Portable atomics. MSVC has no __atomic_* builtins; use the Interlocked
 * intrinsics instead (full barriers, which are at least as strong as the
 * acquire/release/relaxed orderings requested at the call sites). */
#if defined(_MSC_VER)
#pragma warning(disable : 4456) // NOTE(Giovanni): "se si dichiara 'prof__scope_once_', la dichiarazione locale precedente verrà nascosta"
#include <intrin.h>
#define PROF_ATOMIC_EXCHANGE32(p, v)  ((i32)_InterlockedExchange((volatile long *)(p), (long)(v)))
#define PROF_ATOMIC_STORE32(p, v)     ((void)_InterlockedExchange((volatile long *)(p), (long)(v)))
#define PROF_ATOMIC_LOAD32(p)         ((i32)_InterlockedCompareExchange((volatile long *)(p), 0, 0))
#define PROF_ATOMIC_FETCH_ADD64(p, v) ((u64)_InterlockedExchangeAdd64((volatile long long *)(p), (long long)(v)))
#define PROF_ATOMIC_LOAD64(p)         ((u64)_InterlockedCompareExchange64((volatile long long *)(p), 0, 0))
#else
#define PROF_ATOMIC_EXCHANGE32(p, v)  (__atomic_exchange_n((p), (v), __ATOMIC_ACQUIRE))
#define PROF_ATOMIC_STORE32(p, v)     (__atomic_store_n((p), (v), __ATOMIC_RELEASE))
#define PROF_ATOMIC_LOAD32(p)         (__atomic_load_n((p), __ATOMIC_ACQUIRE))
#define PROF_ATOMIC_FETCH_ADD64(p, v) (__atomic_fetch_add((p), (v), __ATOMIC_RELAXED))
#define PROF_ATOMIC_LOAD64(p)         (__atomic_load_n((p), __ATOMIC_RELAXED))
#endif

#if defined(_WIN32) || defined(_WIN64)

#if defined(_MSC_VER)
__declspec(dllimport) void         *__stdcall GetStdHandle(unsigned long nStdHandle);
__declspec(dllimport) i32           __stdcall WriteFile(void *hFile, const void *lpBuffer, unsigned long nNumberOfBytesToWrite, unsigned long *lpNumberOfBytesWritten, void *lpOverlapped);
__declspec(dllimport) i32           __stdcall CloseHandle(void *hObject);
__declspec(dllimport) void         *__stdcall CreateFileA(const char *lpFileName, unsigned long dwDesiredAccess, unsigned long dwShareMode, void *lpSecurityAttributes, unsigned long dwCreationDisposition, unsigned long dwFlagsAndAttributes, void *hTemplateFile);
__declspec(dllimport) i32           __stdcall QueryPerformanceCounter(i64 *lpPerformanceCount);
__declspec(dllimport) i32           __stdcall QueryPerformanceFrequency(i64 *lpFrequency);
__declspec(dllimport) unsigned long __stdcall GetCurrentThreadId(void);
#else
__attribute__((dllimport)) void         *__stdcall GetStdHandle(unsigned long nStdHandle);
__attribute__((dllimport)) i32           __stdcall WriteFile(void *hFile, const void *lpBuffer, unsigned long nNumberOfBytesToWrite, unsigned long *lpNumberOfBytesWritten, void *lpOverlapped);
__attribute__((dllimport)) i32           __stdcall CloseHandle(void *hObject);
__attribute__((dllimport)) void         *__stdcall CreateFileA(const char *lpFileName, unsigned long dwDesiredAccess, unsigned long dwShareMode, void *lpSecurityAttributes, unsigned long dwCreationDisposition, unsigned long dwFlagsAndAttributes, void *hTemplateFile);
__attribute__((dllimport)) i32           __stdcall QueryPerformanceCounter(i64 *lpPerformanceCount);
__attribute__((dllimport)) i32           __stdcall QueryPerformanceFrequency(i64 *lpFrequency);
__attribute__((dllimport)) unsigned long __stdcall GetCurrentThreadId(void);
#endif

#define PROF_GENERIC_WRITE     0x40000000u
#define PROF_CREATE_ALWAYS     2u
#define PROF_FILE_ATTRIBUTE_NORMAL 0x80u
#define PROF_STD_OUTPUT_HANDLE ((unsigned long)-11)
#define PROF_STD_ERROR_HANDLE  ((unsigned long)-12)

static u64 prof__now_ns(void) {
    static i64 freq = 0;
    i64 counter = 0;
    if (freq == 0) QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&counter);
    return (u64)((counter / freq) * 1000000000ll + ((counter % freq) * 1000000000ll) / freq);
}

static u32 prof__get_tid(void) {
    return (u32)GetCurrentThreadId();
}

static i64 prof__os_open_trunc(const char *path) {
    void *handle = CreateFileA(path, PROF_GENERIC_WRITE, 0, (void *)0,
                               PROF_CREATE_ALWAYS, PROF_FILE_ATTRIBUTE_NORMAL, (void *)0);
    if (handle == (void *)(i64)-1) return -1;
    return (i64)(uptr)handle;
}

static i64 prof__os_write(i64 fd, const void *buf, u64 len) {
    void *handle;
    unsigned long written = 0;
    if      (fd == 1) handle = GetStdHandle(PROF_STD_OUTPUT_HANDLE);
    else if (fd == 2) handle = GetStdHandle(PROF_STD_ERROR_HANDLE);
    else              handle = (void *)(uptr)fd;
    WriteFile(handle, buf, (unsigned long)len, &written, (void *)0);
    return (i64)written;
}

static void prof__os_close(i64 fd) {
    if (fd > 2) CloseHandle((void *)(uptr)fd);
}

#elif defined(__linux__) && (defined(__x86_64__) || defined(_M_X64))

#define PROF_CLOCK_MONOTONIC 1
#define PROF_SYS_WRITE   1
#define PROF_SYS_OPEN    2
#define PROF_SYS_CLOSE   3
#define PROF_SYS_GETTID  186
#define PROF_SYS_CLOCK_GETTIME 228

#define PROF_O_WRONLY 1
#define PROF_O_CREAT  64
#define PROF_O_TRUNC  512

typedef struct { i64 tv_sec; i64 tv_nsec; } ProfTimespec;

static u64 prof__now_ns(void) {
    ProfTimespec ts;
    i64 ret;
    __asm__ volatile (
        "syscall"
        : "=a"(ret)
        : "a"((i64)PROF_SYS_CLOCK_GETTIME), "D"((i64)PROF_CLOCK_MONOTONIC), "S"(&ts)
        : "rcx", "r11", "memory"
    );
    (void)ret;
    return (u64)ts.tv_sec * 1000000000ull + (u64)ts.tv_nsec;
}

static u32 prof__get_tid(void) {
    i64 ret;
    __asm__ volatile (
        "syscall"
        : "=a"(ret)
        : "a"((i64)PROF_SYS_GETTID)
        : "rcx", "r11", "memory"
    );
    return (u32)ret;
}

static i64 prof__os_open_trunc(const char *path) {
    i64 ret;
    __asm__ volatile (
        "syscall"
        : "=a"(ret)
        : "a"((i64)PROF_SYS_OPEN), "D"(path),
          "S"((i64)(PROF_O_WRONLY | PROF_O_CREAT | PROF_O_TRUNC)), "d"((i64)0644)
        : "rcx", "r11", "memory"
    );
    return ret;
}

static i64 prof__os_write(i64 fd, const void *buf, u64 len) {
    i64 ret;
    __asm__ volatile (
        "syscall"
        : "=a"(ret)
        : "a"((i64)PROF_SYS_WRITE), "D"(fd), "S"(buf), "d"(len)
        : "rcx", "r11", "memory"
    );
    return ret;
}

static void prof__os_close(i64 fd) {
    i64 ret;
    __asm__ volatile (
        "syscall"
        : "=a"(ret)
        : "a"((i64)PROF_SYS_CLOSE), "D"(fd)
        : "rcx", "r11", "memory"
    );
    (void)ret;
}

#else
#error "profiler.h: unsupported platform (expected linux x86-64 or windows)"
#endif

typedef struct ProfState {
    ProfZone   zones[PROF_MAX_ZONES];
    u32        zone_count;
    u32        zone_slots[PROF_ZONE_SLOTS];
    ProfThread threads[PROF_MAX_THREADS];
    ProfEvent *events;
    u64        event_cap;
    u64        event_count;
    u64        event_dropped;
    u64        total_ns;
    u64        frames;
    u8        *arena;
    usize      arena_size;
    i32        lock;
    b32        initialized;
    b32        record_events;
} ProfState;

static ProfState  prof__state;
static u8         prof__default_arena[PROF_ARENA_BYTES];
static PROF_TLS ProfThread *prof__tls_thread;

static void prof__lock(void) {
    while (PROF_ATOMIC_EXCHANGE32(&prof__state.lock, 1)) { }
}

static void prof__unlock(void) {
    PROF_ATOMIC_STORE32(&prof__state.lock, 0);
}

static u64 prof__hash(const char *s, const char *file, u32 line) {
    u64 h = 1469598103934665603ull;
    while (*s) {
        h ^= (u8)*s++;
        h *= 1099511628211ull;
    }
    h ^= (u64)(uptr)file;
    h *= 1099511628211ull;
    h ^= (u64)line;
    h *= 1099511628211ull;
    return h;
}

static b32 prof__streq(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

static void prof__ensure_init(void) {
    if (PROF_ATOMIC_LOAD32(&prof__state.initialized)) return;
    prof__lock();
    if (!prof__state.initialized) {
        prof__state.arena      = prof__default_arena;
        prof__state.arena_size = PROF_ARENA_BYTES;
        prof__state.events     = (ProfEvent *)prof__state.arena;
        prof__state.event_cap  = prof__state.arena_size / (u64)sizeof(ProfEvent);
        prof__state.record_events = true;
        PROF_ATOMIC_STORE32(&prof__state.initialized, true);
    }
    prof__unlock();
}

static ProfZone *prof__get_zone(const char *name, const char *file, u32 line) {
    const u32 mask = PROF_ZONE_SLOTS - 1;
    u64 h = prof__hash(name, file, line);
    u32 i = (u32)h & mask;

    /* Lock-free read of already published entries. */
    for (;;) {
        u32 slot = (u32)PROF_ATOMIC_LOAD32(&prof__state.zone_slots[i]);
        ProfZone *z;
        if (slot == 0) break;
        z = &prof__state.zones[slot - 1];
        if (z->hash == h && z->line == line && z->file == file && prof__streq(z->name, name))
            return z;
        i = (i + 1) & mask;
    }

    prof__lock();
    i = (u32)h & mask;
    for (;;) {
        u32 slot = prof__state.zone_slots[i];
        ProfZone *z;
        if (slot == 0) break;
        z = &prof__state.zones[slot - 1];
        if (z->hash == h && z->line == line && z->file == file && prof__streq(z->name, name)) {
            prof__unlock();
            return z;
        }
        i = (i + 1) & mask;
    }

    if (prof__state.zone_count >= PROF_MAX_ZONES) {
        prof__unlock();
        return (ProfZone *)0;
    }

    {
        u32 idx = prof__state.zone_count++;
        ProfZone *z = &prof__state.zones[idx];
        z->name = name;
        z->file = file;
        z->line = line;
        z->hash = h;
        z->id   = idx;
        z->self_ns = 0;
        z->incl_ns = 0;
        z->hits    = 0;
        PROF_ATOMIC_STORE32(&prof__state.zone_slots[i], (i32)(idx + 1));
        prof__unlock();
        return z;
    }
}

static void prof__push_event(u64 ts, u32 zone_id, u32 tid, u8 phase) {
    u64 i = PROF_ATOMIC_FETCH_ADD64(&prof__state.event_count, 1);
    if (i < prof__state.event_cap) {
        ProfEvent *e = &prof__state.events[i];
        e->ts = ts;
        e->zone_id = zone_id;
        e->tid = tid;
        e->phase = phase;
    } else {
        PROF_ATOMIC_FETCH_ADD64(&prof__state.event_dropped, 1);
    }
}

static ProfThread *prof__thread(void) {
    ProfThread *t = prof__tls_thread;
    u32 tid;
    u32 i;

    if (t) return t;

    prof__ensure_init();
    tid = prof__get_tid();

    prof__lock();
    for (i = 0; i < PROF_MAX_THREADS; i++) {
        if (prof__state.threads[i].tid == tid) { t = &prof__state.threads[i]; break; }
    }
    if (!t) {
        for (i = 0; i < PROF_MAX_THREADS; i++) {
            if (prof__state.threads[i].tid == 0) {
                t = &prof__state.threads[i];
                t->depth = 0;
                t->overflow = 0;
                t->tid = tid ? tid : 1;
                break;
            }
        }
    }
    prof__unlock();

    prof__tls_thread = t;
    return t;
}

void prof__begin(const char *name, const char *file, u32 line) {
    ProfThread *t = prof__thread();
    ProfZone *z;
    u64 now;

    if (!t) return;

    if (t->depth >= PROF_MAX_DEPTH) {
        t->overflow++;
        return;
    }

    z = prof__get_zone(name, file, line);
    if (!z) return;

    now = prof__now_ns();
    t->stack[t->depth]    = z;
    t->start_ns[t->depth] = now;
    t->child_ns[t->depth] = 0;
    t->depth++;

    if (prof__state.record_events) prof__push_event(now, z->id, t->tid, 0);
}

void prof__end(void) {
    ProfThread *t = prof__tls_thread;
    ProfZone *z;
    u64 now, elapsed, child;

    if (!t) return;

    if (t->overflow) { t->overflow--; return; }
    if (t->depth == 0) return;

    t->depth--;
    now    = prof__now_ns();
    z      = t->stack[t->depth];
    child  = t->child_ns[t->depth];
    elapsed = now - t->start_ns[t->depth];

    PROF_ATOMIC_FETCH_ADD64(&z->incl_ns, elapsed);
    PROF_ATOMIC_FETCH_ADD64(&z->self_ns, elapsed - child);
    PROF_ATOMIC_FETCH_ADD64(&z->hits, 1);

    if (t->depth > 0) {
        t->child_ns[t->depth - 1] += elapsed;
    } else {
        PROF_ATOMIC_FETCH_ADD64(&prof__state.total_ns, elapsed);
    }

    if (prof__state.record_events) prof__push_event(now, z->id, t->tid, 1);
}

b32 prof_init(void *memory, usize size) {
    if (!memory || size < (usize)(sizeof(ProfEvent) * 64)) return false;
    prof__ensure_init();
    prof__lock();
    prof__state.arena      = (u8 *)memory;
    prof__state.arena_size = (u64)size;
    prof__state.events     = (ProfEvent *)memory;
    prof__state.event_cap  = (u64)size / (u64)sizeof(ProfEvent);
    prof__state.event_count = 0;
    prof__state.event_dropped = 0;
    prof__unlock();
    return true;
}

void prof_frame_end(void) {
    prof__ensure_init();
    PROF_ATOMIC_FETCH_ADD64(&prof__state.frames, 1);
}

void prof_reset(void) {
    u32 i;
    prof__ensure_init();
    prof__lock();
    for (i = 0; i < prof__state.zone_count; i++) {
        prof__state.zones[i].self_ns = 0;
        prof__state.zones[i].incl_ns = 0;
        prof__state.zones[i].hits    = 0;
    }
    prof__state.total_ns = 0;
    prof__state.frames = 0;
    prof__state.event_count = 0;
    prof__state.event_dropped = 0;
    prof__unlock();
}

void prof_set_recording(b32 enabled) {
    prof__ensure_init();
    prof__state.record_events = enabled ? true : false;
}

u64 prof_frame_count(void)   { prof__ensure_init(); return PROF_ATOMIC_LOAD64(&prof__state.frames); }
u64 prof_event_count(void)   { prof__ensure_init(); return PROF_ATOMIC_LOAD64(&prof__state.event_count); }
u64 prof_event_dropped(void) { prof__ensure_init(); return PROF_ATOMIC_LOAD64(&prof__state.event_dropped); }

typedef struct {
    i64 fd;
    u64 len;
    char buf[16384];
} ProfWriter;

static void prof__w_flush(ProfWriter *w) {
    if (w->len) {
        prof__os_write(w->fd, w->buf, w->len);
        w->len = 0;
    }
}

static void prof__w_char(ProfWriter *w, char c) {
    if (w->len == sizeof(w->buf)) prof__w_flush(w);
    w->buf[w->len++] = c;
}

static void prof__w_bytes(ProfWriter *w, const char *s, u64 n) {
    u64 i;
    for (i = 0; i < n; i++) prof__w_char(w, s[i]);
}

static void prof__w_cstr(ProfWriter *w, const char *s) {
    while (*s) prof__w_char(w, *s++);
}

static void prof__w_u64(ProfWriter *w, u64 v) {
    char tmp[20];
    u32 n = 0;
    if (v == 0) { prof__w_char(w, '0'); return; }
    while (v) { tmp[n++] = (char)('0' + (u32)(v % 10)); v /= 10; }
    while (n) prof__w_char(w, tmp[--n]);
}

static u32 prof__u64_digits(u64 v) {
    u32 n = 1;
    while (v >= 10) { v /= 10; n++; }
    return n;
}

static void prof__w_rj_u64(ProfWriter *w, u64 v, u32 width) {
    u32 d = prof__u64_digits(v);
    while (d < width) { prof__w_char(w, ' '); width--; }
    prof__w_u64(w, v);
}

/* nanoseconds rendered as "ms.uuu" */
static void prof__w_ms(ProfWriter *w, u64 ns, u32 width) {
    char tmp[32];
    u32 n = 0, d;
    u64 ms = ns / 1000000ull;
    u32 frac = (u32)((ns % 1000000ull) / 1000ull);
    char fbuf[3];
    u32 i;

    fbuf[0] = (char)('0' + (frac / 100) % 10);
    fbuf[1] = (char)('0' + (frac / 10) % 10);
    fbuf[2] = (char)('0' + frac % 10);

    if (ms == 0) { tmp[n++] = '0'; }
    else { char rev[20]; u32 r = 0; while (ms) { rev[r++] = (char)('0' + (u32)(ms % 10)); ms /= 10; } while (r) tmp[n++] = rev[--r]; }

    tmp[n++] = '.';
    for (i = 0; i < 3; i++) tmp[n++] = fbuf[i];

    d = n;
    while (d < width) { prof__w_char(w, ' '); width--; }
    prof__w_bytes(w, tmp, n);
}

/* part/whole as a one-decimal percentage, right justified */
static void prof__w_pct(ProfWriter *w, u64 part, u64 whole, u32 width) {
    char tmp[32];
    u32 n = 0, d;
    u64 tenths = whole ? (part * 1000ull) / whole : 0;
    u64 ip = tenths / 10ull;
    u32 fp = (u32)(tenths % 10ull);

    if (ip == 0) { tmp[n++] = '0'; }
    else { char rev[20]; u32 r = 0; while (ip) { rev[r++] = (char)('0' + (u32)(ip % 10)); ip /= 10; } while (r) tmp[n++] = rev[--r]; }
    tmp[n++] = '.';
    tmp[n++] = (char)('0' + fp);

    d = n;
    while (d < width) { prof__w_char(w, ' '); width--; }
    prof__w_bytes(w, tmp, n);
}

/* name left justified into a fixed column */
static void prof__w_field(ProfWriter *w, const char *s, u32 width) {
    u32 i = 0;
    while (s[i] && i < width) { prof__w_char(w, s[i]); i++; }
    while (i < width) { prof__w_char(w, ' '); i++; }
}

/* JSON string with minimal escaping */
static void prof__w_json_str(ProfWriter *w, const char *s) {
    prof__w_char(w, '"');
    while (*s) {
        u8 c = (u8)*s++;
        if (c == '"' || c == '\\') { prof__w_char(w, '\\'); prof__w_char(w, (char)c); }
        else if (c < 0x20) {
            static const char hex[] = "0123456789abcdef";
            prof__w_cstr(w, "\\u00");
            prof__w_char(w, hex[(c >> 4) & 0xF]);
            prof__w_char(w, hex[c & 0xF]);
        } else {
            prof__w_char(w, (char)c);
        }
    }
    prof__w_char(w, '"');
}

// NOTE(Giovanni): Report

void prof_report(void) {
    ProfWriter w;
    u32 order[PROF_MAX_ZONES];
    u32 count, i, j;
    u64 total_ns, frames;

    prof__ensure_init();

    w.fd = 1;
    w.len = 0;

    prof__lock();
    count = prof__state.zone_count;
    for (i = 0; i < count; i++) order[i] = i;
    total_ns = prof__state.total_ns;
    frames = prof__state.frames;
    prof__unlock();

    /* insertion sort by self time, descending */
    for (i = 1; i < count; i++) {
        u32 key = order[i];
        u64 key_self = prof__state.zones[key].self_ns;
        j = i;
        while (j > 0 && prof__state.zones[order[j - 1]].self_ns < key_self) {
            order[j] = order[j - 1];
            j--;
        }
        order[j] = key;
    }

    prof__w_cstr(&w, "\n");
    prof__w_field(&w, "zone", 34);
    prof__w_cstr(&w, "self(ms)");
    prof__w_cstr(&w, "    incl(ms)");
    prof__w_cstr(&w, "   self%");
    prof__w_cstr(&w, "    hits");
    prof__w_cstr(&w, "\n");

    for (i = 0; i < 34 + 8 + 12 + 8 + 8; i++) prof__w_char(&w, '-');
    prof__w_cstr(&w, "\n");

    for (i = 0; i < count; i++) {
        ProfZone *z = &prof__state.zones[order[i]];
        if (z->hits == 0) continue;
        prof__w_field(&w, z->name, 34);
        prof__w_ms(&w, z->self_ns, 8);
        prof__w_cstr(&w, "    ");
        prof__w_ms(&w, z->incl_ns, 8);
        prof__w_cstr(&w, "   ");
        prof__w_pct(&w, z->self_ns, total_ns, 6);
        prof__w_cstr(&w, "    ");
        prof__w_rj_u64(&w, z->hits, 8);
        prof__w_cstr(&w, "\n");
    }

    prof__w_cstr(&w, "\nframes: ");
    prof__w_u64(&w, frames);
    prof__w_cstr(&w, "   total: ");
    prof__w_ms(&w, total_ns, 1);
    prof__w_cstr(&w, " ms   avg frame: ");
    prof__w_ms(&w, frames ? total_ns / frames : 0, 1);
    prof__w_cstr(&w, " ms\n");
    prof__w_cstr(&w, "events: ");
    prof__w_u64(&w, prof__state.event_count);
    prof__w_cstr(&w, " (dropped ");
    prof__w_u64(&w, prof__state.event_dropped);
    prof__w_cstr(&w, ")\n\n");

    prof__w_flush(&w);
}

//NOTE(Giovanni): Chrome Trace
b32 prof_write_trace(const char *path) {
    ProfWriter w;
    i64 fd;
    u64 i, n;
    b32 first = true;

    prof__ensure_init();

    fd = prof__os_open_trunc(path);
    if (fd < 0) return false;

    w.fd = fd;
    w.len = 0;

    prof__w_cstr(&w, "{\"traceEvents\":[");
    prof__w_cstr(&w, "{\"ph\":\"M\",\"pid\":1,\"tid\":0,\"name\":\"process_name\",\"args\":{\"name\":");
    prof__w_json_str(&w, PROF_PROCESS_NAME);
    prof__w_cstr(&w, "}}");
    first = false;

    n = PROF_ATOMIC_LOAD64(&prof__state.event_count);
    if (n > prof__state.event_cap) n = prof__state.event_cap;

    for (i = 0; i < n; i++) {
        ProfEvent *e = &prof__state.events[i];
        ProfZone *z = &prof__state.zones[e->zone_id];
        u64 us = e->ts / 1000ull;
        u32 frac = (u32)(e->ts % 1000ull);

        if (!first) prof__w_char(&w, ',');
        first = false;

        prof__w_cstr(&w, "{\"ph\":\"");
        prof__w_char(&w, e->phase == 0 ? 'B' : 'E');
        prof__w_cstr(&w, "\",\"pid\":1,\"tid\":");
        prof__w_u64(&w, e->tid);
        prof__w_cstr(&w, ",\"ts\":");
        prof__w_u64(&w, us);
        prof__w_char(&w, '.');
        prof__w_char(&w, (char)('0' + (frac / 100) % 10));
        prof__w_char(&w, (char)('0' + (frac / 10) % 10));
        prof__w_char(&w, (char)('0' + frac % 10));
        prof__w_cstr(&w, ",\"cat\":\"zone\",\"name\":");
        prof__w_json_str(&w, z->name);
        prof__w_char(&w, '}');
    }

    prof__w_cstr(&w, "],\"displayTimeUnit\":\"ms\"}");
    prof__w_flush(&w);

    prof__os_close(fd);
    return true;
}

#endif /* MICROPROF_IMPLEMENTATION */

#endif /* MICROPROF_H */
