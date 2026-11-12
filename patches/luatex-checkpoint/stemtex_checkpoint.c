/* Experimental, same-process Lua/TeX rollback. All bookkeeping and copies live
   on a private Win32 heap, outside both the wrapped CRT allocator and the
   engine's writable-image ranges. The engine owns its continuation/longjmp.

   Sidecar: <exe>.stemtex-checkpoint, ASCII:
     STEMTEX_CHECKPOINT_RANGES_V1
     <exe byte size, decimal> <FNV-1a-64 of all exe bytes, hexadecimal>
     D <image-relative address, hexadecimal> <length, hexadecimal>
     C <image-relative address, hexadecimal> <length, hexadecimal>
   D describes writable engine/library state, C the matching allocation domain.
   Do not include CRT/toolchain objects or this module/continuation machinery.
*/
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <io.h>
#include "stemtex_checkpoint.h"

#define CP_BUCKETS 65536u
#define CP_MAX_COPY ((size_t)1024 * 1024 * 1024)
#define CP_CALLER() __builtin_return_address(0)

typedef struct CpBlock CpBlock;
struct CpBlock {
    void *pointer, *caller, *copy;
    size_t size;
    int baseline, live;
    CpBlock *hash_next, *next, *previous;
};
typedef struct CpRegion CpRegion;
struct CpRegion { unsigned char *address, *copy; size_t size; CpRegion *next; };
typedef struct CpFile CpFile;
struct CpFile { FILE *file; void *caller; fpos_t position; int readonly, baseline, closed; CpFile *next; };

typedef struct CpState {
    HANDLE heap;
    CRITICAL_SECTION lock;
    int enabled, active, safe, pending, ranges_loaded, debug;
    char error[256];
    CpBlock **buckets, *blocks;
    CpRegion *data, *code;
    CpFile *files;
    FILE *output;
    __int64 output_position;
    size_t heap_bytes, global_bytes, block_count;
    unsigned long restores;
    LARGE_INTEGER clock_frequency;
    void *(__cdecl *malloc_fn)(size_t);
    void *(__cdecl *calloc_fn)(size_t, size_t);
    void *(__cdecl *realloc_fn)(void *, size_t);
    void (__cdecl *free_fn)(void *);
    FILE *(__cdecl *fopen_fn)(const char *, const char *);
    FILE *(__cdecl *wfopen_fn)(const wchar_t *, const wchar_t *);
    FILE *(__cdecl *fdopen_fn)(int, const char *);
    FILE *(__cdecl *freopen_fn)(const char *, const char *, FILE *);
    FILE *(__cdecl *tmpfile_fn)(void);
    int (__cdecl *fclose_fn)(FILE *);
    LPVOID (WINAPI *virtual_alloc_fn)(LPVOID, SIZE_T, DWORD, DWORD);
    BOOL (WINAPI *virtual_free_fn)(LPVOID, SIZE_T, DWORD);
    LPVOID (WINAPI *map_view_fn)(HANDLE, DWORD, DWORD, DWORD, SIZE_T);
    BOOL (WINAPI *unmap_view_fn)(LPCVOID);
    HMODULE (WINAPI *load_library_a_fn)(LPCSTR);
    HMODULE (WINAPI *load_library_w_fn)(LPCWSTR);
    HMODULE (WINAPI *load_library_ex_a_fn)(LPCSTR, HANDLE, DWORD);
    HMODULE (WINAPI *load_library_ex_w_fn)(LPCWSTR, HANDLE, DWORD);
    BOOL (WINAPI *free_library_fn)(HMODULE);
    HANDLE (WINAPI *create_mapping_a_fn)(HANDLE, LPSECURITY_ATTRIBUTES, DWORD, DWORD, DWORD, LPCSTR);
    HANDLE (WINAPI *create_mapping_w_fn)(HANDLE, LPSECURITY_ATTRIBUTES, DWORD, DWORD, DWORD, LPCWSTR);
} CpState;

static INIT_ONCE cp_once = INIT_ONCE_STATIC_INIT;
static CpState *cp_state;

