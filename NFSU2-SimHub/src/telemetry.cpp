// NFSU2Telemetry - streams NFSU2 (v1.2, 4,800,512 byte SPEED2.EXE) car telemetry to SimHub.
//
// SimHub's "External sims" support reads a fixed binary UDP packet described by a definition file
// (NFSU2Telemetry.simdef, next to this ASI). At startup the ASI registers that definition by
// writing %LOCALAPPDATA%\SimHub\ExternalSims\Registrations\{id}.simlink, so Need for Speed
// Underground 2 shows up in SimHub as a normal game: dashboards, rev lights, ShakeIt and motion
// all use the standard fields below.
//
// A background thread polls game memory (same addresses as NFSU2HeadTrack's HUD) and sends one
// packet per new physics sample, plus a heartbeat while nothing changes (menus, pause).
//
// Settings: NFSU2Telemetry.ini next to the ASI
//   [SIMHUB] Enabled=1  Host=127.0.0.1  Port=30777  MaxRpm=0 (0 = highest rpm seen in this car)

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <mmsystem.h>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cstdlib>

#include "ffb.h"

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "winmm.lib")

// ---------------------------------------------------------------- packet (from SimHub's generator)
// Must match NFSU2Telemetry.simdef. Regenerate both together if fields change.
static const char* kDefinitionId = "4e465355-3200-4d0d-9f1e-7e1e00000002";
static const uint32_t kGameSignature = 0x5F47F8C3u;
static const uint32_t kTelemetrySignature = 0x12E9E242u;  // layout 1.1

#pragma pack(push, 1)
struct Packet
{
    // header
    uint32_t GameSignature, TelemetrySignature;
    uint16_t LayoutMajorVersion, LayoutMinorVersion;
    uint64_t EmitterInstanceId;
    uint8_t PacketId;
    uint64_t PacketsCounter;
    uint8_t IsSessionRunning, IsSessionPaused;
    uint64_t SessionId;
    uint8_t IsReplay, IsUserInControl, IsAIInControl, IsSpectator;
    double SessionTimeSeconds;
    uint32_t PhysicsDiscontinuityCounter;
    // fields
    float SpeedKmh, EngineRpm, EngineMaxRpm;
    char Gear[8];
    int32_t RacePosition;
    uint32_t CompletedLaps;
    int32_t TotalLaps;
    double CurrentLapTime, LastLapTimeSeconds, BestLapTimeSeconds;
    float YawDegrees, PitchDegrees, RollDegrees;
    float YawRateDegreesPerSecond, PitchRateDegreesPerSecond, RollRateDegreesPerSecond;
    float LocalSurgeMs2, LocalSwayMs2, LocalHeaveMs2;
    float LocalVelocityForwardMps, LocalVelocityLateralMps, LocalVelocityUpMps;
    double VehiclePositionNorth, VehiclePositionEast, VehiclePositionUp;
    int32_t OpponentCount;
    // layout 1.1 (wheel arrays are FL, FR, RL, RR)
    float Throttle, Brake, Handbrake;
    float WheelSlip[4];
    float WheelLock[4];
    float SuspensionVelocityMps[4];
    uint8_t PushToPassActive;
    float NitroBoost;
};
#pragma pack(pop)
static_assert(sizeof(Packet) == 252, "packet layout must match NFSU2Telemetry.simdef");

// ---------------------------------------------------------------- settings / log
static char g_dir[MAX_PATH], g_iniPath[MAX_PATH], g_logPath[MAX_PATH];

