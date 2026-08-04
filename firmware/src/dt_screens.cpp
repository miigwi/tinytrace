#include "dt_screens.h"

#include <math.h>
#include <time.h>

#include "dt.h"

// ------------------------------------------------------------------- helpers

// timegm isn't in ESP32's newlib, so convert a UTC struct tm to epoch seconds
// with the standard days-from-civil algorithm (Howard Hinnant, public domain).
static long daysFromCivil(int y, unsigned m, unsigned d) {
  y -= m <= 2;
  long era = (y >= 0 ? y : y - 399) / 400;
  unsigned yoe = (unsigned)(y - era * 400);
  unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097L + (long)doe - 719468;
}
static time_t timegmPortable(const struct tm *tm) {
  long days = daysFromCivil(tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday);
  return (time_t)(days * 86400L + tm->tm_hour * 3600L + tm->tm_min * 60L + tm->tm_sec);
}

// nowHHMM formats the current wall clock as "15:06" in local time (TT_TZ).
static String nowHHMM() {
  time_t t = time(nullptr);
  struct tm tm;
  localtime_r(&t, &tm);
  char buf[8];
  snprintf(buf, sizeof(buf), "%02d:%02d", tm.tm_hour, tm.tm_min);
  return String(buf);
}

// parseTs turns a Grail timestamp (ISO-8601 UTC string, or epoch millis/seconds)
// into epoch seconds. Grail times carry 'Z' (UTC), so we use timegm — NOT mktime,
// which would misread them through the local TZ.
static time_t parseTs(JsonVariantConst v) {
  if (v.is<const char *>()) {
    const char *s = v.as<const char *>();
    if (!s) return 0;
    struct tm tm = {};
    int Y, M, D, h, mi, se;
    if (sscanf(s, "%d-%d-%dT%d:%d:%d", &Y, &M, &D, &h, &mi, &se) == 6) {
      tm.tm_year = Y - 1900;
      tm.tm_mon = M - 1;
      tm.tm_mday = D;
      tm.tm_hour = h;
      tm.tm_min = mi;
      tm.tm_sec = se;
      return timegmPortable(&tm);
    }
    return 0;
  }
  if (v.is<long long>()) {
    long long n = v.as<long long>();
    return (time_t)(n > 1000000000000LL ? n / 1000 : n);  // millis vs seconds
  }
  return 0;
}

// hhmm renders a timestamp as local "15:06".
static String hhmm(time_t t) {
  struct tm tm;
  localtime_r(&t, &tm);
  char buf[8];
  snprintf(buf, sizeof(buf), "%02d:%02d", tm.tm_hour, tm.tm_min);
  return String(buf);
}

// firstStr returns a string field, or the first element of an array field (the
// timeseries splitBy dimension comes back as either).
static String firstStr(JsonVariantConst v) {
  if (v.is<JsonArrayConst>()) {
    for (JsonVariantConst e : v.as<JsonArrayConst>()) return e.as<String>();
    return String();
  }
  return v.as<String>();
}

// age renders "12m" / "3h" / "1d" from a start timestamp.
static String age(time_t start) {
  if (start <= 0) return "";
  long d = (long)(time(nullptr) - start);
  if (d < 0) d = 0;
  if (d < 3600) return String(d / 60) + "m";
  if (d < 86400) return String(d / 3600) + "h";
  return String(d / 86400) + "d";
}

// clip bounds a string to a character count (the renderer clips to pixels too;
// this just keeps payloads sane).
static String clip(String s, int maxChars) {
  s.replace("\n", " ");
  s.replace("\r", " ");
  if ((int)s.length() > maxChars) {
    s.remove(maxChars - 1);
    s += "\x7F";  // will just be clipped by the renderer; keep ASCII
    s.remove(s.length() - 1);
  }
  return s;
}

// joinArr renders an array field (e.g. affected_entity_names) as "a, b", or a
// plain string field as-is.
static String joinArr(JsonVariantConst v) {
  if (v.is<JsonArrayConst>()) {
    String o;
    int c = 0;
    for (JsonVariantConst e : v.as<JsonArrayConst>()) {
      if (c++) o += ", ";
      o += e.as<String>();
      if (c >= 2) break;
    }
    return o;
  }
  return v.as<String>();
}

// toDoubles copies a numeric JSON array into a C array; returns the count.
static int toDoubles(JsonArrayConst a, double *out, int cap) {
  int n = 0;
  for (JsonVariantConst v : a) {
    if (n >= cap) break;
    out[n++] = v.isNull() ? 0.0 : v.as<double>();
  }
  return n;
}

// normSpark maps a series into the 0..100 range the renderer expects, using the
// series' own min/max so the shape fills the box.
static void normSpark(const double *v, int n, uint8_t *out) {
  if (n <= 0) return;
  double mn = v[0], mx = v[0];
  for (int i = 1; i < n; i++) {
    if (v[i] < mn) mn = v[i];
    if (v[i] > mx) mx = v[i];
  }
  double range = mx - mn;
  for (int i = 0; i < n; i++) {
    out[i] = range > 0 ? (uint8_t)(10 + (v[i] - mn) / range * 80) : 50;
  }
}