static BOOL CALLBACK cp_initialize(PINIT_ONCE once, PVOID parameter, PVOID *context) {
    HANDLE heap = HeapCreate(0, 0, 0);
    CpState *s;
    HMODULE crt = GetModuleHandleW(L"msvcrt.dll");
    HMODULE kernel = GetModuleHandleW(L"kernel32.dll");
    char enabled[8];
    (void)once; (void)parameter; (void)context;
    if (!heap || !crt || !kernel) ExitProcess(201);
    s = (CpState *)HeapAlloc(heap, HEAP_ZERO_MEMORY, sizeof(*s));
    if (!s) ExitProcess(201);
    s->heap = heap;
    s->safe = 1;
    QueryPerformanceFrequency(&s->clock_frequency);
    InitializeCriticalSection(&s->lock);
#define CP_RESOLVE(member, name) do { FARPROC entry = GetProcAddress(crt, name); memcpy(&s->member, &entry, sizeof(entry)); } while (0)
    CP_RESOLVE(malloc_fn, "malloc"); CP_RESOLVE(calloc_fn, "calloc");
    CP_RESOLVE(realloc_fn, "realloc"); CP_RESOLVE(free_fn, "free");
    CP_RESOLVE(fopen_fn, "fopen"); CP_RESOLVE(wfopen_fn, "_wfopen");
    CP_RESOLVE(fdopen_fn, "_fdopen"); CP_RESOLVE(freopen_fn, "freopen");
    CP_RESOLVE(tmpfile_fn, "tmpfile"); CP_RESOLVE(fclose_fn, "fclose");
#undef CP_RESOLVE
#define CP_RESOLVE(member, name) do { FARPROC entry = GetProcAddress(kernel, name); memcpy(&s->member, &entry, sizeof(entry)); } while (0)
    CP_RESOLVE(virtual_alloc_fn, "VirtualAlloc"); CP_RESOLVE(virtual_free_fn, "VirtualFree");
    CP_RESOLVE(map_view_fn, "MapViewOfFile"); CP_RESOLVE(unmap_view_fn, "UnmapViewOfFile");
    CP_RESOLVE(load_library_a_fn, "LoadLibraryA"); CP_RESOLVE(load_library_w_fn, "LoadLibraryW");
    CP_RESOLVE(load_library_ex_a_fn, "LoadLibraryExA"); CP_RESOLVE(load_library_ex_w_fn, "LoadLibraryExW");
    CP_RESOLVE(free_library_fn, "FreeLibrary");
    CP_RESOLVE(create_mapping_a_fn, "CreateFileMappingA"); CP_RESOLVE(create_mapping_w_fn, "CreateFileMappingW");
#undef CP_RESOLVE
    if (!s->malloc_fn || !s->calloc_fn || !s->realloc_fn || !s->free_fn ||
        !s->fopen_fn || !s->wfopen_fn || !s->fdopen_fn || !s->freopen_fn ||
        !s->tmpfile_fn || !s->fclose_fn) ExitProcess(202);
    if (!s->virtual_alloc_fn || !s->virtual_free_fn || !s->map_view_fn || !s->unmap_view_fn ||
        !s->load_library_a_fn || !s->load_library_w_fn || !s->load_library_ex_a_fn ||
        !s->load_library_ex_w_fn || !s->free_library_fn ||
        !s->create_mapping_a_fn || !s->create_mapping_w_fn) ExitProcess(202);
    s->enabled = GetEnvironmentVariableA("STEMTEX_LUA_CHECKPOINT", enabled, sizeof(enabled)) == 1 && enabled[0] == '1';
    s->debug = GetEnvironmentVariableA("STEMTEX_LUA_CHECKPOINT_DEBUG", enabled, sizeof(enabled)) == 1 && enabled[0] == '1';
    if (s->enabled) {
        s->buckets = (CpBlock **)HeapAlloc(heap, HEAP_ZERO_MEMORY, CP_BUCKETS * sizeof(CpBlock *));
        if (!s->buckets) ExitProcess(201);
    }
    cp_state = s;
    return TRUE;
}

static CpState *cp_get(void) {
    InitOnceExecuteOnce(&cp_once, cp_initialize, NULL, NULL);
    return cp_state;
}

static void cp_fail(CpState *s, const char *reason) {
    s->safe = 0;
    if (!s->error[0]) lstrcpynA(s->error, reason, sizeof(s->error));
}

static void cp_trace(CpState *s, const char *phase, size_t index, const void *address, size_t size) {
    if (!s->debug) return;
    fprintf(stderr, "STEMTEX_CHECKPOINT_STAGE phase=%s index=%llu address=%p size=%llu safe=%d\n",
            phase, (unsigned long long)index, address, (unsigned long long)size, s->safe);
    fflush(stderr);
}

