// NFSU2HeadTrack - drives the NFSU2 (v1.2, 4,800,512 byte SPEED2.EXE) player camera with
// the 6DoF pose of a SteamVR headset and shows the game window as a head-locked
// (optionally side-by-side 3D) SteamVR overlay.
//
// Camera hook: Camera::SetCameraMatrix (0x447280) copies the incoming view matrix with
// a cdecl helper at 0x4012B0 (call site 0x447351). We redirect that call, copy the
// matrix ourselves and, for the player camera (eView[1].pCamera at 0x832E90), multiply
// a seat offset + head pose into it. Everything the game derives from the matrix
// afterwards (camera position, frustum) stays consistent.
//
// Game view space (from eCreateLookAtMatrix 0x5BA9A0): x = right, y = down, z = forward,
// row vectors (p_view = p_world * M). OpenVR: x = right, y = up, -z = forward.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define _WIN32_WINNT 0x0A00
#include <windows.h>
#include <algorithm>
#include <tlhelp32.h>
#include <wincodec.h>
#include <vector>
#include <d3d9.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <timeapi.h>
#include <dxgi1_2.h>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cstdarg>
#include <string>
#include "openvr.h"
#define VK_USE_PLATFORM_WIN32_KHR
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "windowscodecs.lib")

// ---------------------------------------------------------------- game addresses
static const DWORD kCopyCallSite = 0x447351;   // E8 -> 0x4012B0 inside Camera::SetCameraMatrix
static const DWORD kCopyMatrix = 0x4012B0;
static void** const kPlayerCamera = (void**)0x832E90;  // eView[1] (0x832DE0 + 0x70) + 0x40
static void** const kRaceCoordinator = (void**)0x8900D8;

// ---------------------------------------------------------------- settings
struct Settings
{
    int Enabled = 1;
    int OnlyInRace = 1;
    int HideCursor = 1;
    float SeatRight = 0.0f, SeatUp = 0.0f, SeatForward = 0.0f;  // meters, relative to the game camera
    float PositionScale = 1.0f;
    float PredictionSeconds = 0.035f;
    int UsePosition = 1;
    int UseRoll = 1;
    int Overlay = 1;
    int OverlaySBS = 1;
    float OverlayDistance = 1.0f;
    float OverlayWidth = 1.6f;
    float OverlayUp = 0.0f;
    int KeyToggle = VK_F7, KeyRecenter = VK_F8, KeySave = VK_F9, KeyOverlay = VK_F10;
    float SeatStep = 0.02f;
    int TestMotion = 0;
    int Cockpit = 1;
    int Backdrop = 1;
    int Stereo = 1;                  // 1 = render left/right eye on alternating frames
    float IPD = 0.0f;                // meters, 0 = take it from the headset
    float PositionScaleStereo = 1.0f;
    float GameFOV = 84.0f;           // camera FOV field in degrees (4:3 based), 0 = leave the game's FOV
    int AutoWidth = 1;               // size the screen from GameFOV so the world has the right scale
    float WidthScale = 1.0f;
    float ReprojectDistance = 3.0f;  // meters: depth at which the picture is pinned in the world
    int WorldLock = 1;               // anchor each frame where your head was when it was rendered
    float OverlayTexelAspect = 0.0f;  // 0 = automatic (2 for side-by-side, 1 otherwise)
    int GameCore = 4;                // logical CPU for the game's threads (-1 = lowest available)
    int PinGameToOneCore = 1;        // game threads on one core (stability), mod threads on the others
    int StereoMode = 1;              // 0 = left/right eye on alternating frames, 1 = both eyes from every frame (depth based)
    int DepthSteps = 48;
    int VRFrames = 1;                // 1 = submit per-eye VR frames to SteamVR, 0 = SteamVR overlays
    int Transport = 0;               // 0 = hand frames to SteamVR on the GPU when possible (DXVK), 1 = always desktop capture
    int Hud = 1;                     // compact HUD (speed/gear, position/lap/lap time)
    float HudScale = 1.0f;
    float HudSpeedX = 0.60f, HudSpeedY = 0.72f;  // panel centers as fractions of the screen
    float HudInfoX = 0.50f, HudInfoY = 0.20f;
    float MapPatchShade = 0.72f;
    int MapPatchMode = 0;     // brightness of the patch that replaces the on-screen map (matches the HUD veil)
    int MapOnDash = 1;               // minimap as third round instrument on the dashboard
    float MapX = -138.7f, MapY = 97.0f, MapRadius = 75.0f;  // minimap on screen, 640x480 HUD units from centre
    int Gauges = 1;                  // round rev counter + speedometer fixed in the car
    float GaugeDistance = 0.62f, GaugeDown = 0.30f, GaugeRight = 0.0f, GaugeSpacing = 0.20f, GaugeSize = 0.075f;
    float GaugeMaxRpm = 9000, GaugeRedline = 7500, GaugeMaxSpeed = 320;
} g;

static char g_iniPath[MAX_PATH];
static char g_logPath[MAX_PATH];

static void Log(const char* fmt, ...)
{
    FILE* f = fopen(g_logPath, "a");
    if (!f) return;
    SYSTEMTIME st; GetLocalTime(&st);
    fprintf(f, "%02d:%02d:%02d ", st.wHour, st.wMinute, st.wSecond);
    va_list a; va_start(a, fmt); vfprintf(f, fmt, a); va_end(a);
    fputc('\n', f);
    fclose(f);
}

static float IniF(const char* sec, const char* key, float def)
{
    char buf[64], d[64];
    sprintf(d, "%g", def);
    GetPrivateProfileStringA(sec, key, d, buf, sizeof(buf), g_iniPath);
    return (float)atof(buf);
}
static int IniI(const char* sec, const char* key, int def) { return GetPrivateProfileIntA(sec, key, def, g_iniPath); }
static void IniWF(const char* sec, const char* key, float v)
{
    char buf[64]; sprintf(buf, "%.3f", v);
    WritePrivateProfileStringA(sec, key, buf, g_iniPath);
}

static void LoadSettings()
{
    g.Enabled = IniI("MAIN", "Enabled", g.Enabled);
    g.OnlyInRace = IniI("MAIN", "OnlyInRace", g.OnlyInRace);
    g.HideCursor = IniI("MAIN", "HideCursor", g.HideCursor);
    g.Cockpit = IniI("SEAT", "Cockpit", g.Cockpit);
    g.OverlayTexelAspect = IniF("OVERLAY", "TexelAspect", g.OverlayTexelAspect);
    g.Backdrop = IniI("OVERLAY", "BlackBackground", g.Backdrop);
    g.Stereo = IniI("STEREO", "AlternateEyes", g.Stereo);
    g.IPD = IniF("STEREO", "IPD", g.IPD);
    g.PositionScaleStereo = IniF("STEREO", "DepthScale", g.PositionScaleStereo);
    g.GameFOV = IniF("STEREO", "GameFOV", g.GameFOV);
    g.AutoWidth = IniI("OVERLAY", "AutoWidth", g.AutoWidth);
    g.WidthScale = IniF("OVERLAY", "WidthScale", g.WidthScale);
    g.WorldLock = IniI("OVERLAY", "WorldLock", g.WorldLock);
    g.ReprojectDistance = IniF("OVERLAY", "ReprojectDistance", g.ReprojectDistance);
    g.VRFrames = IniI("OVERLAY", "VRFrames", g.VRFrames);
    g.Transport = IniI("OVERLAY", "Transport", g.Transport);
    g.StereoMode = IniI("STEREO", "Mode", g.StereoMode);
    g.DepthSteps = IniI("STEREO", "DepthSteps", g.DepthSteps);
    g.PinGameToOneCore = IniI("MAIN", "PinGameToOneCore", g.PinGameToOneCore);
    g.GameCore = IniI("MAIN", "GameCore", g.GameCore);
    g.Hud = IniI("HUD", "Enabled", g.Hud);
    g.HudScale = IniF("HUD", "Scale", g.HudScale);
    g.HudSpeedX = IniF("HUD", "SpeedX", g.HudSpeedX);
    g.HudSpeedY = IniF("HUD", "SpeedY", g.HudSpeedY);
    g.HudInfoX = IniF("HUD", "InfoX", g.HudInfoX);
    g.HudInfoY = IniF("HUD", "InfoY", g.HudInfoY);
    g.Gauges = IniI("GAUGES", "Enabled", g.Gauges);
    g.MapOnDash = IniI("GAUGES", "Map", g.MapOnDash);
    g.MapPatchShade = IniF("GAUGES", "MapPatchShade", g.MapPatchShade);
    g.MapPatchMode = IniI("GAUGES", "MapPatchMode", g.MapPatchMode);
    g.MapX = IniF("GAUGES", "MapScreenX", g.MapX);
    g.MapY = IniF("GAUGES", "MapScreenY", g.MapY);
    g.MapRadius = IniF("GAUGES", "MapScreenRadius", g.MapRadius);
    g.GaugeDistance = IniF("GAUGES", "Distance", g.GaugeDistance);
    g.GaugeDown = IniF("GAUGES", "Down", g.GaugeDown);
    g.GaugeRight = IniF("GAUGES", "Right", g.GaugeRight);
    g.GaugeSpacing = IniF("GAUGES", "Spacing", g.GaugeSpacing);
    g.GaugeSize = IniF("GAUGES", "Size", g.GaugeSize);
    g.GaugeMaxRpm = IniF("GAUGES", "MaxRpm", g.GaugeMaxRpm);
    g.GaugeRedline = IniF("GAUGES", "Redline", g.GaugeRedline);
    g.GaugeMaxSpeed = IniF("GAUGES", "MaxSpeed", g.GaugeMaxSpeed);
    g.SeatRight = IniF("SEAT", "Right", g.SeatRight);
    g.SeatUp = IniF("SEAT", "Up", g.SeatUp);
    g.SeatForward = IniF("SEAT", "Forward", g.SeatForward);
    g.SeatStep = IniF("SEAT", "Step", g.SeatStep);
    g.PositionScale = IniF("TRACKING", "PositionScale", g.PositionScale);
    g.PredictionSeconds = IniF("TRACKING", "PredictionSeconds", g.PredictionSeconds);
    g.UsePosition = IniI("TRACKING", "UsePosition", g.UsePosition);
    g.UseRoll = IniI("TRACKING", "UseRoll", g.UseRoll);
    g.TestMotion = IniI("TRACKING", "TestMotion", g.TestMotion);
    g.Overlay = IniI("OVERLAY", "Enabled", g.Overlay);
    g.OverlaySBS = IniI("OVERLAY", "SideBySide3D", g.OverlaySBS);
    g.OverlayDistance = IniF("OVERLAY", "Distance", g.OverlayDistance);
    g.OverlayWidth = IniF("OVERLAY", "Width", g.OverlayWidth);
    g.OverlayUp = IniF("OVERLAY", "Up", g.OverlayUp);
    g.KeyToggle = IniI("KEYS", "Toggle", g.KeyToggle);
    g.KeyRecenter = IniI("KEYS", "Recenter", g.KeyRecenter);
    g.KeySave = IniI("KEYS", "Save", g.KeySave);
    g.KeyOverlay = IniI("KEYS", "ToggleOverlay", g.KeyOverlay);
}

static void SaveAdjustables()
{
    IniWF("SEAT", "Right", g.SeatRight);
    IniWF("SEAT", "Up", g.SeatUp);
    IniWF("SEAT", "Forward", g.SeatForward);
    IniWF("OVERLAY", "Distance", g.OverlayDistance);
    IniWF("OVERLAY", "Width", g.OverlayWidth);
    IniWF("OVERLAY", "WidthScale", g.WidthScale);
    IniWF("OVERLAY", "Up", g.OverlayUp);
    Log("settings saved");
}

// ---------------------------------------------------------------- OpenVR
static HMODULE g_vrDll;
static decltype(&vr::VR_InitInternal2) pVR_InitInternal2;
static decltype(&vr::VR_ShutdownInternal) pVR_ShutdownInternal;
static decltype(&vr::VR_GetGenericInterface) pVR_GetGenericInterface;
static decltype(&vr::VR_IsHmdPresent) pVR_IsHmdPresent;
static vr::IVRSystem* g_sys;
static vr::IVROverlay* g_ovl;
static volatile bool g_vrReady;
static SRWLOCK g_vrLock = SRWLOCK_INIT;

static bool LoadOpenVR()
{
    // Use SteamVR's own 32-bit openvr_api.dll so nothing has to be copied into the game folder.
    char path[MAX_PATH * 2] = {};
    char vrpath[MAX_PATH];
    ExpandEnvironmentStringsA("%LOCALAPPDATA%\\openvr\\openvrpaths.vrpath", vrpath, MAX_PATH);
    std::string runtime = "C:\\Program Files (x86)\\Steam\\steamapps\\common\\SteamVR";
    if (FILE* f = fopen(vrpath, "rb"))
    {
        std::string s; char buf[4096]; size_t n;
        while ((n = fread(buf, 1, sizeof(buf), f)) > 0) s.append(buf, n);
        fclose(f);
        size_t k = s.find("\"runtime\"");
        if (k != std::string::npos)
        {
            size_t q1 = s.find('"', s.find('[', k) + 1);
            size_t q2 = s.find('"', q1 + 1);
            if (q1 != std::string::npos && q2 != std::string::npos)
            {
                std::string r = s.substr(q1 + 1, q2 - q1 - 1), o;
                for (size_t i = 0; i < r.size(); i++)
                {
                    if (r[i] == '\\' && i + 1 < r.size() && r[i + 1] == '\\') { o += '\\'; i++; }
                    else o += r[i];
                }
                runtime = o;
            }
        }
    }
    sprintf(path, "%s\\bin\\win32\\openvr_api.dll", runtime.c_str());
    // ReShade 5+ hooks every module named openvr_api.dll and would run its effects over our VR
    // frames a second time. A copy under another name next to this mod keeps it out.
    char copy[MAX_PATH];
    strcpy(copy, g_iniPath);
    if (char* sl = strrchr(copy, '\\')) strcpy(sl + 1, "NFSU2HeadTrack_openvr.dll");
    WIN32_FILE_ATTRIBUTE_DATA fa, fb;
    bool same = GetFileAttributesExA(path, GetFileExInfoStandard, &fa) && GetFileAttributesExA(copy, GetFileExInfoStandard, &fb) &&
                fa.nFileSizeLow == fb.nFileSizeLow && CompareFileTime(&fa.ftLastWriteTime, &fb.ftLastWriteTime) == 0;
    if (!same) CopyFileA(path, copy, FALSE);
    g_vrDll = LoadLibraryA(copy);
    if (g_vrDll) strcpy(path, copy);
    else g_vrDll = LoadLibraryA(path);
    if (!g_vrDll) { Log("could not load %s (error %lu)", path, GetLastError()); return false; }
    pVR_InitInternal2 = (decltype(pVR_InitInternal2))GetProcAddress(g_vrDll, "VR_InitInternal2");
    pVR_ShutdownInternal = (decltype(pVR_ShutdownInternal))GetProcAddress(g_vrDll, "VR_ShutdownInternal");
    pVR_GetGenericInterface = (decltype(pVR_GetGenericInterface))GetProcAddress(g_vrDll, "VR_GetGenericInterface");
    pVR_IsHmdPresent = (decltype(pVR_IsHmdPresent))GetProcAddress(g_vrDll, "VR_IsHmdPresent");
    if (!pVR_InitInternal2 || !pVR_GetGenericInterface || !pVR_IsHmdPresent)
    {
        Log("openvr_api.dll is missing exports");
        return false;
    }
    Log("loaded %s", path);
    return true;
}

static bool InitVR()
{
    vr::EVRInitError err = vr::VRInitError_None;
    pVR_InitInternal2(&err, g.VRFrames ? vr::VRApplication_Scene : vr::VRApplication_Overlay, nullptr);
    if (err != vr::VRInitError_None)
    {
        Log("VR_Init failed: %d", (int)err);
        return false;
    }
    g_sys = (vr::IVRSystem*)pVR_GetGenericInterface(vr::IVRSystem_Version, &err);
    g_ovl = (vr::IVROverlay*)pVR_GetGenericInterface(vr::IVROverlay_Version, &err);
    if (!g_sys || !g_ovl)
    {
        Log("VR interfaces unavailable: %d", (int)err);
        pVR_ShutdownInternal();
        g_sys = nullptr; g_ovl = nullptr;
        return false;
    }
    Log("SteamVR connected");
    return true;
}

// ---------------------------------------------------------------- pose math
struct M3 { float m[3][3]; };

static float g_refYawS = 0.0f, g_refYawC = 1.0f, g_refPos[3] = {};
static volatile LONG g_recenterRequest = 1;
static volatile bool g_trackingOn = true;

// Head pose published by the VR thread (VR-frames mode): pose at time t (QPC) plus velocities.
struct PubPose { vr::HmdMatrix34_t m; float v[3], w[3]; LONGLONG t; bool valid; };
static PubPose g_pub;
static SRWLOCK g_pubLock = SRWLOCK_INIT;

static void PublishPose(const vr::TrackedDevicePose_t& p)
{
    if (!p.bPoseIsValid) return;
    LARGE_INTEGER now; QueryPerformanceCounter(&now);
    AcquireSRWLockExclusive(&g_pubLock);
    g_pub.m = p.mDeviceToAbsoluteTracking;
    for (int i = 0; i < 3; i++) { g_pub.v[i] = p.vVelocity.v[i]; g_pub.w[i] = p.vAngularVelocity.v[i]; }
    g_pub.t = now.QuadPart;
    g_pub.valid = true;
    ReleaseSRWLockExclusive(&g_pubLock);
}

static bool GetHmdPose(vr::HmdMatrix34_t& out)
{
    if (g.TestMotion)
    {
        // Synthetic head motion for testing without a headset: yaw +-30 degrees, sway +-10 cm.
        float a = (float)(GetTickCount() % 6283) / 1000.0f;
        float yaw = 0.52f * sinf(a), c = cosf(yaw), s = sinf(yaw);
        vr::HmdMatrix34_t m = { { { c, 0, s, 0.10f * sinf(a) }, { 0, 1, 0, 0 }, { -s, 0, c, 0 } } };
        if (g.TestMotion == 2)  // nod up/down +-20 degrees with 8 cm vertical movement
        {
            float p = 0.35f * sinf(a), cp = cosf(p), sp = sinf(p);
            vr::HmdMatrix34_t n = { { { 1, 0, 0, 0 }, { 0, cp, -sp, 0.08f * sinf(a) }, { 0, sp, cp, 0 } } };
            m = n;
        }
        out = m;
        return true;
    }
    if (!g_vrReady) return false;
    if (g.VRFrames)
    {
        // In VR-frames mode only the VR thread talks to SteamVR (it is not thread-safe, and calling
        // it from the game thread while the VR thread waits in WaitGetPoses crashed the game).
        // The VR thread publishes each pose with its velocities; extrapolate it to now + prediction.
        PubPose p;
        AcquireSRWLockShared(&g_pubLock);
        p = g_pub;
        ReleaseSRWLockShared(&g_pubLock);
        if (!p.valid) return false;
        LARGE_INTEGER now, f;
        QueryPerformanceCounter(&now); QueryPerformanceFrequency(&f);
        // The published pose is already predicted to its display time; SteamVR reprojects the rest,
        // because each picture is submitted with exactly the pose it was rendered with.
        float dt = (float)(now.QuadPart - p.t) / (float)f.QuadPart;
        if (dt < 0) dt = 0;
        if (dt > 0.1f) dt = 0.1f;
        float wl = sqrtf(p.w[0] * p.w[0] + p.w[1] * p.w[1] + p.w[2] * p.w[2]);
        float Q[3][3] = { { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 } };
        if (wl > 1e-5f)
        {
            float a = wl * dt, x = p.w[0] / wl, y = p.w[1] / wl, z = p.w[2] / wl, c = cosf(a), s = sinf(a), t = 1 - c;
            float R[3][3] = { { t * x * x + c, t * x * y - s * z, t * x * z + s * y },
                              { t * x * y + s * z, t * y * y + c, t * y * z - s * x },
                              { t * x * z - s * y, t * y * z + s * x, t * z * z + c } };
            memcpy(Q, R, sizeof(Q));
        }
        for (int i = 0; i < 3; i++)
        {
            for (int j = 0; j < 3; j++)
                out.m[i][j] = Q[i][0] * p.m.m[0][j] + Q[i][1] * p.m.m[1][j] + Q[i][2] * p.m.m[2][j];
            out.m[i][3] = p.m.m[i][3] + p.v[i] * dt;
        }
        return true;
    }
    AcquireSRWLockShared(&g_vrLock);
    bool ok = false;
    if (g_sys)
    {
        vr::TrackedDevicePose_t p;
        g_sys->GetDeviceToAbsoluteTrackingPose(vr::TrackingUniverseSeated, g.PredictionSeconds, &p, 1);
        if (p.bPoseIsValid) { out = p.mDeviceToAbsoluteTracking; ok = true; }
    }
    ReleaseSRWLockShared(&g_vrLock);
    return ok;
}

// Frame counter advanced at every Present; even frames are the left eye, odd frames the right eye.
static volatile LONG g_frame = 0;
// Raw seated-universe head pose used to render each frame, for anchoring the overlay.
struct FramePose { vr::HmdMatrix34_t m; LONG frame; bool valid; };
static FramePose g_framePose[64];
static float g_ipd = 0.064f;

