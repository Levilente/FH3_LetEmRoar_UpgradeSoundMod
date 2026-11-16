// EngineSoundTiers 1.0: FH6-style engine sound that follows the car's intake and exhaust upgrade level, for FH3.
//
// How it works (see RECON-allcars.md):
//  - Every car sound bank (PG_<engine>.bank / GS_<engine>.bank) has one car event with nested sub-events; the one that takes the
//    ExhaustUpgradeLevel parameter is the exhaust, the one that takes EngineUpgradeLevel is the engine bay (intake + mechanical).
//    tools/tiers_table.py reads this from the banks into bank_tiers.inc: which sample of which bank is exhaust / engine bay.
//  - The game plays every sample of an event through Studio's sound-instrument player (game+0x29823B0) into the channel group of
//    the sample's track. We recognise the sample (bank + index, learnt at System::createSound) and put FMOD's own effects on that
//    track's channel group: a three-band EQ (louder, more presence, more top end) and, on the higher levels, a little distortion
//    (rasp). How much depends on the level: the exhaust tracks follow the car's exhaust part, the engine-bay tracks its intake.
//  - The level is the driven car's part: Car+0x7120 (exhaust) and Car+0x7124 (intake) hold the part id (engine * 1000 + n,
//    n = 0 for the stock part, 1..3 Street / Sport / Race). The Car comes from the HUD speedometer controller, as in the platform.
// Nothing of the game's files is changed, and no sample is replaced: the car keeps its own sound.
#include "fh3mod.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <new>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <vector>
#include <unordered_map>
#include <unordered_set>

namespace {

struct BankTier { uint32_t off, len; uint64_t exh[8], eng[8], tur[8], bov[8]; const char* name; };
struct EngineMap { const char* media; const char* engine; const char* granular; };
#include "bank_tiers.inc"
#include "engine_map.inc"
struct BankBf { uint32_t off, len; uint8_t cnt; uint16_t idx[16]; };
#include "bank_backfires.inc"   // the game's backfire samples per bank (the same table the Anti-Lag mod uses)
constexpr int kBanks = sizeof kBankTier / sizeof kBankTier[0];

const FH3ModApi* g_api = nullptr;
FH3Mod* g_mod          = nullptr;
char* g_game           = nullptr;
HANDLE g_thread        = nullptr;

// ---- game addresses (RVA) -------------------------------------------------------------------------------------------
constexpr uintptr_t kCreateSound = 0x27BE3F0;   // FMOD::System::createSound(sys, name, mode, exinfo, sound**)
constexpr uintptr_t kStudioPlay  = 0x29823B0;   // Studio's sound-instrument player (snd, sys, group, paused, a5, channel**)
constexpr uintptr_t kPlaySound   = 0x27BFA40;   // FMOD::System::playSound(sys, sound, group, paused, channel**)
constexpr uintptr_t kSetVolume = 0x27C3450, kSetPitch = 0x27C3160;   // ChannelControl::setVolume / setPitch (as the Anti-Lag mod uses them)
constexpr uintptr_t kSynthRead   = 0x1948A30;   // the read callback of the game's "PG Engine Synth" DSP (granular engines)
constexpr uintptr_t kCreateDspDesc = 0x27BDF70; // FMOD::System::createDSP(sys, const FMOD_DSP_DESCRIPTION*, dsp**)
constexpr uintptr_t kCreateDsp   = 0x27BE190;   // FMOD::System::createDSPByType(sys, type, dsp**)
constexpr uintptr_t kAddDsp      = 0x27C1910;   // FMOD::ChannelControl::addDSP(cc, index, dsp)
constexpr uintptr_t kDspRelease  = 0x27C5710;   // FMOD::DSP::release(dsp) (validates the handle: a stale one returns an error)
constexpr uintptr_t kGetDsp      = 0x27C1C30;   // FMOD::ChannelControl::getDSP(cc, index, dsp**)
constexpr uintptr_t kSetFloat    = 0x27C5F10;   // FMOD::DSP::setParameterFloat(dsp, index, value)
constexpr uintptr_t kHud         = 0xB26690;    // HUD speedometer controller update: the player's Car at +0x120
constexpr size_t kCarExhaust = 0x7120, kCarIntake = 0x7124, kCarName = 0x118;

constexpr int kDspEcho = 6, kDspDistortion = 9, kDspThreeEq = 28;   // FMOD_DSP_TYPE of FMOD 1.08 (the game's: 1.08.04)

struct Sig { uintptr_t rva; unsigned char b[6]; };
const Sig kSigs[] = {
    {kCreateSound, {0x40, 0x53, 0x55, 0x56, 0x57, 0x41}}, {kStudioPlay, {0x48, 0x89, 0x5c, 0x24, 0x10, 0x48}},
    {kPlaySound, {0x40, 0x53, 0x55, 0x56, 0x57, 0x41}}, {kSynthRead, {0x4C, 0x8B, 0xDC, 0x57, 0x41, 0x54}}, {kCreateDspDesc, {0x40, 0x53, 0x55, 0x56, 0x57, 0x41}}, {kCreateDsp, {0x40, 0x53, 0x55, 0x56, 0x57, 0x41}},   {kAddDsp, {0x40, 0x53, 0x55, 0x56, 0x57, 0x41}},
    {kDspRelease, {0x40, 0x57, 0x48, 0x81, 0xEC, 0x60}}, {kGetDsp, {0x40, 0x53, 0x55, 0x56, 0x57, 0x41}},      {kSetFloat, {0x40, 0x53, 0x55, 0x56, 0x48, 0x81}},
    {kHud, {0x40, 0x57, 0x48, 0x83, 0xec, 0x20}},
};

typedef int (*CreateSoundFn)(void*, const char*, unsigned, unsigned char*, void**);
typedef int (*StudioPlayFn)(void*, void*, void*, bool, int, void**);
typedef int (*CreateDspFn)(void*, int, void**);
typedef int (*AddDspFn)(void*, int, void*);
typedef int (*GetDspFn)(void*, int, void**);
typedef int (*ReleaseDspFn)(void*);
typedef int (*SetFloatFn)(void*, int, float);
typedef int (*SetCtlFloatFn)(void*, float);
typedef uint64_t (*HudFn)(void*, void*, void*, void*);
CreateSoundFn g_orig_create = nullptr;
StudioPlayFn g_orig_play    = nullptr;
HudFn g_orig_hud            = nullptr;

void say(const char* fmt, ...) {
    char b[400];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    g_api->log(g_mod, b);
}

// Reads game memory without ever faulting.
bool rd(const void* p, void* dst, size_t n) {
    SIZE_T got = 0;
    return p && ReadProcessMemory(GetCurrentProcess(), p, dst, n, &got) && got == n;
}

// ---- settings -----------------------------------------------------------------------------------------------------------
std::atomic<int> g_enabled{1};
std::atomic<float> g_exh_amount{1.0f};  // 0 .. 2
std::atomic<float> g_int_amount{1.0f};
std::atomic<float> g_rasp{0.25f};
std::atomic<float> g_pops{1.0f};         // 0 .. 2, extra backfire pops
std::atomic<float> g_pipe{1.0f};        // pipe resonance amount 0 .. 2 (short echo on the Sport / Race exhaust)
std::atomic<float> g_air_amount{1.0f}, g_air{0.0f}, g_boost{0.0f};   // intake air hiss: the car's amount 0 .. 2, and the level in force (follows throttle and revs)
std::atomic<float> g_tur_amount{1.0f}, g_bov_amount{1.0f};
std::atomic<int> g_bov_choice{0};
std::atomic<bool> g_fi_sc{false};
// The intake air hiss is made in exactly ONE place (one effect instance at a time, its "owner"): every intake track carrying it made it
// once per track (a flat, constant layer), and a value that stopped being updated (a menu, a failed read) kept it going for ever.
std::atomic<uint64_t> g_air_tick{0};       // when the hiss level was last worked out from live physics
std::atomic<void*> g_air_owner{nullptr};
std::atomic<uint64_t> g_air_owner_t{0};
float air_level() { return g_air.load(); }
float air_gain(void* self, bool sounding) {
    const uint64_t now = GetTickCount64();
    if (now - g_air_tick.load() > 250) return 0.0f;   // no live physics lately
    const float a = air_level();
    if (a <= 0.002f) return 0.0f;
    if (g_air_owner.load() != self) {
        if (now - g_air_owner_t.load() <= 120 || !sounding) return 0.0f;   // another instance makes it (or this track is idle)
        g_air_owner = self;
    }
    g_air_owner_t = now;
    return a * 0.5f;
}
std::atomic<float> g_whl_amp{0.0f}, g_whl_freq{3000.0f}, g_sc_rich{0.2f};   // synthesised supercharger whine (level, fundamental Hz, harmonics)
std::atomic<bool> g_sc_cent{false};   // the supercharger is a centrifugal one (else positive displacement: Roots / twin-screw)
// The supercharger whine: made by one effect instance at a time, like the hiss (see air_gain)
std::atomic<void*> g_sc_owner{nullptr};
std::atomic<uint64_t> g_sc_owner_t{0};

struct ScSyn { float ph = 0, f = 0, a = 0, nl = 0, nb = 0, nb2 = 0; uint32_t rng = 0x1F2E3D4Cu; };
ScSyn g_scs;   // only the owner touches it
float sc_next() {   // one mono sample of the whine
    ScSyn& s = g_scs;
    constexpr float sr = 48000.0f;
    s.f += (g_whl_freq.load() - s.f) * 0.0012f;   // the pitch glides with the revs
    s.a += (g_whl_amp.load() - s.a) * 0.0012f;
    if (s.a < 1e-5f) return 0.0f;
    s.rng ^= s.rng << 13; s.rng ^= s.rng >> 17; s.rng ^= s.rng << 5;
    const float nz = static_cast<float>(s.rng & 0xFFFF) / 32768.0f - 1.0f;
    s.nl += 0.0008f * (nz - s.nl);   // very slow wander: a belt and bearings are never perfectly steady
    s.ph += 6.2831853f * s.f * (1.0f + 0.004f * s.nl * 20.0f) / sr;
    if (s.ph > 6.2831853f) s.ph -= 6.2831853f;
    const float r = g_sc_rich.load();
    float y;
    s.nb += 0.35f * (nz - s.nb); s.nb2 += 0.08f * (s.nb - s.nb2);
    const float air = s.nb - s.nb2;   // band noise (the air through the rotors / impeller)
    if (g_sc_cent.load()) {   // centrifugal: a clean rising whistle with a little air
        y = std::sin(s.ph) + 0.12f * r * std::sin(2.0f * s.ph) + air * (0.25f + 0.4f * r);
    } else {                  // Roots / screw: the lobes' whine, rich in harmonics, with a gritty air pulse on every lobe
        const float p1 = std::sin(s.ph), p2 = std::sin(2.0f * s.ph), p3 = std::sin(3.0f * s.ph), p4 = std::sin(4.0f * s.ph);
        y = p1 + 0.55f * r * p2 + 0.35f * r * r * p3 + 0.2f * r * r * p4 + air * (0.3f + 0.6f * r) * (0.6f + 0.4f * p1);
    }
    return y * s.a;
}
bool sc_on(void* self, bool sounding) {
    const uint64_t now = GetTickCount64();
    if (now - g_air_tick.load() > 250 || (g_whl_amp.load() < 1e-5f && g_scs.a < 1e-5f)) return false;
    if (g_sc_owner.load() != self) {
        if (now - g_sc_owner_t.load() <= 120 || !sounding) return false;
        g_sc_owner = self;
    }
    g_sc_owner_t = now;
    return true;
}
std::atomic<bool> g_fi_turbo{false};   // the installed forced induction is a turbo (not a supercharger)     // turbo / boost layers' effect 0 .. 2
std::atomic<int> g_tur_tier{0};
std::atomic<int> g_ov_exh{0}, g_ov_int{0};   // the driven car's chosen sound type: 0 = as installed, 1..4 = Stock / Street / Sport / Race
std::atomic<int> g_all_cars{0};         // also other cars that use the same sound bank (they do anyway); 1 = every car on the road
std::atomic<unsigned> g_cfg_serial{1};  // bumped when anything that changes the effect changes

wchar_t g_ini[MAX_PATH];
int ini_int(const wchar_t* key, int def) { return static_cast<int>(GetPrivateProfileIntW(L"EngineSoundTiers", key, def, g_ini)); }
void ini_set(const wchar_t* key, int v) {
    wchar_t t[32];
    _snwprintf(t, 32, L"%d", v);
    WritePrivateProfileStringW(L"EngineSoundTiers", key, t, g_ini);
}
void on_change(void*, const char* key, int value) {
    if (!strcmp(key, "Enabled")) { g_enabled = value; ini_set(L"Enabled", value); }
            else if (!strcmp(key, "AllCars")) { g_all_cars = value; ini_set(L"AllCars", value); }
    g_cfg_serial.fetch_add(1);
}
void on_slide(void*, const char* key, float value) {
    if (!strcmp(key, "ExhaustAmount")) { g_exh_amount = value / 100.0f; ini_set(L"ExhaustAmount", static_cast<int>(value + 0.5f)); }
    else if (!strcmp(key, "Pops")) { g_pops = value / 100.0f; ini_set(L"Pops", static_cast<int>(value + 0.5f)); }
    else if (!strcmp(key, "Rasp")) { g_rasp = value / 100.0f; ini_set(L"Rasp", static_cast<int>(value + 0.5f)); }
    else if (!strcmp(key, "IntakeAmount")) { g_int_amount = value / 100.0f; ini_set(L"IntakeAmount", static_cast<int>(value + 0.5f)); }
    else if (!strcmp(key, "PipeAmount")) { g_pipe = value / 100.0f; ini_set(L"PipeAmount", static_cast<int>(value + 0.5f)); }
    g_cfg_serial.fetch_add(1);
}

std::atomic<int> g_exh_tier{0}, g_int_tier{0};
// ---- the Engine tab's settings, saved per car -------------------------------------------------------------------------------
struct CarCfg { int e = 0, i = 0, ea = 100, ia = 100, rasp = 25, pipe = 100, pops = 100, ta = 100, ah = 100, bv = 100, bc = 0; };   // ta turbo / supercharger volume %, ah intake air hiss %, bv blow-off volume %, bc blow-off sound (0 = the game's own)   // sound types (0 = as installed), then percents
CarCfg g_def;   // what a car without its own entry gets (the old global ini values)
std::mutex g_selmu;
std::unordered_map<uint32_t, CarCfg> g_sel;   // garage id -> its settings
uint32_t cur_garage() { return g_api && g_api->upgrade_active_garage ? g_api->upgrade_active_garage() : 0; }
CarCfg cfg_of(uint32_t garage) {
    std::lock_guard<std::mutex> l(g_selmu);
    auto it = g_sel.find(garage);
    return it == g_sel.end() ? g_def : it->second;
}
void cfg_apply(const CarCfg& c) {   // make it the settings in force (the driven car's)
    const bool ch = g_ov_exh.exchange(c.e) != c.e | g_ov_int.exchange(c.i) != c.i;
    g_exh_amount = c.ea / 100.0f; g_int_amount = c.ia / 100.0f; g_rasp = c.rasp / 100.0f; g_pipe = c.pipe / 100.0f; g_pops = c.pops / 100.0f; g_tur_amount = c.ta / 100.0f; g_air_amount = c.ah / 100.0f; g_bov_amount = c.bv / 100.0f;
    if (g_bov_choice.exchange(c.bc) != c.bc) g_cfg_serial.fetch_add(1);
    if (ch) g_cfg_serial.fetch_add(1);
}
void sel_refresh(uint32_t garage) {
    const CarCfg c = cfg_of(garage);
    cfg_apply(c); g_cfg_serial.fetch_add(1);
    say("settings of garage car %u: types %d/%d, exhaust %d%%, intake %d%%, rasp %d, pipe %d%%, pops %d%%, turbo %d%%, hiss %d%%, blow-off %d%%", garage, c.e, c.i, c.ea, c.ia, c.rasp, c.pipe, c.pops, c.ta, c.ah, c.bv);
}
void sel_load() {
    wchar_t buf[8192] = {};
    GetPrivateProfileSectionW(L"Cars", buf, 8192, g_ini);
    for (const wchar_t* q = buf; *q; q += wcslen(q) + 1) {
        unsigned id = 0;
        CarCfg c = g_def;
        const int n = swscanf(q, L"%u=%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d", &id, &c.e, &c.i, &c.ea, &c.ia, &c.rasp, &c.pipe, &c.pops, &c.ta, &c.ah, &c.bv, &c.bc);
        if (n < 3 || !id) continue;
        auto cl = [](int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); };
        c.e = cl(c.e, 0, 4); c.i = cl(c.i, 0, 4); c.ea = cl(c.ea, 0, 200); c.ia = cl(c.ia, 0, 200); c.rasp = cl(c.rasp, 0, 35); c.pipe = cl(c.pipe, 0, 200); c.pops = cl(c.pops, 0, 200); c.ta = cl(c.ta, 0, 200); c.ah = cl(c.ah, 0, 200); c.bv = cl(c.bv, 0, 200); c.bc = cl(c.bc, 0, 15);
        g_sel[id] = c;
    }
}
// field: 0 exhaust type, 1 intake type, 2 exhaust %, 3 intake %, 4 rasp, 5 pipe, 6 pops, 7 turbo %, 8 intake air hiss %, 9 blow-off volume %, 10 blow-off sound
void sel_set(int field, int v) {
    const uint32_t id = cur_garage();
    if (!id) return;
    CarCfg c;
    {
        std::lock_guard<std::mutex> l(g_selmu);
        auto it = g_sel.find(id);
        c = it == g_sel.end() ? g_def : it->second;
        int* f[11] = {&c.e, &c.i, &c.ea, &c.ia, &c.rasp, &c.pipe, &c.pops, &c.ta, &c.ah, &c.bv, &c.bc};
        *f[field] = v;
        g_sel[id] = c;
    }
    wchar_t k[16], t[160];
    _snwprintf(k, 16, L"%u", id);
    _snwprintf(t, 160, L"%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d", c.e, c.i, c.ea, c.ia, c.rasp, c.pipe, c.pops, c.ta, c.ah, c.bv, c.bc);
    WritePrivateProfileStringW(L"Cars", k, t, g_ini);
    sel_refresh(id);
}
float sel_get(int field) {
    const CarCfg c = cfg_of(cur_garage());
    const int f[11] = {c.e, c.i, c.ea, c.ia, c.rasp, c.pipe, c.pops, c.ta, c.ah, c.bv, c.bc};
    return static_cast<float>(f[field]);
}
// the turbo / boost layers' level (the installed forced induction part: 0 none / stock .. 3 race)
int eff_turbo_tier() { return g_enabled.load() ? g_tur_tier.load() : 0; }
// the tier to use for the driven car's exhaust (exh) or intake
int eff_tier(int exh) {
    int t = exh ? g_exh_tier.load() : g_int_tier.load();
    const int ov = exh ? g_ov_exh.load() : g_ov_int.load();
    if (ov >= 1 && ov <= 4) t = ov - 1;
    if (!g_enabled.load()) t = 0;
    return t;
}

