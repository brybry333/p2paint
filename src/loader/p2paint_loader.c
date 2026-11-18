/* libp2paint.so — LD_PRELOAD loader for the p2paint client mod, built as a shader library (CS:S, Linux x64).
 *
 * CS:S never loads mod shader libraries: CShaderSystem::LoadAllShaderDLLs skips the
 * game_shader_generic*.so search when the game dir is a Valve game ("cstrike", "hl2", "tf", ...).
 * This loader asks the game's own shader system to load ours, once, after the stock shader
 * libraries are up: CShaderSystem::LoadShaderDLL(fullPath, pathID = NULL, bModShaderDLL = true).
 *
 * No game code is patched. The only non-exported thing used is that one LoadShaderDLL method,
 * found at runtime: ShaderSystem002 (exported) is the IShaderSystem part of CShaderSystem, the
 * primary vtable sits 8 bytes before it, and LoadShaderDLL is the single method in that vtable
 * whose code references the "ShaderDLL004" string. If anything doesn't match, it logs and does
 * nothing (the game runs normally without the paint mod).
 *
 * Log: ~/.cache/p2paint/loader.log (the game's runtime container keeps ~/.cache writable).
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <link.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#define EXPORT __attribute__((visibility("default")))
#define SHADER_LIB "game_shader_generic_p2paint.so"

typedef void *(*CreateInterfaceFn)(const char *name, int *ret);
typedef char (*LoadShaderDLLFn)(void *self, const char *fullPath, const char *pathID, char modShaderDLL);

static void *(*real_dlopen)(const char *, int);
static int g_done;      /* 1 once we loaded our library or gave up */
static int g_busy;      /* re-entrancy guard: LoadShaderDLL itself calls dlopen */
static FILE *g_log;

static void logmsg(const char *fmt, ...)
{
    if (!g_log)
    {
        const char *home = getenv("HOME");
        char path[4096];
        if (!home) return;
        snprintf(path, sizeof(path), "%s/.cache/p2paint", home);
        mkdir(path, 0755);
        snprintf(path, sizeof(path), "%s/.cache/p2paint/loader.log", home);
        g_log = fopen(path, "w");
        if (!g_log) return;
        setvbuf(g_log, NULL, _IOLBF, 0);
    }
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    fprintf(g_log, "[%5ld.%03ld] ", (long)ts.tv_sec % 100000, ts.tv_nsec / 1000000);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(g_log, fmt, ap);
    va_end(ap);
    fputc('\n', g_log);
}

/* ---- loaded-module lookup ----------------------------------------------------------------- */

#define MAX_SEGS 16
struct module
{
    const char *suffix;          /* in: path suffix to match, e.g. "/materialsystem.so" */
    char path[4096];             /* out */
    int nseg;
    struct { uintptr_t lo, hi; int exec; } seg[MAX_SEGS];
    int found;
};

static int module_cb(struct dl_phdr_info *info, size_t size, void *data)
{
    (void)size;
    struct module *m = data;
    size_t n = strlen(info->dlpi_name), s = strlen(m->suffix);
    if (n < s || strcmp(info->dlpi_name + n - s, m->suffix)) return 0;
    snprintf(m->path, sizeof(m->path), "%s", info->dlpi_name);
    m->nseg = 0;
    for (int i = 0; i < info->dlpi_phnum && m->nseg < MAX_SEGS; i++)
    {
        const ElfW(Phdr) *ph = &info->dlpi_phdr[i];
        if (ph->p_type != PT_LOAD) continue;
        m->seg[m->nseg].lo = info->dlpi_addr + ph->p_vaddr;
        m->seg[m->nseg].hi = info->dlpi_addr + ph->p_vaddr + ph->p_memsz;
        m->seg[m->nseg].exec = (ph->p_flags & PF_X) != 0;
        m->nseg++;
    }
    m->found = 1;
    return 1;
}

static int find_module(struct module *m)
{
    m->found = 0;
    dl_iterate_phdr(module_cb, m);
    return m->found;
}

static int in_module(const struct module *m, uintptr_t a, int want_exec)
{
    for (int i = 0; i < m->nseg; i++)
        if (a >= m->seg[i].lo && a < m->seg[i].hi && (want_exec < 0 || m->seg[i].exec == want_exec))
            return 1;
    return 0;
}

static uintptr_t find_string(const struct module *m, const char *s)
{
    size_t len = strlen(s) + 1;
    for (int i = 0; i < m->nseg; i++)
    {
        if (m->seg[i].exec) continue;
        const char *p = memmem((const void *)m->seg[i].lo, m->seg[i].hi - m->seg[i].lo, s, len);
        if (p) return (uintptr_t)p;
    }
    return 0;
}

/* does the function at f load the address 'target' with a RIP-relative lea? Scans until the next
 * function's "push rbp; mov rbp, rsp" (Valve builds keep frame pointers), at most 4 KB. */