// Head pose relative to the recentered reference, converted to game view space.
static bool HeadInGameSpace(M3& R, float t[3])
{
    vr::HmdMatrix34_t p;
    if (!GetHmdPose(p)) return false;
    {
        LONG f = g_frame;
        FramePose& fp = g_framePose[f & 63];
        fp.m = p; fp.frame = f; fp.valid = true;
    }

    if (InterlockedExchange(&g_recenterRequest, 0))
    {
        float fx = -p.m[0][2], fz = -p.m[2][2];
        float n = sqrtf(fx * fx + fz * fz);
        if (n > 1e-4f) { g_refYawS = -fx / n; g_refYawC = -fz / n; }
        g_refPos[0] = p.m[0][3]; g_refPos[1] = p.m[1][3]; g_refPos[2] = p.m[2][3];
        Log("recentered");
    }

    // Ry^T applied to rotation and position (Ry rotates about +y by the reference yaw).
    float s = g_refYawS, c = g_refYawC;
    float Rr[3][3], tr[3];
    for (int j = 0; j < 3; j++)
    {
        float x = p.m[0][j], y = p.m[1][j], z = p.m[2][j];
        Rr[0][j] = c * x - s * z;
        Rr[1][j] = y;
        Rr[2][j] = s * x + c * z;
    }
    {
        float x = p.m[0][3] - g_refPos[0], y = p.m[1][3] - g_refPos[1], z = p.m[2][3] - g_refPos[2];
        tr[0] = c * x - s * z; tr[1] = y; tr[2] = s * x + c * z;
    }

    if (!g.UseRoll)
    {
        // Remove roll: rebuild the basis from the forward vector and world up.
        float f[3] = { Rr[0][2], Rr[1][2], Rr[2][2] };  // local +z (backward) in world
        float up[3] = { 0, 1, 0 };
        float x[3] = { up[1] * f[2] - up[2] * f[1], up[2] * f[0] - up[0] * f[2], up[0] * f[1] - up[1] * f[0] };
        float n = sqrtf(x[0] * x[0] + x[1] * x[1] + x[2] * x[2]);
        if (n > 1e-4f)
        {
            for (int i = 0; i < 3; i++) x[i] /= n;
            float y[3] = { f[1] * x[2] - f[2] * x[1], f[2] * x[0] - f[0] * x[2], f[0] * x[1] - f[1] * x[0] };
            for (int i = 0; i < 3; i++) { Rr[i][0] = x[i]; Rr[i][1] = y[i]; Rr[i][2] = f[i]; }
        }
    }

    // C = diag(1,-1,-1): R_game = C R C, t_game = C t.
    static const float C[3] = { 1, -1, -1 };
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++)
            R.m[i][j] = C[i] * Rr[i][j] * C[j];
    float sc = g.UsePosition ? g.PositionScale : 0.0f;
    for (int i = 0; i < 3; i++) t[i] = C[i] * tr[i] * sc;
    return true;
}

static bool InRace() { return *kRaceCoordinator != nullptr; }

// Player car frame from the camera anchor of view 1's first camera mover (mover + 0x2C).
// Anchor layout: +0x50 position, +0x90/+0xA0/+0xB0 rows of the body matrix
// (forward, left, up; world is z-up).
static bool GetCarFrame(float pos[3], float fwd[3], float left[3], float up[3])
{
    __try
    {
        BYTE* view = (BYTE*)0x832E50;
        BYTE* node = *(BYTE**)(view + 0x44);
        if (!node || node == view + 0x44) return false;
        BYTE* anchor = *(BYTE**)(node - 4 + 0x2C);
        if (!anchor) return false;
        const float* p = (const float*)(anchor + 0x50);
        const float* r = (const float*)(anchor + 0x90);
        for (int i = 0; i < 3; i++)
        {
            pos[i] = p[i]; fwd[i] = r[i]; left[i] = r[4 + i]; up[i] = r[8 + i];
        }
        auto len = [](const float* v) { return sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]); };
        if (fabsf(len(fwd) - 1) > 0.05f || fabsf(len(left) - 1) > 0.05f || fabsf(len(up) - 1) > 0.05f) return false;
        if (!(fabsf(pos[0]) < 1e5f && fabsf(pos[1]) < 1e5f && fabsf(pos[2]) < 1e5f)) return false;
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

struct ViewInfo { float m[16]; float seat[3], fwd[3], right[3], up[3]; float fov; LONG frame = -100; };
static ViewInfo g_view;

static volatile LONG g_simUpdates, g_renderUpdates;  // diagnostics, logged with the fps

static bool CockpitActive() { return g_trackingOn && g.Enabled && g.Cockpit && (!g.OnlyInRace || InRace()); }

// Builds the cockpit view matrix (car body + seat + head pose, optionally shifted to the eye of
// the frame being rendered) and records it for the gauges. Returns false if the car is unknown.
static bool BuildCockpitView(float* M, bool eyeOffset, float fov)
{
    float pos[3], fwd[3], left[3], up[3];
    if (!GetCarFrame(pos, fwd, left, up)) return false;
    M3 R = { { { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 } } };
    float t[3] = { 0, 0, 0 };
    HeadInGameSpace(R, t);  // identity when no headset pose is available
    if (eyeOffset && g.Stereo && g.StereoMode == 0 && (g_vrReady || g.TestMotion))
    {
        // Eye offset along the head's right axis (game view space column 0 of R).
        float e = ((g_frame & 1) ? 0.5f : -0.5f) * (g.IPD > 0 ? g.IPD : g_ipd) * g.PositionScaleStereo;
        for (int i = 0; i < 3; i++) t[i] += e * R.m[i][0];
    }
    // Camera fixed to the car body: B = world axes of the seat (right, down, forward).
    float B[3][3];
    for (int i = 0; i < 3; i++) { B[i][0] = -left[i]; B[i][1] = -up[i]; B[i][2] = fwd[i]; }
    float eye[3], ax[3][3];
    for (int i = 0; i < 3; i++)
    {
        eye[i] = pos[i] + fwd[i] * g.SeatForward - left[i] * g.SeatRight + up[i] * g.SeatUp;
        for (int k = 0; k < 3; k++) eye[i] += B[i][k] * t[k];
        for (int j = 0; j < 3; j++)
            ax[i][j] = B[i][0] * R.m[0][j] + B[i][1] * R.m[1][j] + B[i][2] * R.m[2][j];
    }
    for (int i = 0; i < 3; i++)
    {
        for (int j = 0; j < 3; j++) M[i * 4 + j] = ax[i][j];
        M[i * 4 + 3] = 0.0f;
    }
    for (int j = 0; j < 3; j++) M[12 + j] = -(eye[0] * ax[0][j] + eye[1] * ax[1][j] + eye[2] * ax[2][j]);
    M[15] = 1.0f;

    // Remember this frame's view and seat for the in-car gauges drawn at Present.
    memcpy(g_view.m, M, sizeof(g_view.m));
    for (int i = 0; i < 3; i++)
    {
        g_view.seat[i] = pos[i] + fwd[i] * g.SeatForward - left[i] * g.SeatRight + up[i] * g.SeatUp;
        g_view.fwd[i] = fwd[i]; g_view.right[i] = -left[i]; g_view.up[i] = up[i];
    }
    if (fov > 0) g_view.fov = fov;
    g_view.frame = g_frame;
    return true;
}

// Render-time hook: the renderer copies the player camera into its per-view data (eView[1]
// platform info) every rendered frame (call at 0x5C7EF0). The game can render more frames than
// it updates the camera, so the head pose and the eye of *this* frame are applied here; otherwise
// a frame could be drawn with the other eye's camera, which shows as flicker.
static void __cdecl RenderCopyView(float* dst, const float* src)
{
    float M[16];
    memcpy(M, src, sizeof(M));
    if (dst == *(float**)0x832E50 && CockpitActive() && BuildCockpitView(M, true, 0))
        InterlockedIncrement(&g_renderUpdates);
    memcpy(dst, M, sizeof(M));
}

static void __cdecl CopyCameraMatrix(float* dst, const float* src)
{
    float M[16];
    memcpy(M, src, sizeof(M));

    if (g.GameFOV > 0 && g_trackingOn && g.Enabled && dst == (float*)*kPlayerCamera && (!g.OnlyInRace || InRace()))
        *(WORD*)((BYTE*)dst + 0xC4) = (WORD)(g.GameFOV * 65536.0f / 360.0f);

    // Game-side camera (culling, sound, LOD): cockpit view for the middle between the eyes.
    if (dst == (float*)*kPlayerCamera && CockpitActive() && BuildCockpitView(M, false, *(WORD*)((BYTE*)dst + 0xC4) * 360.0f / 65536.0f))
    {
        InterlockedIncrement(&g_simUpdates);
        memcpy(dst, M, sizeof(M));
        return;
    }

    if (g_trackingOn && g.Enabled && dst == (float*)*kPlayerCamera && (!g.OnlyInRace || InRace()))
    {
        M3 R = { { { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 } } };
        float t[3] = { 0, 0, 0 };
        bool posed = HeadInGameSpace(R, t);  // identity when no headset pose is available
        static int logged = 0;
        if (logged < 2 && (posed || !logged))
        {
            logged = posed ? 2 : 1;
            Log("player camera %p updated (%s)", dst, posed ? "with head pose" : "seat offset only, no headset pose yet");
        }

        if (g.Stereo && (g_vrReady || g.TestMotion))
        {
            // Eye offset along the head's right axis (game view space column 0 of R).
            float e = ((g_frame & 1) ? 0.5f : -0.5f) * (g.IPD > 0 ? g.IPD : g_ipd) * g.PositionScaleStereo;
            for (int i = 0; i < 3; i++) t[i] += e * R.m[i][0];
        }
        if (g.GameFOV > 0)
            *(WORD*)((BYTE*)dst + 0xC4) = (WORD)(g.GameFOV * 65536.0f / 360.0f);

        float pos[3], fwd[3], left[3], up[3];
        if (g.Cockpit && GetCarFrame(pos, fwd, left, up))
        {
            // Camera fixed to the car body: B = world axes of the seat (right, down, forward).
            float B[3][3];
            for (int i = 0; i < 3; i++) { B[i][0] = -left[i]; B[i][1] = -up[i]; B[i][2] = fwd[i]; }
            float eye[3], ax[3][3];
            for (int i = 0; i < 3; i++)
            {
                eye[i] = pos[i] + fwd[i] * g.SeatForward - left[i] * g.SeatRight + up[i] * g.SeatUp;
                for (int k = 0; k < 3; k++) eye[i] += B[i][k] * t[k];
                for (int j = 0; j < 3; j++)
                    ax[i][j] = B[i][0] * R.m[0][j] + B[i][1] * R.m[1][j] + B[i][2] * R.m[2][j];
            }
            for (int i = 0; i < 3; i++)
            {
                for (int j = 0; j < 3; j++) M[i * 4 + j] = ax[i][j];
                M[i * 4 + 3] = 0.0f;
            }
            for (int j = 0; j < 3; j++) M[12 + j] = -(eye[0] * ax[0][j] + eye[1] * ax[1][j] + eye[2] * ax[2][j]);
            M[15] = 1.0f;
            memcpy(dst, M, sizeof(M));

            // Remember this frame's view and seat for the in-car gauges drawn at Present.
            memcpy(g_view.m, M, sizeof(M));
            for (int i = 0; i < 3; i++)
            {
                g_view.seat[i] = pos[i] + fwd[i] * g.SeatForward - left[i] * g.SeatRight + up[i] * g.SeatUp;
                g_view.fwd[i] = fwd[i]; g_view.right[i] = -left[i]; g_view.up[i] = up[i];
            }
            g_view.fov = *(WORD*)((BYTE*)dst + 0xC4) * 360.0f / 65536.0f;
            g_view.frame = g_frame;
            return;
        }

        // Fallback: seat offset relative to the game's own camera (x right, y down, z forward).
        float o[3] = { g.SeatRight + t[0], -g.SeatUp + t[1], g.SeatForward + t[2] };

        // A = T(-o) * R (row-vector convention), M' = M * A.
        float A[4][4] = {};
        for (int i = 0; i < 3; i++)
            for (int j = 0; j < 3; j++)
                A[i][j] = R.m[i][j];
        for (int j = 0; j < 3; j++)
            A[3][j] = -(o[0] * R.m[0][j] + o[1] * R.m[1][j] + o[2] * R.m[2][j]);
        A[3][3] = 1.0f;

        float out[16];
        for (int i = 0; i < 4; i++)
            for (int j = 0; j < 4; j++)
            {
                float s = 0;
                for (int k = 0; k < 4; k++) s += M[i * 4 + k] * A[k][j];
                out[i * 4 + j] = s;
            }
        memcpy(M, out, sizeof(M));
    }
    memcpy(dst, M, sizeof(M));
}

// ---------------------------------------------------------------- cursor
static HWND g_wnd;
static WNDPROC g_origProc;

static LRESULT CALLBACK WndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_SETCURSOR && LOWORD(lp) == HTCLIENT && g.HideCursor && g_trackingOn && InRace())
    {
        SetCursor(nullptr);
        return TRUE;
    }
    return CallWindowProcA(g_origProc, h, msg, wp, lp);
}

static BOOL CALLBACK FindGameWindow(HWND h, LPARAM)
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

// ---------------------------------------------------------------- compact HUD
// Data (SPEED2.EXE v1.2):
//   car anchor (view 1 mover + 0x2C): +0x70 velocity (m/s, world), +0x440 engine rpm,
//                                     +0x1C -> car, car +0x1CC gear (0 = R, 1 = N, 2 = 1st ...)
//   race status *(0x890118): +0x00 racer count, +0x14 race clock (4000 ticks/s), +0xBE8 laps
//   player racer entry = race status + 0x30: +0x0A position (1-based), +0x14 laps done (float), +0x44 lap start tick
struct HudData
{
    bool valid = false;
    int speed = 0, gear = 1, pos = 0, racers = 0, lap = 0, laps = 0;
    float lapTime = 0, rpm = 0;
};

static bool ReadHud(HudData& h)
{
    __try
    {
        BYTE* view = (BYTE*)0x832E50;
        BYTE* node = *(BYTE**)(view + 0x44);
        if (!node || node == view + 0x44) return false;
        BYTE* anchor = *(BYTE**)(node - 4 + 0x2C);
        if (!anchor) return false;
        const float* v = (const float*)(anchor + 0x70);
        float sp = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]) * 3.6f;
        h.speed = sp < 999 ? (int)(sp + 0.5f) : 0;
        h.rpm = *(float*)(anchor + 0x440);  // engine rpm (800 at idle)
        if (!(h.rpm >= 0 && h.rpm < 20000)) h.rpm = 0;
        BYTE* car = *(BYTE**)(anchor + 0x1C);
        h.gear = car ? *(int*)(car + 0x1CC) : 1;
        if (h.gear < 0 || h.gear > 9) h.gear = 1;

        BYTE* rs = *(BYTE**)0x890118;
        h.racers = 0; h.laps = 0;
        if (rs)
        {
            h.racers = *(int*)rs;
            h.laps = *(int*)(rs + 0xBE8);
            BYTE* e = rs + 0x30;
            h.pos = e[0x0A];
            float done = *(float*)(e + 0x14);
            h.lap = (int)done + 1;
            if (h.lap > h.laps) h.lap = h.laps;
            int now = *(int*)(rs + 0x14), start = *(int*)(e + 0x44);
            h.lapTime = (now >= start && start > 0) ? (now - start) / 4000.0f : now / 4000.0f;
            if (h.racers < 1 || h.racers > 8 || h.pos < 1 || h.pos > h.racers) h.racers = 0;
            if (h.laps < 1 || h.laps > 127) h.laps = 0;
        }
        h.valid = true;
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

struct HudVtx { float x, y, z, rhw; D3DCOLOR c; float tu, tv; };
static HudVtx g_hudV[3 * 8192];
static int g_hudN;

// Draw batches: consecutive triangles that share a texture (nullptr = plain colour).
struct HudCmd { IDirect3DTexture9* tex; int first; };
static HudCmd g_hudCmd[64];
static int g_hudCmdN;

static void UseTexture(IDirect3DTexture9* t)
{
    if (g_hudCmdN > 0 && g_hudCmd[g_hudCmdN - 1].tex == t) return;
    if (g_hudCmdN > 0 && g_hudCmd[g_hudCmdN - 1].first == g_hudN) { g_hudCmd[g_hudCmdN - 1].tex = t; return; }
    if (g_hudCmdN < 64) g_hudCmd[g_hudCmdN++] = { t, g_hudN };
}

// Optional mapping from a gauge's local 2D coordinates (y down) onto a plane in the car,
// projected with this frame's view matrix. Off = plain screen pixels.
struct Xform
{
    bool on = false;
    float c[3], u[3], v[3];  // plane origin and axes in world units per local unit
    float W, H, tanH, tanV;
    const float* m;
} g_xf;

static bool Map(float x, float y, HudVtx& o, D3DCOLOR c)
{
    o.z = 0; o.rhw = 1; o.c = c; o.tu = o.tv = 0;
    if (!g_xf.on) { o.x = x; o.y = y; return true; }
    float p[3];
    for (int i = 0; i < 3; i++) p[i] = g_xf.c[i] + g_xf.u[i] * x + g_xf.v[i] * y;
    // m is the game's own view * projection for this frame (row vectors, D3D clip space).
    const float* m = g_xf.m;
    float cx = p[0] * m[0] + p[1] * m[4] + p[2] * m[8] + m[12];
    float cy = p[0] * m[1] + p[1] * m[5] + p[2] * m[9] + m[13];
    float cw = p[0] * m[3] + p[1] * m[7] + p[2] * m[11] + m[15];
    if (cw < 0.05f) return false;
    o.x = g_xf.W * 0.5f * (1.0f + cx / cw);
    o.y = g_xf.H * 0.5f * (1.0f - cy / cw);
    o.rhw = 1.0f / cw;  // perspective-correct texturing on the tilted gauge plane
    return true;
}

static void TexTri(float x0, float y0, float u0, float v0, float x1, float y1, float u1, float v1,
                   float x2, float y2, float u2, float v2, IDirect3DTexture9* t, D3DCOLOR c = 0xFFFFFFFF)
{
    if (!t || g_hudN + 3 > (int)(sizeof(g_hudV) / sizeof(g_hudV[0]))) return;
    UseTexture(t);
    HudVtx* o = g_hudV + g_hudN;
    if (Map(x0, y0, o[0], c) && Map(x1, y1, o[1], c) && Map(x2, y2, o[2], c))
    {
        o[0].tu = u0; o[0].tv = v0; o[1].tu = u1; o[1].tv = v1; o[2].tu = u2; o[2].tv = v2;
        g_hudN += 3;
    }
    UseTexture(nullptr);
}

// ---------------------------------------------------------------- minimap capture
// The game draws its HUD in the last pass of the frame (call 0x5CBF40(0) at 0x5D2B60). Around that
// call the minimap's screen square is copied before (scene only) and after (scene + map); the
// "after" copy becomes the map instrument on the dashboard and the "before" copy is put back, so
// the map vanishes from the picture in front of your eyes.
static IDirect3DTexture9* g_mapTexA;
static IDirect3DTexture9* g_mapTexB;
static UINT g_mapSize;
static float g_mapW = 1, g_mapH = 1;      // size of the copied frame
static float g_mapU, g_mapV, g_mapRU, g_mapRV;  // map circle centre and radius in texture coordinates
static D3DFORMAT g_mapFmt;
static volatile LONG g_mapFrame = -100;
static const float g_mapMargin = 1.25f;  // captured square = map diameter * margin

static void ReleaseMapTextures()
{
    if (g_mapTexA) { g_mapTexA->Release(); g_mapTexA = nullptr; }
    if (g_mapTexB) { g_mapTexB->Release(); g_mapTexB = nullptr; }
    g_mapSize = 0;
    g_mapFrame = -100;
}

typedef void(__cdecl* HudPassFn)(int);

// State of the capture in progress during one HUD pass.
static struct { bool active, grabbed, restored; RECT R; IDirect3DSurface9* rt; } g_grab;
static DWORD g_grabAt, g_grabTarget;  // optional: grab the "before" copy right before this inner call

static void __cdecl GrabBefore()
{
    if (!g_grab.active || g_grab.grabbed) return;
    IDirect3DDevice9* dev = *(IDirect3DDevice9**)0x870974;
    IDirect3DSurface9* a = nullptr;
    if (g_mapTexA && SUCCEEDED(g_mapTexA->GetSurfaceLevel(0, &a)))
    {
        // Whole-frame copies: copying just a sub-rectangle of the game's render target made the
        // AMD driver drop everything drawn into that rectangle afterwards (a visible square).
        g_grab.grabbed = SUCCEEDED(dev->StretchRect(g_grab.rt, nullptr, a, nullptr, D3DTEXF_NONE));
        a->Release();
    }
}

static RECT g_darkenR;
static volatile LONG g_darkenFrame = -100;

// Multiplies a rectangle of the back buffer by MapPatchShade (called from the Present hook).
static void DarkenMapSquare(IDirect3DDevice9* dev)
{
    if (g_darkenFrame != g_frame) return;
    IDirect3DSurface9* bb = nullptr;
    if (FAILED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) || !bb) return;
    IDirect3DStateBlock9* sb = nullptr;
    IDirect3DSurface9* oldRT = nullptr;
    if (SUCCEEDED(dev->CreateStateBlock(D3DSBT_ALL, &sb)) && sb)
    {
        sb->Capture();
        dev->GetRenderTarget(0, &oldRT);
        dev->SetRenderTarget(0, bb);
        D3DSURFACE_DESC sd; bb->GetDesc(&sd);
        D3DVIEWPORT9 vp = { 0, 0, sd.Width, sd.Height, 0.0f, 1.0f };
        dev->SetViewport(&vp);
        dev->BeginScene();
        const RECT& R = g_darkenR;
        BYTE k = (BYTE)std::min(255.0f, std::max(0.0f, g.MapPatchShade * 255.0f));
        D3DCOLOR c = D3DCOLOR_ARGB(255, k, k, k);
        float x0 = R.left - 0.5f, y0 = R.top - 0.5f, x1 = R.right - 0.5f, y1 = R.bottom - 0.5f;
        HudVtx q[6] = { { x0, y0, 0, 1, c, 0, 0 }, { x1, y0, 0, 1, c, 0, 0 }, { x0, y1, 0, 1, c, 0, 0 },
                        { x1, y0, 0, 1, c, 0, 0 }, { x1, y1, 0, 1, c, 0, 0 }, { x0, y1, 0, 1, c, 0, 0 } };
        dev->SetVertexShader(nullptr);
        dev->SetPixelShader(nullptr);
        dev->SetTexture(0, nullptr);
        dev->SetFVF(D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1);
        dev->SetRenderState(D3DRS_ZENABLE, FALSE);
        dev->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
        dev->SetRenderState(D3DRS_STENCILENABLE, FALSE);
        dev->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
        dev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
        dev->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_ZERO);
        dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_SRCCOLOR);  // back buffer * k
        dev->SetRenderState(D3DRS_BLENDOP, D3DBLENDOP_ADD);
        dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
        dev->SetRenderState(D3DRS_FILLMODE, D3DFILL_SOLID);
        dev->SetRenderState(D3DRS_LIGHTING, FALSE);
        dev->SetRenderState(D3DRS_FOGENABLE, FALSE);
        dev->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
        dev->SetRenderState(D3DRS_COLORWRITEENABLE, 0x7);
        dev->SetRenderState(D3DRS_SRGBWRITEENABLE, FALSE);
        dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
        dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
        dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
        dev->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE);
        dev->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
        dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 2, q, sizeof(HudVtx));
        dev->EndScene();
        if (oldRT) { dev->SetRenderTarget(0, oldRT); oldRT->Release(); }
        sb->Apply();
        sb->Release();
    }
    bb->Release();
}

