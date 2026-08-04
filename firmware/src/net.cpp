#include "net.h"

#include <WiFi.h>
#include <time.h>

bool wifiConnect(const Config &c, uint32_t timeoutMs) {
  WiFi.mode(WIFI_STA);
  WiFi.begin(c.ssid.c_str(), c.pass.c_str());
  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < timeoutMs) {
    delay(200);
  }
  return WiFi.status() == WL_CONNECTED;
}

bool timeSync(uint32_t timeoutMs) {
  // TT_TZ is a POSIX TZ string (default Central European w/ DST) so the panel
  // shows local wall-clock. Grail timestamps are absolute UTC and parsed as such
  // in dt_screens, so ages stay correct regardless of this.
  configTzTime(TT_TZ, "pool.ntp.org", "time.nist.gov");
  uint32_t start = millis();
  while (millis() - start < timeoutMs) {
    if (time(nullptr) > 1700000000) return true;  // clock is set (past 2023-11)
    delay(200);
  }
  return false;
}