static unsigned cp_hash(void *p) { return (unsigned)(((uintptr_t)p >> 4) & (CP_BUCKETS - 1)); }
static CpBlock *cp_find(CpState *s, void *p) {
    CpBlock *b = s->buckets[cp_hash(p)];
    while (b && b->pointer != p) b = b->hash_next;
    return b;
}
static int cp_owned(CpState *s, void *caller) {
    CpRegion *r;
    uintptr_t p = (uintptr_t)caller;
    for (r = s->code; r; r = r->next)
        if (p >= (uintptr_t)r->address && p - (uintptr_t)r->address < r->size) return 1;
    return 0;
}

/* These handles and mappings live outside the tracked heap. Preserve normal
   API semantics, but never claim a successful rollback after engine code has
   touched them. Runtime-owned calls are excluded with the same code domain as
   allocations, so CRT bookkeeping cannot invalidate ordinary file input. */
static CpState *cp_external_resource(void *caller, const char *reason) {
    CpState *s = cp_get();
    if (s->enabled && s->active) {
        EnterCriticalSection(&s->lock);
        if (cp_owned(s, caller)) cp_fail(s, reason);
        LeaveCriticalSection(&s->lock);
    }
    return s;
}

#define CP_EXTERNAL(name) cp_external_resource(CP_CALLER(), name " after checkpoint cannot be rolled back")
LPVOID WINAPI __wrap_VirtualAlloc(LPVOID address, SIZE_T size, DWORD type, DWORD protect) {
    CpState *s = CP_EXTERNAL("VirtualAlloc"); return s->virtual_alloc_fn(address, size, type, protect);
}
BOOL WINAPI __wrap_VirtualFree(LPVOID address, SIZE_T size, DWORD type) {
    CpState *s = CP_EXTERNAL("VirtualFree"); return s->virtual_free_fn(address, size, type);
}
LPVOID WINAPI __wrap_MapViewOfFile(HANDLE mapping, DWORD access, DWORD high, DWORD low, SIZE_T size) {
    CpState *s = CP_EXTERNAL("MapViewOfFile"); return s->map_view_fn(mapping, access, high, low, size);
}
BOOL WINAPI __wrap_UnmapViewOfFile(LPCVOID address) {
    CpState *s = CP_EXTERNAL("UnmapViewOfFile"); return s->unmap_view_fn(address);
}
HMODULE WINAPI __wrap_LoadLibraryA(LPCSTR name) {
    CpState *s = CP_EXTERNAL("LoadLibraryA"); return s->load_library_a_fn(name);
}
HMODULE WINAPI __wrap_LoadLibraryW(LPCWSTR name) {
    CpState *s = CP_EXTERNAL("LoadLibraryW"); return s->load_library_w_fn(name);
}
HMODULE WINAPI __wrap_LoadLibraryExA(LPCSTR name, HANDLE file, DWORD flags) {
    CpState *s = CP_EXTERNAL("LoadLibraryExA"); return s->load_library_ex_a_fn(name, file, flags);
}
HMODULE WINAPI __wrap_LoadLibraryExW(LPCWSTR name, HANDLE file, DWORD flags) {
    CpState *s = CP_EXTERNAL("LoadLibraryExW"); return s->load_library_ex_w_fn(name, file, flags);
}
BOOL WINAPI __wrap_FreeLibrary(HMODULE module) {
    CpState *s = CP_EXTERNAL("FreeLibrary"); return s->free_library_fn(module);
}
HANDLE WINAPI __wrap_CreateFileMappingA(HANDLE file, LPSECURITY_ATTRIBUTES security, DWORD protect,
                                       DWORD high, DWORD low, LPCSTR name) {
    CpState *s = CP_EXTERNAL("CreateFileMappingA"); return s->create_mapping_a_fn(file, security, protect, high, low, name);
}
HANDLE WINAPI __wrap_CreateFileMappingW(HANDLE file, LPSECURITY_ATTRIBUTES security, DWORD protect,
                                       DWORD high, DWORD low, LPCWSTR name) {
    CpState *s = CP_EXTERNAL("CreateFileMappingW"); return s->create_mapping_w_fn(file, security, protect, high, low, name);
}
#undef CP_EXTERNAL