// Puts the clean picture (texture A, grabbed before the HUD stage) back over the map's round
// footprint on the current render target, fading out at the edge. The HUD stage lays a dark veil
// over the whole screen that A does not have yet, so A is darkened by MapPatchShade to match.
static void RoundRestore(IDirect3DDevice9* dev, const RECT& R)
{
    IDirect3DStateBlock9* sb = nullptr;
    if (FAILED(dev->CreateStateBlock(D3DSBT_ALL, &sb)) || !sb) return;
    sb->Capture();
    float size = (float)(R.right - R.left), cx = R.left + size * 0.5f, cy = R.top + size * 0.5f;
    float rIn = size * 0.5f / g_mapMargin * 1.10f, rOut = size * 0.5f * 0.98f;
    BYTE k = (BYTE)std::min(255.0f, std::max(0.0f, g.MapPatchShade * 255.0f));
    const int N = 64;
    static HudVtx v[N * 9];
    int n = 0;
    auto vert = [&](float r, float ang, BYTE alpha)
    {
        float x = cx + r * cosf(ang), y = cy + r * sinf(ang);
        HudVtx& o = v[n++];
        o.x = x - 0.5f; o.y = y - 0.5f; o.z = 0; o.rhw = 1;
        o.c = D3DCOLOR_ARGB(alpha, k, k, k);
        o.tu = x / g_mapW; o.tv = y / g_mapH;  // textures hold the whole frame
    };
    for (int i = 0; i < N; i++)
    {
        float a0 = 6.2831853f * i / N, a1 = 6.2831853f * (i + 1) / N;
        vert(0, 0, 255); vert(rIn, a0, 255); vert(rIn, a1, 255);
        vert(rIn, a0, 255); vert(rOut, a0, 0); vert(rOut, a1, 0);
        vert(rIn, a0, 255); vert(rOut, a1, 0); vert(rIn, a1, 255);
    }
    dev->SetVertexShader(nullptr);
    dev->SetPixelShader(nullptr);
    dev->SetTexture(0, g_mapTexA);
    dev->SetFVF(D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1);
    dev->SetRenderState(D3DRS_ZENABLE, FALSE);
    dev->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
    dev->SetRenderState(D3DRS_STENCILENABLE, FALSE);
    dev->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
    dev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    dev->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    dev->SetRenderState(D3DRS_BLENDOP, D3DBLENDOP_ADD);
    dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    dev->SetRenderState(D3DRS_FILLMODE, D3DFILL_SOLID);
    dev->SetRenderState(D3DRS_LIGHTING, FALSE);
    dev->SetRenderState(D3DRS_FOGENABLE, FALSE);
    dev->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
    dev->SetRenderState(D3DRS_COLORWRITEENABLE, 0x7);  // colour only, keep the target's alpha
    dev->SetRenderState(D3DRS_SRGBWRITEENABLE, FALSE);
    dev->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_POINT);
    dev->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
    dev->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
    dev->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
    dev->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
    dev->SetSamplerState(0, D3DSAMP_SRGBTEXTURE, FALSE);
    dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
    dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    dev->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
    dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
    dev->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE);
    dev->SetTextureStageState(0, D3DTSS_TEXCOORDINDEX, 0);
    dev->SetTextureStageState(0, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
    dev->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
    dev->SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
    dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, n / 3, v, sizeof(HudVtx));
    sb->Apply();
    sb->Release();
}

// After the HUD call: copy the frame (picture + map) for the map instrument and put the clean
// picture back over the map, before the rest of the pass applies its full-screen effects.
static void __cdecl GrabAfter()
{
    if (!g_grab.active || !g_grab.grabbed) return;
    IDirect3DDevice9* dev = *(IDirect3DDevice9**)0x870974;
    IDirect3DSurface9* rt2 = nullptr, *b = nullptr;
    bool same = SUCCEEDED(dev->GetRenderTarget(0, &rt2)) && rt2 == g_grab.rt;
    static bool logged;
    if (!logged) { logged = true; Log("map restore point: render target %s", same ? "unchanged" : "changed, restoring at the end of the pass"); }
    if (same && SUCCEEDED(g_mapTexB->GetSurfaceLevel(0, &b)) && SUCCEEDED(dev->StretchRect(rt2, nullptr, b, nullptr, D3DTEXF_NONE)))
    {
        RoundRestore(dev, g_grab.R);
        g_mapFrame = g_frame;
        g_grab.restored = true;
    }
    if (b) b->Release();
    if (rt2) rt2->Release();
}

// Two E8 calls inside the HUD pass are redirected: before the first one the clean picture is
// grabbed, after the second one the map is copied and the clean picture put back.
static DWORD g_grabRet, g_restoreAt, g_restoreTarget;
static __declspec(naked) void GrabThunk()
{
    __asm
    {
        pushad
        pushfd
        call GrabBefore
        popfd
        popad
        jmp dword ptr [g_grabTarget]
    }
}

static __declspec(naked) void RestoreThunk()
{
    __asm
    {
        pop dword ptr [g_grabRet]       // caller's return address; arguments stay in place
        call dword ptr [g_restoreTarget]
        pushad
        pushfd
        call GrabAfter
        popfd
        popad
        push dword ptr [g_grabRet]
        ret
    }
}

// Grab and restore around one and the same call.
static __declspec(naked) void GrabRestoreThunk()
{
    __asm
    {
        pushad
        pushfd
        call GrabBefore
        popfd
        popad
        pop dword ptr [g_grabRet]
        call dword ptr [g_grabTarget]
        pushad
        pushfd
        call GrabAfter
        popfd
        popad
        push dword ptr [g_grabRet]
        ret
    }
}

static volatile bool g_hooksReady;  // device hooks (incl. depth-stencil get/set) installed
static void ResolveDepth(IDirect3DDevice9* dev);
static bool InitDepthStereo(IDirect3DDevice9* dev);
static void __cdecl HudPassHook(int arg)
{
    IDirect3DDevice9* dev = *(IDirect3DDevice9**)0x870974;
    if (dev && g_hooksReady && InRace() && g.StereoMode == 1 && g_vrReady && g.Overlay && InitDepthStereo(dev)) ResolveDepth(dev);
    IDirect3DSurface9* rt = nullptr;
    RECT R = {};
    bool ok = false;
    g_grab.active = g_grab.grabbed = g_grab.restored = false;
    if (dev && g.MapOnDash && g.Gauges && g.Hud && CockpitActive() && InRace() && (g_frame - g_view.frame) <= 2 &&
        SUCCEEDED(dev->GetRenderTarget(0, &rt)) && rt)
    {
        D3DSURFACE_DESC sd; rt->GetDesc(&sd);
        float sc = sd.Height / 480.0f;
        float cx = sd.Width * 0.5f + g.MapX * sc, cy = sd.Height * 0.5f + g.MapY * sc, half = g.MapRadius * g_mapMargin * sc;
        R = { (LONG)(cx - half), (LONG)(cy - half), (LONG)(cx + half), (LONG)(cy + half) };
        UINT size = (UINT)(R.right - R.left);
        if (R.left >= 0 && R.top >= 0 && R.right <= (LONG)sd.Width && R.bottom <= (LONG)sd.Height && size > 8)
        {
            if (!g_mapTexA || g_mapSize != size || g_mapW != (float)sd.Width || g_mapH != (float)sd.Height || g_mapFmt != sd.Format)
            {
                ReleaseMapTextures();
                if (SUCCEEDED(dev->CreateTexture(sd.Width, sd.Height, 1, D3DUSAGE_RENDERTARGET, sd.Format, D3DPOOL_DEFAULT, &g_mapTexA, nullptr)) &&
                    SUCCEEDED(dev->CreateTexture(sd.Width, sd.Height, 1, D3DUSAGE_RENDERTARGET, sd.Format, D3DPOOL_DEFAULT, &g_mapTexB, nullptr)))
                {
                    g_mapSize = size; g_mapFmt = sd.Format; g_mapW = (float)sd.Width; g_mapH = (float)sd.Height;
                    Log("minimap capture: map %u px at %ld,%ld (frame %ux%u)", size, R.left, R.top, sd.Width, sd.Height);
                }
                else ReleaseMapTextures();
            }
            g_mapU = cx / g_mapW; g_mapV = cy / g_mapH;
            g_mapRU = g.MapRadius * sc / g_mapW; g_mapRV = g.MapRadius * sc / g_mapH;
            if (g_mapTexA)
            {
                g_grab.active = true; g_grab.R = R; g_grab.rt = rt;
                if (!g_grabAt) GrabBefore();
                ok = true;
            }
        }
    }
    ((HudPassFn)0x5CBF40)(arg);
    g_grab.active = false;
    ok = ok && g_grab.grabbed && !g_grab.restored;  // with the inner grab point everything is done already
    if (ok)
    {
        IDirect3DSurface9* rt2 = nullptr, *a = nullptr, *b = nullptr;
        if (SUCCEEDED(dev->GetRenderTarget(0, &rt2)) && rt2 && SUCCEEDED(g_mapTexA->GetSurfaceLevel(0, &a)) &&
            SUCCEEDED(g_mapTexB->GetSurfaceLevel(0, &b)))
        {
            static bool logged;
            if (!logged) { logged = true; Log("minimap pass: render target %s", rt2 == rt ? "unchanged" : "CHANGED"); }
            if (rt2 == rt && SUCCEEDED(dev->StretchRect(rt2, nullptr, b, nullptr, D3DTEXF_NONE)))
            {
                RoundRestore(dev, R);
                g_mapFrame = g_frame;
            }
        }
        if (a) a->Release();
        if (b) b->Release();
        if (rt2) rt2->Release();
    }
    if (rt) rt->Release();
}

// Textured quad: local corners (clockwise from top left) with texture coordinates.
static void TexQuad(const float* xy, const float* uv, IDirect3DTexture9* t, D3DCOLOR c = 0xFFFFFFFF)
{
    if (!t || g_hudN + 6 > (int)(sizeof(g_hudV) / sizeof(g_hudV[0]))) return;
    UseTexture(t);
    HudVtx q[4];
    for (int i = 0; i < 4; i++)
    {
        if (!Map(xy[i * 2], xy[i * 2 + 1], q[i], c)) { UseTexture(nullptr); return; }
        q[i].tu = uv[i * 2]; q[i].tv = uv[i * 2 + 1];
    }
    HudVtx* o = g_hudV + g_hudN;
    o[0] = q[0]; o[1] = q[1]; o[2] = q[2]; o[3] = q[0]; o[4] = q[2]; o[5] = q[3];
    g_hudN += 6;
    UseTexture(nullptr);
}

static void Tri(float x0, float y0, float x1, float y1, float x2, float y2, D3DCOLOR c)
{
    if (g_hudN + 3 > (int)(sizeof(g_hudV) / sizeof(g_hudV[0]))) return;
    HudVtx* o = g_hudV + g_hudN;
    if (Map(x0, y0, o[0], c) && Map(x1, y1, o[1], c) && Map(x2, y2, o[2], c)) g_hudN += 3;
}

static void Poly4(float x0, float y0, float x1, float y1, float x2, float y2, float x3, float y3, D3DCOLOR c)
{
    Tri(x0, y0, x1, y1, x2, y2, c);
    Tri(x0, y0, x2, y2, x3, y3, c);
}

static void Quad(float x, float y, float w, float h, D3DCOLOR c)
{
    Poly4(x, y, x + w, y, x + w, y + h, x, y + h, c);
}

// Seven-segment glyphs: bits a b c d e f g (a = top, then clockwise, g = middle).
static int SegMask(char ch)
{
    switch (ch)
    {
    case '0': return 0x3F; case '1': return 0x06; case '2': return 0x5B; case '3': return 0x4F; case '4': return 0x66;
    case '5': return 0x6D; case '6': return 0x7D; case '7': return 0x07; case '8': return 0x7F; case '9': return 0x6F;
    case 'P': return 0x73; case 'L': return 0x38; case 'R': return 0x50; case 'N': return 0x54; case '-': return 0x40;
    default: return 0;
    }
}

// Draws text at (x, y) with glyph height h; returns the advance width.
static float Text(float x, float y, float h, const char* s, D3DCOLOR c)
{
    float w = h * 0.5f, t = std::max(2.0f, h * 0.12f), x0 = x;
    for (; *s; s++)
    {
        char ch = *s;
        if (ch == ' ') { x += w * 0.6f; continue; }
        if (ch == '.') { Quad(x, y + h - t, t, t, c); x += t * 2.2f; continue; }
        if (ch == ':') { Quad(x, y + h * 0.28f, t, t, c); Quad(x, y + h * 0.66f, t, t, c); x += t * 2.2f; continue; }
        if (ch == '/')
        {
            for (int i = 0; i < 6; i++) Quad(x + w * 0.5f * (5 - i) / 5.0f, y + h * i / 6.0f, t, h / 6.0f + 1, c);
            x += w * 0.75f; continue;
        }
        int m = SegMask(ch);
        float hh = h * 0.5f;
        if (m & 0x01) Quad(x, y, w, t, c);                    // a
        if (m & 0x02) Quad(x + w - t, y, t, hh, c);           // b
        if (m & 0x04) Quad(x + w - t, y + hh, t, hh, c);      // c
        if (m & 0x08) Quad(x, y + h - t, w, t, c);            // d
        if (m & 0x10) Quad(x, y + hh, t, hh, c);              // e
        if (m & 0x20) Quad(x, y, t, hh, c);                   // f
        if (m & 0x40) Quad(x, y + hh - t * 0.5f, w, t, c);    // g
        x += w + t * 1.6f;
    }
    return x - x0;
}

static float TextWidth(float h, const char* s)
{
    int save = g_hudN;
    float w = Text(0, -10000, h, s, 0);
    g_hudN = save;
    return w;
}

// ---------------------------------------------------------------- gauge_mod (PNG gauges)
// <game folder>\gauge_mod\gauges.ini names a face and a needle PNG per gauge plus its scale.
// Faces are square and fill the whole gauge; needles use the same square canvas, point straight
// up (12 o'clock) and turn around the image center. Missing files fall back to the built-in gauge.
struct GaugeImage
{
    std::vector<BYTE> bgra;  // straight alpha, top-down
    UINT w = 0, h = 0;
    IDirect3DTexture9* tex = nullptr;
    IDirect3DDevice9* dev = nullptr;
};

struct GaugeStyle
{
    GaugeImage face, needle;
    float minAngle = -135, maxAngle = 135;  // degrees from 12 o'clock, clockwise
    float maxValue = 0, redline = 0;        // 0 = use the [GAUGES] values
    int digital = 1;
    float digitalY = 0.34f, digitalSize = 0.26f;  // fractions of the gauge radius
    D3DCOLOR digitalColor = 0;
};
static GaugeStyle g_style[2];  // 0 = tachometer, 1 = speedometer
static char g_gameDir[MAX_PATH];
static volatile bool g_stylesLoaded;

static bool LoadPng(const std::wstring& path, GaugeImage& img)
{
    IWICImagingFactory* fac = nullptr;
    IWICBitmapDecoder* dec = nullptr;
    IWICBitmapFrameDecode* frm = nullptr;
    IWICFormatConverter* cv = nullptr;
    bool ok = false;
    if (SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&fac))) &&
        SUCCEEDED(fac->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnLoad, &dec)) &&
        SUCCEEDED(dec->GetFrame(0, &frm)) && SUCCEEDED(fac->CreateFormatConverter(&cv)) &&
        SUCCEEDED(cv->Initialize(frm, GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom)))
    {
        cv->GetSize(&img.w, &img.h);
        if (img.w && img.h && img.w <= 4096 && img.h <= 4096)
        {
            img.bgra.resize((size_t)img.w * img.h * 4);
            ok = SUCCEEDED(cv->CopyPixels(nullptr, img.w * 4, (UINT)img.bgra.size(), img.bgra.data()));
        }
    }
    if (cv) cv->Release();
    if (frm) frm->Release();
    if (dec) dec->Release();
    if (fac) fac->Release();
    if (!ok) img.bgra.clear();
    return ok;
}

static D3DCOLOR ParseColor(const char* s, D3DCOLOR def)
{
    unsigned r, g2, b, a = 255;
    if (sscanf(s, "%u,%u,%u,%u", &r, &g2, &b, &a) >= 3) return D3DCOLOR_ARGB(a & 255, r & 255, g2 & 255, b & 255);
    return def;
}

static void LoadGaugeStyles()
{
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    char ini[MAX_PATH];
    sprintf(ini, "%s\\gauge_mod\\gauges.ini", g_gameDir);
    const char* sec[2] = { "Tachometer", "Speedometer" };
    for (int k = 0; k < 2; k++)
    {
        GaugeStyle& s = g_style[k];
        char buf[MAX_PATH], def[64];
        auto F = [&](const char* key, float d) { sprintf(def, "%g", d); GetPrivateProfileStringA(sec[k], key, def, buf, sizeof(buf), ini); return (float)atof(buf); };
        s.minAngle = F("MinAngle", s.minAngle);
        s.maxAngle = F("MaxAngle", s.maxAngle);
        s.maxValue = F("MaxValue", 0);
        s.redline = F("Redline", 0);
        s.digital = GetPrivateProfileIntA(sec[k], "Digital", 1, ini);
        s.digitalY = F("DigitalY", s.digitalY);
        s.digitalSize = F("DigitalSize", s.digitalSize);
        GetPrivateProfileStringA(sec[k], "DigitalColor", "", buf, sizeof(buf), ini);
        s.digitalColor = ParseColor(buf, k == 0 ? D3DCOLOR_ARGB(255, 255, 140, 30) : D3DCOLOR_ARGB(255, 240, 240, 240));
        const char* keys[2] = { "Face", "Needle" };
        GaugeImage* imgs[2] = { &s.face, &s.needle };
        for (int i = 0; i < 2; i++)
        {
            GetPrivateProfileStringA(sec[k], keys[i], "", buf, sizeof(buf), ini);
            if (!buf[0]) continue;
            char full[MAX_PATH * 2];
            sprintf(full, "%s\\gauge_mod\\%s", g_gameDir, buf);
            wchar_t wfull[MAX_PATH * 2];
            MultiByteToWideChar(CP_ACP, 0, full, -1, wfull, MAX_PATH * 2);
            if (LoadPng(wfull, *imgs[i])) Log("gauge_mod: %s %s %ux%u", sec[k], buf, imgs[i]->w, imgs[i]->h);
            else Log("gauge_mod: could not load %s", full);
        }
    }
    g_stylesLoaded = true;
}

// Creates (once per device) a managed texture with a full mip chain, so the gauge stays sharp
// and free of shimmer at any distance.
static IDirect3DTexture9* GaugeTexture(IDirect3DDevice9* dev, GaugeImage& img)
{
    if (img.bgra.empty()) return nullptr;
    if (img.tex && img.dev == dev) return img.tex;
    if (img.tex) { img.tex->Release(); img.tex = nullptr; }
    if (FAILED(dev->CreateTexture(img.w, img.h, 0, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &img.tex, nullptr))) { img.bgra.clear(); return nullptr; }
    img.dev = dev;
    std::vector<BYTE> level = img.bgra;
    UINT w = img.w, h = img.h;
    for (DWORD l = 0; l < img.tex->GetLevelCount(); l++)
    {
        D3DLOCKED_RECT lr;
        if (SUCCEEDED(img.tex->LockRect(l, &lr, nullptr, 0)))
        {
            for (UINT y = 0; y < h; y++) memcpy((BYTE*)lr.pBits + y * lr.Pitch, level.data() + (size_t)y * w * 4, w * 4);
            img.tex->UnlockRect(l);
        }
        if (w == 1 && h == 1) break;
        // Box-filter the next level, weighting colour by alpha so transparent edges do not darken.
        UINT nw = std::max(1u, w / 2), nh = std::max(1u, h / 2);
        std::vector<BYTE> next((size_t)nw * nh * 4);
        for (UINT y = 0; y < nh; y++)
            for (UINT x = 0; x < nw; x++)
            {
                float acc[4] = {};
                for (UINT dy = 0; dy < 2; dy++)
                    for (UINT dx = 0; dx < 2; dx++)
                    {
                        UINT sx = std::min(w - 1, x * 2 + dx), sy = std::min(h - 1, y * 2 + dy);
                        const BYTE* p = level.data() + ((size_t)sy * w + sx) * 4;
                        float a = p[3] / 255.0f;
                        acc[0] += p[0] * a; acc[1] += p[1] * a; acc[2] += p[2] * a; acc[3] += a;
                    }
                BYTE* o = next.data() + ((size_t)y * nw + x) * 4;
                for (int c = 0; c < 3; c++) o[c] = acc[3] > 0 ? (BYTE)std::min(255.0f, acc[c] / acc[3]) : 0;
                o[3] = (BYTE)(acc[3] / 4 * 255 + 0.5f);
            }
        level.swap(next); w = nw; h = nh;
    }
    return img.tex;
}

// ---------------------------------------------------------------- in-car round gauges
// Drawn in local units: radius 100, y down. Value fraction f maps to 225 deg (bottom left)
// clockwise through the top to -45 deg (bottom right).
static const GaugeStyle* g_curStyle;

// Angle (degrees from 12 o'clock, clockwise) for value fraction f.
static float GaugeAngle(float f)
{
    f = std::min(std::max(f, 0.0f), 1.05f);
    float a0 = g_curStyle ? g_curStyle->minAngle : -135.0f, a1 = g_curStyle ? g_curStyle->maxAngle : 135.0f;
    return a0 + (a1 - a0) * f;
}

static void GaugeDir(float f, float& dx, float& dy)
{
    float a = GaugeAngle(f) * 3.14159265f / 180.0f;
    dx = sinf(a); dy = -cosf(a);  // local y points down
}

static void Disc(float r, D3DCOLOR c, int n = 48)
{
    for (int i = 0; i < n; i++)
    {
        float a0 = 6.2831853f * i / n, a1 = 6.2831853f * (i + 1) / n;
        Tri(0, 0, r * cosf(a0), r * sinf(a0), r * cosf(a1), r * sinf(a1), c);
    }
}

static void Arc(float r0, float r1, float f0, float f1, D3DCOLOR c, int n = 40)
{
    for (int i = 0; i < n; i++)
    {
        float x0, y0, x1, y1;
        GaugeDir(f0 + (f1 - f0) * i / n, x0, y0);
        GaugeDir(f0 + (f1 - f0) * (i + 1) / n, x1, y1);
        Poly4(x0 * r0, y0 * r0, x0 * r1, y0 * r1, x1 * r1, y1 * r1, x1 * r0, y1 * r0, c);
    }
}