// ---- the driven car: its levels and its sound bank ----------------------------------------------------------------------
std::atomic<float> g_load{0.0f};   // 0..1: throttle x revs of the driven car, smoothed (an open pipe is rawest under load)
std::atomic<float> g_thr{0.0f};
std::atomic<float> g_rpm_peak{0.0f};
constexpr size_t kCarIface = 0x71E8, kIfaceToDyn = 0x4B0, kDynThrottle = 0x1BD0, kDynOmega = 0x5F8;   // as the Anti-Lag mod reads them
std::atomic<int> g_bank_pg{-1}, g_bank_gs{-1};
bool g_no_graph = false, g_no_own_dsp = false, g_no_echo = false;
std::atomic<uint64_t> g_hud_tick{0}, g_busy_until{0};   // the HUD ticks every frame in the world, never on a loading screen
std::atomic<long> g_grp_new{0}, g_grp_reuse{0};   // groups given an effect: new pointers / pointers seen before
std::atomic<long> g_n_released{0};
std::atomic<long> g_dc[7]{};   // diagnostics for the driven car: plays classified as other / exhaust / engine bay
std::atomic<uint64_t> g_dnext{0};
std::atomic<long> g_n_groups{0}, g_n_err{0};   // rows of kBankTier of the driven car's banks, -1 = not known

// ---- timing of our hooks (to trace a slow load): time inside the game's own function vs. in our code ------------------------
int64_t now_us() {
    LARGE_INTEGER f, c;
    QueryPerformanceFrequency(&f); QueryPerformanceCounter(&c);
    return c.QuadPart * 1000000 / f.QuadPart;
}
struct Stat {
    std::atomic<long> n{0};
    std::atomic<long long> orig{0}, own{0};
    std::atomic<long> max_orig{0}, max_own{0};
};
enum { S_CREATE, S_PLAY, S_PLAY2, S_SYNTH, S_HUD, S_APPLY, S_COUNT };
const char* const kStName[S_COUNT] = {"createSound", "studio play", "playSound", "synth", "HUD", "effect set-up"};
Stat g_st[S_COUNT];
thread_local int64_t t_orig = 0;
void st_max(std::atomic<long>& m, long v) { long o = m.load(); while (v > o && !m.compare_exchange_weak(o, v)) {} }
struct Tm {
    Stat& s; int64_t t0;
    explicit Tm(Stat& st) : s(st), t0(now_us()) { t_orig = 0; }
    ~Tm() {
        const int64_t tot = now_us() - t0, o = t_orig, w = tot > o ? tot - o : 0;
        s.n.fetch_add(1); s.orig.fetch_add(o); s.own.fetch_add(w);
        st_max(s.max_orig, static_cast<long>(o)); st_max(s.max_own, static_cast<long>(w));
    }
};
template <class F> auto orig_timed(F&& f) {
    const int64_t a = now_us();
    auto r = f();
    t_orig += now_us() - a;
    return r;
}
// one line per load: how long the HUD was silent, and what our hooks did in that time (game's own call / our code, ms; longest single call, ms)
void st_report(double gap_s) {
    char b[700];
    int o = _snprintf(b, sizeof b, "load of %.1f s; in our hooks (calls: in the game's own function / in our code, ms; longest call):", gap_s);
    for (int i = 0; i < S_COUNT && o > 0 && o < 600; ++i) {
        Stat& t = g_st[i];
        const long n = t.n.exchange(0);
        const double go = static_cast<double>(t.orig.exchange(0)) / 1000.0, ow = static_cast<double>(t.own.exchange(0)) / 1000.0;
        const double mo = t.max_orig.exchange(0) / 1000.0, mw = t.max_own.exchange(0) / 1000.0;
        if (n) o += _snprintf(b + o, sizeof b - o, " | %s %ld: %.0f / %.0f (max %.0f / %.0f)", kStName[i], n, go, ow, mo, mw);
    }
    say("%s", b);
}

int bank_row(const char* name) {
    for (int i = 0; i < kBanks; ++i)
        if (!_stricmp(kBankTier[i].name, name)) return i;
    return -1;
}