LPVOID (WINAPI *__wrap___imp_VirtualAlloc)(LPVOID, SIZE_T, DWORD, DWORD) = __wrap_VirtualAlloc;
BOOL (WINAPI *__wrap___imp_VirtualFree)(LPVOID, SIZE_T, DWORD) = __wrap_VirtualFree;
LPVOID (WINAPI *__wrap___imp_MapViewOfFile)(HANDLE, DWORD, DWORD, DWORD, SIZE_T) = __wrap_MapViewOfFile;
BOOL (WINAPI *__wrap___imp_UnmapViewOfFile)(LPCVOID) = __wrap_UnmapViewOfFile;
HMODULE (WINAPI *__wrap___imp_LoadLibraryA)(LPCSTR) = __wrap_LoadLibraryA;
HMODULE (WINAPI *__wrap___imp_LoadLibraryW)(LPCWSTR) = __wrap_LoadLibraryW;
HMODULE (WINAPI *__wrap___imp_LoadLibraryExA)(LPCSTR, HANDLE, DWORD) = __wrap_LoadLibraryExA;
HMODULE (WINAPI *__wrap___imp_LoadLibraryExW)(LPCWSTR, HANDLE, DWORD) = __wrap_LoadLibraryExW;
BOOL (WINAPI *__wrap___imp_FreeLibrary)(HMODULE) = __wrap_FreeLibrary;
HANDLE (WINAPI *__wrap___imp_CreateFileMappingA)(HANDLE, LPSECURITY_ATTRIBUTES, DWORD, DWORD, DWORD, LPCSTR) = __wrap_CreateFileMappingA;
HANDLE (WINAPI *__wrap___imp_CreateFileMappingW)(HANDLE, LPSECURITY_ATTRIBUTES, DWORD, DWORD, DWORD, LPCWSTR) = __wrap_CreateFileMappingW;
static void cp_record(CpState *s, void *p, size_t size, void *caller) {
    CpBlock *b;
    unsigned bucket;
    if (!p) return;
    b = (CpBlock *)HeapAlloc(s->heap, HEAP_ZERO_MEMORY, sizeof(*b));
    if (!b) { cp_fail(s, "allocation metadata exhausted"); return; }
    b->pointer = p; b->size = size; b->caller = caller; b->live = 1;
    bucket = cp_hash(p); b->hash_next = s->buckets[bucket]; s->buckets[bucket] = b;
    b->next = s->blocks; if (b->next) b->next->previous = b; s->blocks = b;
}
static void cp_remove(CpState *s, CpBlock *b) {
    CpBlock **link = &s->buckets[cp_hash(b->pointer)];
    while (*link && *link != b) link = &(*link)->hash_next;
    if (*link) *link = b->hash_next;
    if (b->previous) b->previous->next = b->next; else s->blocks = b->next;
    if (b->next) b->next->previous = b->previous;
    if (b->copy) HeapFree(s->heap, 0, b->copy);
    HeapFree(s->heap, 0, b);
}