void Log(const char* fmt, ...)
{
    FILE* f = fopen(g_logPath, "a");
    if (!f) return;
    SYSTEMTIME t; GetLocalTime(&t);
    fprintf(f, "%02d:%02d:%02d.%03d ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    va_list a; va_start(a, fmt); vfprintf(f, fmt, a); va_end(a);
    fputc('\n', f);
    fclose(f);
}

// ---------------------------------------------------------------- game data (SPEED2.EXE v1.2)
//   race coordinator *(0x8900D8) != 0 while a race is loaded
//   car anchor (view 1 mover + 0x2C): +0x50 position, +0x70 velocity (m/s, world, z up),
//     +0x90/+0xA0/+0xB0 body matrix rows (forward, left, up), +0x440 engine rpm,
//     +0x1C -> car, car +0x1CC gear (0 = R, 1 = N, 2 = 1st ...)
//     +0x438 throttle applied by the engine (0-1)
//     per wheel, order FL, FR, RR, RL (wheel offsets at +0x160 + 0x40 * i):
//       +0x330 angular speed (rad/s), +0x380 suspension compression (m)
//     +0x30 -> driver input: +0x20C throttle, +0x210 handbrake, +0x214 brake, +0x274 nitro boost factor
//   (found by recording memory while driving; handbrake and nitro are the least certain)
//   race status *(0x890118): +0x00 racer count, +0x14 race clock (4000 ticks/s), +0xBE8 laps
//   player racer entry = race status + 0x30: +0x0A position (1-based), +0x14 laps done (float),
//     +0x44 lap start tick
struct Sample
{
    bool inRace = false, car = false, race = false;
    float pos[3], vel[3], fwd[3], left[3], up[3];
    float rpm = 0;
    int gear = 1;
    float throttle = 0, brake = 0, handbrake = 0, nitro = 1;
    float wheelSpeed[4] = {}, susp[4] = {};  // FL, FR, RR, RL as stored by the game
    int racers = 0, position = 0, laps = 0;
    float lapsDone = 0;
    int clock = 0, lapStart = 0;
};

static bool Unit(const float* v) { return fabsf(sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]) - 1) < 0.05f; }

