// Device configuration.
//
// Two layers:
//   • Config  — a single *active* connection (one WiFi + one tenant), the flat
//     struct that net.cpp / dt.cpp / dt_screens.cpp consume. Unchanged shape.
//   • Settings — the persisted store: lists of known WiFi networks and known
//     Dynatrace tenants, each with a selected index. The captive portal manages
//     these; settingsActive() resolves the selection down to a Config.
//
// Everything lives in NVS (flash key/value), never compiled in, so no secret is
// committed. The store is a small JSON blob under one key; older single-config
// installs are migrated on first load.
#pragma once

#include <Arduino.h>

// The active connection resolved from the current selection. Empty ssid means
// "no WiFi selected" → the device boots non-connected (demo mode); empty
// tenant/token means "no tenant" → demo even if WiFi associates.
struct Config {
  String ssid;
  String pass;
  String tenant;  // e.g. https://abc12345.apps.dynatrace.com (no trailing slash)
  String token;   // dt0s16… read-only platform token

  bool complete() const { return ssid.length() && tenant.length() && token.length(); }
};

static const int MAX_WIFI = 6;
static const int MAX_TENANTS = 6;

struct WifiNet {
  String ssid;
  String pass;
};
struct TenantConn {
  String url;    // tenant URL, no trailing slash
  String token;  // platform token
};

// The persisted configuration store.
struct Settings {
  WifiNet wifi[MAX_WIFI];
  int nwifi = 0;
  int wifiSel = -1;  // index into wifi[], or -1 for none

  TenantConn tenant[MAX_TENANTS];
  int ntenant = 0;
  int tenantSel = -1;  // index into tenant[], or -1 for none

  bool wifiReady() const { return wifiSel >= 0 && wifiSel < nwifi; }
  bool tenantReady() const { return tenantSel >= 0 && tenantSel < ntenant; }
};

// settingsLoad reads the store from NVS (migrating an old single-config install
// on the way). settingsSave persists it. settingsReset wipes everything back to
// the empty, non-connected state ("no networks known, no tenants known").
void settingsLoad(Settings &out);
void settingsSave(const Settings &s);
void settingsReset();

// settingsActive resolves the selected WiFi + tenant into a flat Config; unset
// selections leave the corresponding fields empty.
Config settingsActive(const Settings &s);

// Mutations used by the portal. addWifi/addTenant dedupe by ssid/url (updating
// the secret), append if new, and select the added entry. Return false if full.
bool settingsAddWifi(Settings &s, const String &ssid, const String &pass);
bool settingsAddTenant(Settings &s, const String &url, const String &token);

// runPortal brings up a SoftAP ("dynaglance-setup") + the captive-portal web UI
// for managing WiFi networks and tenants, then blocks serving it. Mutations are
// persisted to NVS immediately; the Apply button reboots. It does not touch the
// display — the caller shows the join instructions first.
void runPortal();