void *__wrap_malloc(size_t size) {
    CpState *s = cp_get(); void *p = s->malloc_fn(size);
    if (s->enabled) { EnterCriticalSection(&s->lock); cp_record(s, p, size, CP_CALLER()); LeaveCriticalSection(&s->lock); }
    return p;
}
void *__wrap_calloc(size_t count, size_t size) {
    CpState *s = cp_get(); void *p = s->calloc_fn(count, size);
    if (s->enabled && p) {
        EnterCriticalSection(&s->lock);
        if (size && count > SIZE_MAX / size) cp_fail(s, "calloc size overflow");
        else cp_record(s, p, count * size, CP_CALLER());
        LeaveCriticalSection(&s->lock);
    }
    return p;
}
void __wrap_free(void *p) {
    CpState *s = cp_get(); CpBlock *b;
    if (!p) return;
    if (!s->enabled) { s->free_fn(p); return; }
    EnterCriticalSection(&s->lock);
    b = cp_find(s, p);
    if (b && b->baseline) b->live = 0;
    else {
        if (!b && s->active && cp_owned(s, CP_CALLER())) cp_fail(s, "engine free of an untracked block");
        if (b) cp_remove(s, b);
        s->free_fn(p);
    }
    LeaveCriticalSection(&s->lock);
}
void *__wrap_realloc(void *p, size_t size) {
    CpState *s = cp_get(); CpBlock *b; void *result, *caller = CP_CALLER();
    if (!s->enabled) return s->realloc_fn(p, size);
    if (!size) { __wrap_free(p); return NULL; }
    EnterCriticalSection(&s->lock);
    b = p ? cp_find(s, p) : NULL;
    if (b && b->baseline) {
        result = s->malloc_fn(size);
        if (result) {
            memcpy(result, p, size < b->size ? size : b->size);
            b->live = 0; cp_record(s, result, size, b->caller);
        }
    } else {
        if (b) caller = b->caller;
        else if (p && s->active && cp_owned(s, caller)) cp_fail(s, "engine realloc of an untracked block");
        result = s->realloc_fn(p, size);
        if (result) { if (b) cp_remove(s, b); cp_record(s, result, size, caller); }
    }
    LeaveCriticalSection(&s->lock);
    return result;
}
static char *cp_strdup(const char *text, void *caller) {
    CpState *s = cp_get(); size_t size = strlen(text) + 1; char *p = (char *)s->malloc_fn(size);
    if (p) memcpy(p, text, size);
    if (s->enabled) { EnterCriticalSection(&s->lock); cp_record(s, p, size, caller); LeaveCriticalSection(&s->lock); }
    return p;
}
char *__wrap_strdup(const char *text) { return cp_strdup(text, CP_CALLER()); }
char *__wrap__strdup(const char *text) { return cp_strdup(text, CP_CALLER()); }

static CpFile *cp_file_find(CpState *s, FILE *file) {
    CpFile *f; for (f = s->files; f; f = f->next) if (f->file == file) return f; return NULL;
}
static void cp_file_record(CpState *s, FILE *file, int readonly, void *caller) {
    CpFile *f;
    if (!file || !s->enabled) return;
    EnterCriticalSection(&s->lock);
    f = (CpFile *)HeapAlloc(s->heap, HEAP_ZERO_MEMORY, sizeof(*f));
    if (!f) cp_fail(s, "FILE metadata exhausted");
    else { f->file = file; f->caller = caller; f->readonly = readonly; f->next = s->files; s->files = f; }
    LeaveCriticalSection(&s->lock);
}
static int cp_readonly(const char *mode) { return mode && mode[0] == 'r' && !strchr(mode, '+'); }
FILE *__wrap_fopen(const char *name, const char *mode) {
    CpState *s = cp_get(); FILE *f = s->fopen_fn(name, mode); cp_file_record(s, f, cp_readonly(mode), CP_CALLER()); return f;
}
FILE *__wrap__wfopen(const wchar_t *name, const wchar_t *mode) {
    CpState *s = cp_get(); FILE *f = s->wfopen_fn(name, mode);
    cp_file_record(s, f, mode && mode[0] == L'r' && !wcschr(mode, L'+'), CP_CALLER()); return f;
}
FILE *__wrap__fdopen(int descriptor, const char *mode) {
    CpState *s = cp_get(); FILE *f = s->fdopen_fn(descriptor, mode); cp_file_record(s, f, cp_readonly(mode), CP_CALLER()); return f;
}
FILE *__wrap_fdopen(int descriptor, const char *mode) {
    CpState *s = cp_get(); FILE *f = s->fdopen_fn(descriptor, mode); cp_file_record(s, f, cp_readonly(mode), CP_CALLER()); return f;
}
FILE *__wrap_tmpfile(void) {
    CpState *s = cp_get(); FILE *f = s->tmpfile_fn(); cp_file_record(s, f, 0, CP_CALLER()); return f;
}
int __wrap_fclose(FILE *file) {
    CpState *s = cp_get(); CpFile **link, *f; int result;
    if (!s->enabled) return s->fclose_fn(file);
    EnterCriticalSection(&s->lock);
    f = cp_file_find(s, file);
    if (f && f->baseline) { f->closed = 1; result = f->readonly ? 0 : fflush(file); }
    else {
        result = s->fclose_fn(file);
        if (f) { link = &s->files; while (*link != f) link = &(*link)->next; *link = f->next; HeapFree(s->heap, 0, f); }
    }
    LeaveCriticalSection(&s->lock); return result;
}
FILE *__wrap_freopen(const char *name, const char *mode, FILE *file) {
    CpState *s = cp_get(); CpFile *f; FILE *result;
    if (!s->enabled) return s->freopen_fn(name, mode, file);
    EnterCriticalSection(&s->lock); f = cp_file_find(s, file);
    if (s->active) cp_fail(s, "freopen after checkpoint cannot be rolled back");
    result = s->freopen_fn(name, mode, file);
    if (f) { f->file = result; f->readonly = cp_readonly(mode); }
    else cp_file_record(s, result, cp_readonly(mode), CP_CALLER());
    LeaveCriticalSection(&s->lock); return result;
}

