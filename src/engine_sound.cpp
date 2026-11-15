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

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <unordered_map>

namespace {

struct BankTier { uint32_t off, len; uint64_t exh[8], eng[8]; const char* name; };
struct EngineMap { const char* media; const char* engine; const char* granular; };
#include "bank_tiers.inc"
#include "engine_map.inc"
constexpr int kBanks = sizeof kBankTier / sizeof kBankTier[0];

const FH3ModApi* g_api = nullptr;
FH3Mod* g_mod          = nullptr;
char* g_game           = nullptr;
HANDLE g_thread        = nullptr;

// ---- game addresses (RVA) -------------------------------------------------------------------------------------------
constexpr uintptr_t kCreateSound = 0x27BE3F0;   // FMOD::System::createSound(sys, name, mode, exinfo, sound**)
constexpr uintptr_t kStudioPlay  = 0x29823B0;   // Studio's sound-instrument player (snd, sys, group, paused, a5, channel**)
constexpr uintptr_t kPlaySound   = 0x27BFA40;   // FMOD::System::playSound(sys, sound, group, paused, channel**)
constexpr uintptr_t kCreateDsp   = 0x27BE190;   // FMOD::System::createDSPByType(sys, type, dsp**)
constexpr uintptr_t kAddDsp      = 0x27C1910;   // FMOD::ChannelControl::addDSP(cc, index, dsp)
constexpr uintptr_t kGetDsp      = 0x27C1C30;   // FMOD::ChannelControl::getDSP(cc, index, dsp**)
constexpr uintptr_t kSetFloat    = 0x27C5F10;   // FMOD::DSP::setParameterFloat(dsp, index, value)
constexpr uintptr_t kHud         = 0xB26690;    // HUD speedometer controller update: the player's Car at +0x120
constexpr size_t kCarExhaust = 0x7120, kCarIntake = 0x7124, kCarName = 0x118;

constexpr int kDspEcho = 6, kDspDistortion = 9, kDspThreeEq = 28;   // FMOD_DSP_TYPE of FMOD 1.08 (the game's: 1.08.04)

struct Sig { uintptr_t rva; unsigned char b[6]; };
const Sig kSigs[] = {
    {kCreateSound, {0x40, 0x53, 0x55, 0x56, 0x57, 0x41}}, {kStudioPlay, {0x48, 0x89, 0x5c, 0x24, 0x10, 0x48}},
    {kPlaySound, {0x40, 0x53, 0x55, 0x56, 0x57, 0x41}}, {kCreateDsp, {0x40, 0x53, 0x55, 0x56, 0x57, 0x41}},   {kAddDsp, {0x40, 0x53, 0x55, 0x56, 0x57, 0x41}},
    {kGetDsp, {0x40, 0x53, 0x55, 0x56, 0x57, 0x41}},      {kSetFloat, {0x40, 0x53, 0x55, 0x56, 0x48, 0x81}},
    {kHud, {0x40, 0x57, 0x48, 0x83, 0xec, 0x20}},
};

typedef int (*CreateSoundFn)(void*, const char*, unsigned, unsigned char*, void**);
typedef int (*StudioPlayFn)(void*, void*, void*, bool, int, void**);
typedef int (*CreateDspFn)(void*, int, void**);
typedef int (*AddDspFn)(void*, int, void*);
typedef int (*GetDspFn)(void*, int, void**);
typedef int (*SetFloatFn)(void*, int, float);
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
std::atomic<int> g_preview{0};          // 0 = the car's own parts, 1..4 = force Stock / Street / Sport / Race on both
std::atomic<float> g_exh_amount{1.0f};  // 0 .. 2
std::atomic<float> g_int_amount{1.0f};
std::atomic<float> g_rasp{0.25f};
std::atomic<int> g_pipe{1};             // pipe resonance (short echo) on the Sport / Race exhaust        // 0 .. 2, scales the distortion (the rasp) of every level
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
    else if (!strcmp(key, "Preview")) { g_preview = value; ini_set(L"Preview", value); }
    else if (!strcmp(key, "Pipe")) { g_pipe = value; ini_set(L"Pipe", value); }
    else if (!strcmp(key, "AllCars")) { g_all_cars = value; ini_set(L"AllCars", value); }
    g_cfg_serial.fetch_add(1);
}
void on_slide(void*, const char* key, float value) {
    if (!strcmp(key, "ExhaustAmount")) { g_exh_amount = value / 100.0f; ini_set(L"ExhaustAmount", static_cast<int>(value + 0.5f)); }
    else if (!strcmp(key, "Rasp")) { g_rasp = value / 100.0f; ini_set(L"Rasp", static_cast<int>(value + 0.5f)); }
    else if (!strcmp(key, "IntakeAmount")) { g_int_amount = value / 100.0f; ini_set(L"IntakeAmount", static_cast<int>(value + 0.5f)); }
    g_cfg_serial.fetch_add(1);
}

