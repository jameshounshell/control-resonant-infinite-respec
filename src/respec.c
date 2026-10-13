/*
 * CONTROL Resonant: Infinite Respec
 *
 * At load time, finds the three places the game removes a Respec Item
 * (Talents, Combat Abilities, Aberrant Abilities) and changes the quantity
 * removed from 1 to 0. The game's separate "holds at least N" check is
 * untouched, so one Respec Item is still required.
 *
 * Every site is validated before anything is written: each pattern must
 * match exactly once in the game's .text, and the item constant it loads
 * must be the respec item hash. If any check fails, nothing is patched.
 *
 * Respec log: each of the three removal calls is also redirected through a
 * small hook that records the Respec Item count before and after the call.
 * The count comes from the game's own HasItem(id, n) check. If the hook
 * can't be installed safely, the count patch still applies and only the
 * per-respec log lines are missing.
 *
 * Settings, optional, in ControlInfiniteRespec.ini next to the DLL:
 *   [settings]
 *   mode=keep-one   ; default: need one Respec Item, never lose it
 *   mode=vanilla    ; game unchanged; respecs are still logged
 *   log=1           ; 0 turns off the per-respec log lines
 *
 * Freestanding: links only against KERNEL32, no C runtime.
 */
#include <windows.h>

#define VERSION        "1.3.0"
#define RESPEC_ITEM_ID 0x6ea9898du /* hash of CURRENCY_RESPEC_ITEMS */
#define WILD           0x100       /* pattern wildcard */

/* Every site starts with the same shape:
 *   mov r9,[reg+X]; mov edx,1; mov r8,[reg+Y]; mov ecx,[rip+disp]; mov byte [rsp+20h],1; call RemoveItem
 * edx is RemoveItem's quantity argument; ecx is the item hash. */
#define COUNT_IMM_OFF 8  /* the 01 of `mov edx,1` (BA 01 00 00 00) */
#define DISP_OFF      21 /* rip-relative disp32 of `mov ecx,[rip+disp]` */
#define DISP_END      25 /* disp is relative to the next instruction */
#define CALL_OFF      30 /* `call RemoveItem` (E8 rel32) */

#define W WILD
static const unsigned short sig_talent[] = {
    0x4C,0x8B,0x8B,0xA8,0x00,0x00,0x00, 0xBA,0x01,0x00,0x00,0x00,
    0x4C,0x8B,0x83,0xA0,0x00,0x00,0x00, 0x8B,0x0D,W,W,W,W, 0xC6,0x44,0x24,0x20,0x01,
};
static const unsigned short sig_aberrant[] = {
    0x4C,0x8B,0x8F,0xA0,0x00,0x00,0x00, 0xBA,0x01,0x00,0x00,0x00,
    0x4C,0x8B,0x87,0x98,0x00,0x00,0x00, 0x8B,0x0D,W,W,W,W, 0xC6,0x44,0x24,0x20,0x01,
};
/* Combat also decrements a cached count after the call (shown on screen as the
 * second number of "1/1"):
 *   call RemoveItem; mov rax,[rbx+A0h]; dec dword [rax+90h] */
static const unsigned short sig_combat[] = {
    0x4C,0x8B,0x8B,0x98,0x00,0x00,0x00, 0xBA,0x01,0x00,0x00,0x00,
    0x4C,0x8B,0x83,0x90,0x00,0x00,0x00, 0x8B,0x0D,W,W,W,W, 0xC6,0x44,0x24,0x20,0x01,
    0xE8,W,W,W,W, 0x48,0x8B,0x83,0xA0,0x00,0x00,0x00, 0xFF,0x88,0x90,0x00,0x00,0x00,
};
/* A HasItem(respec, 1) call: mov edx,1; mov ecx,[rip+disp]; call HasItem */
static const unsigned short sig_hascall[] = {
    0xBA,0x01,0x00,0x00,0x00, 0x8B,0x0D,W,W,W,W, 0xE8,W,W,W,W,
};
#undef W
#define DEC_OFF 42 /* `dec dword [rax+90h]`, 6 bytes */
#define DEC_LEN 6