/* MinGW headers sometimes call imported function-pointer symbols directly.
   Resolve the real CRT entries with GetProcAddress above, so wrapping both the
   function and its import slot cannot recursively enter an import thunk. */
void *(__cdecl *__wrap___imp_malloc)(size_t) = __wrap_malloc;
void *(__cdecl *__wrap___imp_calloc)(size_t,size_t) = __wrap_calloc;
void *(__cdecl *__wrap___imp_realloc)(void*,size_t) = __wrap_realloc;
void (__cdecl *__wrap___imp_free)(void*) = __wrap_free;
char *(__cdecl *__wrap___imp__strdup)(const char*) = __wrap__strdup;
FILE *(__cdecl *__wrap___imp_fopen)(const char*,const char*) = __wrap_fopen;
FILE *(__cdecl *__wrap___imp__wfopen)(const wchar_t*,const wchar_t*) = __wrap__wfopen;
FILE *(__cdecl *__wrap___imp__fdopen)(int,const char*) = __wrap__fdopen;
FILE *(__cdecl *__wrap___imp_freopen)(const char*,const char*,FILE*) = __wrap_freopen;
FILE *(__cdecl *__wrap___imp_tmpfile)(void) = __wrap_tmpfile;
int (__cdecl *__wrap___imp_fclose)(FILE*) = __wrap_fclose;

static int cp_add(CpState *s, CpRegion **head, void *address, size_t size) {
    CpRegion *r; MEMORY_BASIC_INFORMATION memory;
    if (!size || !address || (uintptr_t)address > UINTPTR_MAX - size) return 0;
    if (!VirtualQuery(address, &memory, sizeof(memory)) || memory.State != MEM_COMMIT ||
        (memory.Protect & (PAGE_NOACCESS | PAGE_GUARD))) return 0;
    r = (CpRegion *)HeapAlloc(s->heap, HEAP_ZERO_MEMORY, sizeof(*r));
    if (!r) return 0;
    r->address = (unsigned char *)address; r->size = size; r->next = *head; *head = r; return 1;
}
int stemtex_checkpoint_add_region(void *address, size_t size) {
    CpState *s = cp_get(); return !s->active && cp_add(s, &s->data, address, size);
}
int stemtex_checkpoint_add_code_region(void *address, size_t size) {
    CpState *s = cp_get(); return !s->active && cp_add(s, &s->code, address, size);
}

