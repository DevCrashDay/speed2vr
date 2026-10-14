// Force feedback for direct-drive / FFB wheels (built for the Fanatec CSL DD).
//
// NFSU2 has no usable force feedback for modern wheels, so this drives the wheel itself through
// DirectInput from the car state the telemetry thread computes:
//   - self-aligning torque: pulls the wheel toward the angle that points the front wheels along
//     the direction the front axle is travelling. Driving straight that is a centering spring;
//     when the rear steps out it pulls into countersteer. Past the grip peak the force fades,
//     so understeer makes the wheel go light.
//   - weight that builds with speed, a damper (handled by the wheelbase at full rate),
//     bumps from the front suspension and jolts on impacts.
// The game keeps reading the wheel as before; this only adds forces. Turn the game's own force
// feedback off if it has any, because only one program can own the wheel's force output.
//
// Settings: [FFB] in NFSU2Telemetry.ini (see FfbStart).

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define DIRECTINPUT_VERSION 0x0800
#include <windows.h>
#include <dinput.h>
#include <cmath>
#include <cstdio>
#include "ffb.h"

#pragma comment(lib, "dxguid.lib")
#pragma comment(lib, "dinput8.lib")  // only for c_dfDIJoystick2; DirectInput8Create is loaded from system32

void Log(const char* fmt, ...);

static CRITICAL_SECTION g_lock;
static FfbInput g_in;
static LONG g_seq;

void FfbPublish(const FfbInput& in)
{
    EnterCriticalSection(&g_lock);
    g_in = in;
    g_seq++;
    LeaveCriticalSection(&g_lock);
}

struct FfbSettings
{
    int Enabled = 1;
    float Gain = 0.35f;          // overall strength, 0-1 of the wheel's maximum (start low on a DD)
    float MaxSteerDeg = 25;      // front wheel angle at full wheel input in the game
    float GripPeakDeg = 6;       // front slip angle where the aligning force peaks
    float FullForceKmh = 60;     // aligning force reaches full strength at this speed
    float StandstillSpring = 0.08f;
    float Damper = 0.25f;        // 0-1
    float Bumps = 0.5f;
    float Impacts = 0.6f;
    int Invert = 0;              // flip all forces if the wheel pushes the wrong way
    char Device[64] = "";        // part of the device name; empty = Fanatec, else first FFB wheel
};
static FfbSettings fs;
static char g_ini[MAX_PATH];

static float IniF(const char* key, float def)
{
    char b[32], d[32];
    sprintf(d, "%g", def);
    GetPrivateProfileStringA("FFB", key, d, b, sizeof(b), g_ini);
    return (float)atof(b);
}

// ---------------------------------------------------------------- DirectInput
static IDirectInput8A* g_di;
static IDirectInputDevice8A* g_dev;
static IDirectInputEffect* g_const;
static IDirectInputEffect* g_damper;
static HWND g_wnd;

static BOOL CALLBACK FindWindowCb(HWND h, LPARAM)
{
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (pid == GetCurrentProcessId() && IsWindowVisible(h) && !GetWindow(h, GW_OWNER))
    {
        RECT r; GetClientRect(h, &r);
        if (r.right > 320) { g_wnd = h; return FALSE; }
    }
    return TRUE;
}

// Candidate wheels in the order they are tried: Fanatec first (the CSL DD shows up as two
// "FANATEC Wheel" interfaces, only one of which may take effects), then any other FFB device.
struct Candidate { GUID guid; char name[MAX_PATH]; bool fanatec; };
struct Found { Candidate c[8]; int n; };

static BOOL CALLBACK EnumDevCb(const DIDEVICEINSTANCEA* d, void* ctx)
{
    Found* f = (Found*)ctx;
    bool match = fs.Device[0] ? (strstr(d->tszProductName, fs.Device) || strstr(d->tszInstanceName, fs.Device)) : true;
    Log("FFB device: %s (vid %04X pid %04X)", d->tszProductName, LOWORD(d->guidProduct.Data1), HIWORD(d->guidProduct.Data1));
    if (match && f->n < 8)
    {
        Candidate& c = f->c[f->n++];
        c.guid = d->guidInstance;
        strncpy(c.name, d->tszProductName, MAX_PATH - 1);
        c.fanatec = LOWORD(d->guidProduct.Data1) == 0x0EB7;
    }
    return DIENUM_CONTINUE;
}

static bool OpenDevice(const Candidate& cand);

