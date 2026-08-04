#include "config.h"

#include <ArduinoJson.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <WebServer.h>
#include <WiFi.h>

static const char *NS = "dynaglance";  // NVS namespace
static const char *KEY = "settings";   // JSON blob key

// ------------------------------------------------------------------ store

static void fromJson(const String &blob, Settings &out) {
  JsonDocument doc;
  if (deserializeJson(doc, blob)) return;  // parse error → leave empty
  for (JsonObjectConst w : doc["w"].as<JsonArrayConst>()) {
    if (out.nwifi >= MAX_WIFI) break;
    out.wifi[out.nwifi].ssid = w["s"].as<String>();
    out.wifi[out.nwifi].pass = w["p"].as<String>();
    out.nwifi++;
  }
  for (JsonObjectConst t : doc["t"].as<JsonArrayConst>()) {
    if (out.ntenant >= MAX_TENANTS) break;
    out.tenant[out.ntenant].url = t["u"].as<String>();
    out.tenant[out.ntenant].token = t["k"].as<String>();
    out.ntenant++;
  }
  out.wifiSel = doc["ws"] | -1;
  out.tenantSel = doc["ts"] | -1;
  if (out.wifiSel >= out.nwifi) out.wifiSel = out.nwifi ? 0 : -1;
  if (out.tenantSel >= out.ntenant) out.tenantSel = out.ntenant ? 0 : -1;
}

static String toJson(const Settings &s) {
  JsonDocument doc;
  JsonArray w = doc["w"].to<JsonArray>();
  for (int i = 0; i < s.nwifi; i++) {
    JsonObject o = w.add<JsonObject>();
    o["s"] = s.wifi[i].ssid;
    o["p"] = s.wifi[i].pass;
  }
  JsonArray t = doc["t"].to<JsonArray>();
  for (int i = 0; i < s.ntenant; i++) {
    JsonObject o = t.add<JsonObject>();
    o["u"] = s.tenant[i].url;
    o["k"] = s.tenant[i].token;
  }
  doc["ws"] = s.wifiSel;
  doc["ts"] = s.tenantSel;
  String out;
  serializeJson(doc, out);
  return out;
}

void settingsSave(const Settings &s) {
  Preferences p;
  p.begin(NS, false);
  p.putString(KEY, toJson(s));
  p.end();
}

void settingsLoad(Settings &out) {
  out = Settings{};
  Preferences p;
  p.begin(NS, true);
  String blob = p.getString(KEY, "");
  // Fields from the pre-list layout, for one-time migration.
  String oldSsid = p.getString("ssid", ""), oldPass = p.getString("pass", "");
  String oldTenant = p.getString("tenant", ""), oldToken = p.getString("token", "");
  p.end();

  if (blob.length()) {
    fromJson(blob, out);
    return;
  }

  // Migrate an old single-config install into one selected entry each, then
  // persist in the new format and drop the legacy keys.
  bool migrated = false;
  if (oldSsid.length()) {
    out.wifi[0] = {oldSsid, oldPass};
    out.nwifi = 1;
    out.wifiSel = 0;
    migrated = true;
  }
  if (oldTenant.length() && oldToken.length()) {
    out.tenant[0] = {oldTenant, oldToken};
    out.ntenant = 1;
    out.tenantSel = 0;
    migrated = true;
  }
  if (migrated) {
    settingsSave(out);
    Preferences q;
    q.begin(NS, false);
    q.remove("ssid");
    q.remove("pass");
    q.remove("tenant");
    q.remove("token");
    q.end();
  }
}

void settingsReset() {
  settingsSave(Settings{});
  Preferences p;
  p.begin(NS, false);
  p.remove("ssid");
  p.remove("pass");
  p.remove("tenant");
  p.remove("token");
  p.end();
}

Config settingsActive(const Settings &s) {
  Config c;
  if (s.wifiReady()) {
    c.ssid = s.wifi[s.wifiSel].ssid;
    c.pass = s.wifi[s.wifiSel].pass;
  }
  if (s.tenantReady()) {
    c.tenant = s.tenant[s.tenantSel].url;
    c.token = s.tenant[s.tenantSel].token;
  }
  return c;
}

bool settingsAddWifi(Settings &s, const String &ssid, const String &pass) {
  for (int i = 0; i < s.nwifi; i++)  // known SSID → update password, reselect
    if (s.wifi[i].ssid == ssid) {
      s.wifi[i].pass = pass;
      s.wifiSel = i;
      return true;
    }
  if (s.nwifi >= MAX_WIFI) return false;
  s.wifi[s.nwifi] = {ssid, pass};
  s.wifiSel = s.nwifi;
  s.nwifi++;
  return true;
}