struct site {
    const char *name;
    const unsigned short *sig;
    unsigned len;
    int has_dec;
    unsigned char *hit;
};

static struct site sites[] = {
    {"Talents",            sig_talent,   sizeof sig_talent / sizeof *sig_talent,     0, 0},
    {"Aberrant Abilities", sig_aberrant, sizeof sig_aberrant / sizeof *sig_aberrant, 0, 0},
    {"Combat Abilities",   sig_combat,   sizeof sig_combat / sizeof *sig_combat,     1, 0},
};
#define NSITES (sizeof sites / sizeof *sites)

/* ---- log: <this dll's path with .log>, rewritten each launch ---- */

static HANDLE logf = INVALID_HANDLE_VALUE;

static void lput(const char *s)
{
    DWORD n = 0, w;
    if (logf == INVALID_HANDLE_VALUE) return;
    while (s[n]) n++;
    WriteFile(logf, s, n, &w, 0);
}

static void lhex(unsigned long long v)
{
    char b[19];
    int i;
    b[0] = '0'; b[1] = 'x';
    for (i = 0; i < 16; i++)
        b[2 + i] = "0123456789abcdef"[(v >> (60 - 4 * i)) & 0xF];
    b[18] = 0;
    lput(b);
}

static void ldec(unsigned v, int width)
{
    char b[11];
    int i = 10;
    b[i] = 0;
    do { b[--i] = (char)('0' + v % 10); v /= 10; width--; } while (v || width > 0);
    lput(b + i);
}

static void ltime(void)
{
    SYSTEMTIME t;
    GetLocalTime(&t);
    ldec(t.wHour, 2); lput(":"); ldec(t.wMinute, 2); lput(":"); ldec(t.wSecond, 2);
}

static int sibling_path(HMODULE self, WCHAR *p, const WCHAR *ext);

static void log_open(HMODULE self)
{
    WCHAR p[MAX_PATH + 8];
    if (!sibling_path(self, p, L".log")) return;
    logf = CreateFileW(p, GENERIC_WRITE, FILE_SHARE_READ, 0, CREATE_ALWAYS,
                       FILE_ATTRIBUTE_NORMAL, 0);
}

/* ---- settings ---- */

enum { MODE_KEEP_ONE, MODE_VANILLA };
static int mode = MODE_KEEP_ONE, log_respecs = 1;
static const char *const mode_names[] = {"keep-one", "vanilla"};