static void Release()
{
    if (g_const) { g_const->Stop(); g_const->Release(); g_const = nullptr; }
    if (g_damper) { g_damper->Stop(); g_damper->Release(); g_damper = nullptr; }
    if (g_dev) { g_dev->Unacquire(); g_dev->Release(); g_dev = nullptr; }
}

// The mod owns the wheel's force output, which takes exclusive access. If the game later asks
// for exclusive access to a controller too (to play its own effects), it would fail and the
// game would not read the wheel at all. All DirectInput devices share one vtable per character
// set, so patching SetCooperativeLevel (slot 13) once turns the game's exclusive requests into
// non-exclusive ones: it still reads the wheel, only its own force effects are refused.
typedef HRESULT(STDMETHODCALLTYPE* SetCoopFn)(void* self, HWND, DWORD);
static SetCoopFn g_origCoopA, g_origCoopW;

static HRESULT STDMETHODCALLTYPE CoopHookA(void* self, HWND w, DWORD flags)
{
    if (self != g_dev && (flags & DISCL_EXCLUSIVE)) flags = (flags & ~(DISCL_EXCLUSIVE | DISCL_FOREGROUND)) | DISCL_NONEXCLUSIVE | DISCL_BACKGROUND;
    return g_origCoopA(self, w, flags);
}
static HRESULT STDMETHODCALLTYPE CoopHookW(void* self, HWND w, DWORD flags)
{
    if (flags & DISCL_EXCLUSIVE) flags = (flags & ~(DISCL_EXCLUSIVE | DISCL_FOREGROUND)) | DISCL_NONEXCLUSIVE | DISCL_BACKGROUND;
    return g_origCoopW(self, w, flags);
}

static void PatchSlot(void* obj, void* hook, SetCoopFn* orig)
{
    void** vt = *(void***)obj;
    if (vt[13] == hook) return;
    DWORD old;
    VirtualProtect(&vt[13], sizeof(void*), PAGE_EXECUTE_READWRITE, &old);
    *orig = (SetCoopFn)vt[13];
    vt[13] = hook;
    VirtualProtect(&vt[13], sizeof(void*), old, &old);
}

static void ShareWheelWithGame(HMODULE dinput, const GUID& wheel)
{
    auto create = (HRESULT(WINAPI*)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN))GetProcAddress(dinput, "DirectInput8Create");
    IDirectInputDevice8A* a = nullptr;
    if (SUCCEEDED(g_di->CreateDevice(wheel, &a, nullptr))) { PatchSlot(a, (void*)CoopHookA, &g_origCoopA); a->Release(); }
    IDirectInput8W* diw = nullptr;
    IDirectInputDevice8W* w = nullptr;
    if (create && SUCCEEDED(create(GetModuleHandleA(nullptr), DIRECTINPUT_VERSION, IID_IDirectInput8W, (void**)&diw, nullptr)))
    {
        if (SUCCEEDED(diw->CreateDevice(wheel, &w, nullptr))) { PatchSlot(w, (void*)CoopHookW, &g_origCoopW); w->Release(); }
        diw->Release();
    }
    Log("FFB: game's exclusive controller access redirected to shared access (%s/%s)", g_origCoopA ? "A" : "-", g_origCoopW ? "W" : "-");
}

static HMODULE g_dinput;

static bool Open()
{
    if (!g_di)
    {
        // the system dinput8, not the ASI loader's proxy in the game folder
        char path[MAX_PATH];
        GetSystemDirectoryA(path, MAX_PATH);
        strcat(path, "\\dinput8.dll");
        HMODULE m = g_dinput = LoadLibraryA(path);
        auto create = m ? (HRESULT(WINAPI*)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN))GetProcAddress(m, "DirectInput8Create") : nullptr;
        if (!create || FAILED(create(GetModuleHandleA(nullptr), DIRECTINPUT_VERSION, IID_IDirectInput8A, (void**)&g_di, nullptr)))
        {
            Log("FFB: DirectInput not available");
            return false;
        }
    }
    Found f = {};
    g_di->EnumDevices(DI8DEVCLASS_GAMECTRL, EnumDevCb, &f, DIEDFL_ATTACHEDONLY | DIEDFL_FORCEFEEDBACK);
    if (!f.n) { Log("FFB: no force feedback wheel found"); return false; }
    if (!g_origCoopA && !g_origCoopW) ShareWheelWithGame(g_dinput, f.c[0].guid);
    // Fanatec interfaces first, then anything else
    for (int pass = 0; pass < 2; pass++)
        for (int i = 0; i < f.n; i++)
            if (f.c[i].fanatec == (pass == 0) && OpenDevice(f.c[i])) return true;
    return false;
}