bool settingsAddTenant(Settings &s, const String &url, const String &token) {
  for (int i = 0; i < s.ntenant; i++)  // known URL → update token, reselect
    if (s.tenant[i].url == url) {
      s.tenant[i].token = token;
      s.tenantSel = i;
      return true;
    }
  if (s.ntenant >= MAX_TENANTS) return false;
  s.tenant[s.ntenant] = {url, token};
  s.tenantSel = s.ntenant;
  s.ntenant++;
  return true;
}

// ------------------------------------------------------------- captive portal

static WebServer server(80);
static DNSServer dns;
static Settings g_set;  // the working copy the portal edits (persisted per change)

// Minimal HTML-escape for values echoed into the page (SSID/URL may carry & " < >).
static String esc(const String &s) {
  String o;
  o.reserve(s.length() + 8);
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    if (c == '&') o += "&amp;";
    else if (c == '"') o += "&quot;";
    else if (c == '<') o += "&lt;";
    else if (c == '>') o += "&gt;";
    else o += c;
  }
  return o;
}

static void redirectHome() {
  server.sendHeader("Location", "/");
  server.send(303, "text/plain", "");
}

// Render the whole management page from g_set: WiFi networks + tenants, each
// with a selectable known-list, a reset, and an add form; then Apply.
static void handleRoot() {
  String h = F(
      "<!doctype html><html><head>"
      "<meta name=viewport content=\"width=device-width,initial-scale=1\">"
      "<title>dynaglance setup</title><style>"
      "body{font-family:system-ui,-apple-system,sans-serif;background:#0d1017;color:#e8eaed;margin:0;padding:20px}"
      ".card{max-width:460px;margin:0 auto}"
      "h1{font-size:20px;margin:0 0 2px}p.sub{color:#8b93a1;margin:0 0 14px;font-size:13px}"
      "h2{font-size:15px;color:#2ee6ff;margin:22px 0 6px;border-bottom:1px solid #1e2634;padding-bottom:4px}"
      "h3{font-size:12px;color:#8b93a1;margin:14px 0 6px;font-weight:600}"
      "label{display:block;margin:10px 0 4px;font-size:12px;color:#b9c0cc}"
      "input{width:100%;box-sizing:border-box;padding:9px;border-radius:8px;border:1px solid #2a3240;background:#151a23;color:#fff;font-size:15px}"
      ".row{display:flex;align-items:center;gap:8px;padding:7px 9px;border:1px solid #222b3a;border-radius:8px;margin:5px 0;background:#121722}"
      ".row .n{flex:1;font-size:13px;word-break:break-all}"
      ".row.sel{border-color:#2effa8;background:#0f1a17}"
      ".tag{font-size:11px;color:#2effa8}"
      "form.inline{margin:0}"
      "button{padding:8px 12px;border:0;border-radius:8px;background:#2f81f7;color:#fff;font-size:13px;font-weight:600;cursor:pointer}"
      "button.small{background:#243043}button.warn{background:#5a2330}button.use{background:#1e6a4c}"
      "button.apply{width:100%;margin:26px 0 4px;padding:14px;font-size:16px;background:#2effa8;color:#052}"
      ".addbox{border:1px dashed #2a3240;border-radius:8px;padding:4px 12px 12px;margin-top:8px}"
      "small{color:#6b7280}.empty{color:#6b7280;font-size:12px;margin:6px 0}"
      "</style></head><body><div class=card>"
      "<h1>dynaglance</h1><p class=sub>configuration portal</p>");

  // ---- WiFi ----
  h += F("<h2>WiFi</h2><h3>Known networks</h3>");
  if (g_set.nwifi == 0) {
    h += F("<p class=empty>none — add one below.</p>");
  } else {
    for (int i = 0; i < g_set.nwifi; i++) {
      bool sel = i == g_set.wifiSel;
      h += F("<div class=\"row");
      if (sel) h += F(" sel");
      h += F("\"><span class=n>");
      h += esc(g_set.wifi[i].ssid);
      h += F("</span>");
      if (sel) h += F("<span class=tag>&#9733; in use</span>");
      else {
        h += F("<form class=inline method=POST action=/wifi/select><input type=hidden name=i value=");
        h += i;
        h += F("><button class=use>Use</button></form>");
      }
      h += F("</div>");
    }
    h += F("<form class=inline method=POST action=/wifi/reset>"
           "<button class=warn>reset all connections</button></form>");
  }
  h += F("<div class=addbox><h3>Add a new connection</h3>"
         "<form method=POST action=/wifi/add>"
         "<label>WiFi network</label><input name=ssid required>"
         "<label>WiFi password</label><input name=pass type=password>"
         "<button class=small style=\"margin-top:12px\">add network</button></form></div>");

  // ---- Dynatrace ----
  h += F("<h2>Dynatrace Setup</h2><h3>Known tenants</h3>");
  if (g_set.ntenant == 0) {
    h += F("<p class=empty>none — add one below.</p>");
  } else {
    for (int i = 0; i < g_set.ntenant; i++) {
      bool sel = i == g_set.tenantSel;
      h += F("<div class=\"row");
      if (sel) h += F(" sel");
      h += F("\"><span class=n>");
      h += esc(g_set.tenant[i].url);
      h += F("</span>");
      if (sel) h += F("<span class=tag>&#9733; in use</span>");
      else {
        h += F("<form class=inline method=POST action=/tenant/select><input type=hidden name=i value=");
        h += i;
        h += F("><button class=use>Use</button></form>");
      }
      h += F("</div>");
    }
    h += F("<form class=inline method=POST action=/tenant/reset>"
           "<button class=warn>reset all tenant connections</button></form>");
  }
  h += F("<div class=addbox><h3>Add a new tenant</h3>"
         "<form method=POST action=/tenant/add>"
         "<label>Dynatrace tenant URL</label>"
         "<input name=url placeholder=\"https://abc12345.apps.dynatrace.com\" required>"
         "<label>Platform token (dt0s16&hellip;)</label><input name=token type=password required>"
         "<button class=small style=\"margin-top:12px\">add tenant</button></form></div>");

  h += F("<form method=POST action=/apply><button class=apply>APPLY &amp; RESTART</button></form>"
         "<p><small>Stored on the device only. Never leaves it.</small></p>"
         "</form></div></body></html>");
  server.send(200, "text/html", h);
}