void car_seen(char* car) {
    int32_t ex = 0, in = 0;
    if (!rd(car + kCarExhaust, &ex, 4) || !rd(car + kCarIntake, &in, 4)) return;
    char name[64] = {};
    {   // Car+0x118 -> std::string (the engine's media name)
        char* s = nullptr;
        unsigned char str[32];
        if (rd(car + kCarName, &s, 8) && rd(s, str, 32)) {
            const uint64_t len = *reinterpret_cast<uint64_t*>(str + 16), cap = *reinterpret_cast<uint64_t*>(str + 24);
            if (len > 0 && len < 60) {
                if (cap >= 16) rd(*reinterpret_cast<char**>(str), name, static_cast<size_t>(len));
                else memcpy(name, str, static_cast<size_t>(len));
            }
        }
    }
    static int32_t last_ex = -12345, last_in = -12345;
    static char last_name[64] = "?";
    if (ex == last_ex && in == last_in && !strcmp(name, last_name)) return;
    last_ex = ex; last_in = in;
    memcpy(last_name, name, sizeof name);
    g_rpm_peak = 0;
    auto tier = [](int32_t v) { if (v < 0) return 0; const int t = v % 1000; return t > 3 ? 3 : t; };
    g_exh_tier = tier(ex);
    g_int_tier = tier(in);
    {   // forced induction: PartEnum 24 single turbo, 25 twin, 26 quad, 27 centrifugal, 28 positive displacement supercharger; the car's part table starts at Car+0x70E0 for enum 2
        int32_t fi[5] = {-1, -1, -1, -1, -1};
        int lv = 0;
        if (rd(car + 0x70E0 + 22 * 4, fi, sizeof fi))
            for (int32_t v : fi)
                if (v >= 0) { const int l = v > 3 ? 3 : v; if (l > lv) lv = l; }
        g_tur_tier = lv;
        g_fi_turbo = fi[0] >= 0 || fi[1] >= 0 || fi[2] >= 0;
        g_fi_sc = fi[3] >= 0 || fi[4] >= 0;
        g_sc_cent = fi[3] >= 0;
    }
    const char* engine = name;
    const char* gran = nullptr;
    for (const EngineMap& m : kEngineMap)
        if (!_stricmp(m.media, name)) { engine = m.engine; gran = m.granular; break; }
    char bn[80];
    std::snprintf(bn, sizeof bn, "PG_%s", engine);
    const int pg = name[0] ? bank_row(bn) : -1;
    const int gs = gran ? bank_row(gran) : -1;
    g_bank_pg = pg;
    g_bank_gs = gs;
    g_cfg_serial.fetch_add(1);
    for (auto& c : g_dc) c = 0;
    g_dnext = GetTickCount64() + 20000;
    say("driven car: engine sound \"%s\" (bank %s%s%s), exhaust part %d -> level %d, intake part %d -> level %d", name,
        pg >= 0 ? kBankTier[pg].name : "not found", gs >= 0 ? " + " : "", gs >= 0 ? kBankTier[gs].name : "", ex, g_exh_tier.load(), in,
        g_int_tier.load());
    {   // the car's installed part ids around the exhaust / intake slots (to find the forced induction slot)
        int32_t v[40] = {};
        char line[600];
        int o = _snprintf(line, sizeof line, "car part ids from Car+0x70E0:");
        if (rd(car + 0x70E0, v, sizeof v))
            for (int i = 0; i < 40 && o > 0 && o < 540; ++i) o += _snprintf(line + o, sizeof line - o, " %d", v[i]);
        say("%s", line);
    }
}

void rev_purge(uint64_t before);
uint64_t detour_hud(void* c, void* a, void* b, void* d) {
    Tm tm(g_st[S_HUD]);
    const uint64_t r = orig_timed([&] { return g_orig_hud(c, a, b, d); });
    {
        const uint64_t prev = g_hud_tick.load(), nw = GetTickCount64();
        g_hud_tick = nw;
        if (prev && nw - prev > 3000) { st_report((nw - prev) / 1000.0); rev_purge(prev); }
    }
    {   // throttle and revs of the driven car, every frame
        char* car = nullptr;
        void* cb  = nullptr;
        char* iface = nullptr;
        if (c && rd(static_cast<char*>(c) + 0x120, &car, 8) && rd(static_cast<char*>(c) + 0x128, &cb, 8) && car && cb && rd(car + kCarIface, &iface, 8) && iface) {
            char* dyn = iface - kIfaceToDyn;
            float thr = 0, omega = 0;
            if (rd(dyn + kDynThrottle, &thr, 4) && rd(dyn + kDynOmega, &omega, 4) && thr == thr && omega == omega) {
                thr = thr < 0 ? 0 : (thr > 1 ? 1 : thr);
                const float rpm = omega * 9.549296586f;
                float pk = g_rpm_peak.load();
                if (rpm > pk && rpm < 20000.0f) { pk = rpm; g_rpm_peak = pk; }
                const float top = pk > 5500.0f ? pk : 5500.0f;
                float rn = (rpm - 1200.0f) / (top - 1200.0f);
                rn = rn < 0 ? 0 : (rn > 1 ? 1 : rn);
                const float ts = g_thr.load() + (thr - g_thr.load()) * 0.35f;
                g_thr = ts;
                const float target = ts * (0.4f + 0.6f * rn);
                g_load = g_load.load() + (target - g_load.load()) * 0.25f;
                {   // synthesised forced-induction sound: hiss + whistle follow the boost pressure (turbo) or the revs (supercharger)
                    static const float kLv[4] = {0.3f, 0.55f, 0.8f, 1.0f}, kWh[4] = {0.012f, 0.02f, 0.03f, 0.045f};
                    const int tr = g_tur_tier.load() & 3;
                    const float en = g_enabled.load() ? 1.0f : 0.0f;
                    float bn = 0.0f;
                    float bv = 0, bmin = 0;
                    static float f_bv = -1, f_om = -1;
                    static uint64_t f_t = 0;
                    {   // the physics stands still (a menu, a pause): the boost reading is frozen, so it counts as none
                        float b2 = 0;
                        const uint64_t tn2 = GetTickCount64();
                        if (!rd(dyn + 0x620, &b2, 4) || b2 != f_bv || omega != f_om) { f_bv = b2; f_om = omega; f_t = tn2; }
                        if (tn2 - f_t > 350) bv = -1e9f;
                    }
                    static float bpk = 0, bmn = 1e9f;
                    static float last_pk = 1e9f;
                    if (g_rpm_peak.load() < last_pk) { bpk = 0; bmn = 1e9f; }   // a new car: the rev peak starts again
                    last_pk = g_rpm_peak.load();
                    const bool frozen = bv < -1e8f;
                    if (!frozen && rd(dyn + 0x620, &bv, 4) && bv == bv && bv > -1000 && bv < 1000) {
                        if (bv < bmn) bmn = bv;
                        if (bv > bpk) bpk = bv;
                        const float span = bpk - bmn;
                        if (span > 1.0f) { bn = (bv - bmn) / span; bn = bn < 0 ? 0 : (bn > 1 ? 1 : bn); }
                    }
                    (void)bmin;
                    bn = bn < 0.12f ? 0.0f : std::pow((bn - 0.12f) / 0.88f, 1.5f);   // a dead zone near idle pressure and a steeper curve: partial boost is clearly quieter
                    g_boost = g_boost.load() + (bn - g_boost.load()) * 0.4f;
                    const float b = g_boost.load();
                    if (g_fi_turbo.load()) {
                        g_air = kLv[tr] * en * g_air_amount.load() * b;
                        g_whl_amp = 0;
                        g_whl_freq = 2200.0f + 5200.0f * b;
                    } else if (g_fi_sc.load()) {
                        g_air = 0;
                        // a bigger blower turned faster (a smaller pulley) at every level: higher pitch, louder, richer. Stock = a faint whine.
                        static const float kPdRatio[4] = {1.9f, 2.25f, 2.6f, 3.0f}, kPdAmp[4] = {0.004f, 0.012f, 0.02f, 0.03f};
                        static const float kCnStep[4] = {26.0f, 32.0f, 38.0f, 46.0f}, kCnAmp[4] = {0.003f, 0.01f, 0.017f, 0.026f};
                        const float rps = rpm > 0 ? rpm / 60.0f : 0.0f;
                        const float ta = g_tur_amount.load();
                        const float vol = en * std::pow(ta, 1.6f);
                        if (g_sc_cent.load()) {   // the impeller's speed (and its boost) climbs with the square of the revs
                            g_whl_freq = rps * kCnStep[tr];
                            g_whl_amp = kCnAmp[tr] * vol * rn * rn * (0.35f + 0.65f * g_load.load());
                        } else {                  // 3-lobe rotors x2: 6 pulses per rotor turn; heard even off throttle (bypass), more on load
                            g_whl_freq = rps * kPdRatio[tr] * 6.0f;
                            g_whl_amp = kPdAmp[tr] * vol * (0.3f + 0.7f * rn) * (0.25f + 0.75f * g_load.load());
                        }
                        g_sc_rich = 0.25f + 0.25f * static_cast<float>(tr);
                    } else { g_air = 0; g_whl_amp = 0; }
                    if (frozen) { g_air = 0; g_boost = 0; g_whl_amp = 0; }
                    else g_air_tick = GetTickCount64();
                    static std::atomic<uint64_t> tnext{0};
                    const uint64_t tn = GetTickCount64();
                    if ((g_fi_turbo.load() || g_fi_sc.load()) && tn > tnext.load() && (b > 0.02f || g_whl_amp.load() > 1e-4f)) { tnext = tn + 5000; say("boost %.2f (raw %.2f, seen %.2f .. %.2f): hiss %.3f, supercharger whine %.4f at %.0f Hz", b, bv, bmn, bpk, g_air.load(), g_whl_amp.load(), g_whl_freq.load()); }
                }
            }
        }
    }
    static std::atomic<uint64_t> next{0};
    const uint64_t now = GetTickCount64();
    if (now < next.load()) return r;
    next.store(now + 500);
    {
        const uint64_t dn = g_dnext.load();
        if (dn && now > dn) {
            g_dnext = now + 60000;
            say("sounds of the driven car so far: exhaust %ld, engine bay %ld, turbo %ld, other %ld (tracks with effect: %ld = %ld new groups + %ld seen before; %ld effect sets re-used); throttle %.2f, load %.2f, rev peak %.0f", g_dc[1].load(), g_dc[2].load(), g_dc[5].load(), g_dc[0].load(), g_n_groups.load(), g_grp_new.load(), g_grp_reuse.load(), g_n_released.load(), g_thr.load(), g_load.load(), g_rpm_peak.load());
        }
    }
    if (g_api->upgrade_active_garage) {   // the driven (or tuned) car's sound type choice
        static uint32_t last_garage = 0xFFFFFFFFu;
        const uint32_t gid = g_api->upgrade_active_garage();
        if (gid != last_garage) { last_garage = gid; sel_refresh(gid); }
    }
    char* car = nullptr;
    void* cb  = nullptr;
    if (c && rd(static_cast<char*>(c) + 0x120, &car, 8) && rd(static_cast<char*>(c) + 0x128, &cb, 8) && car && cb) car_seen(car);
    return r;
}

// ---- which sample is what ------------------------------------------------------------------------------------------------
// Studio opens every sample of a bank as its own sound: createSound with the sample data's place in the bank file at
// exinfo.fileoffset (+0x8), exinfo.length (+0x4) and an inclusion list (+0x28, count +0x30) of one entry, the sample's index.
struct Made { int row; uint32_t idx; };
std::mutex g_mu;
std::unordered_map<void*, Made> g_made;
struct Rev { void* snd; uint64_t t; };
std::unordered_map<uint32_t, Rev> g_rev;   // (bank row << 16 | sample index) -> the Sound made for it, guarded by g_mu

std::unordered_set<void*> g_pop;          // sounds the Anti-Lag mod made (its pop samples), guarded by g_mu
HMODULE g_antilag_mod = nullptr;
std::atomic<long> g_n_create{0}, g_n_play{0}, g_n_apply{0}, g_apply_max_us{0};   // activity, for the watchdog line
std::unordered_map<void*, int> g_user;   // sounds that are fed by a PCM callback (the granular engine synth), guarded by g_mu
std::atomic<int> g_user_logged{0};