static int references(const struct module *m, uintptr_t f, uintptr_t target)
{
    const unsigned char *c = (const unsigned char *)f;
    for (int i = 0; i < 0x1000; i++)
    {
        if (!in_module(m, f + i + 7, 1)) return 0;
        if (i > 0 && c[i] == 0x55 && c[i + 1] == 0x48 && c[i + 2] == 0x89 && c[i + 3] == 0xe5) return 0;
        if ((c[i] == 0x48 || c[i] == 0x4c) && c[i + 1] == 0x8d && (c[i + 2] & 0xc7) == 0x05)
        {
            int32_t disp;
            memcpy(&disp, c + i + 3, 4);
            if (f + i + 7 + (intptr_t)disp == target) return 1;
        }
    }
    return 0;
}

/* ---- the load ----------------------------------------------------------------------------- */

static void load_our_shaders(void)
{
    g_done = 1;

    struct module ms = { .suffix = "/materialsystem.so" };
    if (!find_module(&ms)) { logmsg("materialsystem.so not loaded - giving up"); return; }

    void *h = real_dlopen(ms.path, RTLD_NOW | RTLD_NOLOAD);
    CreateInterfaceFn ci = h ? (CreateInterfaceFn)dlsym(h, "CreateInterface") : NULL;
    if (h) dlclose(h);
    void *iface = ci ? ci("ShaderSystem002", NULL) : NULL;
    if (!iface) { logmsg("ShaderSystem002 not available - giving up"); return; }

    uintptr_t str = find_string(&ms, "ShaderDLL004");
    if (!str) { logmsg("\"ShaderDLL004\" string not found in materialsystem - giving up"); return; }

    void *self = (char *)iface - 8;
    void **vtbl = *(void ***)self;
    if (!in_module(&ms, (uintptr_t)vtbl, 0)) { logmsg("unexpected CShaderSystem layout (vtable %p) - giving up", (void *)vtbl); return; }

    int slot = -1, matches = 0;
    for (int i = 0; i < 64; i++)
    {
        uintptr_t f = (uintptr_t)vtbl[i];
        if (!in_module(&ms, f, 1)) break;   /* past the end of this vtable */
        if (references(&ms, f, str)) { slot = i; matches++; }
    }
    if (matches != 1) { logmsg("LoadShaderDLL not identified (%d candidates) - giving up", matches); return; }

    Dl_info me;
    char path[4096];
    if (!dladdr((void *)load_our_shaders, &me) || !me.dli_fname) { logmsg("dladdr failed - giving up"); return; }
    snprintf(path, sizeof(path), "%s", me.dli_fname);
    char *slash = strrchr(path, '/');
    snprintf(slash ? slash + 1 : path, sizeof(path) - (slash ? (size_t)(slash + 1 - path) : 0), "%s", SHADER_LIB);

    logmsg("LoadShaderDLL = vtable slot %d (+%#x), loading %s", slot, slot * 8, path);
#ifdef P2PAINT_PROBE_ONLY
    return;
#endif
    g_busy = 1;
    char ok = ((LoadShaderDLLFn)vtbl[slot])(self, path, NULL, 1);
    g_busy = 0;
    logmsg("LoadShaderDLL returned %d", ok);
    if (!ok)
    {
        /* say why: the usual cause is the library itself not loading (missing symbol/dependency) */
        void *t = real_dlopen(path, RTLD_NOW | RTLD_LOCAL);
        if (!t) logmsg("  dlopen of it fails: %s", dlerror());
        else { logmsg("  dlopen of it works, so the shader system rejected it after loading"); dlclose(t); }
        return;
    }
    /* Pin it: at quit the shader system unloads mod shader libraries BEFORE the material system frees its
     * materials and textures and VGUI drops its panels, and ours are still pointed at by those (shaders,
     * texture regenerators, the menu / HUD panels) - unloaded code there crashed every quit. NODELETE plus
     * this never-closed reference keep it mapped (no destructors) until the process exits, like stdshader. */
    void *lib = real_dlopen(path, RTLD_NOW | RTLD_NOLOAD | RTLD_NODELETE);
    logmsg("%s", lib ? "pinned in memory until exit" : "pinning FAILED");
    int (*init)(void) = lib ? (int (*)(void))dlsym(lib, "P2Paint_Init") : NULL;
    logmsg("P2Paint_Init %s", init ? (init() ? "OK" : "FAILED (game interfaces missing)") : "not found");
    const char *(*engine_log)(void) = lib ? (const char *(*)(void))dlsym(lib, "P2Paint_EngineLog") : NULL;
    if (engine_log) logmsg("engine code: %s", engine_log());
}

EXPORT void *dlopen(const char *file, int mode)
{
    if (!real_dlopen) real_dlopen = (void *(*)(const char *, int))dlsym(RTLD_NEXT, "dlopen");
    void *h = real_dlopen(file, mode);
    if (file && !g_busy)
    {
        const char *base = strrchr(file, '/');
        base = base ? base + 1 : file;
        if (!g_done) logmsg("dlopen %s%s", base, h ? "" : " (failed)");
        /* the engine loads client.so after the material system has loaded its stock shaders */
        if (!g_done && h && !strcmp(base, "client.so"))
        {
            struct module std = { .suffix = "/stdshader_dx9.so" };
            if (find_module(&std)) load_our_shaders();
            else logmsg("client.so loaded but stdshader_dx9.so isn't - waiting");
        }
    }
    return h;
}

__attribute__((constructor)) static void loader_init(void)
{
    logmsg("p2paint loader in %s", program_invocation_short_name);
}