static void Tick(float f, float r0, float r1, float w, D3DCOLOR c)
{
    float dx, dy; GaugeDir(f, dx, dy);
    float nx = -dy * w * 0.5f, ny = dx * w * 0.5f;
    Poly4(dx * r0 + nx, dy * r0 + ny, dx * r1 + nx, dy * r1 + ny, dx * r1 - nx, dy * r1 - ny, dx * r0 - nx, dy * r0 - ny, c);
}

static void Needle(float f, D3DCOLOR c)
{
    float dx, dy; GaugeDir(f, dx, dy);
    float nx = -dy, ny = dx;
    Poly4(-dx * 14 + nx * 4, -dy * 14 + ny * 4, dx * 86 + nx * 1, dy * 86 + ny * 1,
          dx * 86 - nx * 1, dy * 86 - ny * 1, -dx * 14 - nx * 4, -dy * 14 - ny * 4, c);
}

static void CenteredText(float cx, float y, float h, const char* s, D3DCOLOR c)
{
    bool on = g_xf.on; g_xf.on = false;
    float w = TextWidth(h, s);
    g_xf.on = on;
    Text(cx - w * 0.5f, y, h, s, c);
}

static void Gauge(IDirect3DDevice9* dev, GaugeStyle& st, float value, float maxValue, float majorStep, int labelEvery,
                  float labelDiv, float redFrom, const char* readout)
{
    const D3DCOLOR face = D3DCOLOR_ARGB(235, 12, 13, 16), rim = D3DCOLOR_ARGB(255, 70, 72, 78);
    const D3DCOLOR tick = D3DCOLOR_ARGB(255, 230, 230, 230), red = D3DCOLOR_ARGB(255, 230, 40, 30);
    const D3DCOLOR needle = D3DCOLOR_ARGB(255, 255, 120, 20), cap = D3DCOLOR_ARGB(255, 40, 40, 44);
    g_curStyle = &st;
    if (st.maxValue > 0) maxValue = st.maxValue;
    if (st.redline > 0) redFrom = st.redline;

    static const float full[8] = { -104, -104, 104, -104, 104, 104, -104, 104 };
    static const float uv[8] = { 0, 0, 1, 0, 1, 1, 0, 1 };
    IDirect3DTexture9* faceTex = GaugeTexture(dev, st.face);
    if (faceTex)
        TexQuad(full, uv, faceTex);
    else
    {
        Disc(104, rim);
        Disc(98, face);
        if (redFrom < maxValue) Arc(84, 94, redFrom / maxValue, 1.0f, red);
        int steps = (int)(maxValue / majorStep + 0.5f);
        for (int i = 0; i <= steps * 2; i++)
        {
            float v = majorStep * 0.5f * i;
            bool major = (i % 2) == 0;
            D3DCOLOR tc = v >= redFrom ? red : tick;
            Tick(v / maxValue, major ? 78 : 86, 95, major ? 3.2f : 1.6f, tc);
            if (major && (i / 2) % labelEvery == 0)
            {
                char s[8]; sprintf(s, "%d", (int)(v / labelDiv + 0.5f));
                float dx, dy; GaugeDir(v / maxValue, dx, dy);
                CenteredText(dx * 62, dy * 62 - 7, 14, s, tc);
            }
        }
    }
    if (st.digital)
    {
        float h = 100 * st.digitalSize;
        CenteredText(0, 100 * st.digitalY, h, readout, st.digitalColor);
    }
    IDirect3DTexture9* needleTex = GaugeTexture(dev, st.needle);
    if (needleTex)
    {
        float a = GaugeAngle(value / maxValue) * 3.14159265f / 180.0f, c = cosf(a), s = sinf(a);
        float xy[8];
        for (int i = 0; i < 4; i++)
        {
            float x = full[i * 2], y = full[i * 2 + 1];
            xy[i * 2] = x * c - y * s;
            xy[i * 2 + 1] = x * s + y * c;
        }
        TexQuad(xy, uv, needleTex);
    }
    else
    {
        Needle(value / maxValue, needle);
        Disc(9, cap, 20);
    }
    g_curStyle = nullptr;
}

// Position / lap / lap time bar centred on (cx, cy); k scales it (screen pixels or gauge units).
static void DrawRacePanel(const HudData& d, float cx, float cy, float k)
{
    const D3DCOLOR white = D3DCOLOR_ARGB(240, 245, 245, 245), dim = D3DCOLOR_ARGB(200, 170, 170, 170);
    const D3DCOLOR accent = D3DCOLOR_ARGB(255, 255, 140, 30), panel = D3DCOLOR_ARGB(200, 10, 12, 16);
    char a[16], b[16], c[24];
    sprintf(a, "P%d/%d", d.pos, d.racers);
    sprintf(b, d.laps > 1 ? "L%d/%d" : "", d.lap, d.laps);
    int mm = (int)(d.lapTime / 60.0f); float ss = d.lapTime - mm * 60.0f;
    sprintf(c, "%d:%05.2f", mm, ss);
    float th = 40 * k, pad = 14 * k, gap = 30 * k;
    float wa = TextWidth(th, a), wb = b[0] ? TextWidth(th, b) : 0, wc = TextWidth(th * 0.8f, c);
    float pw = wa + wb + wc + gap * (b[0] ? 2 : 1) + pad * 2, ph = th + pad * 2;
    float px = cx - pw * 0.5f, py = cy - ph * 0.5f;
    Quad(px, py, pw, ph, panel);
    Quad(px, py + ph - 4 * k, pw, 4 * k, accent);
    float x = px + pad;
    x += Text(x, py + pad, th, a, d.pos == 1 ? accent : white) + gap;
    if (b[0]) x += Text(x, py + pad, th, b, white) + gap;
    Text(x, py + pad + th * 0.2f, th * 0.8f, c, dim);
}

static void DrawGauges(IDirect3DDevice9* dev, const HudData& d, float W, float H)
{
    if (!g_stylesLoaded) return;
    const ViewInfo& v = g_view;
    if (g_frame - v.frame > 2 || v.fov <= 1) return;  // no fresh cockpit view this frame
    // The renderer's per-view data (eView[1] + 0x00 -> +0x00 view matrix, +0x40 projection matrix),
    // filled for exactly the frame being presented, so the gauges line up with the car geometry.
    static float vp[16];
    __try
    {
        const float* plat = *(const float**)0x832E50;
        if (!plat) return;
        const float* V = plat;
        const float* P = plat + 16;
        for (int i = 0; i < 4; i++)
            for (int j = 0; j < 4; j++)
                vp[i * 4 + j] = V[i * 4 + 0] * P[0 * 4 + j] + V[i * 4 + 1] * P[1 * 4 + j] + V[i * 4 + 2] * P[2 * 4 + j] + V[i * 4 + 3] * P[3 * 4 + j];
        static bool logged;
        if (!logged) { logged = true; Log("projection %.3f %.3f %.3f %.3f / %.3f %.3f", P[0], P[5], P[10], P[11], P[14], P[15]); }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return; }
    g_xf.W = W; g_xf.H = H; g_xf.m = vp;

    float s = g.GaugeSize / 100.0f;
    // Left to right: rev counter, speedometer, map; the race bar sits above the middle.
    bool map = g.MapOnDash && g_mapTexB && g_mapFrame >= 0 && (g_frame - g_mapFrame) <= 2;
    for (int k = 0; k < 4; k++)
    {
        if (k == 2 && !map) continue;
        if (k == 3 && d.racers <= 0) continue;
        float side, down = g.GaugeDown;
        if (map) side = g.GaugeRight + (k == 3 ? 0.0f : (k - 1) * g.GaugeSpacing);
        else side = g.GaugeRight + (k == 3 ? 0.0f : (k == 0 ? -0.5f : 0.5f) * g.GaugeSpacing);
        if (k == 3) down -= g.GaugeSize * 1.55f;
        float c[3], n[3], u[3], dv[3];
        for (int i = 0; i < 3; i++)
            c[i] = v.seat[i] + v.fwd[i] * g.GaugeDistance - v.up[i] * down + v.right[i] * side;
        float len = 0;
        for (int i = 0; i < 3; i++) { n[i] = v.seat[i] - c[i]; len += n[i] * n[i]; }
        len = sqrtf(len);
        for (int i = 0; i < 3; i++) n[i] /= len;
        float rn = v.right[0] * n[0] + v.right[1] * n[1] + v.right[2] * n[2];
        len = 0;
        for (int i = 0; i < 3; i++) { u[i] = v.right[i] - n[i] * rn; len += u[i] * u[i]; }
        len = sqrtf(len);
        for (int i = 0; i < 3; i++) u[i] /= len;
        dv[0] = u[1] * n[2] - u[2] * n[1]; dv[1] = u[2] * n[0] - u[0] * n[2]; dv[2] = u[0] * n[1] - u[1] * n[0];
        for (int i = 0; i < 3; i++) { g_xf.c[i] = c[i]; g_xf.u[i] = u[i] * s; g_xf.v[i] = dv[i] * s; }
        g_xf.on = true;
        if (k == 0)
        {
            char gr[4] = { d.gear == 0 ? 'R' : d.gear == 1 ? 'N' : (char)('0' + d.gear - 1), 0 };
            Gauge(dev, g_style[0], d.rpm, g.GaugeMaxRpm, 1000, 1, 1000, g.GaugeRedline, gr);
        }
        else if (k == 1)
        {
            char sp[8]; sprintf(sp, "%d", d.speed);
            Gauge(dev, g_style[1], (float)d.speed, g.GaugeMaxSpeed, 20, 2, 1, 1e9f, sp);
        }
        else if (k == 2)
        {
            // The game's own minimap, lifted out of the screen image, as a round instrument.
            Disc(104, D3DCOLOR_ARGB(255, 70, 72, 78));
            Disc(99, D3DCOLOR_ARGB(255, 8, 9, 11));
            const int N = 48;
            for (int i = 0; i < N; i++)
            {
                float a0 = 6.2831853f * i / N, a1 = 6.2831853f * (i + 1) / N;
                TexTri(0, 0, g_mapU, g_mapV,
                       98 * cosf(a0), 98 * sinf(a0), g_mapU + g_mapRU * cosf(a0), g_mapV + g_mapRV * sinf(a0),
                       98 * cosf(a1), 98 * sinf(a1), g_mapU + g_mapRU * cosf(a1), g_mapV + g_mapRV * sinf(a1), g_mapTexB);
            }
        }
        else
            DrawRacePanel(d, 0, 0, 0.75f);
        g_xf.on = false;
    }
}

static void DrawHud(IDirect3DDevice9* dev)
{
    if (!g.Hud || !InRace()) return;
    HudData d;
    if (!ReadHud(d) || !d.valid) return;

    IDirect3DSurface9* bb = nullptr;
    if (FAILED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) || !bb) return;
    D3DSURFACE_DESC sd; bb->GetDesc(&sd);
    float W = (float)sd.Width, H = (float)sd.Height, k = H / 1080.0f * g.HudScale;

    const D3DCOLOR white = D3DCOLOR_ARGB(240, 245, 245, 245), dim = D3DCOLOR_ARGB(200, 170, 170, 170);
    const D3DCOLOR accent = D3DCOLOR_ARGB(255, 255, 140, 30), panel = D3DCOLOR_ARGB(150, 10, 12, 16);
    g_hudN = 0;
    g_hudCmdN = 0;

    bool gauges = g.Gauges && g.Cockpit && g_trackingOn && g.Enabled && (g_frame - g_view.frame) <= 2;
    if (gauges) DrawGauges(dev, d, W, H);

    // Speed + gear panel (screen-space fallback when the in-car gauges are off).
    if (!gauges)
    {
        char sp[8]; sprintf(sp, "%d", d.speed);
        char gr[4] = { d.gear == 0 ? 'R' : d.gear == 1 ? 'N' : (char)('0' + d.gear - 1), 0 };
        float bh = 70 * k, gh = 46 * k, pad = 16 * k;
        float sw = TextWidth(bh, sp), gw = TextWidth(gh, gr);
        float pw = sw + gw + pad * 3.5f, ph = bh + pad * 2;
        float px = W * g.HudSpeedX - pw * 0.5f, py = H * g.HudSpeedY - ph * 0.5f;
        Quad(px, py, pw, ph, panel);
        Quad(px, py + ph - 4 * k, pw, 4 * k, accent);
        Text(px + pad, py + pad, bh, sp, white);
        Text(px + pad * 2.5f + sw, py + pad + (bh - gh), gh, gr, accent);
    }

    // Race panel: on the dashboard with the gauges, otherwise at the top of the screen.
    if (d.racers > 0 && !gauges)
        DrawRacePanel(d, W * g.HudInfoX, H * g.HudInfoY, k);

    if (g_hudN)
    {
        IDirect3DStateBlock9* sb = nullptr;
        IDirect3DSurface9* oldRT = nullptr;
        if (SUCCEEDED(dev->CreateStateBlock(D3DSBT_ALL, &sb)) && sb)
        {
            sb->Capture();
            dev->GetRenderTarget(0, &oldRT);
            dev->SetRenderTarget(0, bb);
            D3DVIEWPORT9 vp = { 0, 0, sd.Width, sd.Height, 0.0f, 1.0f };
            dev->SetViewport(&vp);
            dev->BeginScene();
            dev->SetVertexShader(nullptr);
            dev->SetPixelShader(nullptr);
            dev->SetTexture(0, nullptr);
            dev->SetFVF(D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1);
            dev->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
            dev->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
            dev->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_LINEAR);
            dev->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
            dev->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
            dev->SetSamplerState(0, D3DSAMP_SRGBTEXTURE, FALSE);
            dev->SetTextureStageState(0, D3DTSS_TEXCOORDINDEX, 0);
            dev->SetTextureStageState(0, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
            dev->SetRenderState(D3DRS_ZENABLE, FALSE);
            dev->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
            dev->SetRenderState(D3DRS_STENCILENABLE, FALSE);
            dev->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
            dev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
            dev->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
            dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
            dev->SetRenderState(D3DRS_BLENDOP, D3DBLENDOP_ADD);
            dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
            dev->SetRenderState(D3DRS_FILLMODE, D3DFILL_SOLID);
            dev->SetRenderState(D3DRS_LIGHTING, FALSE);
            dev->SetRenderState(D3DRS_FOGENABLE, FALSE);
            dev->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
            dev->SetRenderState(D3DRS_COLORWRITEENABLE, 0xF);
            dev->SetRenderState(D3DRS_SRGBWRITEENABLE, FALSE);
            dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
            dev->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
            dev->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
            dev->SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
            dev->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
            dev->SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
            if (g_hudCmdN == 0 || g_hudCmd[0].first != 0) { memmove(g_hudCmd + 1, g_hudCmd, sizeof(HudCmd) * std::min(g_hudCmdN, 63)); g_hudCmd[0] = { nullptr, 0 }; g_hudCmdN = std::min(g_hudCmdN + 1, 64); }
            for (int i = 0; i < g_hudCmdN; i++)
            {
                int first = g_hudCmd[i].first, last = i + 1 < g_hudCmdN ? g_hudCmd[i + 1].first : g_hudN;
                if (last <= first) continue;
                IDirect3DTexture9* t = g_hudCmd[i].tex;
                dev->SetTexture(0, t);
                // Plain triangles take the vertex colour only; textured ones modulate texture and colour.
                dev->SetTextureStageState(0, D3DTSS_COLOROP, t ? D3DTOP_MODULATE : D3DTOP_SELECTARG2);
                dev->SetTextureStageState(0, D3DTSS_ALPHAOP, t ? D3DTOP_MODULATE : D3DTOP_SELECTARG2);
                dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, (last - first) / 3, g_hudV + first, sizeof(HudVtx));
            }
            dev->SetTexture(0, nullptr);
            dev->EndScene();
            if (oldRT) { dev->SetRenderTarget(0, oldRT); oldRT->Release(); }
            sb->Apply();
            sb->Release();
        }
    }
    bb->Release();
}

// ---------------------------------------------------------------- depth-based stereo (one frame -> both eyes)
// [STEREO] Mode=1: the game renders every frame once from the middle of the head. The game's depth
// buffer is swapped for a readable INTZ texture, and at Present a shader builds the left and right
// eye views from colour + depth (side by side, each half the width), so both eyes get every frame.
struct DepthStereo
{
    IDirect3DDevice9* dev = nullptr;
    IDirect3DSurface9* origDS = nullptr;   // the game's own depth buffer
    IDirect3DTexture9* intz = nullptr;     // readable replacement
    IDirect3DSurface9* intzSurf = nullptr;
    IDirect3DTexture9* color = nullptr;    // copy of the finished frame
    UINT cw = 0, ch = 0;
    IDirect3DVertexShader9* vs = nullptr;
    IDirect3DPixelShader9* ps = nullptr;
    bool failed = false;
    bool resz = false;                     // multisampled depth: resolved with AMD's RESZ into intz
    LONG resolvedFrame = -100;
} g_ds;

typedef HRESULT(STDMETHODCALLTYPE* SetDSFn)(IDirect3DDevice9*, IDirect3DSurface9*);
typedef HRESULT(STDMETHODCALLTYPE* GetDSFn)(IDirect3DDevice9*, IDirect3DSurface9**);
static SetDSFn oSetDS;
static GetDSFn oGetDS;

static bool DepthModeOn() { return g.StereoMode == 1 && g_vrReady && g.Overlay; }

static HRESULT STDMETHODCALLTYPE HkSetDS(IDirect3DDevice9* dev, IDirect3DSurface9* s)
{
    if (s && s == g_ds.origDS && g_ds.intzSurf && DepthModeOn()) s = g_ds.intzSurf;
    return oSetDS(dev, s);
}

static HRESULT STDMETHODCALLTYPE HkGetDS(IDirect3DDevice9* dev, IDirect3DSurface9** out)
{
    HRESULT hr = oGetDS(dev, out);
    if (SUCCEEDED(hr) && out && *out && *out == g_ds.intzSurf && g_ds.origDS)
    {
        (*out)->Release();
        *out = g_ds.origDS;
        g_ds.origDS->AddRef();
    }
    return hr;
}

static void ReleaseDepthStereo()
{
    auto rel = [](auto*& p) { if (p) { p->Release(); p = nullptr; } };
    if (g_ds.dev && g_ds.origDS && oSetDS)
    {
        IDirect3DSurface9* cur = nullptr;
        if (SUCCEEDED(oGetDS(g_ds.dev, &cur)) && cur)
        {
            if (cur == g_ds.intzSurf) oSetDS(g_ds.dev, g_ds.origDS);
            cur->Release();
        }
    }
    rel(g_ds.intzSurf); rel(g_ds.intz); rel(g_ds.origDS); rel(g_ds.color);
    g_ds.cw = g_ds.ch = 0;
}

static const char* kDibrHlsl =
    "float4 P : register(c0);      // x = P0 (projection x scale), y = P22, z = P32, w = eye shift (m)\n"
    "float4 Q : register(c1);      // x = steps, y = max disparity (uv), z = 1/width, w = near clamp (m)\n"
    "sampler2D colorTex : register(s0);\n"
    "sampler2D depthTex : register(s1);\n"
    "struct VI { float4 p : POSITION; float2 uv : TEXCOORD0; };\n"
    "struct VO { float4 p : POSITION; float2 uv : TEXCOORD0; };\n"
    "VO vs(VI i) { VO o; o.p = i.p; o.uv = i.uv; return o; }\n"
    "float disparity(float2 uv, float side) {\n"
    "  float d = tex2Dlod(depthTex, float4(uv, 0, 0)).r;\n"
    "  float z = max(P.z / max(d - P.y, 1e-6), Q.w);\n"
    "  return side * 0.5 * P.x * P.w / z;   // horizontal shift of this pixel in that eye (uv units)\n"
    "}\n"
    "float4 ps(VO i) : COLOR {\n"
    "  float side = i.uv.x < 0.5 ? 1.0 : -1.0;   // left eye: near things move right\n"
    "  float u = frac(i.uv.x * 2.0);\n"
    "  float2 best = float2(u, i.uv.y);\n"
    "  float steps = Q.x;\n"
    "  // March from the largest possible shift down; the first surface that reaches this pixel is\n"
    "  // the nearest one, so it correctly hides what is behind it.\n"
    "  [loop] for (float s = steps; s >= 0; s -= 1.0) {\n"
    "    float cand = Q.y * s / steps;\n"
    "    float2 src = float2(u - side * cand, i.uv.y);\n"
    "    float dsp = disparity(src, side);\n"
    "    if (side * dsp >= cand - Q.y / steps) { best = float2(u - dsp, i.uv.y); break; }\n"
    "  }\n"
    "  return float4(tex2Dlod(colorTex, float4(best, 0, 0)).rgb, 1);\n"
    "}\n";

static bool InitDepthStereo(IDirect3DDevice9* dev)
{
    if (g_ds.failed) return false;
    if (g_ds.dev != dev) { ReleaseDepthStereo(); if (g_ds.vs) { g_ds.vs->Release(); g_ds.vs = nullptr; } if (g_ds.ps) { g_ds.ps->Release(); g_ds.ps = nullptr; } g_ds.dev = dev; }
    if (!g_ds.vs || !g_ds.ps)
    {
        ID3DBlob *vb = nullptr, *pb = nullptr, *err = nullptr;
        if (FAILED(D3DCompile(kDibrHlsl, strlen(kDibrHlsl), "dibr", nullptr, nullptr, "vs", "vs_3_0", 0, 0, &vb, &err)) ||
            FAILED(D3DCompile(kDibrHlsl, strlen(kDibrHlsl), "dibr", nullptr, nullptr, "ps", "ps_3_0", 0, 0, &pb, &err)))
        {
            Log("depth stereo: shader compile failed: %s", err ? (const char*)err->GetBufferPointer() : "?");
            if (err) err->Release();
            if (vb) vb->Release();
            g_ds.failed = true;
            return false;
        }
        dev->CreateVertexShader((const DWORD*)vb->GetBufferPointer(), &g_ds.vs);
        dev->CreatePixelShader((const DWORD*)pb->GetBufferPointer(), &g_ds.ps);
        vb->Release(); pb->Release();
    }
    if (!g_ds.intz)
    {
        IDirect3DSurface9* ds = nullptr;
        if (FAILED(oGetDS(dev, &ds)) || !ds) return false;
        D3DSURFACE_DESC dd; ds->GetDesc(&dd);
        if (dd.MultiSampleType != D3DMULTISAMPLE_NONE)
        {
            // Anti-aliased depth cannot be read directly; AMD's RESZ resolves it into a plain INTZ
            // texture once per frame, right before the HUD pass (see ResolveDepth).
            IDirect3D9* d3d = nullptr;
            bool ok = SUCCEEDED(dev->GetDirect3D(&d3d)) && d3d &&
                      SUCCEEDED(d3d->CheckDeviceFormat(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, D3DFMT_X8R8G8B8, D3DUSAGE_RENDERTARGET, D3DRTYPE_SURFACE, (D3DFORMAT)MAKEFOURCC('R', 'E', 'S', 'Z')));
            if (d3d) d3d->Release();
            ds->Release();
            if (!ok || FAILED(dev->CreateTexture(dd.Width, dd.Height, 1, D3DUSAGE_DEPTHSTENCIL, (D3DFORMAT)MAKEFOURCC('I', 'N', 'T', 'Z'), D3DPOOL_DEFAULT, &g_ds.intz, nullptr)))
            {
                Log("depth stereo: depth buffer is multisampled and RESZ is not available");
                g_ds.failed = true; return false;
            }
            g_ds.resz = true;
            Log("depth stereo: multisampled depth %ux%u, resolving with RESZ", dd.Width, dd.Height);
            return true;
        }
        if (FAILED(dev->CreateTexture(dd.Width, dd.Height, 1, D3DUSAGE_DEPTHSTENCIL, (D3DFORMAT)MAKEFOURCC('I', 'N', 'T', 'Z'), D3DPOOL_DEFAULT, &g_ds.intz, nullptr)))
        {
            Log("depth stereo: could not create INTZ depth texture");
            ds->Release(); g_ds.failed = true; return false;
        }
        g_ds.intz->GetSurfaceLevel(0, &g_ds.intzSurf);
        g_ds.origDS = ds;  // keeps our reference
        oSetDS(dev, g_ds.intzSurf);
        Log("depth stereo: game depth %ux%u replaced by a readable INTZ texture", dd.Width, dd.Height);
    }
    return true;
}