int detour_create(void* sys, const char* name, unsigned mode, unsigned char* ex, void** out) {
    g_n_create.fetch_add(1);
    Tm tm(g_st[S_CREATE]);
    const int r = orig_timed([&] { return g_orig_create(sys, name, mode, ex, out); });
    // The Anti-Lag mod's pop samples are raw PCM held in memory (OPENMEMORY | OPENRAW); the game's bank samples never are. (The caller's
    // module can not be used: with chained hooks every call may appear to come from the Anti-Lag mod.)
    if (r == 0 && out && *out && g_antilag_mod && (mode & 0x800) && (mode & 0x1000)) {
        std::scoped_lock lk{g_mu};
        g_pop.insert(*out);
        static std::atomic<int> logged{0};
        if (logged.fetch_add(1) < 3) say("an Anti-Lag pop sample noted");
        return r;
    }
    if (r != 0 || !out || !ex || !*out) return r;
    if (mode & 0x200) {   // FMOD_OPENUSER
        int cb0 = 0, nch = 0, freq = 0, fmt = 0;
        uint64_t pcm = 0;
        if (rd(ex, &cb0, 4) && cb0 >= 0x40 && rd(ex + 0x38, &pcm, 8) && pcm && rd(ex + 0xC, &nch, 4) && rd(ex + 0x10, &freq, 4) && rd(ex + 0x14, &fmt, 4)) {
            {
                std::scoped_lock lk{g_mu};
                g_user[*out] = 1;
            }
            if (g_user_logged.fetch_add(1) < 12)
                say("PCM-callback sound made: callback game+0x%llX, %d channel(s), %d Hz, format %d", static_cast<unsigned long long>(pcm - reinterpret_cast<uint64_t>(g_game)), nch, freq, fmt);
            return r;
        }
    }
    int cb = 0, cnt = 0;
    int* incl = nullptr;
    uint32_t off = 0, len = 0, idx = 0;
    if (!rd(ex, &cb, 4) || cb < 0x38 || !rd(ex + 0x30, &cnt, 4) || cnt != 1 || !rd(ex + 0x28, &incl, 8) || !rd(incl, &idx, 4) ||
        !rd(ex + 0x8, &off, 4) || !rd(ex + 0x4, &len, 4))
        return r;
    int row = -1;
    for (int i = 0; i < kBanks; ++i)
        if (kBankTier[i].off == off && kBankTier[i].len == len) { row = i; break; }
    {   // how the game makes a bank's sample (to be able to make another bank's blow-off sample ourselves)
        static std::atomic<int> dumped{0};
        if (row >= 0 && dumped.fetch_add(1) < 3) {
            char nm[200] = {};
            if (!(mode & 0x800) && name) rd(name, nm, 199);
            char b[900];
            int o = _snprintf(b, sizeof b, "bank sample made: mode 0x%X, name \"%s\", exinfo size 0x%X:", mode, nm, cb);
            for (int k = 0; k + 8 <= cb && k < 0xE0 && o > 0 && o < 800; k += 8) {
                uint64_t q = 0;
                if (rd(ex + k, &q, 8) && q) o += _snprintf(b + o, sizeof b - o, " +%X=%llX", k, static_cast<unsigned long long>(q));
            }
            say("%s", b);
        }
    }
    std::scoped_lock lk{g_mu};
    if (row < 0) g_made.erase(*out);
    else {
        if (g_made.size() > 60000) { g_made.clear(); g_rev.clear(); }
        g_made[*out] = Made{row, idx};
        g_rev[(static_cast<uint32_t>(row) << 16) | (idx & 0xFFFF)] = Rev{*out, GetTickCount64()};
    }
    return r;
}

// 0 = neither, 1 = exhaust, 2 = engine bay, 4 = the game's own backfire pop; *row = the bank
// Pops: the Anti-Lag mod owns their sound while it is installed (its own sample plays in their place); we only add extra ones then.
bool g_antilag = false;
int classify(void* snd, int* row) {
    if (g_pop.count(snd)) { *row = g_bank_pg.load(); return 4; }
    auto it = g_made.find(snd);
    if (it == g_made.end()) {
        void* parent = nullptr;
        if (rd(static_cast<char*>(snd) + 0xB0, &parent, 8) && parent) it = g_made.find(parent);
    }
    if (it == g_made.end()) return 0;
    const Made m = it->second;
    if (m.idx >= 512) return 0;
    *row = m.row;
    const BankTier& b = kBankTier[m.row];
    for (const BankBf& bf : kBankBf) {
        if (bf.off != b.off || bf.len != b.len) continue;
        for (int j = 0; j < bf.cnt; ++j)
            if (bf.idx[j] == m.idx) return 4;
    }
    if (b.bov[m.idx >> 6] >> (m.idx & 63) & 1) return 6;   // blow-off / flutter
    if (b.tur[m.idx >> 6] >> (m.idx & 63) & 1) return 5;   // turbo / supercharger spool and whistle
    if (b.exh[m.idx >> 6] >> (m.idx & 63) & 1) return 1;
    if (b.eng[m.idx >> 6] >> (m.idx & 63) & 1) return 2;
    return 0;
}

// sounds made before a loading screen began may be gone: forget them (g_mu taken inside)
void rev_purge(uint64_t before) {
    std::scoped_lock lk{g_mu};
    for (auto it = g_rev.begin(); it != g_rev.end();) it = it->second.t < before ? g_rev.erase(it) : std::next(it);
}
// the Sound of the n-th (1-based, limited to the count) blow-off sample of a bank, or null (g_mu held)
void* bov_pick(int row, int n) {
    if (row < 0 || n < 1) return nullptr;
    const BankTier& b = kBankTier[row];
    int cnt = 0;
    for (int i = 0; i < 512; ++i) if (b.bov[i >> 6] >> (i & 63) & 1) ++cnt;
    if (!cnt) return nullptr;
    const int want = (n > cnt ? cnt : n) - 1;
    int k = 0;
    for (int i = 0; i < 512; ++i)
        if (b.bov[i >> 6] >> (i & 63) & 1) {
            if (k++ == want) {
                auto it = g_rev.find((static_cast<uint32_t>(row) << 16) | static_cast<uint32_t>(i));
                return it == g_rev.end() ? nullptr : it->second.snd;
            }
        }
    return nullptr;
}

// ---- the effect ----------------------------------------------------------------------------------------------------------
// Three-band EQ gains in dB (low / mid / high, crossovers 250 Hz and 2.5 kHz) and distortion level per upgrade level.
// What changes on a real car, and how it is modelled with FMOD's own effects:
//  - a muffler is a low-pass filter and a level drop: taking mufflers away gives level and top end back   -> "tilt" three-band EQ
//  - without resonators the upper-mid harmonics (the rasp band, about 0.6 - 2 kHz) come through        -> a band boost (second EQ)
//  - an open pipe rings: its length gives a comb of resonances (the hollow, tubular drone)             -> a very short echo, mixed low
//  - an open intake is heard as induction growl in the low mids, ITBs bark higher                      -> band boost, lower / wider
//  - grit: only a trace of distortion, on its own slider
struct Fx {
    float low, mid, high, x1, x2;   // tilt EQ: gains in dB, crossovers in Hz
    float band, b1, b2;             // band boost in dB between b1 and b2 Hz
    float echo_ms, echo_wet, echo_fb;   // pipe resonance: delay, wet level in dB (<= -80: none), feedback %
    float dist;                     // distortion level (scaled by the Rasp slider)
};
// Turbo / boost layers (spool, whistle, flutter): airy, not echoing: the top end opens up (breath, hiss, whistle) with the level, no delay.
// Levels follow the forced induction upgrade. Fields: low, mid, high, x1, x2, band, b1, b2, echo_ms, echo_wet, echo_fb, dist.
const Fx kTurFx[4] = {
    {0, 0, 0, 1000, 4500, 0, 4500, 11000, 60, -80, 0, 0},
    {1.5f, 3.0f, 5.0f, 1000, 4500, 2.5f, 4500, 11000, 60, -80, 0, 0},
    {3.0f, 5.0f, 8.0f, 1000, 4500, 4.5f, 4500, 11000, 60, -80, 0, 0},
    {4.5f, 7.0f, 10.0f, 1000, 4500, 6.0f, 4500, 11000, 60, -80, 0, 0}};
// Exhaust: stock; sport rear muffler; sport rear + middle muffler; open straight-through system.
const Fx kExhFx[4] = {
    {0, 0, 0, 250, 2500, 0, 700, 1800, 12, -80, 0, 0},
    {1.0f, 1.5f, 1.5f, 250, 2500, 1.5f, 700, 1800, 12, -80, 0, 0},              // a little louder and less muffled
    {2.5f, 2.5f, 2.5f, 230, 2500, 3.0f, 600, 2000, 13, -19, 22, 0.10f},        // bark: upper mids open up, the pipe starts to ring
    {4.5f, 3.5f, 4.5f, 210, 2600, 5.5f, 450, 2200, 11, -13, 32, 0.40f}};       // open pipe: loud, raspy band, hollow ring, bright
// Intake (the engine-bay layer): stock; sport panel filter; cone filter; ITB / mesh.
const Fx kIntFx[4] = {
    {0, 0, 0, 250, 2500, 0, 250, 700, 10, -80, 0, 0},
    {0, 0.5f, 1.0f, 250, 3000, 2.0f, 250, 700, 10, -80, 0, 0},                 // a hint of induction growl
    {0.5f, 1.0f, 1.5f, 250, 3000, 4.0f, 250, 900, 10, -80, 0, 0},              // cone filter: clear induction growl
    {1.0f, 2.0f, 3.0f, 250, 2800, 6.0f, 300, 1200, 10, -80, 0, 0.20f}};        // ITB: loud bark, wider and higher


// ---- our own FMOD DSP: what an EQ cannot do ------------------------------------------------------------------------------------
// 1. Pulse emphasis: a muffler smooths the exhaust's pulse train; an open pipe doesn't. A fast and a slow envelope follower tell where a
//    pulse is; the gain is raised on the pulse and eased between pulses (a downward-biased transient shaper), so single cylinders speak.
// 2. Harmonic rasp: the part above ~700 Hz goes through an asymmetric soft clipper (even + odd harmonics) and is mixed back low: a
//    ragged, open edge instead of the hard "distorted" sound of a clipper over the whole signal.
// 3. A little make-up gain, and a soft limiter so nothing clips.
// a cheap tanh (rational, exact at 0, saturating at +-3): the effect runs on every track, so the real tanh costs too much
inline float ftanh(float x) {
    if (x > 3.0f) return 1.0f;
    if (x < -3.0f) return -1.0f;
    const float x2 = x * x;
    return x * (27.0f + x2) / (27.0f + 9.0f * x2);
}
struct Dsp { std::atomic<float> k{0}, drive{1}, mix{0}, bias{0}, gain{1}, floor{0.4f}, vol{1}; float ef[8] = {}, es[8] = {}, lp[8] = {}, wl[8] = {}, nl[8] = {}, nh[8] = {}; uint32_t rng = 0x2545F491u; float ph = 0, ph2 = 0; std::atomic<bool> air{false}, dead{false}; std::atomic<int> kind{0}; float airg_now = 0; bool sc_now = false; };
thread_local Dsp* t_new_dsp = nullptr;
constexpr float kSr = 48000.0f;

