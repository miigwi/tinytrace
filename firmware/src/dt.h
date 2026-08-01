// Dynatrace client: runs DQL against the tenant's Grail query API over HTTPS,
// authenticated with the read-only platform token. This is the whole reason the
// Feather is standalone — no sidecar. Path A: one token, all-DQL.
//
//   POST {tenant}/platform/storage/query/v1/query:execute   (long-poll)
//   GET  {tenant}/platform/storage/query/v1/query:poll?request-token=…
//
// Records land at doc["result"]["records"]; the screen builders (dt_screens)
// walk those.
#pragma once

#include <ArduinoJson.h>

#include "config.h"

// dqlQuery executes a DQL statement and parses the response into doc. Returns
// true on a SUCCEEDED query (even with zero records). doc must outlive the use
// of the records it holds.
bool dqlQuery(const Config &cfg, const String &dql, JsonDocument &doc);