// Copies the multisampled scene depth into g_ds.intz (AMD RESZ). Called before the HUD pass,
// while the bound depth buffer still holds the 3D scene.
static void ResolveDepth(IDirect3DDevice9* dev)
{
    if (!DepthModeOn() || !g_ds.resz || !g_ds.intz || g_ds.dev != dev) return;
    IDirect3DStateBlock9* sb = nullptr;
    if (FAILED(dev->CreateStateBlock(D3DSBT_ALL, &sb)) || !sb) return;
    sb->Capture();
    dev->SetVertexShader(nullptr);
    dev->SetPixelShader(nullptr);
    dev->SetFVF(D3DFVF_XYZ);
    dev->SetRenderState(D3DRS_ZENABLE, FALSE);
    dev->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
    dev->SetRenderState(D3DRS_COLORWRITEENABLE, 0);
    dev->SetTexture(0, g_ds.intz);
    float pt[3] = { 0, 0, 0 };
    dev->DrawPrimitiveUP(D3DPT_POINTLIST, 1, pt, sizeof(pt));
    dev->SetRenderState(D3DRS_POINTSIZE, 0x7FA05000);  // RESZ: resolve the bound depth into sampler 0's texture
    dev->SetTexture(0, nullptr);
    sb->Apply();
    sb->Release();
    g_ds.resolvedFrame = g_frame;
}

// Builds the side-by-side stereo picture in the back buffer from colour + depth.
static bool g_frameIsPair;  // this frame's picture is a left/right pair (set at Present)
static void DrawDepthStereo(IDirect3DDevice9* dev)
{
    g_frameIsPair = false;
    if (!DepthModeOn() || !InitDepthStereo(dev)) return;
    if (g_ds.resz && g_ds.resolvedFrame != g_frame) return;  // no 3D scene this frame (menus)
    IDirect3DSurface9* bb = nullptr;
    if (FAILED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) || !bb) return;
    D3DSURFACE_DESC sd; bb->GetDesc(&sd);
    if (!g_ds.color || g_ds.cw != sd.Width || g_ds.ch != sd.Height)
    {
        if (g_ds.color) { g_ds.color->Release(); g_ds.color = nullptr; }
        dev->CreateTexture(sd.Width, sd.Height, 1, D3DUSAGE_RENDERTARGET, sd.Format, D3DPOOL_DEFAULT, &g_ds.color, nullptr);
        g_ds.cw = sd.Width; g_ds.ch = sd.Height;
    }
    IDirect3DSurface9* cs = nullptr;
    if (!g_ds.color || FAILED(g_ds.color->GetSurfaceLevel(0, &cs)) || FAILED(dev->StretchRect(bb, nullptr, cs, nullptr, D3DTEXF_NONE)))
    {
        if (cs) cs->Release();
        bb->Release();
        return;
    }
    cs->Release();

    float P0 = 0.66f, P22 = 1.0f, P32 = -0.5f;
    __try
    {
        const float* plat = *(const float**)0x832E50;
        if (plat) { P0 = plat[16]; P22 = plat[16 + 10]; P32 = plat[16 + 14]; }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
    float shift = 0.5f * (g.IPD > 0 ? g.IPD : g_ipd) * g.PositionScaleStereo;
    float nearZ = 0.30f;  // nearest surfaces in the cabin are a bit further than this
    float maxDisp = 0.5f * P0 * shift / nearZ;
    float c0[4] = { P0, P22, P32, shift };
    float c1[4] = { (float)g.DepthSteps, maxDisp, 1.0f / sd.Width, nearZ };

    IDirect3DStateBlock9* sb = nullptr;
    IDirect3DSurface9* oldRT = nullptr, *oldDS = nullptr;
    if (SUCCEEDED(dev->CreateStateBlock(D3DSBT_ALL, &sb)) && sb)
    {
        sb->Capture();
        dev->GetRenderTarget(0, &oldRT);
        oGetDS(dev, &oldDS);
        dev->SetRenderTarget(0, bb);
        oSetDS(dev, nullptr);  // the depth texture is read, not written
        D3DVIEWPORT9 vp = { 0, 0, sd.Width, sd.Height, 0.0f, 1.0f };
        dev->SetViewport(&vp);
        dev->BeginScene();
        dev->SetVertexShader(g_ds.vs);
        dev->SetPixelShader(g_ds.ps);
        dev->SetVertexShaderConstantF(0, c0, 1);
        dev->SetPixelShaderConstantF(0, c0, 1);
        dev->SetPixelShaderConstantF(1, c1, 1);
        dev->SetTexture(0, g_ds.color);
        dev->SetTexture(1, g_ds.intz);
        for (int s = 0; s < 2; s++)
        {
            dev->SetSamplerState(s, D3DSAMP_MINFILTER, s ? D3DTEXF_POINT : D3DTEXF_LINEAR);
            dev->SetSamplerState(s, D3DSAMP_MAGFILTER, s ? D3DTEXF_POINT : D3DTEXF_LINEAR);
            dev->SetSamplerState(s, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
            dev->SetSamplerState(s, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
            dev->SetSamplerState(s, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
            dev->SetSamplerState(s, D3DSAMP_SRGBTEXTURE, FALSE);
        }
        dev->SetRenderState(D3DRS_ZENABLE, FALSE);
        dev->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
        dev->SetRenderState(D3DRS_STENCILENABLE, FALSE);
        dev->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
        dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
        dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
        dev->SetRenderState(D3DRS_FILLMODE, D3DFILL_SOLID);
        dev->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
        dev->SetRenderState(D3DRS_COLORWRITEENABLE, 0xF);
        dev->SetRenderState(D3DRS_SRGBWRITEENABLE, FALSE);
        dev->SetRenderState(D3DRS_FOGENABLE, FALSE);
        struct V { float x, y, z, w, u, v; };
        // Clip-space full-screen quad; D3D9 half-pixel offset applied in clip units.
        float ox = -1.0f / sd.Width, oy = 1.0f / sd.Height;
        V q[4] = { { -1 + ox, 1 + oy, 0.5f, 1, 0, 0 }, { 1 + ox, 1 + oy, 0.5f, 1, 1, 0 },
                   { -1 + ox, -1 + oy, 0.5f, 1, 0, 1 }, { 1 + ox, -1 + oy, 0.5f, 1, 1, 1 } };
        dev->SetFVF(D3DFVF_XYZW | D3DFVF_TEX1);
        dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, q, sizeof(V));
        dev->EndScene();
        dev->SetTexture(0, nullptr);
        dev->SetTexture(1, nullptr);
        if (oldRT) { dev->SetRenderTarget(0, oldRT); oldRT->Release(); }
        oSetDS(dev, oldDS);
        if (oldDS) oldDS->Release();
        sb->Apply();
        sb->Release();
        g_frameIsPair = true;
        static bool logged;
        if (!logged) { logged = true; Log("depth stereo active: max shift %.3f of the width, %d steps", maxDisp, g.DepthSteps); }
    }
    bb->Release();
}

// ---------------------------------------------------------------- GPU hand-over (needs DXVK)
// With DXVK the finished frames go to SteamVR without capturing the desktop, at the game's own
// resolution. The VR thread owns a ring of D3D11 textures with a keyed mutex. The game thread imports
// them into DXVK's Vulkan device and, at Present, copies the frame into a free one on DXVK's own
// queue (mutex key 0 -> 1). The VR thread takes the newest one (key 1 -> 0).
MIDL_INTERFACE("d56344f5-8d35-46fd-806d-94c351b472c1")
ID3D9VkInteropTexture : public IUnknown
{
    virtual HRESULT STDMETHODCALLTYPE GetVulkanImageInfo(VkImage* pHandle, VkImageLayout* pLayout, VkImageCreateInfo* pInfo) = 0;
};
MIDL_INTERFACE("2eaa4b89-0107-4bdb-87f7-0f541c493ce0")
ID3D9VkInteropDevice : public IUnknown
{
    virtual void STDMETHODCALLTYPE GetVulkanHandles(VkInstance* pInstance, VkPhysicalDevice* pPhysDev, VkDevice* pDevice) = 0;
    virtual void STDMETHODCALLTYPE GetSubmissionQueue(VkQueue* pQueue, uint32_t* pQueueIndex, uint32_t* pQueueFamilyIndex) = 0;
    virtual void STDMETHODCALLTYPE TransitionTextureLayout(ID3D9VkInteropTexture* pTexture, const VkImageSubresourceRange* pSubresources, VkImageLayout OldLayout, VkImageLayout NewLayout) = 0;
    virtual void STDMETHODCALLTYPE FlushRenderingCommands() = 0;
    virtual void STDMETHODCALLTYPE LockSubmissionQueue() = 0;
    virtual void STDMETHODCALLTYPE ReleaseSubmissionQueue() = 0;
    virtual void STDMETHODCALLTYPE LockDevice() = 0;
    virtual void STDMETHODCALLTYPE UnlockDevice() = 0;
    virtual bool STDMETHODCALLTYPE WaitForResource(IDirect3DResource9* pResource, DWORD MapFlags) = 0;
};

static const int kHoSlots = 4;
static const VkImageUsageFlags kHoUsage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                          VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
// Shared between the game thread and the VR thread.
static HANDLE g_hoHandle[kHoSlots];         // VR thread: shared handles of the current ring
static volatile LONG g_hoSlotState[kHoSlots];  // 0 = free, 1 = written by the game, not yet taken
static LONG g_hoFrame[kHoSlots];
static bool g_hoPair[kHoSlots];
static volatile LONG g_hoWantW, g_hoWantH;  // game -> VR thread: size of the game's frames
static volatile LONG g_hoW, g_hoH;          // size of the current ring
static volatile LONG g_hoGen;               // bumped by the VR thread after (re)creating the ring
static volatile LONG g_hoGameGen = -1;      // ring generation the game thread has imported
static volatile LONG g_hoLatest = -1;       // (sequence << 4) | slot of the newest written frame
static volatile LONG g_hoSeq;
static volatile int g_hoState;              // 0 = not tried, 1 = working, -1 = not possible

static bool HandoverWanted() { return g.Transport == 0 && g.VRFrames && g.Overlay && g_vrReady && g_hoState >= 0; }

#define HO_VK_FUNCS(X) X(vkCreateImage) X(vkDestroyImage) X(vkGetImageMemoryRequirements) X(vkAllocateMemory) \
    X(vkFreeMemory) X(vkBindImageMemory) X(vkCreateCommandPool) X(vkAllocateCommandBuffers) X(vkFreeCommandBuffers) \
    X(vkResetCommandBuffer) X(vkBeginCommandBuffer) X(vkEndCommandBuffer) X(vkCmdPipelineBarrier) X(vkCmdCopyImage) \
    X(vkQueueSubmit) X(vkCreateFence) X(vkDestroyFence) X(vkWaitForFences) X(vkResetFences)
#define HO_DECL(n) static PFN_##n p_##n;
HO_VK_FUNCS(HO_DECL)
static PFN_vkGetPhysicalDeviceMemoryProperties p_vkGetPhysicalDeviceMemoryProperties;
static PFN_vkGetPhysicalDeviceImageFormatProperties2 p_vkGetPhysicalDeviceImageFormatProperties2;

struct HoVkSlot { VkImage img; VkDeviceMemory mem; VkCommandBuffer cmd; VkFence fence; bool submitted; };
static struct
{
    bool init, failed;
    IDirect3DDevice9* dev;
    ID3D9VkInteropDevice* interop;
    VkInstance inst; VkPhysicalDevice phys; VkDevice vkd; VkQueue queue; uint32_t family;
    VkCommandPool pool;
    VkPhysicalDeviceMemoryProperties memProps;
    HoVkSlot s[kHoSlots];
    LONG gen = -1; LONG ringW, ringH;
    IDirect3DTexture9* src; ID3D9VkInteropTexture* srcVk; UINT srcW, srcH;
} hv;

static bool HoFail(const char* why, int code = 0)
{
    Log("GPU hand-over not available: %s (%d), using desktop capture", why, code);
    hv.failed = true;
    g_hoState = -1;
    return false;
}

static bool InitHandoverVk(IDirect3DDevice9* dev)
{
    if (hv.failed) return false;
    if (hv.init && hv.dev == dev) return true;
    if (hv.init)
    {
        // The game replaced its device: the old Vulkan objects went with it.
        Log("GPU hand-over: game device changed");
        hv = {};
        g_hoGameGen = -1;
    }
    hv.dev = dev;
    if (FAILED(dev->QueryInterface(__uuidof(ID3D9VkInteropDevice), (void**)&hv.interop)) || !hv.interop)
        return HoFail("the game is not running on DXVK");
    hv.interop->GetVulkanHandles(&hv.inst, &hv.phys, &hv.vkd);
    uint32_t qi = 0;
    hv.interop->GetSubmissionQueue(&hv.queue, &qi, &hv.family);
    HMODULE vkl = GetModuleHandleA("vulkan-1.dll");
    auto gipa = vkl ? (PFN_vkGetInstanceProcAddr)GetProcAddress(vkl, "vkGetInstanceProcAddr") : nullptr;
    if (!gipa || !hv.inst || !hv.vkd || !hv.queue) return HoFail("no Vulkan handles");
    auto gdpa = (PFN_vkGetDeviceProcAddr)gipa(hv.inst, "vkGetDeviceProcAddr");
    p_vkGetPhysicalDeviceMemoryProperties = (PFN_vkGetPhysicalDeviceMemoryProperties)gipa(hv.inst, "vkGetPhysicalDeviceMemoryProperties");
    p_vkGetPhysicalDeviceImageFormatProperties2 = (PFN_vkGetPhysicalDeviceImageFormatProperties2)gipa(hv.inst, "vkGetPhysicalDeviceImageFormatProperties2");
    bool ok = gdpa && p_vkGetPhysicalDeviceMemoryProperties && p_vkGetPhysicalDeviceImageFormatProperties2;
#define HO_LOAD(n) if (ok) { p_##n = (PFN_##n)gdpa(hv.vkd, #n); ok = p_##n != nullptr; }
    HO_VK_FUNCS(HO_LOAD)
#undef HO_LOAD
    if (!ok) return HoFail("Vulkan functions missing");
    p_vkGetPhysicalDeviceMemoryProperties(hv.phys, &hv.memProps);

    VkPhysicalDeviceExternalImageFormatInfo ext = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO, nullptr, VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_KMT_BIT };
    VkPhysicalDeviceImageFormatInfo2 fi = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2, &ext, VK_FORMAT_B8G8R8A8_UNORM,
                                            VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL, kHoUsage, 0 };
    VkExternalImageFormatProperties eprops = { VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES };
    VkImageFormatProperties2 props = { VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2, &eprops };
    VkResult r = p_vkGetPhysicalDeviceImageFormatProperties2(hv.phys, &fi, &props);
    VkExternalMemoryFeatureFlags feat = eprops.externalMemoryProperties.externalMemoryFeatures;
    if (r != VK_SUCCESS || !(feat & VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT)) return HoFail("D3D11 textures cannot be imported", (int)r);

    VkCommandPoolCreateInfo pci = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, nullptr, VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, hv.family };
    if ((r = p_vkCreateCommandPool(hv.vkd, &pci, nullptr, &hv.pool)) != VK_SUCCESS) return HoFail("vkCreateCommandPool", (int)r);
    hv.init = true;
    g_hoState = 1;
    Log("GPU hand-over: DXVK Vulkan device found (queue family %u, import features %x)", hv.family, (unsigned)feat);
    return true;
}

static void WaitHoSlots()
{
    for (auto& s : hv.s)
        if (s.submitted) { p_vkWaitForFences(hv.vkd, 1, &s.fence, VK_TRUE, 1000000000ull); s.submitted = false; }
}

static void ReleaseHoImports()
{
    if (!hv.init) return;
    WaitHoSlots();
    for (auto& s : hv.s)
    {
        if (s.fence) p_vkDestroyFence(hv.vkd, s.fence, nullptr);
        if (s.cmd) p_vkFreeCommandBuffers(hv.vkd, hv.pool, 1, &s.cmd);
        if (s.img) p_vkDestroyImage(hv.vkd, s.img, nullptr);
        if (s.mem) p_vkFreeMemory(hv.vkd, s.mem, nullptr);
        s = {};
    }
    hv.gen = -1;
    InterlockedExchange(&g_hoGameGen, -1);
}

static void ReleaseHoSrc()
{
    if (hv.init) WaitHoSlots();
    if (hv.srcVk) { hv.srcVk->Release(); hv.srcVk = nullptr; }
    if (hv.src) { hv.src->Release(); hv.src = nullptr; }
    hv.srcW = hv.srcH = 0;
}

static uint32_t HoMemType(uint32_t bits)
{
    for (uint32_t i = 0; i < hv.memProps.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (hv.memProps.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) return i;
    for (uint32_t i = 0; i < 32; i++) if (bits & (1u << i)) return i;
    return 0;
}

// Imports the VR thread's current ring into DXVK's device. False while there is no ring of size w x h.
static bool ImportRing(LONG w, LONG h)
{
    LONG gen = g_hoGen;
    if (gen == hv.gen) return hv.ringW == w && hv.ringH == h;
    if (g_hoW != w || g_hoH != h) return false;
    ReleaseHoImports();
    for (int i = 0; i < kHoSlots; i++)
    {
        HoVkSlot& s = hv.s[i];
        VkExternalMemoryImageCreateInfo ext = { VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO, nullptr, VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_KMT_BIT };
        VkImageCreateInfo ci = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, &ext };
        ci.imageType = VK_IMAGE_TYPE_2D; ci.format = VK_FORMAT_B8G8R8A8_UNORM; ci.extent = { (uint32_t)w, (uint32_t)h, 1 };
        ci.mipLevels = 1; ci.arrayLayers = 1; ci.samples = VK_SAMPLE_COUNT_1_BIT; ci.tiling = VK_IMAGE_TILING_OPTIMAL;
        ci.usage = kHoUsage; ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE; ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        VkResult r = p_vkCreateImage(hv.vkd, &ci, nullptr, &s.img);
        if (r != VK_SUCCESS) { ReleaseHoImports(); return HoFail("vkCreateImage", (int)r); }
        VkMemoryRequirements mr; p_vkGetImageMemoryRequirements(hv.vkd, s.img, &mr);
        VkMemoryDedicatedAllocateInfo ded = { VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO, nullptr, s.img, VK_NULL_HANDLE };
        VkImportMemoryWin32HandleInfoKHR imp = { VK_STRUCTURE_TYPE_IMPORT_MEMORY_WIN32_HANDLE_INFO_KHR, &ded, VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_KMT_BIT, g_hoHandle[i], nullptr };
        VkMemoryAllocateInfo ai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &imp, mr.size, HoMemType(mr.memoryTypeBits) };
        if ((r = p_vkAllocateMemory(hv.vkd, &ai, nullptr, &s.mem)) != VK_SUCCESS) { ReleaseHoImports(); return HoFail("importing the shared texture", (int)r); }
        if ((r = p_vkBindImageMemory(hv.vkd, s.img, s.mem, 0)) != VK_SUCCESS) { ReleaseHoImports(); return HoFail("vkBindImageMemory", (int)r); }
        VkCommandBufferAllocateInfo cai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, nullptr, hv.pool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1 };
        VkFenceCreateInfo fci = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
        if (p_vkAllocateCommandBuffers(hv.vkd, &cai, &s.cmd) != VK_SUCCESS || p_vkCreateFence(hv.vkd, &fci, nullptr, &s.fence) != VK_SUCCESS)
        {
            ReleaseHoImports(); return HoFail("command buffer");
        }
    }
    hv.gen = gen; hv.ringW = w; hv.ringH = h;
    InterlockedExchange(&g_hoGameGen, gen);
    Log("GPU hand-over: imported %d shared %ldx%ld textures", kHoSlots, w, h);
    return true;
}