int dsp_create(void* st) {
    Dsp* d = new (std::nothrow) Dsp();
    if (!d) return 1;
    *reinterpret_cast<void**>(static_cast<char*>(st) + 8) = d;   // FMOD_DSP_STATE.plugindata
    t_new_dsp = d;
    return 0;
}
int dsp_release(void* st) {
    Dsp* d = nullptr;
    if (rd(static_cast<char*>(st) + 8, &d, 8) && d) d->dead = true;   // the memory stays: a dangling pointer of ours must stay readable
    return 0;
}
int dsp_reset(void*) { return 0; }
int dsp_read(void* st, float* in, float* out, unsigned length, int inch, int* outch) {
    if (outch) *outch = inch;
    Dsp* d = *reinterpret_cast<Dsp**>(static_cast<char*>(st) + 8);
    if (!d || inch < 1 || inch > 8) { if (in != out) memcpy(out, in, sizeof(float) * length * static_cast<size_t>(inch > 0 ? inch : 1)); return 0; }
    {   // silent block (most tracks, most of the time): nothing to shape, and no maths
        const unsigned n = length * static_cast<unsigned>(inch);
        float mx = 0;
        for (unsigned i = 0; i < n; ++i) { const float a = in[i] < 0 ? -in[i] : in[i]; mx = a > mx ? a : mx; }
        d->airg_now = d->air.load() ? air_gain(d, mx >= 1e-5f) : 0.0f;
        d->sc_now = d->air.load() && g_fi_sc.load() && sc_on(d, mx >= 1e-5f);
        if (mx < 1e-5f && d->airg_now <= 0 && !d->sc_now) { if (in != out) memcpy(out, in, sizeof(float) * n); return 0; }   // (the hiss goes on over a silent intake)
    }
    static const float af = 1.0f - std::exp(-1.0f / (0.0007f * kSr)), rf = 1.0f - std::exp(-1.0f / (0.012f * kSr)),
                       as = 1.0f - std::exp(-1.0f / (0.030f * kSr)), rs = 1.0f - std::exp(-1.0f / (0.060f * kSr)),
                       al = 1.0f - std::exp(-6.2831853f * 700.0f / kSr);
    const float fl = d->floor.load(), dyn = fl + (1.0f - fl) * g_load.load();   // rawer under load, cleaner on a lift
    const float k = d->k.load() * dyn, drive = d->drive.load(), mix = d->mix.load() * dyn, bias = d->bias.load(), gain = 1.0f + (d->gain.load() - 1.0f) * dyn;
    float vl = d->vol.load();
    {   // the turbo / blow-off volume follows the slider live (a looping sound is set up only once)
        static const float kTv[4] = {1.0f, 1.3f, 1.7f, 2.2f}, kBv[4] = {1.0f, 1.2f, 1.5f, 1.9f};
        const int kd = d->kind.load(), t = g_tur_tier.load() & 3;
        (void)kTv; (void)kBv; (void)t;
        if (kd == 5) {   // 100% = the game's own level; 200% about 3x; the game's own loudness curve stays
            const float b = g_boost.load();
            vl = 6.0f * std::pow(g_tur_amount.load(), 1.6f) * (0.15f + 0.85f * std::pow(b, 0.6f));   // 100% = 6x at full boost, 200% = 18x
        } else if (kd == 6) vl = 3.0f * std::pow(g_bov_amount.load(), 1.6f);
        vl = vl < 0 ? 0 : (vl > 20.0f ? 20.0f : vl);
    }
    const float tb = ftanh(bias);
    const float lim = d->kind.load() >= 5 ? 9.0f : 0.8f;   // turbo / blow-off layers: no soft limiter (a loud setting must stay louder)
    const float airg = d->airg_now;
    const bool scw = d->sc_now;   // intake air hiss: band-limited noise, 1.5 .. 7 kHz
    static const float nlp = 1.0f - std::exp(-6.2831853f * 7000.0f / kSr), nhp = 1.0f - std::exp(-6.2831853f * 1500.0f / kSr);
    for (unsigned i = 0; i < length; ++i) {
        const float wh = scw ? sc_next() : 0.0f;   // the supercharger whine
        for (int c = 0; c < inch; ++c) {
            const float x = in[i * static_cast<unsigned>(inch) + static_cast<unsigned>(c)];
            const float ax = x < 0 ? -x : x;
            d->ef[c] += (ax - d->ef[c]) * (ax > d->ef[c] ? af : rf);
            d->es[c] += (ax - d->es[c]) * (ax > d->es[c] ? as : rs);
            float g = 1.0f + k * ((d->ef[c] + 1e-4f) / (d->es[c] + 1e-4f) - 1.0f);
            g = g < 0.5f ? 0.5f : (g > 2.5f ? 2.5f : g);
            const float y = x * g;
            d->lp[c] += al * (y - d->lp[c]);
            const float hp = y - d->lp[c];
            const float wet = (ftanh(drive * hp + bias) - tb) * 0.7f;
            d->wl[c] += 0.45f * (wet - d->wl[c]);   // the added rasp is smoothed above ~7 kHz: ragged, not fizzy
            float o = (y + mix * d->wl[c]) * gain * vl;
            const float ao = o < 0 ? -o : o;
            if (lim < 1.5f && ao > lim) o = (o < 0 ? -1.0f : 1.0f) * (lim + (1.0f - lim) * ftanh((ao - lim) / (1.0f - lim)));
            if (airg > 0) {
                d->rng ^= d->rng << 13; d->rng ^= d->rng >> 17; d->rng ^= d->rng << 5;
                const float nz = static_cast<float>(d->rng & 0xFFFF) / 32768.0f - 1.0f;
                d->nl[c] += nlp * (nz - d->nl[c]);
                d->nh[c] += nhp * (d->nl[c] - d->nh[c]);
                o += (d->nl[c] - d->nh[c]) * airg;
            }
            o += wh;
            out[i * static_cast<unsigned>(inch) + static_cast<unsigned>(c)] = o;
        }
    }
    return 0;
}
typedef int (*CreateDspDescFn)(void*, const void*, void**);
Dsp* make_dsp(void* sys, void** dsp_out) {
    static unsigned char desc[0xD8] = {};   // static: FMOD may keep a pointer to it
    *reinterpret_cast<unsigned*>(desc + 0x00) = 108;                 // pluginsdkversion (what the game's own plugin descriptions say)
    memcpy(desc + 4, "EngineSoundTiers", 17);                        // name[32]
    *reinterpret_cast<unsigned*>(desc + 0x24) = 0x00010000;          // version
    *reinterpret_cast<int*>(desc + 0x28) = 1;                        // numinputbuffers
    *reinterpret_cast<int*>(desc + 0x2C) = 1;                        // numoutputbuffers
    *reinterpret_cast<void**>(desc + 0x30) = reinterpret_cast<void*>(&dsp_create);
    *reinterpret_cast<void**>(desc + 0x38) = reinterpret_cast<void*>(&dsp_release);
    *reinterpret_cast<void**>(desc + 0x40) = reinterpret_cast<void*>(&dsp_reset);
    *reinterpret_cast<void**>(desc + 0x48) = reinterpret_cast<void*>(&dsp_read);
    t_new_dsp = nullptr;
    void* dsp = nullptr;
    const int r = reinterpret_cast<CreateDspDescFn>(g_game + kCreateDspDesc)(sys, desc, &dsp);
    if (r != 0 || !dsp || !t_new_dsp) return nullptr;
    *dsp_out = dsp;
    static std::atomic<int> logged{0};
    if (logged.fetch_add(1) == 0) say("our own FMOD effect (pulse emphasis + rasp) is working");
    return t_new_dsp;
}
struct Dp { float k, drive, mix, bias, gain_db; };
const Dp kExhDp[4] = {{0, 1, 0, 0, 0}, {0.12f, 1.5f, 0.10f, 0.10f, 0}, {0.40f, 2.5f, 0.28f, 0.20f, 1.0f}, {0.80f, 3.0f, 0.55f, 0.30f, 1.5f}};
const Dp kIntDp[4] = {{0, 1, 0, 0, 0}, {0.05f, 1.5f, 0.05f, 0.10f, 0}, {0.20f, 2.0f, 0.15f, 0.15f, 0.5f}, {0.45f, 3.0f, 0.35f, 0.25f, 1.0f}};

// ---- the granular engine synth ---------------------------------------------------------------------------------------------------
// The 50 granular cars' main engine sound is made by an FMOD DSP the game registers itself, "PG Engine Synth" (a static description in the
// exe: read callback game+0x1948A30). It generates the sound into `out`; we run the original and then shape that buffer: the same pulse
// emphasis and rasp as above plus a three-band tilt, with the level of the driven car's exhaust and intake mixed (the synth makes both).
typedef int (*SynthReadFn)(void*, float*, float*, unsigned, int, int*);
SynthReadFn g_orig_synth = nullptr;

struct Blk {   // everything process_block needs, per call
    float k, drive, mix, bias, gain, lg, mg, hg, x1, x2, limit;
};
struct Inst { void* key = nullptr; uint64_t last = 0; Dsp d; float l1[8] = {}, l2[8] = {}; };
Inst g_inst[16];

void process_block(Inst& in, float* buf, unsigned frames, int ch, const Blk& b) {
    static const float af = 1.0f - std::exp(-1.0f / (0.0007f * kSr)), rf = 1.0f - std::exp(-1.0f / (0.012f * kSr)),
                       as = 1.0f - std::exp(-1.0f / (0.030f * kSr)), rs = 1.0f - std::exp(-1.0f / (0.060f * kSr)),
                       al = 1.0f - std::exp(-6.2831853f * 700.0f / kSr);
    const float a1 = 1.0f - std::exp(-6.2831853f * b.x1 / kSr), a2 = 1.0f - std::exp(-6.2831853f * b.x2 / kSr), tb = ftanh(b.bias);
    Dsp& d = in.d;
    const float airg = (g_fi_turbo.load() && g_enabled.load()) ? air_gain(&in, true) : 0.0f;   // the intake air hiss of a turbo car (a granular engine has one track: it goes in here)
    static const float nlp = 1.0f - std::exp(-6.2831853f * 7000.0f / kSr), nhp = 1.0f - std::exp(-6.2831853f * 1500.0f / kSr);
    const bool scw = g_fi_sc.load() && g_enabled.load() && sc_on(&in, true);
    for (unsigned i = 0; i < frames; ++i) {
        const float wh = scw ? sc_next() : 0.0f;   // the supercharger whine
        for (int c = 0; c < ch; ++c) {
            float x = buf[i * static_cast<unsigned>(ch) + static_cast<unsigned>(c)];
            in.l1[c] += a1 * (x - in.l1[c]);
            in.l2[c] += a2 * (x - in.l2[c]);
            x = in.l1[c] * b.lg + (in.l2[c] - in.l1[c]) * b.mg + (x - in.l2[c]) * b.hg;
            const float ax = x < 0 ? -x : x;
            d.ef[c] += (ax - d.ef[c]) * (ax > d.ef[c] ? af : rf);
            d.es[c] += (ax - d.es[c]) * (ax > d.es[c] ? as : rs);
            float g = 1.0f + b.k * ((d.ef[c] + 1e-4f) / (d.es[c] + 1e-4f) - 1.0f);
            g = g < 0.5f ? 0.5f : (g > 2.5f ? 2.5f : g);
            const float y = x * g;
            d.lp[c] += al * (y - d.lp[c]);
            const float wet = (ftanh(b.drive * (y - d.lp[c]) + b.bias) - tb) * 0.7f;
            d.wl[c] += 0.45f * (wet - d.wl[c]);
            float o = (y + b.mix * d.wl[c]) * b.gain;
            const float ao = o < 0 ? -o : o;
            if (ao > b.limit) o = (o < 0 ? -1.0f : 1.0f) * (b.limit + (1.0f - b.limit) * ftanh((ao - b.limit) / (1.0f - b.limit)));
            if (airg > 0) {
                d.rng ^= d.rng << 13; d.rng ^= d.rng >> 17; d.rng ^= d.rng << 5;
                const float nz = static_cast<float>(d.rng & 0xFFFF) / 32768.0f - 1.0f;
                d.nl[c] += nlp * (nz - d.nl[c]);
                d.nh[c] += nhp * (d.nl[c] - d.nh[c]);
                o += (d.nl[c] - d.nh[c]) * airg;
            }
            buf[i * static_cast<unsigned>(ch) + static_cast<unsigned>(c)] = o + wh;
        }
    }
}

struct Group { void* eq = nullptr; void* eq2 = nullptr; void* echo = nullptr; void* dist = nullptr; void* cdsp = nullptr; Dsp* cst = nullptr; unsigned serial = 0; uint64_t last = 0; int cls = 0; };
// FMOD is never asked about a group that may be gone (a stale handle crashed it): the number of effect sets is kept down by re-using the
// set of the least recently used track for a new one (addDSP moves a DSP that is already in another chain).
constexpr size_t kMaxSets = 48;
std::mutex g_gmu;
std::unordered_map<void*, Group> g_groups;   // track channel groups that carry our effect

template <class F> F fn(uintptr_t rva) { return reinterpret_cast<F>(g_game + rva); }

