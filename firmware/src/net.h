// Networking: WiFi association and SNTP time-sync. The DQL/TLS client lands in
// Phase 3 (dt.cpp). Time-sync matters before then: TLS certificate validation
// needs a real wall-clock, so the device must know the date before it can talk
// to the tenant over HTTPS.
#pragma once

#include "config.h"

// wifiConnect joins the configured network; returns true once associated.
bool wifiConnect(const Config &c, uint32_t timeoutMs = 20000);

// timeSync sets the system clock from SNTP (UTC). Returns true once the clock
// is plausibly set (past 2023). Local-time display formatting comes later.
bool timeSync(uint32_t timeoutMs = 15000);