static void HandoverFrame(IDirect3DDevice9* dev, LONG frame, bool pair)
{
    if (!InitHandoverVk(dev)) return;
    IDirect3DSurface9* bb = nullptr;
    if (FAILED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) || !bb) return;
    D3DSURFACE_DESC d; bb->GetDesc(&d);
    InterlockedExchange(&g_hoWantW, (LONG)d.Width);
    InterlockedExchange(&g_hoWantH, (LONG)d.Height);
    if (!ImportRing((LONG)d.Width, (LONG)d.Height)) { bb->Release(); return; }
    if (!hv.src || hv.srcW != d.Width || hv.srcH != d.Height)
    {
        ReleaseHoSrc();
        if (FAILED(dev->CreateTexture(d.Width, d.Height, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &hv.src, nullptr)) || !hv.src ||
            FAILED(hv.src->QueryInterface(__uuidof(ID3D9VkInteropTexture), (void**)&hv.srcVk)) || !hv.srcVk)
        {
            ReleaseHoSrc(); bb->Release(); HoFail("copy texture"); return;
        }
        hv.srcW = d.Width; hv.srcH = d.Height;
    }
    LONG latest = g_hoLatest;
    int k = -1;
    for (int i = 0; i < kHoSlots && k < 0; i++)
        if (g_hoSlotState[i] == 0 && (latest < 0 || (latest & 15) != i)) k = i;
    IDirect3DSurface9* ss = nullptr;
    if (k < 0 || FAILED(hv.src->GetSurfaceLevel(0, &ss)) || !ss) { bb->Release(); return; }
    HRESULT hr = dev->StretchRect(bb, nullptr, ss, nullptr, D3DTEXF_NONE);
    ss->Release();
    bb->Release();
    if (FAILED(hr)) return;

    VkImage srcImg = VK_NULL_HANDLE; VkImageLayout layout = VK_IMAGE_LAYOUT_GENERAL;
    if (FAILED(hv.srcVk->GetVulkanImageInfo(&srcImg, &layout, nullptr)) || !srcImg) return;
    HoVkSlot& s = hv.s[k];
    if (s.submitted) { p_vkWaitForFences(hv.vkd, 1, &s.fence, VK_TRUE, 100000000ull); s.submitted = false; }
    p_vkResetFences(hv.vkd, 1, &s.fence);
    p_vkResetCommandBuffer(s.cmd, 0);
    VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, nullptr, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
    p_vkBeginCommandBuffer(s.cmd, &bi);
    VkImageSubresourceRange range = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    VkImageMemoryBarrier b[2] = {};
    b[0].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b[0].srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT; b[0].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    b[0].oldLayout = b[0].newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    b[0].srcQueueFamilyIndex = b[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b[0].image = srcImg; b[0].subresourceRange = range;
    b[1].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b[1].srcAccessMask = 0; b[1].dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    b[1].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED; b[1].newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b[1].srcQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL; b[1].dstQueueFamilyIndex = hv.family;
    b[1].image = s.img; b[1].subresourceRange = range;
    p_vkCmdPipelineBarrier(s.cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 2, b);
    VkImageCopy rg = {};
    rg.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    rg.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    rg.extent = { d.Width, d.Height, 1 };
    p_vkCmdCopyImage(s.cmd, srcImg, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, s.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &rg);
    VkImageMemoryBarrier rel = b[1];
    rel.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; rel.dstAccessMask = 0;
    rel.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; rel.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    rel.srcQueueFamilyIndex = hv.family; rel.dstQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL;
    p_vkCmdPipelineBarrier(s.cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr, 1, &rel);
    p_vkEndCommandBuffer(s.cmd);

    uint64_t key0 = 0, key1 = 1;
    uint32_t timeout = 500;
    VkWin32KeyedMutexAcquireReleaseInfoKHR km = { VK_STRUCTURE_TYPE_WIN32_KEYED_MUTEX_ACQUIRE_RELEASE_INFO_KHR, nullptr,
                                                  1, &s.mem, &key0, &timeout, 1, &s.mem, &key1 };
    VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO, &km, 0, nullptr, nullptr, 1, &s.cmd, 0, nullptr };
    hv.interop->TransitionTextureLayout(hv.srcVk, &range, layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    hv.interop->FlushRenderingCommands();
    hv.interop->LockSubmissionQueue();
    VkResult r = p_vkQueueSubmit(hv.queue, 1, &si, s.fence);
    hv.interop->ReleaseSubmissionQueue();
    hv.interop->TransitionTextureLayout(hv.srcVk, &range, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, layout);
    if (r != VK_SUCCESS)
    {
        static int fails;
        if (++fails <= 3) Log("GPU hand-over: vkQueueSubmit failed %d", (int)r);
        if (fails >= 100) HoFail("vkQueueSubmit keeps failing", (int)r);
        return;
    }
    s.submitted = true;
    if (g_hoGen != hv.gen) return;  // the ring is being replaced; this frame is dropped
    g_hoFrame[k] = frame;
    g_hoPair[k] = pair;
    InterlockedExchange(&g_hoSlotState[k], 1);
    InterlockedExchange(&g_hoLatest, (InterlockedIncrement(&g_hoSeq) << 4) | k);
    static bool logged;
    if (!logged) { logged = true; Log("GPU hand-over: first frame sent (%ux%u, Vulkan layout %d)", d.Width, d.Height, (int)layout); }
}

// ---------------------------------------------------------------- frame tagging (Present hook)
// Every presented frame gets a small code in its bottom-right corner: block 0 is always white,
// blocks 1..6 carry the low 6 bits of the frame number (bit 0 = eye). The overlay thread reads
// it back from the captured desktop image to know which eye and which head pose a frame has.
static const int kMarkBlocks = 9, kMarkBlock = 8;
static const int kMarkW = kMarkBlocks * kMarkBlock, kMarkH = kMarkBlock;

typedef HRESULT(STDMETHODCALLTYPE* PresentFn)(IDirect3DDevice9*, const RECT*, const RECT*, HWND, const RGNDATA*);
static PresentFn oPresent;

static HRESULT STDMETHODCALLTYPE HkPresent(IDirect3DDevice9* dev, const RECT* a, const RECT* b, HWND w, const RGNDATA* r)
{
    LONG f = g_frame;
    static int logged;
    if (!logged) { logged = 1; Log("first Present through hook"); }
    {
        static DWORD t0 = GetTickCount(); static int n, reports;
        n++;
        DWORD now = GetTickCount();
        if (now - t0 >= 5000 && reports < 2000)
        {
            Log("game fps %.1f (camera updates %ld, render-time view updates %ld)", n * 1000.0f / (now - t0),
                InterlockedExchange(&g_simUpdates, 0), InterlockedExchange(&g_renderUpdates, 0));
            t0 = now; n = 0; reports++;
        }
    }
    {
        // Probe (once in a race): is the depth buffer a readable texture (INTZ, e.g. via ReShade)?
        static bool probed;
        if (!probed && InRace())
        {
            probed = true;
            IDirect3DSurface9* ds = nullptr;
            if (SUCCEEDED(dev->GetDepthStencilSurface(&ds)) && ds)
            {
                D3DSURFACE_DESC dd; ds->GetDesc(&dd);
                IDirect3DTexture9* tex = nullptr;
                HRESULT hr = ds->GetContainer(__uuidof(IDirect3DTexture9), (void**)&tex);
                Log("depth probe: %ux%u format %08X usage %08X, texture container %s", dd.Width, dd.Height, (unsigned)dd.Format, (unsigned)dd.Usage, SUCCEEDED(hr) && tex ? "yes" : "no");
                if (tex) tex->Release();
                ds->Release();
            }
            else Log("depth probe: no depth stencil bound at Present");
            IDirect3D9* d3d = nullptr;
            if (SUCCEEDED(dev->GetDirect3D(&d3d)) && d3d)
            {
                HRESULT hr = d3d->CheckDeviceFormat(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, D3DFMT_X8R8G8B8, D3DUSAGE_DEPTHSTENCIL, D3DRTYPE_TEXTURE, (D3DFORMAT)MAKEFOURCC('I', 'N', 'T', 'Z'));
                Log("depth probe: INTZ textures %s", SUCCEEDED(hr) ? "supported" : "not supported");
                d3d->Release();
            }
        }
    }
    DarkenMapSquare(dev);
    DrawHud(dev);
    DrawDepthStereo(dev);
    bool shared = false;
    if (HandoverWanted())
    {
        HandoverFrame(dev, f, g_frameIsPair);
        shared = g_hoState == 1;
    }
    if (!shared && g_vrReady && g.Overlay && g.Stereo)
    {
        if (logged == 1) { logged = 2; Log("tagging frames"); }
        IDirect3DSurface9* bb = nullptr;
        if (SUCCEEDED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) && bb)
        {
            D3DSURFACE_DESC d;
            bb->GetDesc(&d);
            for (int i = 0; i < kMarkBlocks; i++)
            {
                bool on = i == 0 || (i <= 6 && ((f >> (i - 1)) & 1)) || (i == 7 && g_frameIsPair);
                RECT rc = { (LONG)d.Width - kMarkW + i * kMarkBlock, (LONG)d.Height - kMarkH,
                            (LONG)d.Width - kMarkW + (i + 1) * kMarkBlock, (LONG)d.Height };
                dev->ColorFill(bb, &rc, on ? D3DCOLOR_XRGB(255, 255, 255) : D3DCOLOR_XRGB(0, 0, 0));
            }
            bb->Release();
        }
    }
    HRESULT hr = oPresent(dev, a, b, w, r);
    InterlockedIncrement(&g_frame);
    return hr;
}

// The game (and the mods wrapping it) can replace the device during start-up, so the hook
// follows whatever device the game currently holds.
// Reset (alt-tab, resolution change) fails while D3DPOOL_DEFAULT textures exist, so the minimap
// copies are released first; they are recreated on the next race frame.
typedef HRESULT(STDMETHODCALLTYPE* ResetFn)(IDirect3DDevice9*, D3DPRESENT_PARAMETERS*);
static ResetFn oReset;
static HRESULT STDMETHODCALLTYPE HkReset(IDirect3DDevice9* dev, D3DPRESENT_PARAMETERS* pp)
{
    ReleaseMapTextures();
    ReleaseDepthStereo();
    ReleaseHoSrc();
    return oReset(dev, pp);
}

static void HookPresent()
{
    IDirect3DDevice9* dev = *(IDirect3DDevice9**)0x870974;
    if (!dev) return;
    void** vt = *(void***)dev;
    if (vt[17] == (void*)HkPresent && vt[16] == (void*)HkReset && vt[39] == (void*)HkSetDS && vt[40] == (void*)HkGetDS) return;
    DWORD old;
    VirtualProtect(&vt[16], 2 * sizeof(void*), PAGE_EXECUTE_READWRITE, &old);
    if (vt[17] != (void*)HkPresent) { oPresent = (PresentFn)vt[17]; vt[17] = (void*)HkPresent; }
    if (vt[16] != (void*)HkReset) { oReset = (ResetFn)vt[16]; vt[16] = (void*)HkReset; }
    VirtualProtect(&vt[16], 2 * sizeof(void*), old, &old);
    VirtualProtect(&vt[39], 2 * sizeof(void*), PAGE_EXECUTE_READWRITE, &old);
    if (vt[39] != (void*)HkSetDS) { oSetDS = (SetDSFn)vt[39]; vt[39] = (void*)HkSetDS; }
    if (vt[40] != (void*)HkGetDS) { oGetDS = (GetDSFn)vt[40]; vt[40] = (void*)HkGetDS; }
    VirtualProtect(&vt[39], 2 * sizeof(void*), old, &old);
    g_hooksReady = true;
    Log("Present/Reset hooks installed on device %p", dev);
}

// ---------------------------------------------------------------- overlay (desktop duplication of the game window)
// In alternate-eye stereo there is one SteamVR overlay per eye. Each is a side-by-side overlay whose
// other half is transparent, so each eye only sees its own picture, and each is anchored at the head
// pose that picture was rendered with. SteamVR then keeps both steady while the head moves, even
// though every eye only gets a new picture every second game frame.
struct OverlayState
{
    ID3D11Device* dev = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    IDXGIOutputDuplication* dup = nullptr;
    ID3D11Texture2D* tex[2] = {};
    ID3D11Texture2D* mark = nullptr;  // CPU-readable copy of the frame tag
    UINT texW = 0, texH = 0;
    RECT outRect = {};
    HMONITOR mon = nullptr;
    vr::VROverlayHandle_t handle[2] = { vr::k_ulOverlayHandleInvalid, vr::k_ulOverlayHandleInvalid };
    bool shown[2] = {};
    float aspect = 16.0f / 9.0f;
    int lastPose[2] = { -1, -1 };
    bool stereoTex = false;
} ov;

static void ReleaseDup()
{
    if (ov.dup) { ov.dup->Release(); ov.dup = nullptr; }
    for (auto& t : ov.tex) if (t) { t->Release(); t = nullptr; }
    if (ov.mark) { ov.mark->Release(); ov.mark = nullptr; }
    if (ov.ctx) { ov.ctx->Release(); ov.ctx = nullptr; }
    if (ov.dev) { ov.dev->Release(); ov.dev = nullptr; }
    ov.texW = ov.texH = 0;
}

static bool CreateDup(HMONITOR mon)
{
    ReleaseDup();
    IDXGIFactory1* fac = nullptr;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)&fac))) return false;
    bool ok = false;
    IDXGIAdapter1* ad = nullptr;
    for (UINT a = 0; !ok && fac->EnumAdapters1(a, &ad) != DXGI_ERROR_NOT_FOUND; a++)
    {
        IDXGIOutput* out = nullptr;
        for (UINT o = 0; !ok && ad->EnumOutputs(o, &out) != DXGI_ERROR_NOT_FOUND; o++)
        {
            DXGI_OUTPUT_DESC d; out->GetDesc(&d);
            if (d.Monitor == mon)
            {
                IDXGIOutput1* out1 = nullptr;
                D3D_FEATURE_LEVEL fl;
                if (SUCCEEDED(D3D11CreateDevice(ad, D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &ov.dev, &fl, &ov.ctx)) &&
                    SUCCEEDED(out->QueryInterface(__uuidof(IDXGIOutput1), (void**)&out1)))
                {
                    HRESULT hr = out1->DuplicateOutput(ov.dev, &ov.dup);
                    if (SUCCEEDED(hr)) { ov.outRect = d.DesktopCoordinates; ov.mon = mon; ok = true; }
                    else Log("DuplicateOutput failed 0x%08lx", hr);
                }
                if (out1) out1->Release();
            }
            out->Release();
        }
        ad->Release();
    }
    fac->Release();
    if (!ok) ReleaseDup();
    return ok;
}

static vr::VROverlayHandle_t g_bgHandle = vr::k_ulOverlayHandleInvalid;

static bool StereoActive() { return g.Stereo && g_vrReady; }

static float ScreenWidth()
{
    if (g.AutoWidth && InRace())
    {
        // Use the horizontal scale of the projection the game actually renders with (eView[1] platform data +0x40).
        float px = 0;
        __try
        {
            const float* plat = *(const float**)0x832E50;
            if (plat) px = plat[16];
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { px = 0; }
        if (px > 0.1f && px < 5.0f)
            return 2.0f * g.OverlayDistance / px * g.WidthScale;
    }
    // Menus and loading screens: a comfortable fixed size.
    return g.OverlayWidth;
}

static void SetOverlayPose(int e)
{
    vr::VROverlayHandle_t h = ov.handle[e];
    const FramePose* fp = ov.lastPose[e] >= 0 ? &g_framePose[ov.lastPose[e] & 63] : nullptr;
    if (g.WorldLock && InRace() && fp && fp->valid && (g_frame - fp->frame) < 64 && !g.TestMotion)
    {
        // Place the picture where the head was when this frame was rendered; SteamVR then keeps it
        // fixed in the world while the head keeps moving.
        vr::HmdMatrix34_t a;
        for (int i = 0; i < 3; i++)
        {
            for (int j = 0; j < 3; j++) a.m[i][j] = fp->m.m[i][j];
            a.m[i][3] = fp->m.m[i][3] + fp->m.m[i][1] * g.OverlayUp - fp->m.m[i][2] * g.OverlayDistance;
        }
        g_ovl->SetOverlayTransformAbsolute(h, vr::TrackingUniverseSeated, &a);
    }
    else
    {
        vr::HmdMatrix34_t m = { { { 1, 0, 0, 0 }, { 0, 1, 0, g.OverlayUp }, { 0, 0, 1, -g.OverlayDistance } } };
        g_ovl->SetOverlayTransformTrackedDeviceRelative(h, vr::k_unTrackedDeviceIndex_Hmd, &m);
    }
}

static void UpdateOverlayTransform()
{
    bool stereo = ov.stereoTex;
    for (int e = 0; e < 2; e++)
    {
        if (ov.handle[e] == vr::k_ulOverlayHandleInvalid) continue;
        SetOverlayPose(e);
        g_ovl->SetOverlayWidthInMeters(ov.handle[e], ScreenWidth());
        g_ovl->SetOverlayFlag(ov.handle[e], vr::VROverlayFlags_SideBySide_Parallel, stereo || g.OverlaySBS != 0);
        // Shader side-by-side squeezes each eye to half width, so stretch texels 2:1 there.
        float aspect = g.OverlayTexelAspect > 0 ? g.OverlayTexelAspect : (!stereo && g.OverlaySBS ? 2.0f : 1.0f);
        g_ovl->SetOverlayTexelAspect(ov.handle[e], aspect);
    }

    // Black backdrop behind the game screen so the SteamVR world is not visible around it.
    if (g_bgHandle == vr::k_ulOverlayHandleInvalid)
    {
        if (g_ovl->CreateOverlay("nfsu2.headtrack.bg", "NFSU2 backdrop", &g_bgHandle) == vr::VROverlayError_None)
        {
            static BYTE black[4 * 4 * 4] = {};
            for (int i = 0; i < 16; i++) black[i * 4 + 3] = 255;
            g_ovl->SetOverlayRaw(g_bgHandle, (void*)black, 4, 4, 4);
            g_ovl->SetOverlaySortOrder(g_bgHandle, 0);
        }
        else g_bgHandle = vr::k_ulOverlayHandleInvalid;
    }
    if (g_bgHandle != vr::k_ulOverlayHandleInvalid)
    {
        vr::HmdMatrix34_t b = { { { 1, 0, 0, 0 }, { 0, 1, 0, 0 }, { 0, 0, 1, -(g.OverlayDistance + 1.0f) } } };
        g_ovl->SetOverlayTransformTrackedDeviceRelative(g_bgHandle, vr::k_unTrackedDeviceIndex_Hmd, &b);
        g_ovl->SetOverlayWidthInMeters(g_bgHandle, 40.0f);
        if (g.Backdrop && ov.shown[0]) g_ovl->ShowOverlay(g_bgHandle);
        else g_ovl->HideOverlay(g_bgHandle);
    }
}

// Returns the 6-bit frame id from the tag, or -1 if the frame carries no tag.
static int ReadFrameTag(ID3D11Texture2D* frame, LONG x, LONG y)
{
    if (!ov.mark)
    {
        D3D11_TEXTURE2D_DESC fd; frame->GetDesc(&fd);
        D3D11_TEXTURE2D_DESC td = {};
        td.Width = kMarkW; td.Height = kMarkH; td.MipLevels = 1; td.ArraySize = 1;
        td.Format = fd.Format; td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_STAGING;
        td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (FAILED(ov.dev->CreateTexture2D(&td, nullptr, &ov.mark))) return -1;
    }
    D3D11_BOX box = { (UINT)x, (UINT)y, 0, (UINT)(x + kMarkW), (UINT)(y + kMarkH), 1 };
    ov.ctx->CopySubresourceRegion(ov.mark, 0, 0, 0, 0, frame, 0, &box);
    D3D11_MAPPED_SUBRESOURCE ms;
    if (FAILED(ov.ctx->Map(ov.mark, 0, D3D11_MAP_READ, 0, &ms))) return -1;
    auto px = [&](int i) { return (const BYTE*)ms.pData + (kMarkBlock / 2) * ms.RowPitch + (i * kMarkBlock + kMarkBlock / 2) * 4; };
    auto bright = [&](int i) { return px(i)[1] > 128; };  // green channel survives colour grading best
    static bool alphaLogged;
    if (!alphaLogged) { alphaLogged = true; Log("captured pixel alpha %d", px(0)[3]); }
    int id = -1;
    if (bright(0) && !bright(8))
    {
        id = 0;
        for (int i = 1; i <= 6; i++)
            if (bright(i)) id |= 1 << (i - 1);
        if (bright(7)) id |= 0x100;  // left/right pair
    }
    ov.ctx->Unmap(ov.mark, 0);
    return id;
}

static void ShowEye(int e)
{
    vr::Texture_t t = { ov.tex[e], vr::TextureType_DirectX, vr::ColorSpace_Auto };
    g_ovl->SetOverlayTexture(ov.handle[e], &t);
    if (!ov.shown[e]) { g_ovl->ShowOverlay(ov.handle[e]); ov.shown[e] = true; }
}

static void OverlayFrame()
{
    for (int e = 0; e < 2; e++)
    {
        if (ov.handle[e] != vr::k_ulOverlayHandleInvalid) continue;
        if (g_ovl->CreateOverlay(e ? "nfsu2.headtrack.r" : "nfsu2.headtrack", e ? "NFSU2 right eye" : "NFSU2", &ov.handle[e]) != vr::VROverlayError_None)
        {
            ov.handle[e] = vr::k_ulOverlayHandleInvalid;
            return;
        }
        g_ovl->SetOverlaySortOrder(ov.handle[e], 10 + e);
    }

    bool want = g.Overlay && g_wnd && !IsIconic(g_wnd);
    if (!want)
    {
        for (int e = 0; e < 2; e++)
            if (ov.shown[e]) { g_ovl->HideOverlay(ov.handle[e]); ov.shown[e] = false; }
        UpdateOverlayTransform();
        Sleep(50);
        return;
    }

    HMONITOR mon = MonitorFromWindow(g_wnd, MONITOR_DEFAULTTONEAREST);
    if (!ov.dup || mon != ov.mon)
    {
        if (!CreateDup(mon)) { Sleep(500); return; }
    }

    DXGI_OUTDUPL_FRAME_INFO fi;
    IDXGIResource* res = nullptr;
    HRESULT hr = ov.dup->AcquireNextFrame(50, &fi, &res);
    if (hr == DXGI_ERROR_WAIT_TIMEOUT) { UpdateOverlayTransform(); return; }
    if (FAILED(hr)) { Log("AcquireNextFrame 0x%08lx, recreating", hr); ReleaseDup(); Sleep(100); return; }

    ID3D11Texture2D* frame = nullptr;
    res->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&frame);
    res->Release();

    RECT cr; GetClientRect(g_wnd, &cr);
    POINT tl = { 0, 0 }; ClientToScreen(g_wnd, &tl);
    LONG x0 = std::max<LONG>(tl.x - ov.outRect.left, 0), y0 = std::max<LONG>(tl.y - ov.outRect.top, 0);
    LONG w = std::min<LONG>(cr.right, (ov.outRect.right - ov.outRect.left) - x0);
    LONG h = std::min<LONG>(cr.bottom, (ov.outRect.bottom - ov.outRect.top) - y0);

    bool stereo = StereoActive();
    if (frame && w > kMarkW * 2 && h > 16)
    {
        ov.aspect = (float)w / (float)h;
        UINT tw = stereo ? (UINT)w * 2 : (UINT)w;
        if (!ov.tex[0] || ov.texW != tw || ov.texH != (UINT)h || ov.stereoTex != stereo)
        {
            for (auto& t : ov.tex) if (t) { t->Release(); t = nullptr; }
            D3D11_TEXTURE2D_DESC fd; frame->GetDesc(&fd);
            D3D11_TEXTURE2D_DESC td = {};
            td.Width = tw; td.Height = h; td.MipLevels = 1; td.ArraySize = 1;
            td.Format = fd.Format; td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DEFAULT;
            td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            // Start fully transparent, so the unused half of each eye's overlay shows nothing.
            std::string zeros((size_t)tw * h * 4, '\0');
            D3D11_SUBRESOURCE_DATA init = { zeros.data(), tw * 4, 0 };
            for (int e = 0; e < (stereo ? 2 : 1); e++) ov.dev->CreateTexture2D(&td, &init, &ov.tex[e]);
            ov.texW = tw; ov.texH = h; ov.stereoTex = stereo;
            if (!stereo && ov.shown[1]) { g_ovl->HideOverlay(ov.handle[1]); ov.shown[1] = false; }
            Log("overlay texture %ux%ld (%s)", tw, h, stereo ? "alternate-eye stereo, one overlay per eye" : "single image");
        }
        D3D11_BOX box = { (UINT)x0, (UINT)y0, 0, (UINT)(x0 + w), (UINT)(y0 + h), 1 };
        if (stereo && ov.tex[0] && ov.tex[1])
        {
            int id = ReadFrameTag(frame, x0 + w - kMarkW, y0 + h - kMarkH);
            static int nL, nR, nNone, reports;
            (id < 0 ? nNone : (id & 1) ? nR : nL)++;
            if (nL + nR + nNone >= 600 && reports < 40)
            {
                Log("captured frames: left %d, right %d, untagged %d", nL, nR, nNone);
                nL = nR = nNone = 0; reports++;
            }
            if (id >= 0)
            {
                // Odd frames are the right eye: its picture goes into the right half of its own overlay.
                int e = id & 1;
                ov.ctx->CopySubresourceRegion(ov.tex[e], 0, e ? w : 0, 0, 0, frame, 0, &box);
                ov.ctx->Flush();
                ov.lastPose[e] = id;
                SetOverlayPose(e);
                ShowEye(e);
            }
            else
            {
                // Untagged frame (loading screen, alt-tab): show it to both eyes.
                for (int e = 0; e < 2; e++)
                {
                    ov.ctx->CopySubresourceRegion(ov.tex[e], 0, e ? w : 0, 0, 0, frame, 0, &box);
                    ov.lastPose[e] = -1;
                }
                ov.ctx->Flush();
                for (int e = 0; e < 2; e++) ShowEye(e);
            }
        }
        else if (ov.tex[0])
        {
            ov.ctx->CopySubresourceRegion(ov.tex[0], 0, 0, 0, 0, frame, 0, &box);
            ov.ctx->Flush();
            ov.lastPose[0] = -1;
            ShowEye(0);
        }
    }
    UpdateOverlayTransform();
    if (frame) frame->Release();
    ov.dup->ReleaseFrame();
}

