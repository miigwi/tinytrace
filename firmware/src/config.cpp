#include "config.h"

#include <DNSServer.h>
#include <Preferences.h>
#include <WebServer.h>
#include <WiFi.h>

static const char *NS = "dynaglance";  // NVS namespace

bool configLoad(Config &out) {
  Preferences p;
  p.begin(NS, true);  // read-only
  out.ssid = p.getString("ssid", "");
  out.pass = p.getString("pass", "");
  out.tenant = p.getString("tenant", "");
  out.token = p.getString("token", "");
  p.end();
  return out.complete();
}

void configSave(const Config &c) {
  Preferences p;
  p.begin(NS, false);
  p.putString("ssid", c.ssid);
  p.putString("pass", c.pass);
  p.putString("tenant", c.tenant);
  p.putString("token", c.token);
  p.end();
}

void configClear() {
  Preferences p;
  p.begin(NS, false);
  p.clear();
  p.end();
}

// ------------------------------------------------------------- captive portal

static WebServer server(80);
static DNSServer dns;
static PortalScope g_scope = PORTAL_ALL;  // which fields this portal session edits

// Minimal HTML-escape for prefilled values (SSID/tenant may carry & " < >).
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

// Build the form for the active scope. Non-secret fields are prefilled from NVS;
// password/token render blank with a "leave blank to keep" hint. A small dark
// form matching the panel's aesthetic; secrets are type=password.
static void handleRoot() {
  Config cur;
  configLoad(cur);  // may be incomplete; we only read what's there

  bool wifi = g_scope == PORTAL_ALL || g_scope == PORTAL_WIFI;
  bool dt = g_scope == PORTAL_ALL || g_scope == PORTAL_DT;
  const char *sub = g_scope == PORTAL_WIFI  ? "WiFi settings"
                    : g_scope == PORTAL_DT  ? "Dynatrace connection"
                                            : "Feather setup";
  bool haveWifi = cur.ssid.length(), haveTok = cur.token.length();

  String h = F("<!doctype html><html><head>"
               "<meta name=viewport content=\"width=device-width,initial-scale=1\">"
               "<title>dynaglance setup</title><style>"
               "body{font-family:system-ui,-apple-system,sans-serif;background:#0d1017;color:#e8eaed;margin:0;padding:24px}"
               ".card{max-width:420px;margin:0 auto}"
               "h1{font-size:20px;margin:0 0 2px}p.sub{color:#8b93a1;margin:0 0 18px;font-size:14px}"
               "label{display:block;margin:14px 0 4px;font-size:13px;color:#b9c0cc}"
               "input{width:100%;box-sizing:border-box;padding:10px;border-radius:8px;border:1px solid #2a3240;background:#151a23;color:#fff;font-size:15px}"
               "button{margin-top:22px;width:100%;padding:12px;border:0;border-radius:8px;background:#2f81f7;color:#fff;font-size:16px;font-weight:600}"
               "small{color:#6b7280}"
               "</style></head><body><div class=card>"
               "<h1>dynaglance</h1><p class=sub>");
  h += sub;
  h += F("</p><form method=POST action=/save>");
  if (wifi) {
    h += F("<label>WiFi network</label><input name=ssid required value=\"");
    h += esc(cur.ssid);
    h += F("\"><label>WiFi password</label><input name=pass type=password");
    if (haveWifi) h += F(" placeholder=\"leave blank to keep current\"");
    h += F(">");
  }
  if (dt) {
    h += F("<label>Dynatrace tenant URL</label>"
           "<input name=tenant placeholder=\"https://abc12345.apps.dynatrace.com\" required value=\"");
    h += esc(cur.tenant);
    h += F("\"><label>Platform token (dt0s16&hellip;)</label><input name=token type=password");
    if (haveTok) h += F(" placeholder=\"leave blank to keep current\"");
    else h += F(" required");
    h += F(">");
  }
  h += F("<button type=submit>Save &amp; reboot</button>"
         "<p><small>Stored on the device only. Never leaves it.</small></p>"
         "</form></div></body></html>");
  server.send(200, "text/html", h);
}

static void handleSave() {
  Config c;
  configLoad(c);  // start from the stored config; merge the scoped fields over it

  bool wifi = g_scope == PORTAL_ALL || g_scope == PORTAL_WIFI;
  bool dt = g_scope == PORTAL_ALL || g_scope == PORTAL_DT;

  if (wifi) {
    String ssid = server.arg("ssid");
    ssid.trim();
    c.ssid = ssid;
    String pass = server.arg("pass");  // blank keeps the stored password
    if (pass.length()) c.pass = pass;
  }
  if (dt) {
    String tenant = server.arg("tenant");
    tenant.trim();
    while (tenant.endsWith("/")) tenant.remove(tenant.length() - 1);
    c.tenant = tenant;
    String token = server.arg("token");  // blank keeps the stored token
    token.trim();
    if (token.length()) c.token = token;
  }

  // Validate only what this scope is responsible for; the other half is
  // preserved as-is (and may legitimately still be empty).
  bool ok = true;
  if (wifi && !c.ssid.length()) ok = false;
  if (dt && (!c.tenant.length() || !c.token.length())) ok = false;
  if (!ok) {
    server.send(400, "text/plain", "missing required fields");
    return;
  }

  configSave(c);
  server.send(200, "text/html",
              "<meta name=viewport content='width=device-width,initial-scale=1'>"
              "<body style='font-family:sans-serif;background:#0d1017;color:#e8eaed;padding:24px'>"
              "Saved. Rebooting&hellip;</body>");
  delay(700);
  ESP.restart();
}

void runPortal(PortalScope scope) {
  g_scope = scope;
  WiFi.mode(WIFI_AP);
  WiFi.softAP("dynaglance-setup");
  IPAddress ip = WiFi.softAPIP();  // 192.168.4.1

  // Catch-all DNS so phones pop the "sign in to network" captive page.
  dns.start(53, "*", ip);

  server.on("/", handleRoot);
  server.on("/save", HTTP_POST, handleSave);
  server.onNotFound(handleRoot);  // any other path → the form (captive behaviour)
  server.begin();

  for (;;) {
    dns.processNextRequest();
    server.handleClient();
    delay(5);
  }
}