static bool OpenDevice(const Candidate& cand)
{
    HRESULT hr;
    if (FAILED(hr = g_di->CreateDevice(cand.guid, &g_dev, nullptr))) { Log("FFB: CreateDevice failed %08lX", hr); return false; }
    g_dev->SetDataFormat(&c_dfDIJoystick2);
    hr = g_dev->SetCooperativeLevel(g_wnd, DISCL_EXCLUSIVE | DISCL_BACKGROUND);
    if (FAILED(hr)) hr = g_dev->SetCooperativeLevel(g_wnd, DISCL_EXCLUSIVE | DISCL_FOREGROUND);
    if (FAILED(hr)) { Log("FFB: SetCooperativeLevel failed %08lX", hr); Release(); return false; }

    DIPROPDWORD ac = {};
    ac.diph.dwSize = sizeof(ac); ac.diph.dwHeaderSize = sizeof(DIPROPHEADER); ac.diph.dwHow = DIPH_DEVICE;
    ac.dwData = DIPROPAUTOCENTER_OFF;
    g_dev->SetProperty(DIPROP_AUTOCENTER, &ac.diph);
    DIPROPRANGE rg = {};
    rg.diph.dwSize = sizeof(rg); rg.diph.dwHeaderSize = sizeof(DIPROPHEADER); rg.diph.dwHow = DIPH_BYOFFSET; rg.diph.dwObj = DIJOFS_X;
    rg.lMin = -10000; rg.lMax = 10000;
    g_dev->SetProperty(DIPROP_RANGE, &rg.diph);

    hr = g_dev->Acquire();
    if (FAILED(hr)) { Log("FFB: Acquire failed %08lX (another program or the game owns the wheel's force feedback)", hr); Release(); return false; }

    DWORD axes[1] = { DIJOFS_X };
    LONG dir[1] = { 0 };
    DICONSTANTFORCE cf = { 0 };
    DIEFFECT e = {};
    e.dwSize = sizeof(e);
    e.dwFlags = DIEFF_CARTESIAN | DIEFF_OBJECTOFFSETS;
    e.dwDuration = INFINITE;
    e.dwGain = DI_FFNOMINALMAX;
    e.dwTriggerButton = DIEB_NOTRIGGER;
    e.cAxes = 1; e.rgdwAxes = axes; e.rglDirection = dir;
    e.cbTypeSpecificParams = sizeof(cf); e.lpvTypeSpecificParams = &cf;
    if (FAILED(hr = g_dev->CreateEffect(GUID_ConstantForce, &e, &g_const, nullptr))) { Log("FFB: constant force not supported %08lX", hr); Release(); return false; }
    g_const->Start(1, 0);

    DICONDITION cond = {};
    cond.lPositiveCoefficient = cond.lNegativeCoefficient = (LONG)(fs.Damper * fs.Gain * 10000);
    cond.dwPositiveSaturation = cond.dwNegativeSaturation = DI_FFNOMINALMAX;
    e.cbTypeSpecificParams = sizeof(cond); e.lpvTypeSpecificParams = &cond;
    if (SUCCEEDED(g_dev->CreateEffect(GUID_Damper, &e, &g_damper, nullptr))) g_damper->Start(1, 0);
    else Log("FFB: damper not supported, continuing without");

    Log("FFB: driving %s (gain %.2f, max steer %.0f deg, invert %d)", cand.name, fs.Gain, fs.MaxSteerDeg, fs.Invert);
    return true;
}

static void SetForce(float f)  // -1..1, positive = toward the right
{
    if (fs.Invert) f = -f;
    f = fmaxf(-1.0f, fminf(1.0f, f));
    // DirectInput directions name where the force comes from: a positive force along +X
    // pushes the wheel to the left, so a push to the right is a negative magnitude.
    DICONSTANTFORCE cf = { (LONG)(-f * DI_FFNOMINALMAX) };
    LONG dir[1] = { 1 };
    DIEFFECT e = {};
    e.dwSize = sizeof(e);
    e.dwFlags = DIEFF_CARTESIAN | DIEFF_OBJECTOFFSETS;
    e.cAxes = 1; e.rglDirection = dir;
    e.cbTypeSpecificParams = sizeof(cf); e.lpvTypeSpecificParams = &cf;
    HRESULT hr = g_const->SetParameters(&e, DIEP_TYPESPECIFICPARAMS | DIEP_DIRECTION);
    if (hr == DIERR_INPUTLOST || hr == DIERR_NOTACQUIRED || hr == DIERR_NOTEXCLUSIVEACQUIRED)
    {
        g_dev->Acquire();
        g_const->Start(1, 0);
        if (g_damper) g_damper->Start(1, 0);
    }
}