// ---------------------------------------------------------------- VR frames (SteamVR scene app)
// Instead of overlays, the mod acts as a normal VR app: every headset refresh it draws the latest
// captured picture of each eye into that eye's view and submits it to SteamVR together with the
// head pose the picture was rendered with (Submit_TextureWithPose). SteamVR's reprojection then
// keeps both eyes steady in the world between game frames, without the overlay timing jumps.
struct SceneState
{
    bool init = false;
    ID3D11Texture2D* src[2] = {};
    ID3D11ShaderResourceView* srv[2] = {};
    UINT srcW = 0, srcH = 0;
    int srcId[2] = { -1, -1 };      // frame id (tag) of the picture in src[e], -1 = untagged
    bool srcPair[2] = {};           // picture is a side-by-side left/right pair
    bool srcValid[2] = {};
    ID3D11Texture2D* rt[2] = {};
    ID3D11RenderTargetView* rtv[2] = {};
    uint32_t rtW = 0, rtH = 0;
    ID3D11VertexShader* vs = nullptr;
    ID3D11PixelShader* ps = nullptr;
    ID3D11Buffer* cb = nullptr;
    ID3D11SamplerState* smp = nullptr;
    ID3D11RasterizerState* rs = nullptr;
} sc;

static void ReleaseScene()
{
    auto rel = [](auto*& p) { if (p) { p->Release(); p = nullptr; } };
    for (int e = 0; e < 2; e++) { rel(sc.srv[e]); rel(sc.src[e]); rel(sc.rtv[e]); rel(sc.rt[e]); sc.srcValid[e] = false; }
    rel(sc.vs); rel(sc.ps); rel(sc.cb); rel(sc.smp); rel(sc.rs);
    sc.srcW = sc.srcH = 0; sc.init = false;
}

static const char* kSceneHlsl =
    "cbuffer C : register(b0) { float4 corner[4]; float4 uvr; };\n"
    "struct VO { float4 p : SV_Position; float2 uv : TEXCOORD0; };\n"
    "VO vs(uint id : SV_VertexID) { VO o; o.p = corner[id]; o.uv = float2(uvr.x + (id & 1) * uvr.y, id >> 1); return o; }\n"
    "Texture2D t : register(t0); SamplerState s : register(s0);\n"
    "float4 ps(VO i) : SV_Target { return float4(t.Sample(s, i.uv).rgb, 1); }\n";

static bool InitScene()
{
    if (sc.init) return true;
    if (!ov.dev) return false;
    ID3DBlob *vsb = nullptr, *psb = nullptr, *err = nullptr;
    if (FAILED(D3DCompile(kSceneHlsl, strlen(kSceneHlsl), "scene", nullptr, nullptr, "vs", "vs_4_0", 0, 0, &vsb, &err)) ||
        FAILED(D3DCompile(kSceneHlsl, strlen(kSceneHlsl), "scene", nullptr, nullptr, "ps", "ps_4_0", 0, 0, &psb, &err)))
    {
        Log("shader compile failed: %s", err ? (const char*)err->GetBufferPointer() : "?");
        if (err) err->Release();
        if (vsb) vsb->Release();
        return false;
    }
    ov.dev->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, &sc.vs);
    ov.dev->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, &sc.ps);
    vsb->Release(); psb->Release();
    D3D11_BUFFER_DESC bd = { 80, D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0 };
    ov.dev->CreateBuffer(&bd, nullptr, &sc.cb);
    D3D11_SAMPLER_DESC sd = {};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    ov.dev->CreateSamplerState(&sd, &sc.smp);
    D3D11_RASTERIZER_DESC rd = {};
    rd.FillMode = D3D11_FILL_SOLID; rd.CullMode = D3D11_CULL_NONE; rd.DepthClipEnable = FALSE;
    ov.dev->CreateRasterizerState(&rd, &sc.rs);

    g_sys->GetRecommendedRenderTargetSize(&sc.rtW, &sc.rtH);
    for (int e = 0; e < 2; e++)
    {
        D3D11_TEXTURE2D_DESC td = {};
        td.Width = sc.rtW; td.Height = sc.rtH; td.MipLevels = 1; td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM; td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        ov.dev->CreateTexture2D(&td, nullptr, &sc.rt[e]);
        if (sc.rt[e]) ov.dev->CreateRenderTargetView(sc.rt[e], nullptr, &sc.rtv[e]);
    }
    sc.init = sc.vs && sc.ps && sc.cb && sc.smp && sc.rs && sc.rtv[0] && sc.rtv[1];
    if (sc.init)
    {
        vr::IVRCompositor* comp = (vr::IVRCompositor*)pVR_GetGenericInterface(vr::IVRCompositor_Version, nullptr);
        if (comp) comp->SetTrackingSpace(vr::TrackingUniverseSeated);
        Log("VR frames: %ux%u per eye", sc.rtW, sc.rtH);
    }
    return sc.init;
}

// Capture polls the desktop every millisecond between headset refreshes (see SceneFrame), so every
// presented game frame is picked up. Capturing only once per headset refresh could lock onto every
// second game frame, which starved one eye. Everything runs on the VR thread: a separate capture
// thread sharing the D3D11 device with SteamVR's submission crashed inside the graphics driver.
static SRWLOCK g_d3dLock = SRWLOCK_INIT;
static void RegisterOurThread();
static volatile bool g_dupLost, g_captureWanted;

// Copies the newest desktop frame of the game window into its eye's source picture.
static void CaptureFrames(UINT timeoutMs = 0)
{
    {
        DXGI_OUTDUPL_FRAME_INFO fi;
        IDXGIResource* res = nullptr;
        HRESULT hr = ov.dup->AcquireNextFrame(timeoutMs, &fi, &res);
        if (hr == DXGI_ERROR_WAIT_TIMEOUT) return;
        if (FAILED(hr)) { Log("AcquireNextFrame 0x%08lx, recreating", hr); g_dupLost = true; return; }
        ID3D11Texture2D* frame = nullptr;
        res->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&frame);
        res->Release();

        RECT cr; GetClientRect(g_wnd, &cr);
        POINT tl = { 0, 0 }; ClientToScreen(g_wnd, &tl);
        LONG x0 = std::max<LONG>(tl.x - ov.outRect.left, 0), y0 = std::max<LONG>(tl.y - ov.outRect.top, 0);
        LONG w = std::min<LONG>(cr.right, (ov.outRect.right - ov.outRect.left) - x0);
        LONG h = std::min<LONG>(cr.bottom, (ov.outRect.bottom - ov.outRect.top) - y0);
        if (frame && w > kMarkW * 2 && h > 16)
        {
            ov.aspect = (float)w / (float)h;
            if (!sc.src[0] || sc.srcW != (UINT)w || sc.srcH != (UINT)h)
            {
                for (int e = 0; e < 2; e++)
                {
                    if (sc.srv[e]) { sc.srv[e]->Release(); sc.srv[e] = nullptr; }
                    if (sc.src[e]) { sc.src[e]->Release(); sc.src[e] = nullptr; }
                    D3D11_TEXTURE2D_DESC fd; frame->GetDesc(&fd);
                    D3D11_TEXTURE2D_DESC td = {};
                    td.Width = w; td.Height = h; td.MipLevels = 1; td.ArraySize = 1;
                    td.Format = fd.Format; td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DEFAULT;
                    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
                    ov.dev->CreateTexture2D(&td, nullptr, &sc.src[e]);
                    if (sc.src[e]) ov.dev->CreateShaderResourceView(sc.src[e], nullptr, &sc.srv[e]);
                    sc.srcValid[e] = false;
                }
                sc.srcW = w; sc.srcH = h;
                Log("capturing %ldx%ld", w, h);
            }
            D3D11_BOX box = { (UINT)x0, (UINT)y0, 0, (UINT)(x0 + w), (UINT)(y0 + h), 1 };
            int tag = g.Stereo ? ReadFrameTag(frame, x0 + w - kMarkW, y0 + h - kMarkH) : -1;
            bool pair = tag >= 0 && (tag & 0x100);
            int id = tag >= 0 ? (tag & 0xFF) : -1;
            static int nL, nR, nNone, reports;
            (id < 0 ? nNone : (id & 1) ? nR : nL)++;
            if (nL + nR + nNone >= 600 && reports < 40)
            {
                Log("captured frames: left %d, right %d, untagged %d", nL, nR, nNone);
                nL = nR = nNone = 0; reports++;
            }
            for (int e = 0; e < 2; e++)
            {
                if (id >= 0 && g.StereoMode == 0 && (id & 1) != e) continue;  // alternating: a tagged frame belongs to one eye
                if (!sc.src[e]) continue;
                ov.ctx->CopySubresourceRegion(sc.src[e], 0, 0, 0, 0, frame, 0, &box);
                sc.srcId[e] = id;
                sc.srcPair[e] = pair;
                sc.srcValid[e] = true;
            }
        }
        if (frame) frame->Release();
        ov.dup->ReleaseFrame();
    }
}

static DWORD WINAPI CaptureThread(LPVOID)
{
    RegisterOurThread();
    timeBeginPeriod(1);
    for (;;)
    {
        Sleep(1);
        if (!g_captureWanted || g_dupLost) continue;
        AcquireSRWLockExclusive(&g_d3dLock);
        if (sc.init && ov.dup && !g_dupLost) CaptureFrames();
        ReleaseSRWLockExclusive(&g_d3dLock);
    }
}

// VR-thread side of the GPU hand-over: the ring of shared D3D11 textures.
static struct HoRing { ID3D11Texture2D* tex[kHoSlots]; IDXGIKeyedMutex* km[kHoSlots]; LONG w, h, gen; } hr_, hrOld;
static LONG g_hoTaken = -1;  // sequence of the last frame taken
static DWORD g_hoStuckSince[kHoSlots];

static void FreeRing(HoRing& r)
{
    for (int i = 0; i < kHoSlots; i++)
    {
        if (r.km[i]) { r.km[i]->Release(); r.km[i] = nullptr; }
        if (r.tex[i]) { r.tex[i]->Release(); r.tex[i] = nullptr; }
    }
    r.w = r.h = 0;
}

static void ReleaseHandover()
{
    FreeRing(hr_);
    FreeRing(hrOld);
    g_hoW = g_hoH = 0;
    InterlockedIncrement(&g_hoGen);
    InterlockedExchange(&g_hoLatest, -1);
    for (auto& st : g_hoSlotState) InterlockedExchange(&st, 0);
}

static bool CreateDeviceOnly()
{
    ReleaseDup();
    D3D_FEATURE_LEVEL fl;
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &ov.dev, &fl, &ov.ctx)))
        return false;
    ov.mon = nullptr;
    return true;
}

static bool CreateRing(LONG w, LONG h)
{
    if (hrOld.tex[0]) return false;  // the game still uses the ring before the last one
    HoRing n = {};
    for (int i = 0; i < kHoSlots; i++)
    {
        D3D11_TEXTURE2D_DESC td = {};
        td.Width = w; td.Height = h; td.MipLevels = 1; td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM; td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
        td.MiscFlags = D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;
        IDXGIResource* res = nullptr;
        HANDLE hd = nullptr;
        bool ok = SUCCEEDED(ov.dev->CreateTexture2D(&td, nullptr, &n.tex[i])) && n.tex[i] &&
                  SUCCEEDED(n.tex[i]->QueryInterface(__uuidof(IDXGIKeyedMutex), (void**)&n.km[i])) &&
                  SUCCEEDED(n.tex[i]->QueryInterface(__uuidof(IDXGIResource), (void**)&res)) && res &&
                  SUCCEEDED(res->GetSharedHandle(&hd)) && hd;
        if (res) res->Release();
        if (!ok)
        {
            Log("GPU hand-over: could not create shared %ldx%ld textures, using desktop capture", w, h);
            FreeRing(n);
            g_hoState = -1;
            return false;
        }
        g_hoHandle[i] = hd;
    }
    hrOld = hr_;  // freed once the game has imported the new ring
    n.w = w; n.h = h;
    hr_ = n;
    for (auto& st : g_hoSlotState) InterlockedExchange(&st, 0);
    InterlockedExchange(&g_hoLatest, -1);
    g_hoW = w; g_hoH = h;
    hr_.gen = InterlockedIncrement(&g_hoGen);
    Log("GPU hand-over: ring of %d shared %ldx%ld textures", kHoSlots, w, h);
    return true;
}

// Copies the newest finished game frame into the eye pictures. False if there was nothing new.
static bool TakeHandover()
{
    LONG w = g_hoWantW, h = g_hoWantH;
    if (w > 0 && h > 0 && (w != hr_.w || h != hr_.h)) CreateRing(w, h);
    if (hrOld.tex[0] && g_hoGameGen == hr_.gen) FreeRing(hrOld);
    if (!hr_.tex[0]) return false;
    LONG latest = g_hoLatest;
    // Frames the game wrote but that were overtaken by newer ones: hand the slot back.
    for (int i = 0; i < kHoSlots; i++)
    {
        if (g_hoSlotState[i] != 1 || (latest >= 0 && (latest & 15) == i)) { g_hoStuckSince[i] = 0; continue; }
        if (hr_.km[i]->AcquireSync(1, 0) == S_OK)
        {
            hr_.km[i]->ReleaseSync(0);
            InterlockedExchange(&g_hoSlotState[i], 0);
            g_hoStuckSince[i] = 0;
        }
        else if (!g_hoStuckSince[i]) g_hoStuckSince[i] = GetTickCount();
        else if (GetTickCount() - g_hoStuckSince[i] > 2000) { InterlockedExchange(&g_hoSlotState[i], 0); g_hoStuckSince[i] = 0; }
    }
    if (latest < 0 || (latest >> 4) == g_hoTaken) return false;
    int k = latest & 15;
    if (g_hoSlotState[k] != 1) { g_hoTaken = latest >> 4; return false; }
    if (hr_.km[k]->AcquireSync(1, 0) != S_OK) return false;  // copy not finished yet: next poll (never block past the headset refresh)
    if (!sc.src[0] || sc.srcW != (UINT)hr_.w || sc.srcH != (UINT)hr_.h)
    {
        for (int e = 0; e < 2; e++)
        {
            if (sc.srv[e]) { sc.srv[e]->Release(); sc.srv[e] = nullptr; }
            if (sc.src[e]) { sc.src[e]->Release(); sc.src[e] = nullptr; }
            D3D11_TEXTURE2D_DESC sd = {};
            sd.Width = hr_.w; sd.Height = hr_.h; sd.MipLevels = 1; sd.ArraySize = 1;
            sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM; sd.SampleDesc.Count = 1; sd.Usage = D3D11_USAGE_DEFAULT;
            sd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            ov.dev->CreateTexture2D(&sd, nullptr, &sc.src[e]);
            if (sc.src[e]) ov.dev->CreateShaderResourceView(sc.src[e], nullptr, &sc.srv[e]);
            sc.srcValid[e] = false;
        }
        sc.srcW = hr_.w; sc.srcH = hr_.h;
        Log("VR picture from the GPU hand-over: %ldx%ld", hr_.w, hr_.h);
    }
    ov.aspect = (float)hr_.w / (float)hr_.h;
    int id = g_hoFrame[k] & 63;
    bool pair = g_hoPair[k];
    for (int e = 0; e < 2; e++)
    {
        if (g.StereoMode == 0 && (id & 1) != e) continue;  // alternating: a frame belongs to one eye
        if (!sc.src[e]) continue;
        ov.ctx->CopyResource(sc.src[e], hr_.tex[k]);
        sc.srcId[e] = id;
        sc.srcPair[e] = pair;
        sc.srcValid[e] = true;
    }
    hr_.km[k]->ReleaseSync(0);
    InterlockedExchange(&g_hoSlotState[k], 0);
    g_hoTaken = latest >> 4;
    {
        static int n, reports; static DWORD t0 = GetTickCount();
        n++;
        DWORD now = GetTickCount();
        if (now - t0 >= 5000 && reports < 200)
        {
            Log("GPU hand-over: %.1f new game frames/s reached the VR thread", n * 1000.0f / (now - t0));
            n = 0; t0 = now; reports++;
        }
    }
    return true;
}

static void DumpTexture(ID3D11Texture2D* tex, const char* name)
{
    if (!tex) return;
    D3D11_TEXTURE2D_DESC td; tex->GetDesc(&td);
    td.Usage = D3D11_USAGE_STAGING; td.BindFlags = 0; td.CPUAccessFlags = D3D11_CPU_ACCESS_READ; td.MiscFlags = 0;
    ID3D11Texture2D* st = nullptr;
    if (FAILED(ov.dev->CreateTexture2D(&td, nullptr, &st)) || !st) return;
    ov.ctx->CopyResource(st, tex);
    D3D11_MAPPED_SUBRESOURCE m;
    if (SUCCEEDED(ov.ctx->Map(st, 0, D3D11_MAP_READ, 0, &m)))
    {
        char path[MAX_PATH]; strcpy(path, g_logPath);
        if (char* sl = strrchr(path, '\\')) strcpy(sl + 1, name);
        if (FILE* f = fopen(path, "wb"))
        {
            BITMAPFILEHEADER fh = {}; BITMAPINFOHEADER ih = {};
            DWORD rowBytes = td.Width * 4;
            fh.bfType = 0x4D42; fh.bfOffBits = sizeof(fh) + sizeof(ih); fh.bfSize = fh.bfOffBits + rowBytes * td.Height;
            ih.biSize = sizeof(ih); ih.biWidth = td.Width; ih.biHeight = -(LONG)td.Height; ih.biPlanes = 1; ih.biBitCount = 32;
            fwrite(&fh, sizeof(fh), 1, f); fwrite(&ih, sizeof(ih), 1, f);
            for (UINT y = 0; y < td.Height; y++) fwrite((BYTE*)m.pData + y * m.RowPitch, rowBytes, 1, f);
            fclose(f);
        }
        ov.ctx->Unmap(st, 0);
    }
    st->Release();
}

