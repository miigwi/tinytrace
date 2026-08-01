#include "dt.h"

#include <HTTPClient.h>
#include <WiFiClientSecure.h>

#include "certs.h"

static const char *EXEC_PATH = "/platform/storage/query/v1/query:execute";
static const char *POLL_PATH = "/platform/storage/query/v1/query:poll";
// Seconds the server holds the request open before replying — small queries
// then usually come back SUCCEEDED in the first call, no polling.
static const int LONGPOLL_S = 25;

// One shared TLS client: the mbedTLS context is heavy, so we configure it once
// and reuse it (each request re-handshakes after the previous end()).
static WiFiClientSecure client;
static bool clientReady = false;

static WiFiClientSecure &secureClient() {
  if (!clientReady) {
#ifdef DG_TLS_INSECURE
    client.setInsecure();  // BRING-UP ONLY — no server auth; harden before ship
#else
    client.setCACert(DG_ROOT_CAS);
#endif
    clientReady = true;
  }
  return client;
}

static String urlEncode(const String &s) {
  String o;
  char buf[4];
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    if (isalnum((unsigned char)c) || c == '-' || c == '_' || c == '.' || c == '~') {
      o += c;
    } else {
      sprintf(buf, "%%%02X", (unsigned char)c);
      o += buf;
    }
  }
  return o;
}

// hasRecords is the success signal: a result object carrying a records array.
static bool hasRecords(JsonDocument &doc) {
  return doc["result"]["records"].is<JsonArray>();
}

bool dqlQuery(const Config &cfg, const String &dql, JsonDocument &doc) {
  HTTPClient http;
  http.setTimeout((LONGPOLL_S + 10) * 1000);

  String url = cfg.tenant + EXEC_PATH + "?request-timeout=" + LONGPOLL_S;
  if (!http.begin(secureClient(), url)) {
    Serial.println("[dt] begin failed");
    return false;
  }
  http.addHeader("Authorization", String("Bearer ") + cfg.token);
  http.addHeader("Content-Type", "application/json");

  JsonDocument req;
  req["query"] = dql;
  String body;
  serializeJson(req, body);

  int code = http.POST(body);
  if (code != 200 && code != 202) {
    Serial.printf("[dt] execute -> %d\n", code);
    Serial.println(http.getString().substring(0, 240));
    http.end();
    return false;
  }
  // Grail result JSON nests deeper than ArduinoJson's default limit of 10.
  DeserializationError e =
      deserializeJson(doc, http.getStream(), DeserializationOption::NestingLimit(50));
  http.end();
  if (e) {
    Serial.printf("[dt] parse: %s\n", e.c_str());
    return false;
  }
  if (hasRecords(doc)) return true;

  // Async: grab the request token and poll until the result is ready.
  String token = doc["requestToken"] | "";
  if (token.isEmpty()) {
    Serial.printf("[dt] no result, state=%s\n", (const char *)(doc["state"] | "?"));
    return false;
  }

  for (int i = 0; i < 12; i++) {
    HTTPClient ph;
    ph.setTimeout((LONGPOLL_S + 10) * 1000);
    String purl =
        cfg.tenant + POLL_PATH + "?request-token=" + urlEncode(token) + "&request-timeout=" + LONGPOLL_S;
    if (!ph.begin(secureClient(), purl)) return false;
    ph.addHeader("Authorization", String("Bearer ") + cfg.token);

    int pc = ph.GET();
    if (pc != 200 && pc != 202) {
      Serial.printf("[dt] poll -> %d\n", pc);
      ph.end();
      return false;
    }
    doc.clear();
    DeserializationError pe =
        deserializeJson(doc, ph.getStream(), DeserializationOption::NestingLimit(50));
    ph.end();
    if (pe) {
      Serial.printf("[dt] poll parse: %s\n", pe.c_str());
      return false;
    }
    if (hasRecords(doc)) return true;

    const char *state = doc["state"] | "";
    if (strcmp(state, "RUNNING") != 0 && strcmp(state, "NOT_STARTED") != 0) {
      Serial.printf("[dt] poll ended state=%s\n", state);
      return false;
    }
    delay(400);
  }
  Serial.println("[dt] poll timed out");
  return false;
}