// ---- the driven car: its levels and its sound bank ----------------------------------------------------------------------
std::atomic<int> g_exh_tier{0}, g_int_tier{0};
std::atomic<int> g_bank_pg{-1}, g_bank_gs{-1};
std::atomic<long> g_dc[3]{};   // diagnostics for the driven car: plays classified as other / exhaust / engine bay
std::atomic<uint64_t> g_dnext{0};
std::atomic<long> g_n_groups{0}, g_n_err{0};   // rows of kBankTier of the driven car's banks, -1 = not known

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
    auto tier = [](int32_t v) { if (v < 0) return 0; const int t = v % 1000; return t > 3 ? 3 : t; };
    g_exh_tier = tier(ex);
    g_int_tier = tier(in);
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
}

uint64_t detour_hud(void* c, void* a, void* b, void* d) {
    const uint64_t r = g_orig_hud(c, a, b, d);
    static std::atomic<uint64_t> next{0};
    const uint64_t now = GetTickCount64();
    if (now < next.load()) return r;
    next.store(now + 500);
    {
        const uint64_t dn = g_dnext.load();
        if (dn && now > dn) {
            g_dnext = now + 60000;
            say("sounds of the driven car so far: exhaust %ld, engine bay %ld, other %ld (tracks with effect: %ld)", g_dc[1].load(), g_dc[2].load(), g_dc[0].load(), g_n_groups.load());
        }
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

std::unordered_map<void*, int> g_user;   // sounds that are fed by a PCM callback (the granular engine synth), guarded by g_mu
std::atomic<int> g_user_logged{0};

int detour_create(void* sys, const char* name, unsigned mode, unsigned char* ex, void** out) {
    const int r = g_orig_create(sys, name, mode, ex, out);
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
    std::scoped_lock lk{g_mu};
    if (row < 0) g_made.erase(*out);
    else {
        if (g_made.size() > 60000) g_made.clear();
        g_made[*out] = Made{row, idx};
    }
    return r;
}

// 0 = neither, 1 = exhaust, 2 = engine bay; *row = the bank
int classify(void* snd, int* row) {
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
    if (b.exh[m.idx >> 6] >> (m.idx & 63) & 1) return 1;
    if (b.eng[m.idx >> 6] >> (m.idx & 63) & 1) return 2;
    return 0;
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
// Exhaust: stock; sport rear muffler; sport rear + middle muffler; open straight-through system.
const Fx kExhFx[4] = {
    {0, 0, 0, 250, 2500, 0, 700, 1800, 12, -80, 0, 0},
    {1.0f, 1.5f, 1.5f, 250, 2500, 1.5f, 700, 1800, 12, -80, 0, 0},              // a little louder and less muffled
    {1.5f, 2.5f, 2.5f, 230, 2500, 3.0f, 600, 2000, 13, -19, 22, 0.10f},        // bark: upper mids open up, the pipe starts to ring
    {2.5f, 3.0f, 4.5f, 210, 2600, 5.0f, 500, 2200, 11, -13, 32, 0.40f}};       // open pipe: loud, raspy band, hollow ring, bright
// Intake (the engine-bay layer): stock; sport panel filter; cone filter; ITB / mesh.
const Fx kIntFx[4] = {
    {0, 0, 0, 250, 2500, 0, 250, 700, 10, -80, 0, 0},
    {0, 0.5f, 1.0f, 250, 3000, 2.0f, 250, 700, 10, -80, 0, 0},                 // a hint of induction growl
    {0.5f, 1.0f, 1.5f, 250, 3000, 4.0f, 250, 900, 10, -80, 0, 0},              // cone filter: clear induction growl
    {1.0f, 2.0f, 3.0f, 250, 2800, 6.0f, 300, 1200, 10, -80, 0, 0.20f}};        // ITB: loud bark, wider and higher

struct Group { void* eq = nullptr; void* eq2 = nullptr; void* echo = nullptr; void* dist = nullptr; unsigned serial = 0; };
std::mutex g_gmu;
std::unordered_map<void*, Group> g_groups;   // track channel groups that carry our effect

template <class F> F fn(uintptr_t rva) { return reinterpret_cast<F>(g_game + rva); }

void apply(void* sys, void* grp, int cls) {
    const int pv = g_preview.load();
    auto pick = [&](int exh) {
        int t = exh ? g_exh_tier.load() : g_int_tier.load();
        if (pv >= 1 && pv <= 4) t = pv - 1;
        if (!g_enabled.load()) t = 0;
        return t;
    };
    auto scaled = [&](const Fx& f, float a) {
        Fx r = f;
        r.low *= a; r.mid *= a; r.high *= a; r.band *= a;
        r.dist *= a * g_rasp.load();
        if (a <= 0.01f || !g_pipe.load()) r.echo_wet = -80;
        else if (r.echo_wet > -80) r.echo_wet += 8.0f * (a - 1.0f);   // amount 200% = 8 dB more ring, 50% = 4 dB less
        return r;
    };
    int tier;
    Fx fx;
    if (cls == 3) {   // a granular engine's synthesised sound: exhaust and intake in one track -> the exhaust's shape, some of the intake's tilt
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
    fx.low = clampf(fx.low, 0, 10); fx.mid = clampf(fx.mid, 0, 10); fx.high = clampf(fx.high, 0, 10); fx.band = clampf(fx.band, 0, 10);
    fx.dist = clampf(fx.dist, 0, 0.6f); fx.echo_wet = clampf(fx.echo_wet, -80, -6);
    const bool nothing = fx.low == 0 && fx.mid == 0 && fx.high == 0 && fx.band == 0 && fx.dist == 0 && fx.echo_wet <= -80;

    const unsigned serial = g_cfg_serial.load();
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
        g = Group{};
        if (nothing) return;   // stock: the group stays untouched
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
        const long n = g_n_groups.fetch_add(1) + 1;
        if (n <= 3 || n == 10 || n == 50) say("effect on %ld track(s) so far (this one: %s, level %d)", n, cls == 1 ? "exhaust" : (cls == 3 ? "granular synth" : "engine bay / intake"), tier);
    } else if (g.serial == serial)
        return;
    g.serial = serial;
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
    if (fx.echo_wet > -80 && !g.echo) {
        void* e = nullptr;
        if (fn<CreateDspFn>(kCreateDsp)(sys, kDspEcho, &e) == 0 && e) {
            setf(e, 3, -80.0f);
            if (fn<AddDspFn>(kAddDsp)(grp, 0, e) == 0) g.echo = e;
        }
    }
    if (g.echo) {   // FMOD Echo: 0 delay ms (10..), 1 feedback %, 2 dry level dB, 3 wet level dB
        setf(g.echo, 0, clampf(fx.echo_ms, 10, 40));
        setf(g.echo, 1, fx.echo_wet > -80 ? fx.echo_fb : 0);
        setf(g.echo, 2, 0);
        setf(g.echo, 3, fx.echo_wet);
    }
    if (fx.dist > 0 && !g.dist) {
        void* ds = nullptr;
        if (fn<CreateDspFn>(kCreateDsp)(sys, kDspDistortion, &ds) == 0 && ds) {
            setf(ds, 0, fx.dist);
            if (fn<AddDspFn>(kAddDsp)(grp, 0, ds) == 0) g.dist = ds;
        }
    }
    if (g.dist) setf(g.dist, 0, fx.dist);
}

int detour_play(void* snd, void* sys, void* grp, bool paused, int a5, void** ch) {
    const int r = g_orig_play(snd, sys, grp, paused, a5, ch);
    if (r != 0 || !snd || !sys || !grp) return r;
    int row = -1, cls = 0;
    {
        std::scoped_lock lk{g_mu};
        cls = classify(snd, &row);
    }
    if (row >= 0 && (row == g_bank_pg.load() || row == g_bank_gs.load())) g_dc[cls].fetch_add(1);
    if (!cls) return r;
    if (!g_all_cars.load()) {
        const int pg = g_bank_pg.load(), gs = g_bank_gs.load();
        if ((pg >= 0 || gs >= 0) && row != pg && row != gs) return r;   // another car's sound (when the driven car's bank is known)
    }
    // FMOD is called below: never wait for our own lock while the game may hold FMOD's
    std::unique_lock gl{g_gmu, std::try_to_lock};
    if (gl.owns_lock()) apply(sys, grp, cls);
    return r;
}

typedef int (*PlaySoundFn)(void*, void*, void*, bool, void**);
PlaySoundFn g_orig_play2 = nullptr;
std::atomic<int> g_play2_logged{0};

int detour_play2(void* sys, void* snd, void* grp, bool paused, void** ch) {
    const int r = g_orig_play2(sys, snd, grp, paused, ch);
    if (r != 0 || !snd || !sys || !grp) return r;
    bool user;
    {
        std::scoped_lock lk{g_mu};
        user = g_user.count(snd) != 0;
    }
    if (!user) return r;
    if (g_play2_logged.fetch_add(1) < 6) say("granular synth sound played into group %p", grp);
    std::unique_lock gl{g_gmu, std::try_to_lock};
    if (gl.owns_lock()) apply(sys, grp, 3);
    return r;
}

// ---- start ---------------------------------------------------------------------------------------------------------------
DWORD WINAPI worker(LPVOID) {
    char* base = static_cast<char*>(g_api->wait_for_code(180000));
    if (!base) { say("the game was not mapped in time: doing nothing"); return 0; }
    g_game = base;
    for (const Sig& s : kSigs) {
        unsigned char got[6] = {};
        if (!rd(base + s.rva, got, 6) || memcmp(got, s.b, 6) != 0) {
            say("the game's code at +0x%llX is not what this mod expects (another game build?): doing nothing", static_cast<unsigned long long>(s.rva));
            return 0;
        }
    }
    FH3Hook* h1 = g_api->hook_function(g_mod, "FMOD System::createSound (Engine Sound Tiers: which sample is exhaust / intake)", base + kCreateSound,
                                       reinterpret_cast<void*>(&detour_create), reinterpret_cast<void**>(&g_orig_create));
    FH3Hook* h2 = g_api->hook_function(g_mod, "Studio sound instrument play (Engine Sound Tiers: the effect)", base + kStudioPlay,
                                       reinterpret_cast<void*>(&detour_play), reinterpret_cast<void**>(&g_orig_play));
    FH3Hook* h4 = g_api->hook_function(g_mod, "FMOD System::playSound (Engine Sound Tiers: the granular engine synth)", base + kPlaySound,
                                       reinterpret_cast<void*>(&detour_play2), reinterpret_cast<void**>(&g_orig_play2));
    FH3Hook* h3 = g_api->hook_function(g_mod, "HUD controller (Engine Sound Tiers: the driven car's parts)", base + kHud,
                                       reinterpret_cast<void*>(&detour_hud), reinterpret_cast<void**>(&g_orig_hud));
    if (!h1 || !h2 || !h3 || !h4 || !g_api->enable_hooks(g_mod, nullptr)) {
        say("could not place the hooks (see the lines above): doing nothing");
        g_api->disable_hooks(g_mod, nullptr);
        return 0;
    }
    say("ready: %d car sound banks known; exhaust and intake sound now follow the car's parts", kBanks);
    return 0;
}

} // namespace

FH3MOD_EXPORT int fh3mod_init(const FH3ModApi* api) {
    if (api->api_version < 9) return 1;   // choice rows are API 9
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
    g_preview    = ini_int(L"Preview", 0);
    g_all_cars   = ini_int(L"AllCars", 0);
    g_exh_amount = static_cast<float>(ini_int(L"ExhaustAmount", 100)) / 100.0f;
    g_int_amount = static_cast<float>(ini_int(L"IntakeAmount", 100)) / 100.0f;
    g_rasp       = static_cast<float>(ini_int(L"Rasp", 25)) / 100.0f;
    g_pipe       = ini_int(L"Pipe", 1);

    FH3Setting s{};
    s.size = sizeof s;
    s.key = "Enabled"; s.label = "Engine Sound Tiers";
    s.description = "The exhaust and intake sound change with the car's exhaust and intake upgrades. A change is heard after the car's sound is loaded again (drive off, or switch cars).";
    s.value = g_enabled.load(); s.on_change = on_change;
    api->add_setting(g_mod, &s);

    static const char* const kPreview[] = {"The car's own parts", "Stock", "Street", "Sport", "Race"};
    FH3Setting p{};
    p.size = sizeof p;
    p.key = "Preview"; p.label = "Sound level";
    p.description = "Which level you hear. \"The car's own parts\" follows the fitted intake and exhaust; the others force that level on both, to listen to it without buying parts.";
    p.kind = FH3_SETTING_CHOICE; p.value = g_preview.load(); p.on_change = on_change;
    p.choices = kPreview; p.choice_count = 5;
    api->add_setting(g_mod, &p);

    FH3Setting e{};
    e.size = sizeof e;
    e.key = "ExhaustAmount"; e.label = "Exhaust effect (%)";
    e.description = "How strongly the exhaust level changes the sound. 100 is the mod's own setting, 0 is none, 200 is double.";
    e.kind = FH3_SETTING_SLIDER; e.min = 0; e.max = 200; e.step = 10; e.slider_value = g_exh_amount.load() * 100.0f; e.on_slide = on_slide;
    api->add_setting(g_mod, &e);

    FH3Setting i{};
    i.size = sizeof i;
    i.key = "IntakeAmount"; i.label = "Intake effect (%)";
    i.description = "How strongly the intake level changes the sound. 100 is the mod's own setting, 0 is none, 200 is double.";
    i.kind = FH3_SETTING_SLIDER; i.min = 0; i.max = 200; i.step = 10; i.slider_value = g_int_amount.load() * 100.0f; i.on_slide = on_slide;
    api->add_setting(g_mod, &i);

    FH3Setting rs{};
    rs.size = sizeof rs;
    rs.key = "Rasp"; rs.label = "Rasp / grit (%)";
    rs.description = "How much grit the higher levels add on top of the open, bright tone. 0 is none (clean), 100 is the mod's full grit, 200 is double. Too much sounds distorted.";
    rs.kind = FH3_SETTING_SLIDER; rs.min = 0; rs.max = 200; rs.step = 5; rs.slider_value = g_rasp.load() * 100.0f; rs.on_slide = on_slide;
    api->add_setting(g_mod, &rs);

    FH3Setting pp{};
    pp.size = sizeof pp;
    pp.key = "Pipe"; pp.label = "Pipe resonance";
    pp.description = "The hollow ring of a freer exhaust pipe on the Sport and Race exhaust. Off: only the tone changes.";
    pp.value = g_pipe.load(); pp.on_change = on_change;
    api->add_setting(g_mod, &pp);

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