static void SceneFrame()
{
    bool want = g.Overlay && g_wnd && !IsIconic(g_wnd);
    bool shared = g.Transport == 0 && g_hoState == 1;
    if (shared && (ov.dup || !ov.dev))
    {
        g_captureWanted = false;
        AcquireSRWLockExclusive(&g_d3dLock);
        ReleaseScene();
        ReleaseHandover();
        bool ok = CreateDeviceOnly();
        ReleaseSRWLockExclusive(&g_d3dLock);
        if (!ok) { Log("GPU hand-over: no D3D11 device, using desktop capture"); g_hoState = -1; Sleep(500); return; }
        Log("GPU hand-over active, desktop capture off");
    }
    HMONITOR mon = g_wnd ? MonitorFromWindow(g_wnd, MONITOR_DEFAULTTONEAREST) : nullptr;
    if (!shared && want && (!ov.dup || mon != ov.mon || g_dupLost))
    {
        g_captureWanted = false;
        AcquireSRWLockExclusive(&g_d3dLock);
        ReleaseScene();
        ReleaseHandover();
        bool ok = CreateDup(mon);
        g_dupLost = false;
        ReleaseSRWLockExclusive(&g_d3dLock);
        if (!ok) { Sleep(500); return; }
    }
    if ((ov.dup || shared) && ov.dev && !InitScene()) { Sleep(500); return; }
    g_captureWanted = want && !shared;
    if (!sc.init) { Sleep(10); return; }

    vr::IVRCompositor* comp = (vr::IVRCompositor*)pVR_GetGenericInterface(vr::IVRCompositor_Version, nullptr);
    if (!comp) { Sleep(100); return; }
    // Until shortly before the next headset refresh, keep picking up every new game frame. The
    // budget is measured from the last refresh: polling past it made the loop miss refreshes
    // (about 69 frames per second reached SteamVR on a 90 Hz headset).
    static LARGE_INTEGER lastWait, qpf;
    static float frameSec = 0;
    if (!qpf.QuadPart) QueryPerformanceFrequency(&qpf);
    if (frameSec <= 0)
    {
        vr::ETrackedPropertyError pe;
        float hz = g_sys->GetFloatTrackedDeviceProperty(vr::k_unTrackedDeviceIndex_Hmd, vr::Prop_DisplayFrequency_Float, &pe);
        frameSec = (pe == vr::TrackedProp_Success && hz > 30 && hz < 200) ? 1.0f / hz : 1.0f / 90.0f;
        Log("headset refresh %.1f Hz", 1.0f / frameSec);
    }
    if (want && (ov.dup || shared))
    {
        float budget = std::max(0.002f, frameSec - 0.0045f);
        for (;;)
        {
            if (shared)
            {
                if (!TakeHandover()) Sleep(1);
                if (g_hoState != 1) return;
            }
            else
            {
                CaptureFrames(1);
                if (g_dupLost) return;
            }
            LARGE_INTEGER t; QueryPerformanceCounter(&t);
            if (!lastWait.QuadPart || (float)(t.QuadPart - lastWait.QuadPart) / qpf.QuadPart >= budget) break;
        }
    }
    vr::TrackedDevicePose_t poses[1];
    comp->WaitGetPoses(poses, 1, nullptr, 0);  // paces this loop to the headset refresh
    QueryPerformanceCounter(&lastWait);
    {
        static int n; static LARGE_INTEGER t0;
        if (!t0.QuadPart) t0 = lastWait;
        if (++n >= 450)
        {
            Log("submitting %.1f frames/s to SteamVR", n / ((float)(lastWait.QuadPart - t0.QuadPart) / qpf.QuadPart));
            n = 0; t0 = lastWait;
        }
    }
    PublishPose(poses[0]);
    vr::HmdMatrix34_t now = poses[0].mDeviceToAbsoluteTracking;

    // Size of the game picture in tangent space (x right, y up, at distance 1).
    float hx, hy;
    float px = 0, py = 0;
    if (InRace())
    {
        __try
        {
            const float* plat = *(const float**)0x832E50;
            if (plat) { px = plat[16]; py = fabsf(plat[21]); }
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { px = py = 0; }
    }
    if (px > 0.1f && py > 0.1f && px < 5 && py < 5)
    {
        hx = g.WidthScale / px; hy = g.WidthScale / py;
    }
    else
    {
        // Menus and loading screens: a flat screen of OverlayWidth at OverlayDistance.
        hx = g.OverlayWidth * 0.5f / g.OverlayDistance;
        hy = hx / ov.aspect;
    }

    float clear[4] = { 0, 0, 0, 1 };
    D3D11_VIEWPORT vp = { 0, 0, (float)sc.rtW, (float)sc.rtH, 0, 1 };
    AcquireSRWLockExclusive(&g_d3dLock);
    for (int e = 0; e < 2; e++)
    {
        vr::EVREye eye = e ? vr::Eye_Right : vr::Eye_Left;
        ov.ctx->ClearRenderTargetView(sc.rtv[e], clear);
        if (want && sc.srcValid[e])
        {
            // SteamVR reprojects a frame with one head pose for both eyes, but the two eyes' pictures
            // were drawn one game frame apart with different head poses. So each picture is placed in
            // the world where it was drawn (a plane ReprojectDistance in front of the eye it was drawn
            // for) and then seen from the current pose; both eyes are submitted with that same pose.
            vr::HmdMatrix34_t render = now;
            const FramePose* fp = sc.srcId[e] >= 0 ? &g_framePose[sc.srcId[e] & 63] : nullptr;
            if (g.WorldLock && InRace() && fp && fp->valid && (g_frame - fp->frame) < 64 && !g.TestMotion)
                render = fp->m;
            vr::HmdMatrix34_t eyeT = g_sys->GetEyeToHeadTransform(eye);
            auto apply = [](const vr::HmdMatrix34_t& m, const float* v, float* o)
            {
                for (int i = 0; i < 3; i++) o[i] = m.m[i][0] * v[0] + m.m[i][1] * v[1] + m.m[i][2] * v[2] + m.m[i][3];
            };
            auto applyInv = [](const vr::HmdMatrix34_t& m, const float* v, float* o)
            {
                float d[3] = { v[0] - m.m[0][3], v[1] - m.m[1][3], v[2] - m.m[2][3] };
                for (int i = 0; i < 3; i++) o[i] = m.m[0][i] * d[0] + m.m[1][i] * d[1] + m.m[2][i] * d[2];
            };
            vr::HmdMatrix44_t P = g_sys->GetProjectionMatrix(eye, 0.05f, 100.0f);
            float up = g.OverlayUp / g.OverlayDistance, D = g.ReprojectDistance;
            float corners[4][2] = { { -hx, hy + up }, { hx, hy + up }, { -hx, -hy + up }, { hx, -hy + up } };
            float cb[20];
            // Depth mode: one side-by-side picture, left half for the left eye, right half for the right.
            bool sbs = g.StereoMode == 1 && sc.srcPair[e];
            cb[16] = sbs ? 0.5f * e : 0.0f; cb[17] = sbs ? 0.5f : 1.0f; cb[18] = cb[19] = 0;
            for (int k = 0; k < 4; k++)
            {
                float c[3] = { corners[k][0] * D, corners[k][1] * D, -D }, headR[3], world[3], headN[3], v3[3];
                apply(eyeT, c, headR);
                apply(render, headR, world);
                applyInv(now, world, headN);
                applyInv(eyeT, headN, v3);
                float v[4] = { v3[0], v3[1], v3[2], 1.0f };
                for (int r = 0; r < 4; r++)
                    cb[k * 4 + r] = P.m[r][0] * v[0] + P.m[r][1] * v[1] + P.m[r][2] * v[2] + P.m[r][3] * v[3];
            }
            ov.ctx->UpdateSubresource(sc.cb, 0, nullptr, cb, 0, 0);
            ov.ctx->OMSetRenderTargets(1, &sc.rtv[e], nullptr);
            ov.ctx->RSSetViewports(1, &vp);
            ov.ctx->RSSetState(sc.rs);
            ov.ctx->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
            ov.ctx->IASetInputLayout(nullptr);
            ov.ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
            ov.ctx->VSSetShader(sc.vs, nullptr, 0);
            ov.ctx->VSSetConstantBuffers(0, 1, &sc.cb);
            ov.ctx->PSSetShader(sc.ps, nullptr, 0);
            ov.ctx->PSSetShaderResources(0, 1, &sc.srv[e]);
            ov.ctx->PSSetSamplers(0, 1, &sc.smp);
            ov.ctx->Draw(4, 0);
            ID3D11ShaderResourceView* none = nullptr;
            ov.ctx->PSSetShaderResources(0, 1, &none);
        }
        vr::VRTextureWithPose_t tex;
        tex.handle = sc.rt[e];
        tex.eType = vr::TextureType_DirectX;
        tex.eColorSpace = vr::ColorSpace_Gamma;
        tex.mDeviceToAbsoluteTracking = now;  // same pose for both eyes
        comp->Submit(eye, &tex, nullptr, vr::Submit_TextureWithPose);
    }
    ov.ctx->Flush();
    {
        static DWORD lastCheck;
        if (GetTickCount() - lastCheck > 2000)
        {
            lastCheck = GetTickCount();
            char flag[MAX_PATH]; strcpy(flag, g_logPath);
            if (char* sl = strrchr(flag, '\\')) strcpy(sl + 1, "dump_vr.flag");
            if (GetFileAttributesA(flag) != INVALID_FILE_ATTRIBUTES)
            {
                DeleteFileA(flag);
                DumpTexture(sc.rt[0], "dump_vr_left.bmp");
                DumpTexture(sc.src[0], "dump_vr_source.bmp");
                Log("saved dump_vr_left.bmp and dump_vr_source.bmp");
            }
        }
    }

    // Diagnostics: how often each eye gets a new picture, and the longest wait, per 900 refreshes.
    {
        static int lastId[2] = { -1, -1 }, fresh[2], gap[2], maxGap[2], vsyncs, reports;
        for (int e = 0; e < 2; e++)
        {
            if (sc.srcId[e] != lastId[e]) { fresh[e]++; lastId[e] = sc.srcId[e]; gap[e] = 0; }
            else maxGap[e] = std::max(maxGap[e], ++gap[e]);
        }
        if (++vsyncs >= 900 && reports < 30)
        {
            Log("headset frames %d: new left %d (longest wait %d), new right %d (longest wait %d)",
                vsyncs, fresh[0], maxGap[0], fresh[1], maxGap[1]);
            vsyncs = 0; fresh[0] = fresh[1] = maxGap[0] = maxGap[1] = 0; reports++;
        }
    }
    ReleaseSRWLockExclusive(&g_d3dLock);
}

// ---------------------------------------------------------------- threads
static bool KeyPressed(int vk)
{
    static SHORT prev[256];
    if (vk <= 0 || vk > 255) return false;
    SHORT s = GetAsyncKeyState(vk);
    bool down = (s & 0x8000) != 0, was = (prev[vk] & 0x8000) != 0;
    prev[vk] = s;
    return down && !was;
}

// The game is only stable on one CPU core (the Widescreen Fix's SingleCoreAffinity), but pinning the
// whole process also squeezes this mod's capture and VR threads onto that core. Instead, every game
// thread is pinned to one core and the mod's own threads run on the others.
static DWORD g_ourThreads[8];
static volatile LONG g_ourThreadCount;

static void RegisterOurThread()
{
    LONG i = InterlockedIncrement(&g_ourThreadCount) - 1;
    if (i < 8) g_ourThreads[i] = GetCurrentThreadId();
}

static void PinGameThreads()
{
    if (!g.PinGameToOneCore) return;
    DWORD_PTR procMask = 0, sysMask = 0;
    if (!GetProcessAffinityMask(GetCurrentProcess(), &procMask, &sysMask) || !procMask) return;
    DWORD_PTR gameMask = procMask & (~procMask + 1);  // lowest available core
    if (procMask == gameMask && sysMask != procMask)
    {
        // Something (Widescreen Fix / compatibility shim) limited the whole process to one core:
        // open the process up again; the game's own threads stay on that one core below.
        if (SetProcessAffinityMask(GetCurrentProcess(), sysMask)) procMask = sysMask;
    }
    // Core 0 also serves most interrupts and system work, so the game gets its own core by default.
    if (g.GameCore >= 0 && g.GameCore < (int)(sizeof(DWORD_PTR) * 8) && (procMask & ((DWORD_PTR)1 << g.GameCore)))
        gameMask = (DWORD_PTR)1 << g.GameCore;
    DWORD_PTR modMask = procMask & ~gameMask;
    if (!modMask) return;  // single-core machine
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return;
    THREADENTRY32 te = { sizeof(te) };
    DWORD pid = GetCurrentProcessId();
    static int pinnedLogged;
    int pinned = 0;
    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te))
    {
        if (te.th32OwnerProcessID != pid) continue;
        bool ours = false;
        for (LONG i = 0; i < std::min<LONG>((LONG)g_ourThreadCount, 8L); i++) ours |= g_ourThreads[i] == te.th32ThreadID;
        HANDLE h = OpenThread(THREAD_SET_INFORMATION | THREAD_QUERY_INFORMATION, FALSE, te.th32ThreadID);
        if (!h) continue;
        if (SetThreadAffinityMask(h, ours ? modMask : gameMask)) pinned++;
        CloseHandle(h);
    }
    CloseHandle(snap);
    if (!pinnedLogged) { pinnedLogged = 1; Log("game threads pinned to core mask %llx, mod threads on %llx (%d threads)", (unsigned long long)gameMask, (unsigned long long)modMask, pinned); }
}

static DWORD WINAPI InputThread(LPVOID)
{
    RegisterOurThread();
    for (;;)
    {
        Sleep(15);
        // Stay idle while the game and the other ASI mods start up (racing them crashed AnimationController.asi).
        static DWORD started = GetTickCount();
        if (GetTickCount() - started < 4000) continue;
        static DWORD lastPin;
        if (GetTickCount() - lastPin > 1000) { lastPin = GetTickCount(); PinGameThreads(); }
        if (!g_stylesLoaded) LoadGaugeStyles();
        HookPresent();
        if (!g_wnd)
        {
            EnumWindows(FindGameWindow, 0);
            if (g_wnd)
            {
                g_origProc = (WNDPROC)SetWindowLongPtrA(g_wnd, GWLP_WNDPROC, (LONG_PTR)WndProc);
                Log("game window found");
            }
            continue;
        }
        if (GetForegroundWindow() != g_wnd) continue;
        bool ctrl = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;

        if (KeyPressed(g.KeyToggle)) { g_trackingOn = !g_trackingOn; Log("head tracking %s", g_trackingOn ? "on" : "off"); }
        if (KeyPressed(g.KeyRecenter)) InterlockedExchange(&g_recenterRequest, 1);
        if (KeyPressed(g.KeySave)) SaveAdjustables();
        if (KeyPressed(g.KeyOverlay)) { g.Overlay = !g.Overlay; Log("overlay %s", g.Overlay ? "on" : "off"); }

        // Ctrl + arrows / PageUp / PageDown move the seat. Ctrl+Shift + the same keys size the overlay.
        bool shift = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
        if (ctrl && !shift)
        {
            if (KeyPressed(VK_LEFT)) g.SeatRight -= g.SeatStep;
            if (KeyPressed(VK_RIGHT)) g.SeatRight += g.SeatStep;
            if (KeyPressed(VK_UP)) g.SeatForward += g.SeatStep;
            if (KeyPressed(VK_DOWN)) g.SeatForward -= g.SeatStep;
            if (KeyPressed(VK_PRIOR)) g.SeatUp += g.SeatStep;
            if (KeyPressed(VK_NEXT)) g.SeatUp -= g.SeatStep;
        }
        else if (ctrl && shift)
        {
            if (KeyPressed(VK_UP)) { if (g.AutoWidth) g.WidthScale += 0.02f; else g.OverlayWidth += 0.05f; }
            if (KeyPressed(VK_DOWN)) { if (g.AutoWidth) g.WidthScale = std::max(0.3f, g.WidthScale - 0.02f); else g.OverlayWidth = std::max(0.2f, g.OverlayWidth - 0.05f); }
            if (KeyPressed(VK_PRIOR)) g.OverlayUp += 0.02f;
            if (KeyPressed(VK_NEXT)) g.OverlayUp -= 0.02f;
            if (KeyPressed(VK_RIGHT)) g.OverlayDistance += 0.05f;
            if (KeyPressed(VK_LEFT)) g.OverlayDistance = std::max(0.2f, g.OverlayDistance - 0.05f);
        }
        else
        {
            // keep edge detection state current while no modifier is held
            KeyPressed(VK_LEFT); KeyPressed(VK_RIGHT); KeyPressed(VK_UP); KeyPressed(VK_DOWN); KeyPressed(VK_PRIOR); KeyPressed(VK_NEXT);
        }
    }
}

static bool SteamVRRunning()
{
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return false;
    PROCESSENTRY32 pe = { sizeof(pe) };
    bool found = false;
    for (BOOL ok = Process32First(snap, &pe); ok && !found; ok = Process32Next(snap, &pe))
        found = _stricmp(pe.szExeFile, "vrcompositor.exe") == 0;
    CloseHandle(snap);
    return found;
}

static DWORD WINAPI VRThread(LPVOID)
{
    RegisterOurThread();
    timeBeginPeriod(1);  // Sleep(1) while waiting for the next game frame
    // Stay out of the way while the game and the other ASI mods initialise.
    while (!g_wnd) Sleep(250);
    Sleep(5000);
    if (!LoadOpenVR()) return 0;
    SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    DWORD lastTry = 0;
    for (;;)
    {
        if (!g_vrReady)
        {
            if (GetTickCount() - lastTry < 3000 && lastTry) { Sleep(200); continue; }
            lastTry = GetTickCount();
            // Only attach to an already running SteamVR; never start it from the game.
            if (!SteamVRRunning() || !pVR_IsHmdPresent()) continue;
            AcquireSRWLockExclusive(&g_vrLock);
            bool ok = InitVR();
            ReleaseSRWLockExclusive(&g_vrLock);
            if (!ok) continue;
            {
                vr::ETrackedPropertyError pe;
                float ipd = g_sys->GetFloatTrackedDeviceProperty(vr::k_unTrackedDeviceIndex_Hmd, vr::Prop_UserIpdMeters_Float, &pe);
                if (pe == vr::TrackedProp_Success && ipd > 0.04f && ipd < 0.09f) g_ipd = ipd;
                Log("IPD %.1f mm", g_ipd * 1000.0f);
            }
            g_vrReady = true;
            InterlockedExchange(&g_recenterRequest, 1);
        }

        vr::VREvent_t ev;
        bool quit = false;
        while (g_sys->PollNextEvent(&ev, sizeof(ev)))
            if (ev.eventType == vr::VREvent_Quit) quit = true;
        if (quit)
        {
            Log("SteamVR is shutting down");
            g_vrReady = false;
            AcquireSRWLockExclusive(&g_vrLock);
            for (int e = 0; e < 2; e++)
            {
                if (ov.handle[e] != vr::k_ulOverlayHandleInvalid) g_ovl->DestroyOverlay(ov.handle[e]);
                ov.handle[e] = vr::k_ulOverlayHandleInvalid; ov.shown[e] = false;
            }
            if (g_bgHandle != vr::k_ulOverlayHandleInvalid) g_ovl->DestroyOverlay(g_bgHandle);
            g_bgHandle = vr::k_ulOverlayHandleInvalid;
            ReleaseScene();
            ReleaseHandover();
            ReleaseDup();
            g_sys->AcknowledgeQuit_Exiting();
            pVR_ShutdownInternal();
            g_sys = nullptr; g_ovl = nullptr;
            ReleaseSRWLockExclusive(&g_vrLock);
            continue;
        }

        if (g.VRFrames) SceneFrame();
        else if (g.Overlay || ov.shown[0] || ov.shown[1]) OverlayFrame();
        else Sleep(20);
    }
}

static bool PatchCall(DWORD site, void* fn, DWORD expectTarget)
{
    BYTE* p = (BYTE*)site;
    if (p[0] != 0xE8 || site + 5 + *(int*)(p + 1) != expectTarget) return false;
    DWORD old;
    VirtualProtect(p, 5, PAGE_EXECUTE_READWRITE, &old);
    *(int*)(p + 1) = (int)((DWORD)fn - (site + 5));
    VirtualProtect(p, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), p, 5);
    return true;
}

BOOL APIENTRY DllMain(HMODULE mod, DWORD reason, LPVOID)
{
    if (reason != DLL_PROCESS_ATTACH) return TRUE;
    DisableThreadLibraryCalls(mod);

    char dir[MAX_PATH];
    GetModuleFileNameA(mod, dir, MAX_PATH);
    *strrchr(dir, '\\') = 0;
    sprintf(g_iniPath, "%s\\NFSU2HeadTrack.ini", dir);
    sprintf(g_logPath, "%s\\NFSU2HeadTrack.log", dir);
    strcpy(g_gameDir, dir);
    if (char* sl = strrchr(g_gameDir, '\\')) *sl = 0;
    DeleteFileA(g_logPath);
    LoadSettings();
    g_trackingOn = g.Enabled != 0;

    // With Windows display scaling (e.g. 133%) the old game sees a shrunken desktop (1920x1080 on a
    // 2560x1440 screen) and renders at that size. Declaring the process DPI-aware before the game
    // and the Widescreen Fix read the desktop size makes it render at the real resolution.
    if (GetPrivateProfileIntA("MAIN", "DpiAware", 1, g_iniPath))
    {
        BOOL ok = SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        if (!ok) ok = SetProcessDPIAware();
        Log("DPI aware: %s, desktop %dx%d", ok ? "yes" : "no", GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN));
    }

    if (!PatchCall(kCopyCallSite, (void*)CopyCameraMatrix, kCopyMatrix))
    {
        Log("unsupported SPEED2.EXE (needs v1.2, 4,800,512 bytes) - mod disabled");
        return TRUE;
    }
    Log("camera hook installed");
    if (PatchCall(0x5C7EF0, (void*)RenderCopyView, kCopyMatrix)) Log("render view hook installed");
    if (PatchCall(0x5D2B60, (void*)HudPassHook, 0x5CBF40)) Log("HUD pass hook installed");
    {
        // Point inside the HUD pass where the clean picture is grabbed (just before the HUD is drawn).
        char buf[32];
        auto inPass = [](DWORD x) { return ((x >= 0x5CBF40 && x < 0x5CC248) || (x >= 0x5BDDE0 && x < 0x5BDF88)) && *(BYTE*)x == 0xE8; };
        GetPrivateProfileStringA("GAUGES", "MapGrabAt", "5CC08D", buf, sizeof(buf), g_iniPath);
        DWORD a = strtoul(buf, nullptr, 16);
        GetPrivateProfileStringA("GAUGES", "MapRestoreAfter", "5CC0C0", buf, sizeof(buf), g_iniPath);
        DWORD r = strtoul(buf, nullptr, 16);
        if (inPass(a))
        {
            g_grabTarget = a + 5 + *(int*)(a + 1);
            if (r == a)
            {
                if (PatchCall(a, (void*)GrabRestoreThunk, g_grabTarget)) { g_grabAt = g_restoreAt = a; Log("map grab+restore around %08lX", a); }
            }
            else if (PatchCall(a, (void*)GrabThunk, g_grabTarget)) { g_grabAt = a; Log("map grab point %08lX", a); }
        }
        if (g_grabAt && !g_restoreAt && inPass(r))
        {
            g_restoreTarget = r + 5 + *(int*)(r + 1);
            if (PatchCall(r, (void*)RestoreThunk, g_restoreTarget)) { g_restoreAt = r; Log("map restore point %08lX", r); }
        }
    }
    {
        // Debug aid: [DEBUG] SkipCall=<hex address of a 5-byte call> turns that call into NOPs.
        char buf[32];
        GetPrivateProfileStringA("DEBUG", "SkipCall", "", buf, sizeof(buf), g_iniPath);
        DWORD a = strtoul(buf, nullptr, 16);
        if (a >= 0x401000 && a < 0x790000 && *(BYTE*)a == 0xE8)
        {
            DWORD old;
            VirtualProtect((void*)a, 5, PAGE_EXECUTE_READWRITE, &old);
            memset((void*)a, 0x90, 5);
            VirtualProtect((void*)a, 5, old, &old);
            Log("debug: skipped call at %08lX", a);
        }
    }
    CreateThread(nullptr, 0, InputThread, nullptr, 0, nullptr);
    CreateThread(nullptr, 0, VRThread, nullptr, 0, nullptr);
    return TRUE;
}
