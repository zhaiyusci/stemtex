/* Native checkpoint regression, independent of TeX Live and the GUI.
   Run scripts/test-luatex-checkpoint-memory.ps1. Each scenario gets a fresh
   process because an unsupported resource deliberately invalidates recovery. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <io.h>
#include <fcntl.h>
#include <errno.h>
#include "stemtex_checkpoint.h"

#define OWNED __attribute__((section(".cpcode"), noinline))
#define RUNTIME __attribute__((noinline, optimize("no-optimize-sibling-calls")))
#define CHECK(condition, code) do { if (!(condition)) { \
    fprintf(stderr, "CHECKPOINT_MEMORY_FAIL line=%d check=%s reason=%s\n", \
            __LINE__, #condition, stemtex_checkpoint_last_error()); return code; \
} } while (0)

static char *baseline, *runtime_block;
static int number;
static FILE *input, *output;

/* MSVCRT tmpfile uses the drive root on some Windows versions. A workspace
   sandbox can reject that location. Skip only when the unwrapped CRT reports
   the exact same access-denied failure; every other failure remains fatal. */
static int tmpfile_access_denied(int wrapped_errno, DWORD wrapped_error) {
    FILE *(__cdecl *native_tmpfile)(void);
    FARPROC entry = GetProcAddress(GetModuleHandleW(L"msvcrt.dll"), "tmpfile");
    FILE *file;
    int native_errno;
    DWORD native_error;
    if (!entry) return 0;
    memcpy(&native_tmpfile, &entry, sizeof(entry));
    errno = 0; SetLastError(0);
    file = native_tmpfile();
    native_errno = errno; native_error = GetLastError();
    if (file) { fclose(file); return 0; }
    if (wrapped_errno != EACCES || native_errno != EACCES ||
        wrapped_error != ERROR_ACCESS_DENIED || native_error != ERROR_ACCESS_DENIED) {
        fprintf(stderr, "Unexpected tmpfile failure: wrapped errno=%d Win32=%lu, native errno=%d Win32=%lu\n",
                wrapped_errno, wrapped_error, native_errno, native_error);
        return 0;
    }
    puts("CHECKPOINT_MEMORY_SKIP feature=tmpfile reason=wrapped-and-native-CRT-access-denied errno=13 Win32=5");
    return 1;
}

/* This function is outside the registered allocation domain. In particular,
   prevent its final API call from tail-calling with the owned caller's PC. */