void apply(void* sys, void* grp, int cls, bool gran) {
    auto pick = [&](int exh) { return eff_tier(exh); };
    auto scaled = [&](const Fx& f, float a) {
        Fx r = f;
        r.low *= a; r.mid *= a; r.high *= a; r.band *= a;
        r.dist *= a * g_rasp.load();
        const float pa = g_pipe.load();
        if (a <= 0.01f || pa <= 0.01f) r.echo_wet = -80;
        else if (r.echo_wet > -80) r.echo_wet += 8.0f * (a - 1.0f) + 10.0f * (pa - 1.0f);   // pipe resonance 200% = 10 dB more ring, 50% = 5 dB less   // amount 200% = 8 dB more ring, 50% = 4 dB less
        return r;
    };
    int tier;
    Fx fx;
    float vol = 1.0f;   // plain volume of the track (turbo / supercharger and blow-off layers)
    if (cls == 5 || cls == 6) {
        tier = eff_turbo_tier();
        static const float kTv[4] = {1.0f, 1.3f, 1.7f, 2.2f}, kBv[4] = {1.0f, 1.2f, 1.5f, 1.9f};
        if (cls == 5) {
            const float ta = g_tur_amount.load();
            (void)ta; (void)kTv;
            fx = kTurFx[0];   // no tone change: 100% sounds like the game's own turbo
            vol = 1.0f;
        } else {
            fx = kTurFx[0];
            (void)kBv;
            vol = 1.0f;
        }
        fx.echo_wet = -80;
        vol = vol < 0 ? 0 : (vol > 4.0f ? 4.0f : vol);
    } else if (cls == 3) {   // a granular engine's synthesised sound: exhaust and intake in one track -> the exhaust's shape, some of the intake's tilt
        const int te = pick(1), ti = pick(0);
        tier = te > ti ? te : ti;
        const Fx i = scaled(kIntFx[ti], g_int_amount.load());
        fx = scaled(kExhFx[te], g_exh_amount.load());
        fx.low += i.low * 0.4f; fx.mid += (i.mid + i.band) * 0.4f; fx.high += i.high * 0.4f; fx.dist += i.dist * 0.4f;
    } else {
        tier = pick(cls == 1);
        fx = scaled((cls == 1 ? kExhFx : kIntFx)[tier], cls == 1 ? g_exh_amount.load() : g_int_amount.load());
    }
    auto clampf = [](float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); };
    Dp dp{};
    {
        const float rasp = clampf(g_rasp.load() * 4.0f, 0, 2.5f);
        auto sdp = [&](const Dp& d, float a) { return Dp{d.k * a, d.drive, d.mix * a * rasp, d.bias, d.gain_db * a}; };
        if (cls == 5 || cls == 6) dp = Dp{0, 1, 0, 0, 0};
        else if (cls == 3) {
            const Dp e = sdp(kExhDp[pick(1)], g_exh_amount.load()), i = sdp(kIntDp[pick(0)], g_int_amount.load());
            dp = Dp{e.k * 0.6f + i.k * 0.4f, e.drive > i.drive ? e.drive : i.drive, e.mix * 0.6f + i.mix * 0.4f, e.bias, e.gain_db * 0.6f + i.gain_db * 0.4f};
        } else dp = sdp((cls == 1 ? kExhDp : kIntDp)[tier], cls == 1 ? g_exh_amount.load() : g_int_amount.load());
        dp.k = clampf(dp.k, 0, 1.5f); dp.mix = clampf(dp.mix, 0, 1.2f); dp.gain_db = clampf(dp.gain_db, 0, 6);
    }
    const bool air_wanted = cls == 2 && !gran && !g_no_own_dsp && (g_fi_turbo.load() || g_fi_sc.load());   // the intake tracks carry the air hiss
    const bool dp_on = !g_no_own_dsp && !gran && (cls == 5 || cls == 6 || air_wanted || dp.k > 0 || dp.mix > 0 || dp.gain_db > 0);   // granular cars: the EQ only (their loading stuck with our own effect)
    fx.low = clampf(fx.low, 0, 10); fx.mid = clampf(fx.mid, 0, 10); fx.high = clampf(fx.high, 0, 10); fx.band = clampf(fx.band, 0, 10);
    fx.dist = clampf(fx.dist, 0, 0.6f); fx.echo_wet = clampf(fx.echo_wet, -80, -6);
    const bool nothing = !dp_on && fx.low == 0 && fx.mid == 0 && fx.high == 0 && fx.band == 0 && fx.echo_wet <= -80;

    const unsigned serial = g_cfg_serial.load();
    const bool known = g_groups.count(grp) != 0;
    Group& g = g_groups[grp];
    // Is our EQ still on this group? (A group's memory is reused for a new group when its event ends.)
    bool have = false;
    if (g.eq) {
        for (int i = 0; i < 8 && !have; ++i) {
            void* d = nullptr;
            if (fn<GetDspFn>(kGetDsp)(grp, i, &d) != 0) break;
            if (d == g.eq) have = true;
        }
    }
    if (!have) {
        (known ? g_grp_reuse : g_grp_new).fetch_add(1);
        if (nothing) { if (!g.eq) g_groups.erase(grp); return; }   // stock: the group stays untouched
        const uint64_t now = GetTickCount64();
        Group set = g;
        g = Group{};
        if (!set.eq && g_groups.size() > kMaxSets) {   // too many sets: take the one of the track that played longest ago
            void* vk = nullptr;
            uint64_t vl = now;
            for (auto& kv : g_groups)
                if (kv.first != grp && kv.second.eq && (kv.second.cls != 5 || now - kv.second.last > 600000) && kv.second.last < vl && now - kv.second.last > 3000) { vl = kv.second.last; vk = kv.first; }
            if (vk) { set = g_groups[vk]; g_groups.erase(vk); }
        }
        if (set.eq) {   // re-use the effects: they move to this group
            if (fn<AddDspFn>(kAddDsp)(grp, 0, set.eq) == 0) {
                g = set;
                if (g.eq2 && fn<AddDspFn>(kAddDsp)(grp, 0, g.eq2) != 0) g.eq2 = nullptr;
                if (g.echo && fn<AddDspFn>(kAddDsp)(grp, 0, g.echo) != 0) g.echo = nullptr;
                if (g.dist && fn<AddDspFn>(kAddDsp)(grp, 0, g.dist) != 0) g.dist = nullptr;
                if (g.cdsp && fn<AddDspFn>(kAddDsp)(grp, 0, g.cdsp) != 0) { g.cdsp = nullptr; g.cst = nullptr; }
                g.serial = 0;
                g_n_released.fetch_add(1);
            }
        }
        if (!g.eq) {
            void* eq = nullptr;
            int r = fn<CreateDspFn>(kCreateDsp)(sys, kDspThreeEq, &eq);
            if (r == 0 && eq) r = fn<AddDspFn>(kAddDsp)(grp, 0, eq);
            if (r != 0 || !eq) {
                if (g_n_err.fetch_add(1) < 5) say("FMOD would not add the EQ to a track (error %d)", r);
                return;
            }
            g.eq = eq;
            void* eq2 = nullptr;
            if (fn<CreateDspFn>(kCreateDsp)(sys, kDspThreeEq, &eq2) == 0 && eq2 && fn<AddDspFn>(kAddDsp)(grp, 0, eq2) == 0) g.eq2 = eq2;
        }
        g.cls = cls;
        const long n = g_n_groups.fetch_add(1) + 1;
        if (n <= 3 || n == 10 || n == 50) say("effect on %ld track(s) so far (this one: %s, level %d)", n, cls == 1 ? "exhaust" : (cls == 3 ? "granular synth" : (cls == 5 ? "turbo" : "engine bay / intake")), tier);
    } else if (g.serial == serial) {
        g.last = GetTickCount64();
        return;
    }
    g.last = GetTickCount64();
    g.serial = serial;
    g.cls = cls;
    const SetFloatFn setf = fn<SetFloatFn>(kSetFloat);
    setf(g.eq, 3, fx.x1);   // low/mid crossover
    setf(g.eq, 4, fx.x2);   // mid/high crossover
    setf(g.eq, 0, fx.low);
    setf(g.eq, 1, fx.mid);
    setf(g.eq, 2, fx.high);
    if (g.eq2) {
        setf(g.eq2, 3, fx.b1);
        setf(g.eq2, 4, fx.b2);
        setf(g.eq2, 0, 0);
        setf(g.eq2, 1, fx.band);
        setf(g.eq2, 2, 0);
    }
    if (!g_no_echo && fx.echo_wet > -80 && !g.echo) {
        void* e = nullptr;
        if (fn<CreateDspFn>(kCreateDsp)(sys, kDspEcho, &e) == 0 && e) {
            setf(e, 3, -80.0f);
            if (fn<AddDspFn>(kAddDsp)(grp, 0, e) == 0) g.echo = e;
        }
    }
    if (g.echo) {   // FMOD Echo: 0 delay ms (10..), 1 feedback %, 2 dry level dB, 3 wet level dB
        setf(g.echo, 0, clampf(fx.echo_ms, 10, cls == 5 ? 120 : 40));
        setf(g.echo, 1, fx.echo_wet > -80 ? fx.echo_fb : 0);
        setf(g.echo, 2, 0);
        setf(g.echo, 3, fx.echo_wet);
    }
    if (dp_on && !g.cdsp) {
        void* cd = nullptr;
        Dsp* st = make_dsp(sys, &cd);
        if (st && fn<AddDspFn>(kAddDsp)(grp, 0, cd) == 0) { g.cdsp = cd; g.cst = st; }
        else if (!st && g_n_err.fetch_add(1) < 5) say("FMOD would not make our own effect (the grit falls back to FMOD's distortion)");
    }
    if (g.cst && !g.cst->dead.load()) {
        g.cst->floor = cls == 2 ? 0.8f : (cls >= 5 ? 1.0f : 0.7f);
        g.cst->vol = vol;
        g.cst->kind = (cls == 5 || cls == 6) ? cls : 0;
        { static std::atomic<int> lg{0}; if ((cls == 5 || cls == 6) && lg.fetch_add(1) < 12) say("%s track set up: volume %.2f (turbo level %d, own effect %s)", cls == 5 ? "turbo" : "blow-off", vol, tier, g.cst ? "on" : "off"); }
        g.cst->air = air_wanted;
        g.cst->k = dp.k; g.cst->drive = dp.drive; g.cst->mix = dp.mix; g.cst->bias = dp.bias;
        g.cst->gain = std::pow(10.0f, dp.gain_db / 20.0f);
    } else if (dp_on && !g.cdsp && fx.dist > 0) {   // fallback: FMOD's own distortion, lightly
        if (!g.dist) {
            void* ds = nullptr;
            if (fn<CreateDspFn>(kCreateDsp)(sys, kDspDistortion, &ds) == 0 && ds && fn<AddDspFn>(kAddDsp)(grp, 0, ds) == 0) g.dist = ds;
        }
        if (g.dist) setf(g.dist, 0, fx.dist * 0.4f);
    }
}

float db2lin(float db) { return std::pow(10.0f, db / 20.0f); }
int synth_tier(int exh) { return eff_tier(exh); }
int detour_synth(void* st, float* in, float* out, unsigned length, int inch, int* outch) {
    Tm tm(g_st[S_SYNTH]);
    const int r = orig_timed([&] { return g_orig_synth(st, in, out, length, inch, outch); });
    int ch = outch ? *outch : inch;
    if (r != 0 || !out || length == 0 || length > 4096 || ch < 1 || ch > 8) return r;
    const int te = synth_tier(1), ti = synth_tier(0);
    if (te == 0 && ti == 0 && !(g_fi_turbo.load() && g_enabled.load() && (g_air.load() > 0.002f || g_whl_amp.load() > 1e-5f) && GetTickCount64() - g_air_tick.load() < 250)) return r;   // stock sound and no hiss to add: left alone
    const float ea = g_exh_amount.load(), ia = g_int_amount.load(), rasp = g_rasp.load() * 4.0f > 2.5f ? 2.5f : g_rasp.load() * 4.0f;
    const Fx &fe = kExhFx[te], &fi = kIntFx[ti];
    const Dp &de = kExhDp[te], &di = kIntDp[ti];
    auto clampf = [](float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); };
    Blk b;
    b.k     = clampf(de.k * ea * 0.6f + di.k * ia * 0.4f, 0, 1.5f);
    b.drive = de.drive > di.drive ? de.drive : di.drive;
    b.mix   = clampf((de.mix * ea * 0.6f + di.mix * ia * 0.4f) * rasp, 0, 1.2f);
    b.bias  = de.bias > di.bias ? de.bias : di.bias;
    b.gain  = db2lin(clampf(de.gain_db * ea * 0.6f + di.gain_db * ia * 0.4f, 0, 6));
    b.lg    = db2lin(clampf(fe.low * ea * 0.6f + fi.low * ia * 0.4f, 0, 10));
    b.mg    = db2lin(clampf((fe.mid + fe.band * 0.5f) * ea * 0.6f + (fi.mid + fi.band * 0.5f) * ia * 0.4f, 0, 10));
    b.hg    = db2lin(clampf(fe.high * ea * 0.6f + fi.high * ia * 0.4f, 0, 10));
    b.x1    = te ? fe.x1 : fi.x1;
    b.x2    = te ? fe.x2 : fi.x2;
    b.limit = 0.95f;
    {   // rawer under load, cleaner on a lift
        const float dyn = 0.7f + 0.3f * g_load.load();
        b.k = b.k * dyn; b.mix = b.mix * dyn; b.gain = 1.0f + (b.gain - 1.0f) * dyn;
        b.hg = 1.0f + (b.hg - 1.0f) * (0.5f + 0.5f * dyn);
    }
    Inst* slot = nullptr;
    const uint64_t now = GetTickCount64();
    for (Inst& s : g_inst)
        if (s.key == st) { slot = &s; break; }
    if (!slot) {   // a new synth: a free slot, else the one not used for the longest time (cars come and go)
        Inst* pick = &g_inst[0];
        for (Inst& s : g_inst) {
            if (!s.key) { pick = &s; break; }
            if (s.last < pick->last) pick = &s;
        }
        memset(pick->d.ef, 0, sizeof pick->d.ef); memset(pick->d.es, 0, sizeof pick->d.es); memset(pick->d.lp, 0, sizeof pick->d.lp);
        memset(pick->d.wl, 0, sizeof pick->d.wl); memset(pick->l1, 0, sizeof pick->l1); memset(pick->l2, 0, sizeof pick->l2);
        pick->key = st;
        slot = pick;
    }
    slot->last = now;
    if (!slot) return r;
    static std::atomic<int> logged{0};
    if (logged.fetch_add(1) == 0) say("granular engine synth is running through us (%d channel(s), %u frames per block)", ch, length);
    process_block(*slot, out, length, ch, b);
    return r;
}

