#pragma once

// Car state handed from the telemetry thread to the force feedback thread.
struct FfbInput
{
    bool running = false, paused = false;
    float vFwd = 0, vLat = 0;        // m/s, car frame, lateral positive to the right
    float yawRateDegS = 0;           // positive turning right
    float swayAcc = 0, surgeAcc = 0; // m/s^2
    float suspVel[4] = {};           // FL, FR, RL, RR
    float frontAxle = 1.2f;          // m ahead of the car origin
};

void FfbStart(const char* iniPath);
void FfbPublish(const FfbInput& in);
