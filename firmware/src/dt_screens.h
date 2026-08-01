// The three live screens, built from DQL results. Each builder runs its query
// via dt.cpp and formats records into a Screen. On the wire these are the same
// screens the CYD gets from the Go sidecar — but here the device builds them
// itself. The problems query is copied verbatim from dynatui/dynaglance's
// internal/screens/screens.go so the panel and the TUI can't disagree.
#pragma once

#include "config.h"
#include "model.h"

// Each returns true on a successful query (even with zero rows). On false the
// caller keeps the previous screen and marks it stale.
bool buildProblems(const Config &cfg, Screen &out);
bool buildLogs(const Config &cfg, Screen &out);
bool buildGolden(const Config &cfg, Screen &out);

// buildIdle synthesises the sleep screen from the active-problem count.
Screen buildIdle(int problemCount, const String &tenant);