// fmtNum renders a value compactly: 12.3k, 128, 3.1.
static String fmtNum(double x) {
  if (x >= 10000) return String(x / 1000.0, 1) + "k";
  if (x >= 100) return String((long)llround(x));
  return String(x, 1);
}

static int8_t trendOf(double first, double last) {
  if (last > first * 1.05) return 1;
  if (last < first * 0.95) return -1;
  return 0;
}

// -------------------------------------------------------------------- screens

// buildProblems: the DQL is copied verbatim from screens.go. Records carry
// {display_id, status, name, start, affected}.
bool buildProblems(const Config &cfg, Screen &out) {
  static const char *DQL =
      "fetch dt.davis.problems, from:now() - 24h "
      "| filter not(dt.davis.is_duplicate) "
      "| sort timestamp asc "
      "| summarize { status = takeLast(event.status), name = takeLast(event.name), "
      "start = takeLast(event.start), affected = takeLast(affected_entity_names) }, by:{display_id} "
      "| filter status == \"ACTIVE\" "
      "| sort start desc "
      "| limit 8";

  JsonDocument doc;
  if (!dqlQuery(cfg, DQL, doc)) return false;
  JsonArrayConst recs = doc["result"]["records"].as<JsonArrayConst>();

  Screen s;
  s.id = "problems";
  s.title = "active problems";
  s.kind = KIND_LIST;
  s.ts = nowHHMM();
  s.env = cfg.tenant;

  int i = 0;
  for (JsonObjectConst r : recs) {
    if (i >= TT_MAX_ROWS) break;
    String name = r["name"].as<String>();
    String aff = joinArr(r["affected"]);
    if (aff.length()) name += " - " + aff;
    s.rows[i].l = r["display_id"].as<String>();
    s.rows[i].m = clip(name, 40);
    s.rows[i].r = age(parseTs(r["start"]));
    s.rows[i].sev = SEV_ERROR;
    i++;
  }
  s.nrows = i;
  s.count = recs.size();
  s.sev = i > 0 ? SEV_ERROR : SEV_OK;
  s.mascot = i > 0 ? MASCOT_ALARM : MASCOT_IDLE;
  s.valid = true;
  out = s;
  return true;
}

// buildLogs: recent error/warn log lines.
bool buildLogs(const Config &cfg, Screen &out) {
  static const char *DQL =
      "fetch logs, from:now() - 1h "
      "| filter loglevel == \"ERROR\" or loglevel == \"WARN\" "
      "| sort timestamp desc "
      "| limit 8 "
      "| fields timestamp, loglevel, content";

  JsonDocument doc;
  if (!dqlQuery(cfg, DQL, doc)) return false;
  JsonArrayConst recs = doc["result"]["records"].as<JsonArrayConst>();

  Screen s;
  s.id = "logs";
  s.title = "last logs";
  s.kind = KIND_LIST;
  s.ts = nowHHMM();
  s.env = cfg.tenant;

  int i = 0;
  Sev worst = SEV_OK;
  for (JsonObjectConst r : recs) {
    if (i >= TT_MAX_ROWS) break;
    String lvl = r["loglevel"].as<String>();
    Sev sev = lvl == "ERROR" ? SEV_ERROR : SEV_WARN;
    if (sev > worst) worst = sev;
    s.rows[i].l = lvl == "ERROR" ? "ERR" : (lvl == "WARN" ? "WRN" : lvl.substring(0, 3));
    s.rows[i].m = clip(r["content"].as<String>(), 40);
    s.rows[i].r = hhmm(parseTs(r["timestamp"]));
    s.rows[i].sev = sev;
    i++;
  }
  s.nrows = i;
  s.count = recs.size();
  s.sev = worst;
  s.mascot = worst >= SEV_WARN ? MASCOT_ALARM : MASCOT_IDLE;
  s.valid = true;
  out = s;
  return true;
}

// signalRow fills one golden-signal row from a numeric series.
static void signalRow(Row &row, const char *label, const char *unit, const double *v, int n,
                      Sev sev) {
  row.l = label;
  double last = n > 0 ? v[n - 1] : 0;
  double first = n > 0 ? v[0] : 0;
  row.m = fmtNum(last) + (unit && *unit ? String(" ") + unit : String());
  row.sev = sev;
  normSpark(v, n, row.spark);
  row.nspark = n > TT_SPARK_MAX ? TT_SPARK_MAX : n;
  row.trend = trendOf(first, last);
}