// ---------------------------------------------------------------- wheel and pedals into the game
// NFSU2 cannot select the wheel when several controllers are connected, so the mod writes the
// wheel and pedal positions straight into the player's control state. The game only writes
// these fields when one of its own input events fires (key press, mapped axis), so values
// written here every few milliseconds stick.
//   player car anchor +0x30 -> driver input (vtable 0x784FC8):
//     +0x2C8 steering (+1 = full left, -1 = full right; checked against yaw in a recorded drive),
//     +0x2D4 throttle, +0x2DC brake, +0x2EC handbrake (0-1)
struct InputSettings
{
    int Enabled = 1;
    int Steering = 1;
    float SteerScale = 1.0f;      // >1 = less wheel rotation for full lock
    float SteerDeadzone = 0.0f;
    int SteerInvert = 0;
    char PedalDevice[64] = "SIMSONN Plus X";
    char ThrottleAxis[8] = "", BrakeAxis[8] = "", HandbrakeAxis[8] = "";
    float PedalDeadzone = 0.03f;
};
static InputSettings is;
static IDirectInputDevice8A* g_pedals;

struct Axis { int index = -1; LONG rest = 0, full = 0; bool init = false; };
static Axis g_axThrottle, g_axBrake, g_axHandbrake;

static int AxisIndex(const char* n)
{
    const char* names[8] = { "X", "Y", "Z", "RX", "RY", "RZ", "S0", "S1" };
    for (int i = 0; i < 8; i++) if (!_stricmp(n, names[i])) return i;
    return -1;
}

static LONG AxisValue(const DIJOYSTATE2& s, int i)
{
    const LONG v[8] = { s.lX, s.lY, s.lZ, s.lRx, s.lRy, s.lRz, s.rglSlider[0], s.rglSlider[1] };
    return v[i];
}

// 0 at the resting position seen first, 1 at the furthest travel seen so far (works for
// pedals that count up or down).
static float PedalValue(Axis& a, const DIJOYSTATE2& s)
{
    if (a.index < 0) return -1;
    LONG v = AxisValue(s, a.index);
    if (!a.init) { a.rest = a.full = v; a.init = true; }
    if (labs(v - a.rest) > labs(a.full - a.rest)) a.full = v;
    LONG span = labs(a.full - a.rest);
    if (span < 5000) return 0;
    float x = (float)labs(v - a.rest) / span;
    x = (x - is.PedalDeadzone) / (1 - is.PedalDeadzone);
    return fmaxf(0.0f, fminf(1.0f, x));
}

static BOOL CALLBACK FindPedalsCb(const DIDEVICEINSTANCEA* d, void* out)
{
    if (strstr(d->tszProductName, is.PedalDevice)) { *(GUID*)out = d->guidInstance; return DIENUM_STOP; }
    return DIENUM_CONTINUE;
}

static void OpenPedals()
{
    g_axThrottle.index = AxisIndex(is.ThrottleAxis);
    g_axBrake.index = AxisIndex(is.BrakeAxis);
    g_axHandbrake.index = AxisIndex(is.HandbrakeAxis);
    if (g_axThrottle.index < 0 && g_axBrake.index < 0 && g_axHandbrake.index < 0) { Log("INPUT: no pedal axes set"); return; }
    GUID g = {};
    g_di->EnumDevices(DI8DEVCLASS_GAMECTRL, FindPedalsCb, &g, DIEDFL_ATTACHEDONLY);
    if (g == GUID() || FAILED(g_di->CreateDevice(g, &g_pedals, nullptr))) { Log("INPUT: pedal device \"%s\" not found", is.PedalDevice); return; }
    g_pedals->SetDataFormat(&c_dfDIJoystick2);
    g_pedals->SetCooperativeLevel(g_wnd, DISCL_NONEXCLUSIVE | DISCL_BACKGROUND);
    g_pedals->Acquire();
    Log("INPUT: pedals from %s (throttle %s, brake %s, handbrake %s)", is.PedalDevice, is.ThrottleAxis, is.BrakeAxis, is.HandbrakeAxis);
}

