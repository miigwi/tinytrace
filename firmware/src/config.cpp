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

// A small dark form matching the panel's aesthetic. The token/password fields
// are type=password so they aren't shoulder-surfed off the setup phone.
static const char PORTAL_HTML[] PROGMEM = R"HTML(<!doctype html><html><head>
<meta name=viewport content="width=device-width,initial-scale=1">
<title>dynaglance setup</title><style>
body{font-family:system-ui,-apple-system,sans-serif;background:#0d1017;color:#e8eaed;margin:0;padding:24px}
.card{max-width:420px;margin:0 auto}
h1{font-size:20px;margin:0 0 2px}p.sub{color:#8b93a1;margin:0 0 18px;font-size:14px}
label{display:block;margin:14px 0 4px;font-size:13px;color:#b9c0cc}
input{width:100%;box-sizing:border-box;padding:10px;border-radius:8px;border:1px solid #2a3240;background:#151a23;color:#fff;font-size:15px}
button{margin-top:22px;width:100%;padding:12px;border:0;border-radius:8px;background:#2f81f7;color:#fff;font-size:16px;font-weight:600}
small{color:#6b7280}
</style></head><body><div class=card>
<h1>dynaglance</h1><p class=sub>Feather setup</p>
<form method=POST action=/save>
<label>WiFi network</label><input name=ssid required>
<label>WiFi password</label><input name=pass type=password>
<label>Dynatrace tenant URL</label><input name=tenant placeholder="https://abc12345.apps.dynatrace.com" required>
<label>Platform token (dt0s16&hellip;)</label><input name=token type=password required>
<button type=submit>Save &amp; reboot</button>
<p><small>Stored on the device only. Never leaves it.</small></p>
</form></div></body></html>)HTML";

static void handleRoot() { server.send_P(200, "text/html", PORTAL_HTML); }

static void handleSave() {
  Config c;
  c.ssid = server.arg("ssid");
  c.pass = server.arg("pass");
  c.tenant = server.arg("tenant");
  c.token = server.arg("token");

  c.ssid.trim();
  c.tenant.trim();
  c.token.trim();
  while (c.tenant.endsWith("/")) c.tenant.remove(c.tenant.length() - 1);

  if (!c.complete()) {
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

void runPortal() {
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
