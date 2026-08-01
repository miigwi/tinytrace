// Device configuration: WiFi credentials, the Dynatrace tenant URL, and the
// read-only platform token. Stored in NVS (flash key/value), provisioned once
// via a captive-portal web form — never compiled into the firmware, so no
// secret is ever committed. NVS survives a normal `pio run -t upload`; only a
// full chip erase wipes it, so provisioning is a one-time step.
#pragma once

#include <Arduino.h>

struct Config {
  String ssid;
  String pass;
  String tenant;  // e.g. https://abc12345.apps.dynatrace.com (no trailing slash)
  String token;   // dt0s16… read-only platform token

  bool complete() const { return ssid.length() && tenant.length() && token.length(); }
};

// configLoad reads NVS into out; returns true if all required fields are present.
bool configLoad(Config &out);
// configSave persists the config to NVS.
void configSave(const Config &c);
// configClear erases the stored config (forces re-provisioning).
void configClear();

// runPortal brings up a SoftAP ("dynaglance-setup") + captive-portal web form,
// then blocks serving it. On submit it saves to NVS and reboots. It does not
// touch the display — the caller shows the join instructions first.
void runPortal();