static bool WriteGameInput(float steer, float thr, float brk, float hb)
{
    __try
    {
        if (!*(void**)0x8900D8) return false;  // not in a race
        BYTE* view = (BYTE*)0x832E50;
        BYTE* node = *(BYTE**)(view + 0x44);
        if (!node || node == view + 0x44) return false;
        BYTE* anchor = *(BYTE**)(node - 4 + 0x2C);
        if (!anchor) return false;
        BYTE* in = *(BYTE**)(anchor + 0x30);
        if (!in || *(DWORD*)in != 0x784FC8) return false;
        if (steer > -2) *(float*)(in + 0x2C8) = steer;
        if (thr >= 0) *(float*)(in + 0x2D4) = thr;
        if (brk >= 0) *(float*)(in + 0x2DC) = brk;
        if (hb >= 0) *(float*)(in + 0x2EC) = hb;
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

static void FeedGame(float wheel)
{
    float steer = -3;
    if (is.Steering)
    {
        float s = wheel;
        float a = fabsf(s);
        a = a <= is.SteerDeadzone ? 0 : (a - is.SteerDeadzone) / (1 - is.SteerDeadzone);
        steer = fminf(1.0f, a * is.SteerScale) * (s < 0 ? 1 : -1);  // game: positive = left
        if (is.SteerInvert) steer = -steer;
    }
    float thr = -1, brk = -1, hb = -1;
    DIJOYSTATE2 ps = {};
    if (g_pedals && (g_pedals->Poll(), SUCCEEDED(g_pedals->GetDeviceState(sizeof(ps), &ps))))
    {
        thr = PedalValue(g_axThrottle, ps);
        brk = PedalValue(g_axBrake, ps);
        hb = PedalValue(g_axHandbrake, ps);
    }
    else if (g_pedals) g_pedals->Acquire();
    static bool logged;
    if (WriteGameInput(steer, thr, brk, hb) && !logged) { logged = true; Log("INPUT: feeding wheel/pedals into the game"); }
}

// ---------------------------------------------------------------- force model
static DWORD WINAPI FfbThread(LPVOID)
{
    // wait for the game window
    while (!g_wnd) { Sleep(500); EnumWindows(FindWindowCb, 0); }
    int tries = 0;
    while (!Open())
    {
        if (++tries == 20) { Log("FFB: giving up"); return 0; }
        Sleep(3000);
    }
    if (is.Enabled) OpenPedals();

    LARGE_INTEGER freq, last, now; QueryPerformanceFrequency(&freq); QueryPerformanceCounter(&last);
    float out = 0, fade = 0, impact = 0, lastSway = 0;
    for (;;)
    {
        Sleep(4);
        QueryPerformanceCounter(&now);
        float dt = (float)(now.QuadPart - last.QuadPart) / freq.QuadPart;
        last = now;

        FfbInput in;
        EnterCriticalSection(&g_lock); in = g_in; LeaveCriticalSection(&g_lock);

        DIJOYSTATE2 js = {};
        if (FAILED(g_dev->Poll()) || FAILED(g_dev->GetDeviceState(sizeof(js), &js))) { g_dev->Acquire(); continue; }
        float wheel = js.lX / 10000.0f;  // -1 left .. +1 right
        if (is.Enabled) FeedGame(wheel);

        bool active = in.running && !in.paused;
        fade += ((active ? 1.0f : 0.0f) - fade) * fminf(1.0f, dt * 2);  // ease in/out over ~0.5 s

        float force = 0;
        if (in.running)
        {
            float v = fabsf(in.vFwd);
            // lateral speed of the front axle (yaw rate positive = turning right)
            float vLatFront = in.vLat + in.yawRateDegS / 57.2958f * in.frontAxle;
            float travelDeg = v > 0.5f ? atan2f(vLatFront, v) * 57.2958f : 0;
            if (in.vFwd < -0.5f) travelDeg = -travelDeg;  // reversing
            float wheelDeg = wheel * fs.MaxSteerDeg;
            float slip = travelDeg - wheelDeg;  // + : front wheels point left of where they travel
            // tyre curve: linear up to the grip peak, then falls off to 60 %
            float x = fabsf(slip) / fs.GripPeakDeg;
            float curve = x < 1 ? x : fmaxf(0.6f, 1 - 0.4f * (x - 1) / 2);
            float speedScale = fminf(1.0f, v * 3.6f / fs.FullForceKmh);
            float align = (slip >= 0 ? 1 : -1) * curve * (0.25f + 0.75f * speedScale);
            // gentle centering spring when (almost) stopped
            float spring = -wheel * fs.StandstillSpring * (1 - speedScale);
            // bumps: front suspension moving differently left/right twists the wheel
            float bump = (in.suspVel[0] - in.suspVel[1]) * fs.Bumps * 0.5f;
            // impacts: a sudden sideways acceleration spike kicks the wheel toward the hit
            float jerk = in.swayAcc - lastSway;
            lastSway = in.swayAcc;
            if (fabsf(jerk) > 15) impact = fmaxf(-1.0f, fminf(1.0f, jerk / 30.0f));
            impact *= expf(-dt * 15);
            force = align + spring + bump + impact * fs.Impacts;
        }
        force *= fs.Gain * fade;
        out += (force - out) * fminf(1.0f, dt * 60);  // ~16 ms smoothing, physics runs at ~60 Hz
        SetForce(out);
    }
}

void FfbStart(const char* iniPath)
{
    InitializeCriticalSection(&g_lock);
    strcpy(g_ini, iniPath);
    fs.Enabled = GetPrivateProfileIntA("FFB", "Enabled", fs.Enabled, g_ini);
    fs.Gain = IniF("Gain", fs.Gain);
    fs.MaxSteerDeg = IniF("MaxSteerDeg", fs.MaxSteerDeg);
    fs.GripPeakDeg = IniF("GripPeakDeg", fs.GripPeakDeg);
    fs.FullForceKmh = IniF("FullForceKmh", fs.FullForceKmh);
    fs.StandstillSpring = IniF("StandstillSpring", fs.StandstillSpring);
    fs.Damper = IniF("Damper", fs.Damper);
    fs.Bumps = IniF("Bumps", fs.Bumps);
    fs.Impacts = IniF("Impacts", fs.Impacts);
    fs.Invert = GetPrivateProfileIntA("FFB", "Invert", fs.Invert, g_ini);
    GetPrivateProfileStringA("FFB", "Device", "", fs.Device, sizeof(fs.Device), g_ini);
    fs.Gain = fmaxf(0.0f, fminf(1.0f, fs.Gain));

    is.Enabled = GetPrivateProfileIntA("INPUT", "Enabled", is.Enabled, g_ini);
    is.Steering = GetPrivateProfileIntA("INPUT", "Steering", is.Steering, g_ini);
    is.SteerInvert = GetPrivateProfileIntA("INPUT", "SteerInvert", is.SteerInvert, g_ini);
    char b[32], d[32];
    sprintf(d, "%g", is.SteerScale); GetPrivateProfileStringA("INPUT", "SteerScale", d, b, sizeof(b), g_ini); is.SteerScale = (float)atof(b);
    sprintf(d, "%g", is.SteerDeadzone); GetPrivateProfileStringA("INPUT", "SteerDeadzone", d, b, sizeof(b), g_ini); is.SteerDeadzone = fminf(0.5f, (float)atof(b));
    sprintf(d, "%g", is.PedalDeadzone); GetPrivateProfileStringA("INPUT", "PedalDeadzone", d, b, sizeof(b), g_ini); is.PedalDeadzone = fminf(0.5f, (float)atof(b));
    char dev[64];
    GetPrivateProfileStringA("INPUT", "PedalDevice", is.PedalDevice, dev, sizeof(dev), g_ini);
    strcpy(is.PedalDevice, dev);
    GetPrivateProfileStringA("INPUT", "ThrottleAxis", "", is.ThrottleAxis, sizeof(is.ThrottleAxis), g_ini);
    GetPrivateProfileStringA("INPUT", "BrakeAxis", "", is.BrakeAxis, sizeof(is.BrakeAxis), g_ini);
    GetPrivateProfileStringA("INPUT", "HandbrakeAxis", "", is.HandbrakeAxis, sizeof(is.HandbrakeAxis), g_ini);

    if (!fs.Enabled && !is.Enabled) { Log("FFB and wheel input disabled in ini"); return; }
    if (!fs.Enabled) { fs.Gain = 0; fs.Damper = 0; Log("FFB disabled in ini, wheel input only"); }
    CreateThread(nullptr, 0, FfbThread, nullptr, 0, nullptr);
}