// The sound graph is changed on our own thread, never inside the game's hook: the game thread may hold FMOD's lock (a car change) and an
// FMOD call made from the hook could wait for it forever. A request is dropped if it has waited too long (its group may be gone by then).
struct Req { void* sys; void* grp; int cls; bool gran; uint64_t t; };
std::mutex g_qm;
std::vector<Req> g_q;
void enqueue(void* sys, void* grp, int cls, bool gran) {
    std::scoped_lock lk{g_qm};
    if (g_q.size() >= 256) return;
    for (const Req& r : g_q) if (r.grp == grp) return;
    g_q.push_back(Req{sys, grp, cls, gran, GetTickCount64()});
}
void drain() {
    std::vector<Req> work;
    {
        std::scoped_lock lk{g_qm};
        work.swap(g_q);
    }
    for (const Req& r : work) {
        const uint64_t now = GetTickCount64();
        if (g_no_graph) continue;
        if (now - r.t > 150) continue;                                            // stale: the group may be gone
        if (now - g_hud_tick.load() > 1500 || now < g_busy_until.load()) continue;   // the game is loading
        std::unique_lock gl{g_gmu, std::try_to_lock};
        if (!gl.owns_lock()) continue;
        const auto t0 = std::chrono::steady_clock::now();
        apply(r.sys, r.grp, r.cls, r.gran);
        const long us = static_cast<long>(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0).count());
        g_st[S_APPLY].n.fetch_add(1); g_st[S_APPLY].own.fetch_add(us); st_max(g_st[S_APPLY].max_own, us);
        g_n_apply.fetch_add(1);
        long m = g_apply_max_us.load();
        while (us > m && !g_apply_max_us.compare_exchange_weak(m, us)) {}
    }
}

int detour_play(void* snd, void* sys, void* grp, bool paused, int a5, void** ch) {
    Tm tm(g_st[S_PLAY]);
    void* use = snd;
    if (g_bov_choice.load() > 0 && snd) {   // a chosen blow-off sound: play that one in place of the game's pick
        std::scoped_lock lk{g_mu};
        int row0 = -1;
        if (g_made.count(snd) && classify(snd, &row0) == 6) {   // only a sound we made ourselves know of (not a sub-sound of one)
            void* t = bov_pick(row0, g_bov_choice.load());
            if (t) use = t;
        }
    }
    const int r = orig_timed([&] { return g_orig_play(use, sys, grp, paused, a5, ch); });
    snd = use;
    g_n_play.fetch_add(1);
    if (r != 0 || !snd || !sys || !grp) return r;
    int row = -1, cls = 0;
    {
        std::scoped_lock lk{g_mu};
        cls = classify(snd, &row);
    }
    if (row >= 0 && (row == g_bank_pg.load() || row == g_bank_gs.load())) g_dc[cls].fetch_add(1);
    if (cls == 5 || cls == 6) { static std::atomic<int> lp{0}; if (lp.fetch_add(1) < 12) say("%s sample played (row %d)", cls == 5 ? "turbo" : "blow-off", row); }
    if (!cls) return r;
    if (!g_all_cars.load()) {
        const int pg = g_bank_pg.load(), gs = g_bank_gs.load();
        if ((pg >= 0 || gs >= 0) && row != pg && row != gs) return r;   // another car's sound (when the driven car's bank is known)
    }
    const bool gran = g_bank_gs.load() >= 0 || (row >= 0 && !strncmp(kBankTier[row].name, "GS_", 3));
    if (cls == 4 && !gran) {   // one of the game's backfire pops: with a better exhaust it is played once more (a "double crack", a little off in pitch)
        const int tier = eff_tier(1);
        static const float kP[4] = {0, 0.25f, 0.55f, 0.90f};
        static uint32_t rng = 0x9E3779B9u;
        static std::atomic<uint64_t> last{0};
        auto rnd = [&] { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return static_cast<float>(rng & 0xFFFF) / 65535.0f; };
        const uint64_t now = GetTickCount64();
        const float p = kP[tier] * g_pops.load() * g_exh_amount.load() * (0.7f + 0.6f * (1.0f - g_thr.load()));   // most on a lift
        if (p > 0 && now - last.load() > 45 && rnd() < p) {
            last.store(now);
            void* ch2 = nullptr;
            if (orig_timed([&] { return g_orig_play(snd, sys, grp, paused, a5, &ch2); }) == 0 && ch2) {
                fn<SetCtlFloatFn>(kSetPitch)(ch2, 0.90f + 0.20f * rnd());
                fn<SetCtlFloatFn>(kSetVolume)(ch2, 0.55f + 0.30f * rnd());
                static std::atomic<int> logged{0};
                if (logged.fetch_add(1) < 3) say("extra pop played (level %d)", tier);
            }
        }
    }
    if (cls == 4) {
        if (g_antilag) return r;   // the Anti-Lag mod owns the pop sound itself: its tone stays as it is
        cls = 1;                   // without Anti-Lag the pop gets the exhaust's tone like the rest of the exhaust
    }
    {   // no changes to the sound graph while the game loads (a car change): no HUD frames lately, or a burst of new sounds
        const uint64_t now = GetTickCount64();
        if (now - g_hud_tick.load() > 1500 || now < g_busy_until.load()) {
            static std::atomic<int> logged{0};
            if (logged.fetch_add(1) < 3) say("the game is loading: the sound graph is left alone for now");
            return r;
        }
    }
    enqueue(sys, grp, cls, gran);
    return r;
}

typedef int (*PlaySoundFn)(void*, void*, void*, bool, void**);
PlaySoundFn g_orig_play2 = nullptr;
std::atomic<int> g_play2_logged{0};

int detour_play2(void* sys, void* snd, void* grp, bool paused, void** ch) {
    Tm tm(g_st[S_PLAY2]);
    const int r = orig_timed([&] { return g_orig_play2(sys, snd, grp, paused, ch); });
    if (r != 0 || !snd || !sys || !grp) return r;
    bool user;
    {
        std::scoped_lock lk{g_mu};
        user = g_user.count(snd) != 0;
    }
    if (!user) return r;
    if (g_play2_logged.fetch_add(1) < 6) say("granular synth sound played into group %p", grp);
    enqueue(sys, grp, 3, true);
    return r;
}

// ---- hang dumps ----------------------------------------------------------------------------------------------------------------
// If the game freezes (its window stops responding, or nothing at all happens in the sound system for a long time after the world was
// running) a minidump with every thread's stack is written to fh3-mods\crashes, so the freeze can be traced to the thread and function.
typedef BOOL(WINAPI* MiniDumpFn)(HANDLE, DWORD, HANDLE, int, void*, void*, void*);
typedef BOOL(WINAPI* IsHungFn)(HWND);
struct FindWin { DWORD pid; HWND w; };
BOOL CALLBACK enum_win(HWND h, LPARAM lp) {
    auto* f = reinterpret_cast<FindWin*>(lp);
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (pid == f->pid && IsWindowVisible(h) && !GetWindow(h, GW_OWNER)) { f->w = h; return FALSE; }
    return TRUE;
}
void write_hang_dump(const char* why) {
    static int n = 0;
    if (n >= 4) return;
    ++n;
    HMODULE dh = LoadLibraryW(L"dbghelp.dll");
    auto md = dh ? reinterpret_cast<MiniDumpFn>(GetProcAddress(dh, "MiniDumpWriteDump")) : nullptr;
    if (!md) { say("hang detected (%s), but dbghelp is not available", why); return; }
    wchar_t path[MAX_PATH];
    _snwprintf(path, MAX_PATH, L"%ls\\fh3-mods\\crashes\\EngineSoundTiers-hang-%llu.dmp", g_api->azimuth_dir, static_cast<unsigned long long>(GetTickCount64() / 1000));
    HANDLE f = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) { say("hang detected (%s), but the dump file could not be created", why); return; }
    // MiniDumpWithThreadInfo (0x1000) | MiniDumpWithIndirectlyReferencedMemory (0x40) | MiniDumpWithUnloadedModules (0x20)
    const BOOL ok = md(GetCurrentProcess(), GetCurrentProcessId(), f, 0x1000 | 0x40 | 0x20, nullptr, nullptr, nullptr);
    CloseHandle(f);
    say("hang detected (%s): %s written to fh3-mods\\crashes (EngineSoundTiers-hang-*.dmp)", why, ok ? "a dump was" : "a dump could NOT be");
}

DWORD WINAPI hang_watch(LPVOID) {
    uint64_t next_hang = GetTickCount64() + 1000, last_act = GetTickCount64();
    long hc = 0;
    uint64_t hh = 0;
    int hung_n = 0;
    bool armed = true;
    HMODULE u32 = GetModuleHandleW(L"user32.dll");
    auto is_hung = u32 ? reinterpret_cast<IsHungFn>(GetProcAddress(u32, "IsHungAppWindow")) : nullptr;
    FindWin fw{GetCurrentProcessId(), nullptr};
    for (;;) {
        Sleep(500);
        const uint64_t tn = GetTickCount64();
        if (tn >= next_hang) {
            next_hang = tn + 1000;
            const long c = g_n_create.load() + g_n_play.load();
            if (c != hc || g_hud_tick.load() != hh) { hc = c; hh = g_hud_tick.load(); last_act = tn; armed = true; }
            if (!fw.w || !IsWindow(fw.w)) { fw.w = nullptr; EnumWindows(enum_win, reinterpret_cast<LPARAM>(&fw)); }
            const bool hung = is_hung && fw.w && is_hung(fw.w);
            hung_n = hung ? hung_n + 1 : 0;
            if (armed && hung_n >= 8) { armed = false; write_hang_dump("the game window is not responding"); }
            else if (armed && hh != 0 && tn - last_act > 90000) { armed = false; write_hang_dump("no sound activity for 90 s after the world was running"); }
        }
    }
    return 0;
}