static void ReadGame(Sample& s)
{
    s = Sample();
    __try
    {
        s.inRace = *(void**)0x8900D8 != nullptr;
        BYTE* view = (BYTE*)0x832E50;
        BYTE* node = *(BYTE**)(view + 0x44);
        if (node && node != view + 0x44)
        {
            BYTE* anchor = *(BYTE**)(node - 4 + 0x2C);
            if (anchor)
            {
                memcpy(s.pos, anchor + 0x50, 12);
                memcpy(s.vel, anchor + 0x70, 12);
                const float* r = (const float*)(anchor + 0x90);
                for (int i = 0; i < 3; i++) { s.fwd[i] = r[i]; s.left[i] = r[4 + i]; s.up[i] = r[8 + i]; }
                s.rpm = *(float*)(anchor + 0x440);
                if (!(s.rpm >= 0 && s.rpm < 20000)) s.rpm = 0;
                BYTE* car = *(BYTE**)(anchor + 0x1C);
                s.gear = car ? *(int*)(car + 0x1CC) : 1;
                if (s.gear < 0 || s.gear > 9) s.gear = 1;
                bool posOk = fabsf(s.pos[0]) < 1e5f && fabsf(s.pos[1]) < 1e5f && fabsf(s.pos[2]) < 1e5f;
                bool velOk = fabsf(s.vel[0]) < 300 && fabsf(s.vel[1]) < 300 && fabsf(s.vel[2]) < 300;
                s.car = posOk && velOk && Unit(s.fwd) && Unit(s.left) && Unit(s.up);
                memcpy(s.wheelSpeed, anchor + 0x330, 16);
                memcpy(s.susp, anchor + 0x380, 16);
                s.throttle = *(float*)(anchor + 0x438);
                BYTE* input = *(BYTE**)(anchor + 0x30);
                if (input)
                {
                    s.handbrake = *(float*)(input + 0x210);
                    s.brake = *(float*)(input + 0x214);
                    s.nitro = *(float*)(input + 0x274);
                }
                auto clamp01 = [](float v) { return v >= 0 && v <= 1 ? v : 0.0f; };
                s.throttle = clamp01(s.throttle); s.brake = clamp01(s.brake); s.handbrake = clamp01(s.handbrake);
                if (!(s.nitro >= 1 && s.nitro < 5)) s.nitro = 1;
                for (int i = 0; i < 4; i++)
                {
                    if (!(fabsf(s.wheelSpeed[i]) < 2000)) s.wheelSpeed[i] = 0;
                    if (!(fabsf(s.susp[i]) < 2)) s.susp[i] = 0;
                }
            }
        }
        BYTE* rs = *(BYTE**)0x890118;
        if (rs)
        {
            s.racers = *(int*)rs;
            s.laps = *(int*)(rs + 0xBE8);
            s.clock = *(int*)(rs + 0x14);
            BYTE* e = rs + 0x30;
            s.position = e[0x0A];
            s.lapsDone = *(float*)(e + 0x14);
            s.lapStart = *(int*)(e + 0x44);
            s.race = s.racers >= 1 && s.racers <= 8 && s.position >= 1 && s.position <= s.racers;
            if (s.laps < 1 || s.laps > 127) s.laps = 0;
            if (!(s.lapsDone >= 0 && s.lapsDone < 200)) s.lapsDone = 0;
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        s.car = s.race = false;
    }
}

// ---------------------------------------------------------------- sender
static float Dot(const float* a, const float* b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
static const float kDeg = 57.2957795f;

static uint64_t Random64()
{
    LARGE_INTEGER c; QueryPerformanceCounter(&c);
    uint64_t x = (uint64_t)c.QuadPart ^ ((uint64_t)GetCurrentProcessId() << 32) ^ GetTickCount();
    x ^= x >> 33; x *= 0xff51afd7ed558ccdULL; x ^= x >> 33; x *= 0xc4ceb9fe1a85ec53ULL; x ^= x >> 33;
    return x ? x : 1;
}

static void RegisterDefinition()
{
    char base[MAX_PATH], path[MAX_PATH], def[MAX_PATH];
    if (!GetEnvironmentVariableA("LOCALAPPDATA", base, MAX_PATH)) return;
    sprintf(path, "%s\\SimHub", base); CreateDirectoryA(path, nullptr);
    sprintf(path, "%s\\SimHub\\ExternalSims", base); CreateDirectoryA(path, nullptr);
    sprintf(path, "%s\\SimHub\\ExternalSims\\Registrations", base); CreateDirectoryA(path, nullptr);
    sprintf(path, "%s\\SimHub\\ExternalSims\\Registrations\\%s.simlink", base, kDefinitionId);
    sprintf(def, "%s\\NFSU2Telemetry.simdef", g_dir);
    if (GetFileAttributesA(def) == INVALID_FILE_ATTRIBUTES)
    {
        Log("NFSU2Telemetry.simdef not found next to the ASI - SimHub registration skipped");
        return;
    }
    FILE* f = fopen(path, "w");
    if (!f) { Log("could not write %s", path); return; }
    fprintf(f, "%s\n", def);
    fclose(f);
    Log("registered with SimHub: %s", path);
}

static DWORD WINAPI SendThread(LPVOID)
{
    char host[64];
    GetPrivateProfileStringA("SIMHUB", "Host", "127.0.0.1", host, sizeof(host), g_iniPath);
    int port = GetPrivateProfileIntA("SIMHUB", "Port", 30777, g_iniPath);
    float iniMaxRpm = (float)GetPrivateProfileIntA("SIMHUB", "MaxRpm", 0, g_iniPath);

    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) { Log("WSAStartup failed"); return 0; }
    SOCKET sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock == INVALID_SOCKET) { Log("socket failed"); return 0; }
    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons((u_short)port);
    if (inet_pton(AF_INET, host, &addr.sin_addr) != 1) { Log("bad host %s", host); return 0; }
    Log("sending telemetry to %s:%d", host, port);

    timeBeginPeriod(1);
    Packet p = {};
    p.GameSignature = kGameSignature;
    p.TelemetrySignature = kTelemetrySignature;
    p.LayoutMajorVersion = 1;
    p.LayoutMinorVersion = 1;
    p.EmitterInstanceId = Random64();

    LARGE_INTEGER freq, t0, now; QueryPerformanceFrequency(&freq); QueryPerformanceCounter(&t0);
    auto secs = [&](const LARGE_INTEGER& a, const LARGE_INTEGER& b) { return (double)(b.QuadPart - a.QuadPart) / freq.QuadPart; };

    Sample prev; bool havePrev = false;
    LARGE_INTEGER prevT = t0, lastSend = t0, lastClockChange = t0, sessionStart = t0;
    bool session = false;
    int lastClock = 0, lastLapStart = -1, lastLapsDone = 0;
    float maxRpm = 0;
    float acc[3] = {}, rate[3] = {}, suspVel[4] = {};
    float wheelRadius = 0.3f;  // learned per car while coasting
    bool wasRunning = false;

    for (;;)
    {
        Sleep(2);
        Sample s; ReadGame(s);
        QueryPerformanceCounter(&now);

        bool running = s.inRace && s.car;
        if (running && !wasRunning)
        {
            // new race: new session, forget lap history and the learned rev limit
            p.SessionId = Random64();
            p.LastLapTimeSeconds = p.BestLapTimeSeconds = 0;
            lastLapStart = -1; lastLapsDone = 0; maxRpm = 0; wheelRadius = 0.3f;
            sessionStart = lastClockChange = now;
            havePrev = false;
            Log("race started, %d racers, %d laps", s.racers, s.laps);
        }
        wasRunning = running;

        // a new physics sample is any change in the car state
        bool fresh = running && (!havePrev || memcmp(s.pos, prev.pos, 12) || memcmp(s.vel, prev.vel, 12) || s.rpm != prev.rpm);
        if (!fresh && secs(lastSend, now) < 0.05) continue;

        if (s.race && s.clock != lastClock) { lastClock = s.clock; lastClockChange = now; }
        bool paused = running && s.race && secs(lastClockChange, now) > 0.25;

        p.IsSessionRunning = running;
        p.IsSessionPaused = paused;
        p.IsUserInControl = running && !paused;
        p.SessionTimeSeconds = running ? (s.race && s.clock > 0 ? s.clock / 4000.0 : secs(sessionStart, now)) : 0;

        if (running)
        {
            // right = -left; local axes straight from the body matrix
            float right[3] = { -s.left[0], -s.left[1], -s.left[2] };
            p.SpeedKmh = sqrtf(Dot(s.vel, s.vel)) * 3.6f;
            if (p.SpeedKmh > 999) p.SpeedKmh = 0;
            p.LocalVelocityForwardMps = Dot(s.vel, s.fwd);
            p.LocalVelocityLateralMps = Dot(s.vel, right);
            p.LocalVelocityUpMps = Dot(s.vel, s.up);

            // World frame: z up, y north. If forward x up points left, the world is mirrored and
            // east is -x, so heading stays clockwise.
            float c[3] = { s.fwd[1] * s.up[2] - s.fwd[2] * s.up[1], s.fwd[2] * s.up[0] - s.fwd[0] * s.up[2], s.fwd[0] * s.up[1] - s.fwd[1] * s.up[0] };
            float eastSign = Dot(c, right) >= 0 ? 1.0f : -1.0f;
            p.VehiclePositionEast = eastSign * s.pos[0];
            p.VehiclePositionNorth = s.pos[1];
            p.VehiclePositionUp = s.pos[2];
            float yaw = atan2f(eastSign * s.fwd[0], s.fwd[1]) * kDeg;
            p.YawDegrees = yaw < 0 ? yaw + 360 : yaw;
            p.PitchDegrees = -asinf(fmaxf(-1, fminf(1, s.fwd[2]))) * kDeg;   // nose down positive
            p.RollDegrees = atan2f(-right[2], s.up[2]) * kDeg;                 // right side down positive

            if (fresh && havePrev)
            {
                float dt = (float)secs(prevT, now);
                float dx = s.pos[0] - prev.pos[0], dy = s.pos[1] - prev.pos[1], dz = s.pos[2] - prev.pos[2];
                if (dx * dx + dy * dy + dz * dz > 30 * 30)
                {
                    // reset to track / teleport
                    p.PhysicsDiscontinuityCounter++;
                    acc[0] = acc[1] = acc[2] = rate[0] = rate[1] = rate[2] = 0;
                }
                else if (dt > 0.004f && dt < 0.2f)
                {
                    float dv[3] = { (s.vel[0] - prev.vel[0]) / dt, (s.vel[1] - prev.vel[1]) / dt, (s.vel[2] - prev.vel[2]) / dt };
                    float df[3] = { (s.fwd[0] - prev.fwd[0]) / dt, (s.fwd[1] - prev.fwd[1]) / dt, (s.fwd[2] - prev.fwd[2]) / dt };
                    float dr[3] = { -(s.left[0] - prev.left[0]) / dt, -(s.left[1] - prev.left[1]) / dt, -(s.left[2] - prev.left[2]) / dt };
                    float a[3] = { Dot(dv, s.fwd), Dot(dv, right), Dot(dv, s.up) };
                    float w[3] = { Dot(df, right) * kDeg, -Dot(df, s.up) * kDeg, -Dot(dr, s.up) * kDeg };
                    const float k = 0.5f;  // light smoothing, sampling is not locked to the physics step
                    for (int i = 0; i < 3; i++) { acc[i] += (a[i] - acc[i]) * k; rate[i] += (w[i] - rate[i]) * k; }
                    for (int i = 0; i < 4; i++) suspVel[i] += ((s.susp[i] - prev.susp[i]) / dt - suspVel[i]) * k;
                }
            }
            p.LocalSurgeMs2 = acc[0]; p.LocalSwayMs2 = acc[1]; p.LocalHeaveMs2 = acc[2];
            p.YawRateDegreesPerSecond = rate[0]; p.PitchRateDegreesPerSecond = rate[1]; p.RollRateDegreesPerSecond = rate[2];

            // Pedals and wheels. The game stores wheels as FL, FR, RR, RL; SimHub wants FL, FR, RL, RR.
            p.Throttle = s.throttle; p.Brake = s.brake; p.Handbrake = s.handbrake;
            p.NitroBoost = s.nitro;
            p.PushToPassActive = s.nitro > 1.01f;
            float vf = p.LocalVelocityForwardMps, vAbs = sqrtf(Dot(s.vel, s.vel));
            if (s.throttle < 0.05f && s.brake < 0.05f && s.handbrake < 0.05f && vf > 8)
            {
                // free-rolling front wheels give the tyre radius
                float wf = 0.5f * (s.wheelSpeed[0] + s.wheelSpeed[1]);
                if (wf > 1) { float r = vf / wf; if (r > 0.2f && r < 0.5f) wheelRadius += (r - wheelRadius) * 0.02f; }
            }
            static const int kMap[4] = { 0, 1, 3, 2 };
            for (int i = 0; i < 4; i++)
            {
                int g = kMap[i];
                float surf = s.wheelSpeed[g] * wheelRadius;
                float ref = fmaxf(fmaxf(fabsf(vf), fabsf(surf)), 2.0f);
                float lon = (surf - vf) / ref;
                float lat = vAbs > 2 ? p.LocalVelocityLateralMps / vAbs : 0;
                p.WheelSlip[i] = fminf(1.0f, sqrtf(lon * lon + lat * lat));
                p.WheelLock[i] = (vf > 3 && surf < vf) ? fminf(1.0f, fmaxf(0.0f, 1 - surf / vf)) : 0;
                if (p.WheelLock[i] < 0.1f) p.WheelLock[i] = 0;
                p.SuspensionVelocityMps[i] = suspVel[g];
            }

            p.EngineRpm = s.rpm;
            if (s.rpm > maxRpm) maxRpm = s.rpm;
            p.EngineMaxRpm = iniMaxRpm > 0 ? iniMaxRpm : fmaxf(maxRpm, 6000.0f);
            const char* gears[] = { "R", "N", "1", "2", "3", "4", "5", "6", "7", "8" };
            memset(p.Gear, 0, sizeof(p.Gear));
            strcpy(p.Gear, gears[s.gear]);

            if (s.race)
            {
                p.RacePosition = s.position;
                p.OpponentCount = s.racers;
                int done = (int)s.lapsDone;
                if (s.laps > 0 && done > s.laps) done = s.laps;
                p.CompletedLaps = done;
                p.TotalLaps = s.laps > 0 ? s.laps : -1;
                p.CurrentLapTime = (s.clock >= s.lapStart && s.lapStart > 0) ? (s.clock - s.lapStart) / 4000.0 : s.clock / 4000.0;
                if (lastLapStart < 0) { lastLapStart = s.lapStart; lastLapsDone = done; }
                else if (done > lastLapsDone)
                {
                    // lap completed: the new lap start tick minus the previous one is the lap time
                    int from = lastLapStart > 0 ? lastLapStart : 0;
                    int to = s.lapStart > from ? s.lapStart : s.clock;
                    double lap = (to - from) / 4000.0;
                    if (lap > 1)
                    {
                        p.LastLapTimeSeconds = lap;
                        if (p.BestLapTimeSeconds <= 0 || lap < p.BestLapTimeSeconds) p.BestLapTimeSeconds = lap;
                        Log("lap %d: %.3f s", done, lap);
                    }
                    lastLapStart = s.lapStart; lastLapsDone = done;
                }
            }
            else
            {
                p.RacePosition = 0; p.OpponentCount = 0; p.CompletedLaps = 0; p.TotalLaps = -1; p.CurrentLapTime = 0;
            }
            prev = s; prevT = now; havePrev = true;
        }

        FfbInput fi;
        fi.running = running; fi.paused = paused;
        fi.vFwd = p.LocalVelocityForwardMps; fi.vLat = p.LocalVelocityLateralMps;
        fi.yawRateDegS = p.YawRateDegreesPerSecond;
        fi.swayAcc = p.LocalSwayMs2; fi.surgeAcc = p.LocalSurgeMs2;
        for (int i = 0; i < 4; i++) fi.suspVel[i] = p.SuspensionVelocityMps[i];
        FfbPublish(fi);

        p.PacketsCounter++;
        sendto(sock, (const char*)&p, sizeof(p), 0, (const sockaddr*)&addr, sizeof(addr));
        lastSend = now;
    }
}