// buildGolden picks the worst service (most failures over 2h) and shows its
// golden signals. Header is just the service name. Signals:
//   LAT  p99 response time (ms)      TPS  request rate (per 1m)
//   ERR  failure rate (%)            4xx  4xx-response rate (per 1m, 2nd query)
//
// ⚠ Metric keys (dt.service.request.*), the response-time unit, and the 4xx
// filter fields (http.response.status_code) are the "validate against the
// tenant" items; adjust here if a query 400s or the numbers look off. The 4xx
// query is best-effort — if it fails, the screen shows the other three signals.
bool buildGolden(const Config &cfg, Screen &out) {
  static const char *PICK =
      "timeseries { "
      "lat = percentile(dt.service.request.response_time, 99), "
      "tps = sum(dt.service.request.count, rate: 1m), "
      "reqs = sum(dt.service.request.count), "
      "fails = sum(dt.service.request.failure_count) "
      "}, by:{dt.entity.service}, from:now()-2h, interval:10m "
      "| fieldsAdd failtotal = arraySum(fails) "
      "| sort failtotal desc "
      "| limit 1 "
      "| fieldsAdd svc = entityName(dt.entity.service), sid = dt.entity.service";

  JsonDocument doc;
  if (!dqlQuery(cfg, PICK, doc)) return false;
  JsonArrayConst recs = doc["result"]["records"].as<JsonArrayConst>();

  Screen s;
  s.id = "golden";
  s.kind = KIND_SIGNALS;
  s.ts = nowHHMM();
  s.env = cfg.tenant;
  s.title = "golden signals";

  if (recs.size() == 0) {
    s.nrows = 0;
    s.sev = SEV_OK;
    s.mascot = MASCOT_IDLE;
    s.valid = true;
    out = s;
    return true;
  }

  JsonObjectConst r = recs[0];
  String svc = firstStr(r["svc"]);
  String sid = firstStr(r["sid"]);
  s.title = svc.isEmpty() ? "service" : svc;  // service name only

  double lat[TT_SPARK_MAX], tps[TT_SPARK_MAX], reqs[TT_SPARK_MAX], fails[TT_SPARK_MAX],
      rate[TT_SPARK_MAX];
  int nl = toDoubles(r["lat"].as<JsonArrayConst>(), lat, TT_SPARK_MAX);
  int np = toDoubles(r["tps"].as<JsonArrayConst>(), tps, TT_SPARK_MAX);
  int nq = toDoubles(r["reqs"].as<JsonArrayConst>(), reqs, TT_SPARK_MAX);
  int nf = toDoubles(r["fails"].as<JsonArrayConst>(), fails, TT_SPARK_MAX);

  for (int i = 0; i < nl; i++) lat[i] /= 1000.0;  // µs → ms
  int nr = nq < nf ? nq : nf;
  for (int i = 0; i < nr; i++) rate[i] = reqs[i] > 0 ? (fails[i] / reqs[i]) * 100.0 : 0.0;

  double lastRate = nr > 0 ? rate[nr - 1] : 0.0;
  Sev errSev = lastRate >= 5.0 ? SEV_ERROR : (lastRate >= 1.0 ? SEV_WARN : SEV_OK);

  signalRow(s.rows[0], "LAT", "ms", lat, nl, SEV_OK);
  signalRow(s.rows[1], "TPS", "", tps, np, SEV_OK);
  signalRow(s.rows[2], "ERR", "%", rate, nr, errSev);
  s.rows[2].m = String(lastRate, 1) + " %";  // reads better than fmtNum
  int nrows = 3;

  // 4xx rate for the chosen service — best effort, second query.
  if (!sid.isEmpty()) {
    String q4 = String(
                    "timeseries fourxx = sum(dt.service.request.count, default: 0, rate: 1m), "
                    "nonempty: true, filter: { (dt.entity.service == \"") +
                sid + "\" or dt.smartscape.service == toSmartscapeId(\"" + sid +
                "\")) and http.response.status_code >= 400 and http.response.status_code <= 499 }, "
                "from:now()-2h, interval:10m";
    JsonDocument d4;
    if (dqlQuery(cfg, q4, d4)) {
      JsonArrayConst r4 = d4["result"]["records"].as<JsonArrayConst>();
      if (r4.size() > 0) {
        double xx[TT_SPARK_MAX];
        int nx = toDoubles(r4[0]["fourxx"].as<JsonArrayConst>(), xx, TT_SPARK_MAX);
        if (nx > 0) {
          signalRow(s.rows[3], "4xx", "", xx, nx, SEV_INFO);
          nrows = 4;
        }
      }
    }
  }

  s.nrows = nrows;
  s.sev = errSev;
  s.mascot = errSev >= SEV_WARN ? MASCOT_ALARM : MASCOT_IDLE;
  s.valid = true;
  out = s;
  return true;
}

Screen buildIdle(int problemCount, const String &tenant) {
  Screen s;
  s.id = "idle";
  s.title = "tinytrace";
  s.ts = nowHHMM();
  s.env = tenant;
  s.valid = true;
  if (problemCount > 0) {
    s.rows[0].m = String(problemCount) + " active problem" + (problemCount == 1 ? "" : "s");
    s.rows[0].sev = SEV_ERROR;
    s.nrows = 1;
    s.count = problemCount;
    s.sev = SEV_ERROR;
    s.mascot = MASCOT_ALARM;
  } else {
    s.rows[0].m = "all quiet";
    s.rows[0].sev = SEV_OK;
    s.nrows = 1;
    s.sev = SEV_OK;
    s.mascot = MASCOT_IDLE;
  }
  return s;
}