// ---- start ---------------------------------------------------------------------------------------------------------------
DWORD WINAPI worker(LPVOID) {
    char* base = static_cast<char*>(g_api->wait_for_code(180000));
    if (!base) { say("the game was not mapped in time: doing nothing"); return 0; }
    g_game = base;
    g_antilag_mod = GetModuleHandleW(L"AntiLag.dll");
    g_antilag = g_antilag_mod != nullptr;
    say(g_antilag ? "the Anti-Lag mod is installed: its pops are left as they are" : "the Anti-Lag mod is not installed: the game's pops get the exhaust tone");
    for (const Sig& s : kSigs) {
        unsigned char got[6] = {};
        // Another mod may already have hooked this function (the platform's patch is a jump): that is not "another game build".
        if (!rd(base + s.rva, got, 6) || (memcmp(got, s.b, 6) != 0 && !(got[0] == 0xE9 || got[0] == 0xEB || (got[0] == 0xFF && got[1] == 0x25)))) {
            say("the game's code at +0x%llX is not what this mod expects (another game build?): doing nothing", static_cast<unsigned long long>(s.rva));
            return 0;
        }
        if (memcmp(got, s.b, 6) != 0) say("the game's code at +0x%llX is already patched by another mod (%02X %02X ...): hooking it through the platform", static_cast<unsigned long long>(s.rva), got[0], got[1]);
    }
    // Diagnostic switch (EngineSoundTiers.ini, Skip=): a sum of 1 createSound, 2 Studio play, 4 playSound, 8 synth, 16 HUD hooks to leave out
    // (to find which part of this mod a freeze comes from); 32 = make no changes to the sound graph, 64 = no own effect, 128 = no echo.
    const int skip = ini_int(L"Skip", 0);
    if (skip) say("diagnostic: Skip=%d (hooks left out: %s%s%s%s%s%s)", skip, skip & 1 ? "createSound " : "", skip & 2 ? "studio-play " : "", skip & 4 ? "playSound " : "",
                  skip & 8 ? "synth " : "", skip & 16 ? "HUD " : "", skip & 32 ? "no-graph" : "");
    g_no_graph = (skip & 32) != 0;
    g_no_own_dsp = (skip & 64) != 0;
    g_no_echo = (skip & 128) != 0;
    auto hk = [&](int bit, const char* nm, uintptr_t rva, void* det, void** orig) -> FH3Hook* {
        if (skip & bit) return reinterpret_cast<FH3Hook*>(1);
        return g_api->hook_function(g_mod, nm, base + rva, det, orig);
    };
    FH3Hook* h1 = hk(1, "FMOD System::createSound (Engine Sound Tiers: which sample is exhaust / intake)", kCreateSound, reinterpret_cast<void*>(&detour_create), reinterpret_cast<void**>(&g_orig_create));
    FH3Hook* h2 = hk(2, "Studio sound instrument play (Engine Sound Tiers: the effect)", kStudioPlay, reinterpret_cast<void*>(&detour_play), reinterpret_cast<void**>(&g_orig_play));
    FH3Hook* h4 = hk(4, "FMOD System::playSound (Engine Sound Tiers: the granular engine synth)", kPlaySound, reinterpret_cast<void*>(&detour_play2), reinterpret_cast<void**>(&g_orig_play2));
    FH3Hook* h5 = hk(8, "PG Engine Synth read (Engine Sound Tiers: granular engines)", kSynthRead, reinterpret_cast<void*>(&detour_synth), reinterpret_cast<void**>(&g_orig_synth));
    FH3Hook* h3 = hk(16, "HUD controller (Engine Sound Tiers: the driven car's parts)", kHud, reinterpret_cast<void*>(&detour_hud), reinterpret_cast<void**>(&g_orig_hud));
    if (!h1 || !h2 || !h3 || !h4 || !h5 || !g_api->enable_hooks(g_mod, nullptr)) {
        say("could not place the hooks (see the lines above): doing nothing");
        g_api->disable_hooks(g_mod, nullptr);
        return 0;
    }
    say("ready: %d car sound banks known; exhaust and intake sound now follow the car's parts", kBanks);
    // watchdog: while the game is busy loading (no HUD line then) say what the sound system is doing, so a slow load can be traced
    long c0 = 0, p0 = 0, a0 = 0;
    uint64_t next = GetTickCount64() + 4000;
    CreateThread(nullptr, 0, hang_watch, nullptr, 0, nullptr);   // its own thread: this one may be the one that is stuck
    for (;;) {
        Sleep(5);
        drain();
        if (GetTickCount64() < next) continue;
        next += 4000;
        const long c = g_n_create.load(), p = g_n_play.load(), a = g_n_apply.load();
        if (c - c0 > 150) g_busy_until = GetTickCount64() + 2500;
        if (c - c0 > 300 || a - a0 > 300) {
            say("busy: %ld sounds created, %ld played, %ld effect set-ups in the last 4 s (slowest set-up so far %.1f ms)", c - c0, p - p0, a - a0, g_apply_max_us.load() / 1000.0);
            g_apply_max_us = 0;
        }
        c0 = c; p0 = p; a0 = a;
    }
    return 0;
}

} // namespace

FH3MOD_EXPORT int fh3mod_init(const FH3ModApi* api) {
    if (api->api_version < 9) return 1;
    g_api = api;
    FH3ModInfo info{};
    info.size             = sizeof info;
    info.name             = "EngineSoundTiers";
    info.version          = ENGINESOUND_VERSION;
    info.github_repo      = nullptr;
    info.platform_version = FH3MOD_PLATFORM_VERSION;
    info.check_updates    = 0;
    info.description      = "The engine sound follows the car's intake and exhaust upgrades, as in Forza Horizon 6: louder, more open and raspier with every level.";
    g_mod                 = api->register_mod(&info);
    if (!g_mod) return 1;

    _snwprintf(g_ini, MAX_PATH, L"%ls\\fh3-mods\\EngineSoundTiers.ini", api->azimuth_dir);
    g_enabled    = ini_int(L"Enabled", 1);
    g_all_cars   = ini_int(L"AllCars", 0);
    g_exh_amount = static_cast<float>(ini_int(L"ExhaustAmount", 100)) / 100.0f;
    g_int_amount = static_cast<float>(ini_int(L"IntakeAmount", 100)) / 100.0f;
    g_rasp       = static_cast<float>(std::min(35, std::max(0, ini_int(L"Rasp", 25)))) / 100.0f;
    g_pipe       = static_cast<float>(std::min(200, std::max(0, ini_int(L"PipeAmount", ini_int(L"Pipe", 1) ? 100 : 0)))) / 100.0f;
    g_def.ea = ini_int(L"ExhaustAmount", 100); g_def.ia = ini_int(L"IntakeAmount", 100); g_def.rasp = std::min(35, std::max(0, ini_int(L"Rasp", 25)));
    g_def.pipe = std::min(200, std::max(0, ini_int(L"PipeAmount", ini_int(L"Pipe", 1) ? 100 : 0))); g_def.pops = ini_int(L"Pops", 100);
    sel_load();
    cfg_apply(g_def);

    FH3Setting s{};
    s.size = sizeof s;
    s.key = "Enabled"; s.label = "Engine Sound Tiers";
    s.description = "The exhaust and intake sound change with the car's exhaust and intake upgrades. A change is heard after the car's sound is loaded again (drive off, or switch cars).";
    s.value = g_enabled.load(); s.on_change = on_change;
    api->add_setting(g_mod, &s);

    if (api->api_version >= 17) {
        // Tune Car > Engine tab: sliders only
        static const FH3TuningRow rows[] = {
            {sizeof(FH3TuningRow), "SndExh", "Engine sound|Exhaust sound type (0-4)",
             "0 = as installed on the car (default), 1 = Stock, 2 = Street, 3 = Sport, 4 = Race. Forces that exhaust sound on this car, e.g. a loud one on a weak car. Heard after the car's sound is loaded again (drive off, or switch cars).",
             0, 4, 1, 0, [](void*) { return sel_get(0); }, [](void*, float v) { sel_set(0, static_cast<int>(v + 0.5f)); }, nullptr, nullptr, "Engine"},
            {sizeof(FH3TuningRow), "SndInt", "Engine sound|Intake sound type (0-4)",
             "0 = as installed on the car (default), 1 = Stock, 2 = Street, 3 = Sport, 4 = Race. Forces that intake sound on this car. Heard after the car's sound is loaded again.",
             0, 4, 1, 0, [](void*) { return sel_get(1); }, [](void*, float v) { sel_set(1, static_cast<int>(v + 0.5f)); }, nullptr, nullptr, "Engine"},
            {sizeof(FH3TuningRow), "ExhaustAmount", "Engine sound|Exhaust intensity (%)",
             "How strongly the exhaust level changes the sound. 100 is the mod's own setting, 0 is none, 200 is double.",
             0, 200, 5, 0, [](void*) { return sel_get(2); }, [](void*, float v) { sel_set(2, static_cast<int>(v + 0.5f)); }, nullptr, nullptr, "Engine"},
            {sizeof(FH3TuningRow), "IntakeAmount", "Engine sound|Intake intensity (%)",
             "How strongly the intake level changes the sound. 100 is the mod's own setting, 0 is none, 200 is double.",
             0, 200, 5, 0, [](void*) { return sel_get(3); }, [](void*, float v) { sel_set(3, static_cast<int>(v + 0.5f)); }, nullptr, nullptr, "Engine"},
            {sizeof(FH3TuningRow), "Rasp", "Engine sound|Rasp / grit (%)",
             "How much grit the higher levels add on top of the open, bright tone. 0 is clean, 35 is the most. It follows throttle and revs.",
             0, 35, 1, 0, [](void*) { return sel_get(4); }, [](void*, float v) { sel_set(4, static_cast<int>(v + 0.5f)); }, nullptr, nullptr, "Engine"},
            {sizeof(FH3TuningRow), "PipeAmount", "Engine sound|Pipe resonance (%)",
             "The hollow ring of a freer exhaust pipe on the Sport and Race exhaust. 0 is none, 100 is the mod's own, 200 is double.",
             0, 200, 5, 0, [](void*) { return sel_get(5); }, [](void*, float v) { sel_set(5, static_cast<int>(v + 0.5f)); }, nullptr, nullptr, "Engine"},
            {sizeof(FH3TuningRow), "TurboAmount", "Engine sound|Turbo / supercharger volume (%)",
             "The game's own turbo sound, and the supercharger whine the mod adds (faint on a stock blower, higher, louder and richer with every level). 0 is silent, 100 is the mod's own, 200 is about three times.",
             0, 200, 5, 0, [](void*) { return sel_get(7); }, [](void*, float v) { sel_set(7, static_cast<int>(v + 0.5f)); }, nullptr, nullptr, "Engine"},
            {sizeof(FH3TuningRow), "BovAmount", "Engine sound|Blow-off valve volume (%)",
             "The blow-off / flutter after you lift off the throttle. 0 turns it off, 100 is the mod's own, 200 is double.",
             0, 200, 5, 0, [](void*) { return sel_get(9); }, [](void*, float v) { sel_set(9, static_cast<int>(v + 0.5f)); }, nullptr, nullptr, "Engine"},
            {sizeof(FH3TuningRow), "BovSound", "Engine sound|Blow-off valve sound (0-15)",
             "0 = the game's own (a random one each time). 1 and up = always the same one of this car's blow-off sounds (the number is limited to how many it has). Heard after the car's sound is loaded again.",
             0, 15, 1, 0, [](void*) { return sel_get(10); }, [](void*, float v) { sel_set(10, static_cast<int>(v + 0.5f)); }, nullptr, nullptr, "Engine"},
            {sizeof(FH3TuningRow), "AirAmount", "Engine sound|Intake air hiss (%)",
             "The hiss of air being sucked in by a turbo or supercharger: it grows with throttle and revs, and with a better forced induction part. Cars without forced induction have none. 0 is none, 100 is the mod's own, 200 is double.",
             0, 200, 5, 0, [](void*) { return sel_get(8); }, [](void*, float v) { sel_set(8, static_cast<int>(v + 0.5f)); }, nullptr, nullptr, "Engine"},
            {sizeof(FH3TuningRow), "Pops", "Engine sound|Extra exhaust pops (%)",
             "How much more the exhaust pops and crackles on a lift with better exhaust parts. 0 is none, 200 is double.",
             0, 200, 5, 0, [](void*) { return sel_get(6); }, [](void*, float v) { sel_set(6, static_cast<int>(v + 0.5f)); }, nullptr, nullptr, "Engine"},
        };
        for (const FH3TuningRow& r : rows) api->add_tuning_row(g_mod, &r);
    } else {
        FH3Setting e{};
        e.size = sizeof e;
        e.key = "ExhaustAmount"; e.label = "Exhaust effect (%)";
        e.kind = FH3_SETTING_SLIDER; e.min = 0; e.max = 200; e.step = 10; e.slider_value = g_exh_amount.load() * 100.0f; e.on_slide = on_slide;
        api->add_setting(g_mod, &e);
        FH3Setting i{};
        i.size = sizeof i;
        i.key = "IntakeAmount"; i.label = "Intake effect (%)";
        i.kind = FH3_SETTING_SLIDER; i.min = 0; i.max = 200; i.step = 10; i.slider_value = g_int_amount.load() * 100.0f; i.on_slide = on_slide;
        api->add_setting(g_mod, &i);
    }

    FH3Setting a{};
    a.size = sizeof a;
    a.key = "AllCars"; a.label = "Effect on every car";
    a.description = "Off: only cars with the same engine sound as yours get your levels. On: every car on the road does.";
    a.value = g_all_cars.load(); a.on_change = on_change;
    api->add_setting(g_mod, &a);

    g_thread = CreateThread(nullptr, 0, worker, nullptr, 0, nullptr);
    return 0;
}

FH3MOD_EXPORT void fh3mod_shutdown(void) {
    if (g_api && g_mod) g_api->disable_hooks(g_mod, nullptr);
    if (g_thread) {
        WaitForSingleObject(g_thread, 5000);
        CloseHandle(g_thread);
        g_thread = nullptr;
    }
}