BOOL APIENTRY DllMain(HMODULE mod, DWORD reason, LPVOID)
{
    if (reason != DLL_PROCESS_ATTACH) return TRUE;
    DisableThreadLibraryCalls(mod);

    GetModuleFileNameA(mod, g_dir, MAX_PATH);
    *strrchr(g_dir, '\\') = 0;
    sprintf(g_iniPath, "%s\\NFSU2Telemetry.ini", g_dir);
    sprintf(g_logPath, "%s\\NFSU2Telemetry.log", g_dir);
    DeleteFileA(g_logPath);

    if (!GetPrivateProfileIntA("SIMHUB", "Enabled", 1, g_iniPath)) { Log("disabled in ini"); return TRUE; }

    char exe[MAX_PATH];
    GetModuleFileNameA(nullptr, exe, MAX_PATH);
    WIN32_FILE_ATTRIBUTE_DATA fa;
    if (!GetFileAttributesExA(exe, GetFileExInfoStandard, &fa) || fa.nFileSizeHigh || fa.nFileSizeLow != 4800512)
    {
        Log("unsupported SPEED2.EXE (needs v1.2, 4,800,512 bytes) - telemetry disabled");
        return TRUE;
    }
    FfbStart(g_iniPath);
    RegisterDefinition();
    CreateThread(nullptr, 0, SendThread, nullptr, 0, nullptr);
    return TRUE;
}