static int cp_load_ranges(CpState *s) {
    wchar_t path[32768]; HANDLE file; LARGE_INTEGER length; DWORD got;
    char bytes[65536], *text, *line, *end; size_t n;
    uint64_t hash = UINT64_C(14695981039346656037), expected_hash, expected_size;
    unsigned char *base = (unsigned char *)GetModuleHandleW(NULL);
    IMAGE_NT_HEADERS *pe = (IMAGE_NT_HEADERS *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
    if (s->data && s->code) return 1;
    n = GetModuleFileNameW(NULL, path, sizeof(path) / sizeof(*path));
    if (!n || n + 22 >= sizeof(path) / sizeof(*path)) return 0;
    file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) return 0;
    if (!GetFileSizeEx(file, &length)) { CloseHandle(file); return 0; }
    while (ReadFile(file, bytes, sizeof(bytes), &got, NULL) && got) {
        DWORD i; for (i = 0; i < got; ++i) { hash ^= (unsigned char)bytes[i]; hash *= UINT64_C(1099511628211); }
    }
    CloseHandle(file);
    lstrcatW(path, L".stemtex-checkpoint");
    file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) return 0;
    {
        LARGE_INTEGER size;
        if (!GetFileSizeEx(file, &size) || size.QuadPart <= 0 || size.QuadPart > 16 * 1024 * 1024) { CloseHandle(file); return 0; }
        text = (char *)HeapAlloc(s->heap, HEAP_ZERO_MEMORY, (size_t)size.QuadPart + 1);
        if (!text || !ReadFile(file, text, (DWORD)size.QuadPart, &got, NULL) || got != size.QuadPart) {
            if (text) HeapFree(s->heap, 0, text);
            CloseHandle(file); return 0;
        }
        CloseHandle(file);
    }
    if (strncmp(text, "STEMTEX_CHECKPOINT_RANGES_V1\n", 29) != 0 &&
        strncmp(text, "STEMTEX_CHECKPOINT_RANGES_V1\r\n", 30) != 0) { HeapFree(s->heap, 0, text); return 0; }
    line = strchr(text, '\n') + 1;
    expected_size = strtoull(line, &end, 10); expected_hash = strtoull(end, &end, 16);
    if (expected_size != (uint64_t)length.QuadPart || expected_hash != hash) { HeapFree(s->heap, 0, text); return 0; }
    line = strchr(end, '\n');
    while (line && *++line) {
        char kind = *line++; uint64_t rva = strtoull(line, &end, 16), size = strtoull(end, &end, 16);
        if ((kind != 'D' && kind != 'C') || !size || rva >= pe->OptionalHeader.SizeOfImage ||
            size > pe->OptionalHeader.SizeOfImage - rva ||
            !cp_add(s, kind == 'D' ? &s->data : &s->code, base + rva, (size_t)size)) {
            HeapFree(s->heap, 0, text); return 0;
        }
        line = strchr(end, '\n');
    }
    HeapFree(s->heap, 0, text); return s->data && s->code;
}

int stemtex_checkpoint_enabled(void) { return cp_get()->enabled; }
unsigned long long stemtex_checkpoint_milliseconds(void) {
    CpState *s = cp_get(); LARGE_INTEGER counter;
    unsigned long long ticks, frequency;
    if (s->clock_frequency.QuadPart <= 0 || !QueryPerformanceCounter(&counter)) return GetTickCount64();
    ticks = (unsigned long long)counter.QuadPart;
    frequency = (unsigned long long)s->clock_frequency.QuadPart;
    return (ticks / frequency) * 1000 + ((ticks % frequency) * 1000) / frequency;
}
void stemtex_checkpoint_set_output(FILE *stream) { cp_get()->output = stream; }
void stemtex_checkpoint_set_recovery_pending(int value) { cp_get()->pending = value; }
int stemtex_checkpoint_recovery_pending(void) { return cp_get()->pending; }
const char *stemtex_checkpoint_last_error(void) { return cp_get()->error; }
void stemtex_checkpoint_invalidate(const char *reason) {
    CpState *s = cp_get(); EnterCriticalSection(&s->lock); cp_fail(s, reason); LeaveCriticalSection(&s->lock);
}