static void handleWifiAdd() {
  String ssid = server.arg("ssid");
  ssid.trim();
  if (ssid.length()) {
    settingsAddWifi(g_set, ssid, server.arg("pass"));
    settingsSave(g_set);
  }
  redirectHome();
}
static void handleWifiSelect() {
  int i = server.arg("i").toInt();
  if (i >= 0 && i < g_set.nwifi) {
    g_set.wifiSel = i;
    settingsSave(g_set);
  }
  redirectHome();
}
static void handleWifiReset() {
  g_set.nwifi = 0;
  g_set.wifiSel = -1;
  settingsSave(g_set);
  redirectHome();
}
static void handleTenantAdd() {
  String url = server.arg("url");
  url.trim();
  while (url.endsWith("/")) url.remove(url.length() - 1);
  String token = server.arg("token");
  token.trim();
  if (url.length() && token.length()) {
    settingsAddTenant(g_set, url, token);
    settingsSave(g_set);
  }
  redirectHome();
}
static void handleTenantSelect() {
  int i = server.arg("i").toInt();
  if (i >= 0 && i < g_set.ntenant) {
    g_set.tenantSel = i;
    settingsSave(g_set);
  }
  redirectHome();
}
static void handleTenantReset() {
  g_set.ntenant = 0;
  g_set.tenantSel = -1;
  settingsSave(g_set);
  redirectHome();
}
static void handleApply() {
  settingsSave(g_set);
  server.send(200, "text/html",
              "<meta name=viewport content='width=device-width,initial-scale=1'>"
              "<meta http-equiv=refresh content='4'>"
              "<body style='font-family:sans-serif;background:#0d1017;color:#e8eaed;padding:24px'>"
              "Applying &amp; restarting&hellip; you can close this and disconnect.</body>");
  delay(700);
  ESP.restart();
}

void runPortal() {
  settingsLoad(g_set);

  WiFi.mode(WIFI_AP);
  WiFi.softAP("tinytrace-setup");
  IPAddress ip = WiFi.softAPIP();  // 192.168.4.1

  // Catch-all DNS so phones pop the "sign in to network" captive page.
  dns.start(53, "*", ip);

  server.on("/", handleRoot);
  server.on("/wifi/add", HTTP_POST, handleWifiAdd);
  server.on("/wifi/select", HTTP_POST, handleWifiSelect);
  server.on("/wifi/reset", HTTP_POST, handleWifiReset);
  server.on("/tenant/add", HTTP_POST, handleTenantAdd);
  server.on("/tenant/select", HTTP_POST, handleTenantSelect);
  server.on("/tenant/reset", HTTP_POST, handleTenantReset);
  server.on("/apply", HTTP_POST, handleApply);
  server.onNotFound(handleRoot);  // any other path → the page (captive behaviour)
  server.begin();

  for (;;) {
    dns.processNextRequest();
    server.handleClient();
    delay(5);
  }
}