RUNTIME static void runtime_resources(void) {
    void *p = VirtualAlloc(NULL, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (p) VirtualFree(p, 0, MEM_RELEASE);
    runtime_block = malloc(20);
    if (runtime_block) strcpy(runtime_block, "runtime-kept");
}

OWNED static int memory_probe(void) {
    int i;
    baseline = malloc(32);
    CHECK(baseline, 1);
    strcpy(baseline, "baseline"); number = 17;
    input = fopen("input.txt", "rb");
    output = fopen("output.dvi", "wb+");
    CHECK(input && output && fgetc(input) == 'a', 2);
    CHECK(fputs("BASE", output) >= 0, 3);
    stemtex_checkpoint_set_output(output);
    CHECK(stemtex_checkpoint_begin(), 4);
    for (i = 0; i < 5; ++i) {
        char *prior = baseline;
        char *extra = calloc(1000, 1);
        char *text = strdup("request allocation");
        FILE *temporary;
        int temporary_errno;
        DWORD temporary_error;
        errno = 0; SetLastError(0);
        temporary = tmpfile();
        temporary_errno = errno; temporary_error = GetLastError();
        FILE *descriptor_file = _fdopen(_open("input.txt", _O_RDONLY | _O_BINARY), "rb");
        HANDLE temporary_handle = INVALID_HANDLE_VALUE, descriptor_handle;
        DWORD flags;
        CHECK(extra && text && descriptor_file, 5);
        CHECK(temporary || tmpfile_access_denied(temporary_errno, temporary_error), 17);
        if (temporary) temporary_handle = (HANDLE)_get_osfhandle(_fileno(temporary));
        descriptor_handle = (HANDLE)_get_osfhandle(_fileno(descriptor_file));
        memset(extra, 'x', 1000);
        baseline = realloc(baseline, 4096);
        CHECK(baseline && baseline != prior, 6);
        strcpy(baseline, "mutated"); number = 42;
        CHECK(fclose(input) == 0 && fputs("BADBAD", output) >= 0, 7);
        runtime_resources();
        CHECK(runtime_block, 8);
        CHECK(stemtex_checkpoint_restore(), 9);
        CHECK(baseline == prior && !strcmp(baseline, "baseline") && number == 17, 10);
        CHECK(fgetc(input) == 'b' && _ftelli64(output) == 4, 11);
        CHECK(_filelengthi64(_fileno(output)) == 4, 12);
        CHECK(!strcmp(runtime_block, "runtime-kept"), 13);
        if (temporary)
            CHECK(!GetHandleInformation(temporary_handle, &flags) && GetLastError() == ERROR_INVALID_HANDLE, 14);
        CHECK(!GetHandleInformation(descriptor_handle, &flags) && GetLastError() == ERROR_INVALID_HANDLE, 15);
        free(runtime_block); runtime_block = NULL;
        free(baseline);
        CHECK(stemtex_checkpoint_restore() && !strcmp(baseline, "baseline"), 16);
    }
    puts("CHECKPOINT_MEMORY_OK restores=10 heap=stable globals=restored input=rewound dvi=truncated runtime=retained");
    return 0;
}

static const char *resource_names[] = {
    "VirtualAlloc", "VirtualFree", "MapViewOfFile", "UnmapViewOfFile",
    "LoadLibraryA", "LoadLibraryW", "LoadLibraryExA", "LoadLibraryExW",
    "FreeLibrary", "CreateFileMappingA", "CreateFileMappingW", "plain-file"
};

OWNED static int resource_probe(int operation) {
    void *memory = VirtualAlloc(NULL, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    HANDLE mapping = CreateFileMappingA(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, 4096, NULL);
    HMODULE module = LoadLibraryW(L"kernel32.dll");
    void *view = mapping ? MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 4096) : NULL;
    unsigned long long started = stemtex_checkpoint_milliseconds();
    int ok = 1;
    CHECK(memory && mapping && module && view, 20);
    baseline = malloc(20);
    CHECK(baseline, 21);
    strcpy(baseline, "baseline"); number = 17;
    CHECK(stemtex_checkpoint_begin(), 22);
    runtime_resources(); number = 32;
    CHECK(stemtex_checkpoint_restore() && number == 17, 23);
    CHECK(runtime_block && !strcmp(runtime_block, "runtime-kept"), 24);
    free(runtime_block); runtime_block = NULL;
    switch (operation) {
    case 0: memory = VirtualAlloc(NULL, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE); ok = memory != NULL; break;
    case 1: ok = VirtualFree(memory, 0, MEM_RELEASE); break;
    case 2: view = MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 4096); ok = view != NULL; break;
    case 3: ok = UnmapViewOfFile(view); break;
    case 4: ok = LoadLibraryA("kernel32.dll") != NULL; break;
    case 5: ok = LoadLibraryW(L"kernel32.dll") != NULL; break;
    case 6: ok = LoadLibraryExA("kernel32.dll", NULL, 0) != NULL; break;
    case 7: ok = LoadLibraryExW(L"kernel32.dll", NULL, 0) != NULL; break;
    case 8: ok = FreeLibrary(module); break;
    case 9: ok = CreateFileMappingA(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, 4096, NULL) != NULL; break;
    case 10: ok = CreateFileMappingW(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, 4096, NULL) != NULL; break;
    case 11: {
        FILE *file = fopen("input.txt", "rb");
        CHECK(file && fgetc(file) == 'a' && fclose(file) == 0, 25);
        CHECK(stemtex_checkpoint_restore(), 26);
        puts("CHECKPOINT_RESOURCE_OK case=plain-file recovery=allowed");
        return 0;
    }
    default: return 27;
    }
    CHECK(ok, 28);
    CHECK(!stemtex_checkpoint_restore(), 29);
    CHECK(strstr(stemtex_checkpoint_last_error(), resource_names[operation]), 30);
    CHECK(strstr(stemtex_checkpoint_last_error(), "cannot be rolled back"), 31);
    CHECK(stemtex_checkpoint_milliseconds() >= started, 32);
    printf("CHECKPOINT_RESOURCE_OK case=%s recovery=refused\n", resource_names[operation]);
    return 0;
}

int main(int argc, char **argv) {
    unsigned char *image = (unsigned char *)GetModuleHandleW(NULL);
    IMAGE_NT_HEADERS *pe = (IMAGE_NT_HEADERS *)(image + ((IMAGE_DOS_HEADER *)image)->e_lfanew);
    IMAGE_SECTION_HEADER *section = IMAGE_FIRST_SECTION(pe);
    unsigned i;
    FILE *fixture;
    CHECK(argc == 2 && stemtex_checkpoint_enabled(), 40);
    for (i = 0; i < pe->FileHeader.NumberOfSections; ++i)
        if (!memcmp(section[i].Name, ".cpcode", 7))
            CHECK(stemtex_checkpoint_add_code_region(image + section[i].VirtualAddress, section[i].Misc.VirtualSize), 41);
    CHECK(stemtex_checkpoint_add_region(&baseline, sizeof(baseline)), 42);
    CHECK(stemtex_checkpoint_add_region(&number, sizeof(number)), 43);
    CHECK(stemtex_checkpoint_add_region(&input, sizeof(input)), 44);
    CHECK(stemtex_checkpoint_add_region(&output, sizeof(output)), 45);
    fixture = fopen("input.txt", "wb");
    CHECK(fixture && fputs("abcdef", fixture) >= 0 && fclose(fixture) == 0, 46);
    if (!strcmp(argv[1], "memory")) return memory_probe();
    for (i = 0; i < sizeof(resource_names) / sizeof(*resource_names); ++i)
        if (!strcmp(argv[1], resource_names[i])) return resource_probe((int)i);
    fprintf(stderr, "Unknown scenario: %s\n", argv[1]);
    return 47;
}