static int weq(const WCHAR *a, const WCHAR *b)
{
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

/* path: this DLL's path; ext is written over its extension (".ini", ".log"). */
static int sibling_path(HMODULE self, WCHAR *p, const WCHAR *ext)
{
    DWORD n = GetModuleFileNameW(self, p, MAX_PATH), dot = n, i;
    if (n == 0 || n >= MAX_PATH) return 0;
    while (dot > 0 && p[dot] != L'.' && p[dot] != L'\\') dot--;
    if (p[dot] != L'.') dot = n;
    for (i = 0; ext[i]; i++) p[dot + i] = ext[i];
    p[dot + i] = 0;
    return 1;
}

static unsigned char *rel32_target(unsigned char *insn_end, unsigned char *rel);
static int ini_found;

static void load_settings(HMODULE self)
{
    WCHAR ini[MAX_PATH + 8], v[32];
    if (!sibling_path(self, ini, L".ini")) return;
    ini_found = GetFileAttributesW(ini) != INVALID_FILE_ATTRIBUTES;
    GetPrivateProfileStringW(L"settings", L"mode", L"keep-one", v, 32, ini);
    if (weq(v, L"vanilla")) mode = MODE_VANILLA;
    log_respecs = GetPrivateProfileIntW(L"settings", L"log", 1, ini) != 0;
}

/* ---- diagnostics for the log header ----
 * Everything a bug report needs, and nothing that identifies the user: no paths,
 * no user or machine names. */

static void lbytes(const unsigned char *p, unsigned n)
{
    char b[4];
    unsigned i;
    b[2] = ' '; b[3] = 0;
    for (i = 0; i < n; i++) {
        b[0] = "0123456789ABCDEF"[p[i] >> 4];
        b[1] = "0123456789ABCDEF"[p[i] & 15];
        if (i + 1 == n) b[2] = 0;
        lput(b);
    }
    b[2] = ' ';
}

/* dir = the game exe's folder with a trailing backslash; returns its length or 0. */
static DWORD game_dir(WCHAR *dir)
{
    DWORD n = GetModuleFileNameW(0, dir, MAX_PATH), cut;
    if (n == 0 || n >= MAX_PATH) return 0;
    for (cut = n; cut > 0 && dir[cut - 1] != L'\\'; cut--) ;
    return cut;
}

static int game_file_exists(const WCHAR *name)
{
    WCHAR dir[MAX_PATH + 64];
    DWORD cut = game_dir(dir), i;
    if (!cut) return 0;
    for (i = 0; name[i] && cut + i < MAX_PATH + 63; i++) dir[cut + i] = name[i];
    dir[cut + i] = 0;
    return GetFileAttributesW(dir) != INVALID_FILE_ATTRIBUTES;
}

static void lwide(const WCHAR *w)
{
    char b[2] = {0, 0};
    for (; *w; w++) { b[0] = (char)*w; lput(b); }
}

static void log_header(unsigned char *base)
{
    SYSTEMTIME t;
    HRSRC r = FindResourceW((HMODULE)base, MAKEINTRESOURCEW(1), (LPCWSTR)RT_VERSION);
    IMAGE_NT_HEADERS64 *nt = (IMAGE_NT_HEADERS64 *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    static const WCHAR *const extra[] = {
        L"winmm.dll", L"version.dll", L"dinput8.dll", L"dxgi.dll", L"d3d11.dll", L"d3d12.dll",
        L"dsound.dll", L"xinput1_3.dll", L"ReShade.ini", L"crmods.txt", 0};
    DWORD i;
    int any = 0;

    GetLocalTime(&t);
    lput("Date: "); ldec(t.wYear, 4); lput("-"); ldec(t.wMonth, 2); lput("-"); ldec(t.wDay, 2);
    lput(" "); ldec(t.wHour, 2); lput(":"); ldec(t.wMinute, 2); lput("\r\n");

    lput("Game: CONTROLResonant.exe ");
    if (r) {
        const unsigned *v = LockResource(LoadResource((HMODULE)base, r));
        DWORD sz = SizeofResource((HMODULE)base, r) / 4;
        for (i = 0; v && i + 4 < sz; i++) {
            if (v[i] == 0xFEEF04BDu) { /* VS_FIXEDFILEINFO */
                ldec(v[i + 2] >> 16, 1); lput("."); ldec(v[i + 2] & 0xFFFF, 1); lput(".");
                ldec(v[i + 3] >> 16, 1); lput("."); ldec(v[i + 3] & 0xFFFF, 1);
                break;
            }
        }
    }
    lput(", PE timestamp "); lhex(nt->FileHeader.TimeDateStamp);
    lput(", image size "); lhex(nt->OptionalHeader.SizeOfImage);
    lput("\r\nStore: ");
    if (game_file_exists(L"steam_api64.dll")) { lput("steam_api64.dll"); any = 1; }
    if (game_file_exists(L"EOSSDK-Win64-Shipping.dll")) { lput(any ? ", EOSSDK" : "EOSSDK"); any = 1; }
    if (!any) lput("neither steam_api64.dll nor EOSSDK found");

    lput("\r\nSystem: ");
    if (ntdll) {
        typedef LONG (WINAPI *rtlgv_fn)(OSVERSIONINFOW *);
        typedef const char *(*winever_fn)(void);
        rtlgv_fn rtlgv = (rtlgv_fn)(void (*)(void))GetProcAddress(ntdll, "RtlGetVersion");
        winever_fn wine = (winever_fn)(void (*)(void))GetProcAddress(ntdll, "wine_get_version");
        OSVERSIONINFOW vi;
        vi.dwOSVersionInfoSize = sizeof vi;
        if (rtlgv && rtlgv(&vi) == 0) {
            lput("Windows "); ldec(vi.dwMajorVersion, 1); lput(".");
            ldec(vi.dwMinorVersion, 1); lput("."); ldec(vi.dwBuildNumber, 1);
        }
        if (wine) { lput(", Wine "); lput(wine()); lput(" (Proton or Wine)"); }
    }

    lput("\r\nOther files in game folder: ");
    any = 0;
    for (i = 0; extra[i]; i++) {
        if (game_file_exists(extra[i])) {
            if (any) lput(", ");
            lwide(extra[i]);
            any = 1;
        }
    }
    if (!any) lput("none of the usual loaders");
    lput("\r\nSettings: ");
    lput(ini_found ? "ControlInfiniteRespec.ini read" : "no ControlInfiniteRespec.ini (defaults)");
    lput(", mode="); lput(mode_names[mode]); lput(log_respecs ? ", log=1\r\n" : ", log=0\r\n");
}

/* Every instruction in .text that loads the Respec Item hash, with the bytes around
 * it and where the next call goes. Enough to write a new signature from a user's log
 * after a game update moves the code. Logged before anything is patched. */
static void log_item_refs(unsigned char *base, unsigned char *text, unsigned long size)
{
    IMAGE_NT_HEADERS64 *nt = (IMAGE_NT_HEADERS64 *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
    IMAGE_SECTION_HEADER *sec = IMAGE_FIRST_SECTION(nt);
    unsigned char *slots[8];
    unsigned nslots = 0, nrefs = 0, i, k;
    unsigned long j;

    for (i = 0; i < nt->FileHeader.NumberOfSections; i++, sec++) {
        unsigned char *p = base + sec->VirtualAddress;
        unsigned long n = sec->Misc.VirtualSize;
        if (!(sec->Characteristics & IMAGE_SCN_MEM_READ) || (sec->Characteristics & IMAGE_SCN_MEM_EXECUTE))
            continue;
        for (j = 0; j + 4 <= n && nslots < 8; j += 4)
            if (*(unsigned *)(p + j) == RESPEC_ITEM_ID) slots[nslots++] = p + j;
    }
    lput("\r\nRespec Item hash found in "); ldec(nslots, 1); lput(" data slot(s):");
    for (i = 0; i < nslots; i++) { lput(" "); lhex((unsigned long long)(slots[i] - base)); }
    lput("\r\nCode that loads it (RVA, next call target, bytes from -16 to +48):\r\n");
    for (j = 16; j + 48 <= size && nrefs < 24; j++) {
        unsigned char *c = text + j, *tgt;
        if (c[0] != 0x8B || (c[1] & 0xC7) != 0x05) continue;  /* mov r32, [rip+disp32] */
        tgt = rel32_target(c + 6, c + 2);
        for (k = 0; k < nslots && slots[k] != tgt; k++) ;
        if (k == nslots) continue;
        nrefs++;
        lput("  "); lhex((unsigned long long)(c - base));
        for (k = 6; k < 40 && c[k] != 0xE8; k++) ;
        lput(" call ");
        if (k < 40) lhex((unsigned long long)(rel32_target(c + k + 5, c + k + 1) - base));
        else lput("(none within 40 bytes)");
        lput("\r\n    ");
        lbytes(c - 16, 64);
        lput("\r\n");
    }
    lput("\r\n");
}

/* ---- respec log hook ---- */

/* Both functions take (item hash, quantity, inventory map, owner); RemoveItem also
 * takes a byte flag on the stack. The game ignores RemoveItem's return value. */
typedef unsigned long long (*remove_fn)(unsigned, unsigned, void *, void *, unsigned char);
typedef unsigned char (*has_fn)(unsigned, unsigned, void *, void *);

static remove_fn real_remove;
static has_fn has_item;

#define COUNT_CAP (1u << 20)

/* Exact count via the game's own "holds at least n" check. */
static unsigned count_of(unsigned id, void *map, void *owner)
{
    unsigned lo = 1, hi = 2, mid;
    if (!has_item(id, 1, map, owner)) return 0;
    while (hi < COUNT_CAP && has_item(id, hi, map, owner)) { lo = hi; hi *= 2; }
    if (hi >= COUNT_CAP) return COUNT_CAP;
    while (hi - lo > 1) {            /* has(lo) true, has(hi) false */
        mid = lo + (hi - lo) / 2;
        if (has_item(id, mid, map, owner)) lo = mid; else hi = mid;
    }
    return lo;
}

static unsigned respecs, items_used;
static unsigned last_owned = 0xFFFFFFFFu;

static void on_remove(unsigned s, unsigned id, unsigned qty, void *map, void *owner,
                      unsigned char flag)
{
    unsigned before, after;
    if (!log_respecs) { real_remove(id, qty, map, owner, flag); return; }
    before = count_of(id, map, owner);
    real_remove(id, qty, map, owner, flag);
    after = count_of(id, map, owner);
    respecs++;
    if (after < before) items_used += before - after;
    last_owned = after;
    ltime();
    lput(" "); lput(sites[s].name);
    lput(" respec: Respec Items before="); ldec(before, 1);
    lput(", removal requested="); ldec(qty, 1);
    lput(", after="); ldec(after, 1);
    lput(after < before ? "  ** ITEM CONSUMED **" : "  (kept)");
    lput("  [session: "); ldec(respecs, 1); lput(respecs == 1 ? " respec, " : " respecs, ");
    ldec(items_used, 1); lput(items_used == 1 ? " item used]\r\n" : " items used]\r\n");
}

/* The respec screens' own "holds at least n Respec Items" checks. Each logs the
 * count the first time it runs and whenever the count changed since the last
 * line. The check's own answer is passed through unchanged. */
/* (defined above; starts unset) */

static unsigned char on_check(unsigned s, unsigned id, unsigned n, void *map, void *owner)
{
    if (log_respecs) {
        unsigned owned = count_of(id, map, owner);
        if (owned != last_owned) {
            last_owned = owned;
            ltime(); lput(" "); lput(sites[s].name);
            lput(" screen: Respec Items owned="); ldec(owned, 1); lput("\r\n");
        }
    }
    return has_item(id, n, map, owner);
}

static unsigned char check0(unsigned i, unsigned n, void *m, void *o) { return on_check(0, i, n, m, o); }
static unsigned char check1(unsigned i, unsigned n, void *m, void *o) { return on_check(1, i, n, m, o); }
static unsigned char check2(unsigned i, unsigned n, void *m, void *o) { return on_check(2, i, n, m, o); }
static unsigned char (*const checks[NSITES])(unsigned, unsigned, void *, void *) = {check0, check1, check2};

static void hook0(unsigned i, unsigned q, void *m, void *o, unsigned char f) { on_remove(0, i, q, m, o, f); }
static void hook1(unsigned i, unsigned q, void *m, void *o, unsigned char f) { on_remove(1, i, q, m, o, f); }
static void hook2(unsigned i, unsigned q, void *m, void *o, unsigned char f) { on_remove(2, i, q, m, o, f); }
static void (*const hooks[NSITES])(unsigned, unsigned, void *, void *, unsigned char) = {hook0, hook1, hook2};

/* ---- scan ---- */

static int match(const unsigned char *p, const unsigned short *sig, unsigned len)
{
    unsigned i;
    for (i = 0; i < len; i++)
        if (sig[i] != WILD && p[i] != sig[i]) return 0;
    return 1;
}

/* Returns the number of matches (stops counting at 2); *out = first match. */
static int scan(unsigned char *b, unsigned long size, const struct site *s,
                unsigned char **out)
{
    unsigned long i;
    int n = 0;
    if (size < s->len) return 0;
    for (i = 0; i <= size - s->len; i++) {
        if (b[i] == s->sig[0] && match(b + i, s->sig, s->len)) {
            if (n++ == 0) *out = b + i;
            else return 2;
        }
    }
    return n;
}

static int find_text(unsigned char *base, unsigned char **text, unsigned long *size,
                     unsigned long *image_size)
{
    IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)base;
    IMAGE_NT_HEADERS64 *nt;
    IMAGE_SECTION_HEADER *sec;
    unsigned i;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return 0;
    nt = (IMAGE_NT_HEADERS64 *)(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return 0;
    *image_size = nt->OptionalHeader.SizeOfImage;
    sec = IMAGE_FIRST_SECTION(nt);
    for (i = 0; i < nt->FileHeader.NumberOfSections; i++, sec++) {
        const char *nm = (const char *)sec->Name;
        if (nm[0] == '.' && nm[1] == 't' && nm[2] == 'e' && nm[3] == 'x' &&
            nm[4] == 't' && nm[5] == 0) {
            *text = base + sec->VirtualAddress;
            *size = sec->Misc.VirtualSize;
            return 1;
        }
    }
    return 0;
}

static unsigned char *rel32_target(unsigned char *insn_end, unsigned char *rel)
{
    return insn_end + *(int *)rel;
}

static unsigned item_at(unsigned char *disp, unsigned char *insn_end)
{
    return *(unsigned *)rel32_target(insn_end, disp);
}

/* The one HasItem every respec-item "holds at least 1" check calls, or 0. */
static unsigned char *find_has_item(unsigned char *text, unsigned long size,
                                    unsigned char *remove)
{
    struct site s = {"HasItem", sig_hascall, sizeof sig_hascall / sizeof *sig_hascall, 0, 0};
    unsigned char *found = 0, *t;
    unsigned long i;
    for (i = 0; i + s.len <= size; i++) {
        if (text[i] != 0xBA || !match(text + i, s.sig, s.len)) continue;
        if (item_at(text + i + 7, text + i + 11) != RESPEC_ITEM_ID) continue;
        t = rel32_target(text + i + 16, text + i + 12);
        if (t == remove) continue;
        if (found && found != t) return 0; /* disagreement: refuse to guess */
        found = t;
    }
    return found;
}

/* A page within rel32 reach of the game's code, for the hook trampolines. */
static unsigned char *alloc_near(unsigned char *base, unsigned long image_size)
{
    unsigned long long a = ((unsigned long long)base + image_size + 0xFFFF) & ~0xFFFFull;
    unsigned long long end = (unsigned long long)base + 0x70000000ull;
    for (; a < end; a += 0x10000) {
        void *p = VirtualAlloc((void *)a, 0x1000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        if (p) return p;
    }
    return 0;
}

static int write_code(unsigned char *at, const unsigned char *bytes, unsigned len)
{
    DWORD old, tmp;
    unsigned i;
    if (!VirtualProtect(at, len, PAGE_EXECUTE_READWRITE, &old)) return 0;
    for (i = 0; i < len; i++) at[i] = bytes[i];
    VirtualProtect(at, len, old, &tmp);
    FlushInstructionCache(GetCurrentProcess(), at, len);
    for (i = 0; i < len; i++)        /* read back what is actually there now */
        if (at[i] != bytes[i]) return 0;
    return 1;
}

static void put_tramp(unsigned char *t, unsigned long long target)
{
    unsigned k;
    t[0] = 0x48; t[1] = 0xB8;            /* mov rax, imm64; jmp rax */
    for (k = 0; k < 8; k++) t[2 + k] = (unsigned char)(target >> (8 * k));
    t[10] = 0xFF; t[11] = 0xE0;
}

static int rel_ok(long long r) { return r <= 0x7FFFFFFFll && r >= -0x80000000ll; }

#define MAX_HAS_SITES 16

/* Redirects each `call RemoveItem` through a trampoline to its hook.
 * Validates everything first; returns 1 only if all three were installed. */
static int install_hooks(unsigned char *base, unsigned long image_size,
                         unsigned char *text, unsigned long size)
{
    unsigned char *remove = 0, *page, *tramp;
    unsigned i;
    long long rel[NSITES];
    unsigned char *has_sites[MAX_HAS_SITES];
    unsigned has_screen[MAX_HAS_SITES];
    unsigned nhas;
    DWORD old;

    for (i = 0; i < NSITES; i++) {
        unsigned char *call = sites[i].hit + CALL_OFF, *t;
        if (call[0] != 0xE8) { lput("Respec log: call instruction not where expected\r\n"); return 0; }
        t = rel32_target(call + 5, call + 1);
        if (remove && t != remove) { lput("Respec log: sites call different functions\r\n"); return 0; }
        remove = t;
    }
    has_item = (has_fn)find_has_item(text, size, remove);
    if (!has_item) { lput("Respec log: could not identify HasItem\r\n"); return 0; }
    real_remove = (remove_fn)remove;

    page = alloc_near(base, image_size);
    if (!page) { lput("Respec log: no memory near the game's code\r\n"); return 0; }
    for (i = 0; i < NSITES; i++) {
        tramp = page + 16 * i;
        put_tramp(tramp, (unsigned long long)hooks[i]);
        rel[i] = (long long)(tramp - (sites[i].hit + CALL_OFF + 5));
        if (!rel_ok(rel[i])) {
            lput("Respec log: trampoline out of reach\r\n");
            VirtualFree(page, 0, MEM_RELEASE);
            return 0;
        }
    }
    nhas = 0;
    if (log_respecs) {
        /* Every `mov ecx,[respec item]; call HasItem` in the game, attributed to a
         * screen by which of the three per-screen item constants it loads. */
        unsigned char *slot[NSITES];
        unsigned long j;
        unsigned k;
        for (i = 0; i < NSITES; i++) {
            slot[i] = rel32_target(sites[i].hit + DISP_END, sites[i].hit + DISP_OFF);
            put_tramp(page + 16 * (NSITES + i), (unsigned long long)checks[i]);
        }
        for (j = 0; j + 11 <= size; j++) {
            unsigned char *c = text + j, *sl;
            if (c[0] != 0x8B || c[1] != 0x0D || c[6] != 0xE8) continue;
            if (rel32_target(c + 11, c + 7) != (unsigned char *)has_item) continue;
            sl = rel32_target(c + 6, c + 2);
            for (k = 0; k < NSITES && slot[k] != sl; k++) ;
            if (k == NSITES) continue;
            if (nhas == MAX_HAS_SITES ||
                !rel_ok((long long)(page + 16 * (NSITES + k) - (c + 11)))) {
                lput("Respec checks: too many or out of reach, not hooked\r\n");
                nhas = 0;
                break;
            }
            has_sites[nhas] = c + 6;
            has_screen[nhas++] = k;
        }
    }
    VirtualProtect(page, 0x1000, PAGE_EXECUTE_READ, &old);
    FlushInstructionCache(GetCurrentProcess(), page, 0x1000);

    for (i = 0; i < nhas; i++) {
        int r = (int)(page + 16 * (NSITES + has_screen[i]) - (has_sites[i] + 5));
        if (!write_code(has_sites[i] + 1, (const unsigned char *)&r, 4)) {
            lput("Respec checks: a redirect was refused\r\n");
            return 0;
        }
    }
    if (nhas) {
        lput("Respec checks hooked: "); ldec(nhas, 1);
        lput(" (count logged when a screen checks it)\r\n");
    }

    for (i = 0; i < NSITES; i++) {
        int r = (int)rel[i];
        if (!write_code(sites[i].hit + CALL_OFF + 1, (const unsigned char *)&r, 4)) {
            lput("Respec log: VirtualProtect refused the call redirect\r\n");
            return 0;
        }
    }
    lput("Respec log: HasItem at RVA "); lhex((unsigned long long)((unsigned char *)has_item - base));
    lput(", RemoveItem at RVA "); lhex((unsigned long long)(remove - base));
    lput("\r\n");
    return 1;
}

static int run(void)
{
    static const unsigned char zero = 0x00;
    static const unsigned char nops[DEC_LEN] = {0x90, 0x90, 0x90, 0x90, 0x90, 0x90};
    unsigned char *base = (unsigned char *)GetModuleHandleW(0), *text;
    unsigned long size, image_size;
    unsigned i;
    int ok = 1, logging;

    lput("CONTROL Resonant Infinite Respec " VERSION "\r\n");
    if (!base || !find_text(base, &text, &size, &image_size)) {
        lput("FAIL - could not locate the game's code section. Nothing patched.\r\n");
        return 0;
    }
    log_header(base);

    log_item_refs(base, text, size);

    /* Validate every site before writing anything. */
    for (i = 0; i < NSITES; i++) {
        struct site *s = &sites[i];
        int n = scan(text, size, s, &s->hit);
        lput(s->name);
        if (n == 0) { lput(": FAIL - pattern not found\r\n"); ok = 0; continue; }
        if (n > 1)  { lput(": FAIL - pattern matched more than once\r\n"); ok = 0; continue; }
        {
            unsigned id = item_at(s->hit + DISP_OFF, s->hit + DISP_END);
            if (id != RESPEC_ITEM_ID) {
                lput(": FAIL - call site does not load the Respec Item (id ");
                lhex(id);
                lput(")\r\n");
                ok = 0;
                continue;
            }
        }
        lput(": found at RVA ");
        lhex((unsigned long long)(s->hit - base));
        lput("\r\n");
    }
    if (!ok) {
        lput("\r\nNothing patched: this game build doesn't match. The game runs unmodified.\r\n");
        return 0;
    }

    for (i = 0; mode != MODE_VANILLA && i < NSITES; i++) {
        struct site *s = &sites[i];
        if (!write_code(s->hit + COUNT_IMM_OFF, &zero, 1) ||
            (s->has_dec && !write_code(s->hit + DEC_OFF, nops, DEC_LEN))) {
            lput(s->name);
            lput(": FAIL - VirtualProtect refused the write\r\n");
            ok = 0;
        }
    }
    logging = ok && install_hooks(base, image_size, text, size);
    if (!ok) lput("\r\nPARTIAL - see failures above.\r\n");
    else if (mode == MODE_VANILLA) lput("\r\nVANILLA - game unchanged; respecs are logged only.\r\n");
    else lput("\r\nACTIVE - respecs no longer consume the Respec Item (you still need one).\r\n");
    lput(logging ? "Each respec is logged below with the item count before and after.\r\n\r\n"
                 : "Per-respec logging is off (see above); the respec patch is unaffected.\r\n");
    return logging;
}

BOOL WINAPI DllMain(HINSTANCE self, DWORD reason, LPVOID reserved)
{
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(self);
        load_settings(self);
        log_open(self);
        /* The log stays open for the hooks while they are installed. */
        if (!run() && logf != INVALID_HANDLE_VALUE) {
            CloseHandle(logf);
            logf = INVALID_HANDLE_VALUE;
        }
    }
    return TRUE;
}