int stemtex_checkpoint_begin(void) {
    CpState *s = cp_get(); CpBlock *b, *next; CpRegion *r; CpFile *f;
    unsigned long long start = stemtex_checkpoint_milliseconds();
    if (!s->enabled) return 0;
    EnterCriticalSection(&s->lock);
    if (!s->safe) { LeaveCriticalSection(&s->lock); return 0; }
    if (!s->ranges_loaded) {
        if (!cp_load_ranges(s)) cp_fail(s, "missing, invalid or mismatched checkpoint range sidecar");
        else s->ranges_loaded = 1;
    }
    s->heap_bytes = s->global_bytes = s->block_count = 0;
    for (b = s->blocks; b && s->safe; b = next) {
        next = b->next;
        if (!b->live) { s->free_fn(b->pointer); cp_remove(s, b); continue; }
        if (!cp_owned(s, b->caller)) continue;
        if (b->size > CP_MAX_COPY - s->heap_bytes) { cp_fail(s, "checkpoint heap exceeds 1 GiB limit"); break; }
        if (b->copy) HeapFree(s->heap, 0, b->copy);
        b->copy = HeapAlloc(s->heap, 0, b->size ? b->size : 1);
        if (!b->copy) { cp_fail(s, "cannot allocate heap checkpoint copy"); break; }
        memcpy(b->copy, b->pointer, b->size); b->baseline = 1;
        s->heap_bytes += b->size; ++s->block_count;
    }
    for (f = s->files; f && s->safe; f = f->next) {
        if (!cp_owned(s, f->caller) && f->file != s->output) continue;
        if (f->readonly && fgetpos(f->file, &f->position)) cp_fail(s, "cannot save input FILE position");
        else if (!f->readonly && fflush(f->file)) cp_fail(s, "cannot flush baseline output FILE");
        f->baseline = 1; f->closed = 0;
    }
    if (s->output && s->safe) {
        if (fflush(s->output) || (s->output_position = _ftelli64(s->output)) < 0)
            cp_fail(s, "cannot save baseline DVI output position");
    }
    for (r = s->data; r && s->safe; r = r->next) {
        if (r->size > CP_MAX_COPY - s->global_bytes) { cp_fail(s, "checkpoint globals exceed 1 GiB limit"); break; }
        if (!r->copy) r->copy = (unsigned char *)HeapAlloc(s->heap, 0, r->size);
        if (!r->copy) { cp_fail(s, "cannot allocate global checkpoint copy"); break; }
        memcpy(r->copy, r->address, r->size); s->global_bytes += r->size;
    }
    if (s->safe && s->block_count) s->active = 1;
    else if (!s->block_count) cp_fail(s, "empty engine allocation domain");
    LeaveCriticalSection(&s->lock);
    fprintf(stderr, "STEMTEX_CHECKPOINT_SAVED ok=%d blocks=%llu heap=%llu globals=%llu ms=%llu\n",
            s->safe && s->active, (unsigned long long)s->block_count, (unsigned long long)s->heap_bytes,
            (unsigned long long)s->global_bytes, stemtex_checkpoint_milliseconds() - start);
    fflush(stderr);
    return s->safe && s->active;
}

int stemtex_checkpoint_restore(void) {
    CpState *s = cp_get(); CpBlock *b, *next; CpRegion *r; CpFile **link, *f;
    size_t discarded = 0, index = 0; unsigned long long start = stemtex_checkpoint_milliseconds();
    cp_trace(s, "enter", 0, NULL, 0);
    if (!s->enabled || !s->active) return 0;
    EnterCriticalSection(&s->lock);
    if (!s->safe) { LeaveCriticalSection(&s->lock); return 0; }
    link = &s->files;
    while ((f = *link) != NULL && s->safe) {
        cp_trace(s, "file", index++, f->file, f->baseline);
        if (!f->baseline && cp_owned(s, f->caller)) {
            if (s->fclose_fn(f->file)) cp_fail(s, "cannot close request FILE");
            *link = f->next; HeapFree(s->heap, 0, f);
        } else if (f->baseline) {
            if (f->readonly) { clearerr(f->file); if (fsetpos(f->file, &f->position)) cp_fail(s, "cannot rewind baseline input FILE"); }
            f->closed = 0; link = &f->next;
        } else link = &f->next;
    }
    cp_trace(s, "output", 0, s->output, (size_t)s->output_position);
    if (s->output && s->safe) {
        if (fflush(s->output) || _chsize_s(_fileno(s->output), s->output_position) ||
            _fseeki64(s->output, s->output_position, SEEK_SET)) cp_fail(s, "cannot truncate DVI to checkpoint position");
        clearerr(s->output);
    }
    index = 0;
    cp_trace(s, "heap", 0, NULL, s->block_count);
    for (b = s->blocks; b && s->safe; b = next, ++index) {
        next = b->next;
        if (!(index % 65536)) cp_trace(s, "heap-progress", index, b->pointer, b->size);
        if (b->baseline) { memcpy(b->pointer, b->copy, b->size); b->live = 1; }
        else if (cp_owned(s, b->caller)) { discarded += b->size; s->free_fn(b->pointer); cp_remove(s, b); }
    }
    index = 0;
    for (r = s->data; r && s->safe; r = r->next) {
        cp_trace(s, "globals", index++, r->address, r->size);
        memcpy(r->address, r->copy, r->size);
    }
    cp_trace(s, "done", index, NULL, discarded);
    if (s->safe) { ++s->restores; s->pending = 0; }
    LeaveCriticalSection(&s->lock);
    fprintf(stderr, "STEMTEX_CHECKPOINT_RESTORE ok=%d count=%lu discarded=%llu ms=%llu\n",
            s->safe, s->restores, (unsigned long long)discarded, stemtex_checkpoint_milliseconds() - start);
    fflush(stderr);
    return s->safe;
}
