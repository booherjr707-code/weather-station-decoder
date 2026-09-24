// ESP32 + CC1101 433 MHz decoder
//
// Listens on 433.92 MHz with a CC1101 radio and decodes whatever it hears with
// rtl_433_ESP (the ESP32 port of rtl_433, which knows a few hundred devices:
// weather stations, driveway alarms, door sensors, remotes...).
//
// Everything it hears shows up on a web page served by the ESP32
// (http://rf433.local or its IP address), on the serial monitor, and, if MQTT
// is set up in secrets.h, in Home Assistant.
//
// Devices that send measurements (temperature, wind, rain...) are shown as
// weather sensors. Devices that only send a code, like a driveway alarm or a
// remote, are shown as alerts: they turn "on" for 30 seconds each time they
// send, and the ESP32's blue LED lights up.

#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <ArduinoLog.h>
#include <rtl_433_ESP.h>
#include "secrets.h"

// ---------- Settings ----------

const char* HOSTNAME = "rf433";                // web page at http://rf433.local
const int LED_PIN = 2;                         // blue LED on most ESP32 boards
const uint32_t ALERT_HOLD_MS = 30000;          // how long an alert stays on
const uint32_t REPEAT_WINDOW_MS = 2000;        // sensors repeat each message 2-3 times
const uint32_t WIFI_TIMEOUT_MS = 20000;

// Optional friendly names. Open the web page, find the device's ID (under its
// name, e.g. "Acurite-5n1-1234-A"), and add a line here.
struct FriendlyName {
  const char* uid;
  const char* name;
};
const FriendlyName NAMES[] = {
  // {"Generic-Remote-12345", "Driveway Alarm"},
  // {"Acurite-5n1-1234-A",   "Weather Station"},
  {nullptr, nullptr}
};

// ---------- Radio ----------

const size_t MSG_SIZE = 768;
char rfBuffer[MSG_SIZE];
rtl_433_ESP rf;
QueueHandle_t rfQueue;

// Called from the radio's decoder task. Only copies the message into a queue;
// loop() does the real work so Wi-Fi, MQTT and the web server stay on one task.
void onRadioMessage(char* message) {
  xQueueSend(rfQueue, message, 0);
}

// ---------- Backup decoder for driveway alarms ----------
// Most driveway alarms and doorbells (including the 58-melody, 500 ft kind)
// send a 24-bit code from an EV1527 chip. rtl_433 decodes these as
// "Generic-Remote", but only when the chip's timing is close to what it expects
// (short pulses of roughly 400-530 us). Cheap transmitters run anywhere from
// about 200 to 700 us, so this decodes the same code at any speed in that range
// and reports it under the same name. When both decoders catch a signal, the
// repeat filter in handleMessage() keeps just one.

// Reads one 24-bit frame starting at pulse `start`. Each bit is a short and a
// long part (1:3); a long pulse is a 1. The frame ends with a short sync pulse.
bool decodeEv1527Frame(const int* pulse, const int* gap, unsigned n, unsigned start,
                       uint32_t& code) {
  if (start + 25 > n) return false;
  int period = pulse[start] + gap[start];
  if (period < 700 || period > 3200) return false;
  code = 0;
  for (unsigned k = 0; k < 24; k++) {
    int p = pulse[start + k], g = gap[start + k];
    if (abs(p + g - period) > period / 4) return false;
    if (max(p, g) < 2 * min(p, g)) return false;
    code = (code << 1) | (p > g ? 1 : 0);
  }
  unsigned sync = start + 24;
  if (pulse[sync] > period / 2) return false;
  // The sync gap is about 31 short pulses long, unless the signal ends here.
  if (sync + 1 < n && gap[sync] < period * 4) return false;
  return true;
}

// Called by the radio's decoder task with every signal it received.
void onRawPulses(const int* pulse, const int* gap, unsigned int n,
                 unsigned long duration, int rssi) {
  static uint32_t lastCode = 0, lastMs = 0;
  static int matches = 0;
  for (unsigned i = 0; i + 25 <= n; i++) {
    uint32_t code;
    if (!decodeEv1527Frame(pulse, gap, n, i, code)) continue;
    i += 24;
    uint32_t id = code >> 8, cmd = code & 0xFF;
    if (id == 0 || cmd == 0) continue;  // same false-positive checks as rtl_433

    // Transmitters repeat the frame many times. Require two matching frames to
    // rule out noise, then report once per burst.
    uint32_t now = millis();
    if (code == lastCode && now - lastMs < 1000) {
      matches++;
    } else {
      matches = 1;
    }
    lastCode = code;
    lastMs = now;
    if (matches != 2) continue;

    char tristate[13];
    const char TRI[] = {'0', 'Z', 'X', '1'};
    for (int b = 0; b < 12; b++) tristate[b] = TRI[(code >> (22 - 2 * b)) & 3];
    tristate[12] = 0;
    char msg[160];
    snprintf(msg, sizeof(msg),
             "{\"model\":\"Generic-Remote\",\"id\":%u,\"cmd\":%u,\"tristate\":\"%s\","
             "\"rssi\":%d,\"protocol\":\"EV1527 backup decoder\"}",
             id, cmd, tristate, rssi);
    static char item[MSG_SIZE];
    strlcpy(item, msg, sizeof(item));
    xQueueSend(rfQueue, item, 0);
  }
}

// ---------- Devices ----------

// Readings rtl_433 can report, with how to show them in Home Assistant.
struct MeasureDef {
  const char* key;
  const char* name;
  const char* unit;
  const char* deviceClass;
  const char* stateClass;
};

const MeasureDef MEASURES[] = {
  {"temperature_C",   "Temperature",   "°C",   "temperature",           "measurement"},
  {"temperature_F",   "Temperature",   "°F",   "temperature",           "measurement"},
  {"humidity",        "Humidity",      "%",    "humidity",              "measurement"},
  {"wind_avg_km_h",   "Wind Speed",    "km/h", "wind_speed",            "measurement"},
  {"wind_max_km_h",   "Wind Gust",     "km/h", "wind_speed",            "measurement"},
  {"wind_avg_m_s",    "Wind Speed",    "m/s",  "wind_speed",            "measurement"},
  {"wind_max_m_s",    "Wind Gust",     "m/s",  "wind_speed",            "measurement"},
  {"wind_avg_mi_h",   "Wind Speed",    "mph",  "wind_speed",            "measurement"},
  {"wind_max_mi_h",   "Wind Gust",     "mph",  "wind_speed",            "measurement"},
  {"wind_dir_deg",    "Wind Direction","°",    nullptr,                 "measurement"},
  {"rain_mm",         "Rain Total",    "mm",   "precipitation",         "total_increasing"},
  {"rain_in",         "Rain Total",    "in",   "precipitation",         "total_increasing"},
  {"rain_rate_mm_h",  "Rain Rate",     "mm/h", "precipitation_intensity","measurement"},
  {"rain_rate_in_h",  "Rain Rate",     "in/h", "precipitation_intensity","measurement"},
  {"pressure_hPa",    "Pressure",      "hPa",  "atmospheric_pressure",  "measurement"},
  {"pressure_kPa",    "Pressure",      "kPa",  "atmospheric_pressure",  "measurement"},
  {"light_lux",       "Light",         "lx",   "illuminance",           "measurement"},
  {"uv",              "UV Index",      nullptr, nullptr,                "measurement"},
  {"uvi",             "UV Index",      nullptr, nullptr,                "measurement"},
  {"moisture",        "Soil Moisture", "%",    "moisture",              "measurement"},
  {"storm_dist",      "Storm Distance","km",   "distance",              "measurement"},
  {"strike_count",    "Lightning Strikes", nullptr, nullptr,            "total_increasing"},
};
const int MEASURE_COUNT = sizeof(MEASURES) / sizeof(MEASURES[0]);

struct Device {
  char uid[56];                // model-id-channel, e.g. "Acurite-5n1-1234-A"
  bool isAlert;                // sends codes only (driveway alarm, remote...)
  bool alertOn;
  uint32_t lastSeen;
  uint32_t lastAlert;
  uint32_t count;
  int rssi;
  uint32_t announced;          // bit per MEASURES entry already sent to HA
  bool alertAnnounced;
  bool batteryAnnounced;
  JsonDocument data;           // latest value of every field it has sent
  String lastPayload;          // for skipping the repeats of one transmission
};

const int MAX_DEVICES = 24;
Device devices[MAX_DEVICES];
int deviceCount = 0;

const char* friendlyName(const char* uid) {
  for (const FriendlyName* n = NAMES; n->uid; n++)
    if (strcmp(n->uid, uid) == 0) return n->name;
  return nullptr;
}

// Fields rtl_433 adds that are not part of the device's own message.
bool isRadioInfo(const char* key) {
  const char* skip[] = {"time", "rssi", "snr", "noise", "duration", "freq",
                        "freq1", "freq2", "protocol", "mic", "mhz"};
  for (const char* s : skip)
    if (strcmp(key, s) == 0) return true;
  return false;
}

int measureIndex(const char* key) {
  for (int i = 0; i < MEASURE_COUNT; i++)
    if (strcmp(MEASURES[i].key, key) == 0) return i;
  return -1;
}

void makeUid(JsonDocument& msg, char* out, size_t size) {
  String uid = msg["model"].as<String>();
  if (!msg["id"].isNull()) uid += "-" + msg["id"].as<String>();
  if (!msg["channel"].isNull()) uid += "-" + msg["channel"].as<String>();
  if (!msg["subtype"].isNull()) uid += "-" + msg["subtype"].as<String>();
  // Keep it safe for MQTT topics and URLs.
  for (size_t i = 0; i < uid.length(); i++) {
    char c = uid[i];
    if (!isalnum(c) && c != '-' && c != '_') uid.setCharAt(i, '_');
  }
  strlcpy(out, uid.c_str(), size);
}

Device* findOrAddDevice(const char* uid) {
  for (int i = 0; i < deviceCount; i++)
    if (strcmp(devices[i].uid, uid) == 0) return &devices[i];
  Device* d;
  if (deviceCount < MAX_DEVICES) {
    d = &devices[deviceCount++];
  } else {
    // Full (lots of neighbors' sensors): reuse the one heard from longest ago.
    d = &devices[0];
    for (int i = 1; i < MAX_DEVICES; i++)
      if (millis() - devices[i].lastSeen > millis() - d->lastSeen) d = &devices[i];
  }
  *d = Device();
  strlcpy(d->uid, uid, sizeof(d->uid));
  return d;
}

// ---------- Home Assistant (MQTT) ----------
// Each device heard gets its own Home Assistant device, with a sensor for each
// reading it sends. Alert devices get an on/off "motion" sensor. Does nothing
// if MQTT_HOST is empty.

WiFiClient mqttNet;
PubSubClient mqtt(mqttNet);
char gatewayId[24];               // "rf433_" + last 3 bytes of the MAC
char availTopic[48];
uint32_t lastMqttTry = 0;
bool mqttStarted = false;

bool mqttEnabled() { return MQTT_HOST[0] != 0; }

void publishJson(const char* topic, JsonDocument& doc, bool retain) {
  static char out[1024];
  size_t n = serializeJson(doc, out, sizeof(out));
  mqtt.publish(topic, (const uint8_t*)out, n, retain);
}

void addHaDevice(JsonDocument& cfg, Device& dev) {
  JsonObject d = cfg["device"].to<JsonObject>();
  d["identifiers"][0] = String("rf433_") + dev.uid;
  const char* nice = friendlyName(dev.uid);
  d["name"] = nice ? nice : dev.uid;
  d["model"] = dev.data["model"].as<const char*>();
  d["via_device"] = gatewayId;
  cfg["availability_topic"] = availTopic;
}

void announce(Device& dev) {
  if (!mqtt.connected()) return;
  char topic[128];
  char stateTopic[96];
  snprintf(stateTopic, sizeof(stateTopic), "rf433/%s/state", dev.uid);

  for (int i = 0; i < MEASURE_COUNT; i++) {
    const MeasureDef& m = MEASURES[i];
    if ((dev.announced & (1UL << i)) || dev.data[m.key].isNull()) continue;
    JsonDocument cfg;
    cfg["name"] = m.name;
    cfg["unique_id"] = String("rf433_") + dev.uid + "_" + m.key;
    cfg["state_topic"] = stateTopic;
    cfg["value_template"] = String("{{ value_json.") + m.key + " }}";
    if (m.unit) cfg["unit_of_measurement"] = m.unit;
    if (m.deviceClass) cfg["device_class"] = m.deviceClass;
    if (m.stateClass) cfg["state_class"] = m.stateClass;
    // Sensors go quiet when their batteries die; mark them unavailable after an hour.
    cfg["expire_after"] = 3600;
    addHaDevice(cfg, dev);
    snprintf(topic, sizeof(topic), "homeassistant/sensor/rf433_%s/%s/config", dev.uid, m.key);
    publishJson(topic, cfg, true);
    dev.announced |= (1UL << i);
  }

  if (!dev.batteryAnnounced && !dev.data["battery_ok"].isNull()) {
    JsonDocument cfg;
    cfg["name"] = "Battery Low";
    cfg["unique_id"] = String("rf433_") + dev.uid + "_battery";
    cfg["state_topic"] = stateTopic;
    cfg["value_template"] = "{{ 'OFF' if value_json.battery_ok else 'ON' }}";
    cfg["device_class"] = "battery";
    cfg["entity_category"] = "diagnostic";
    addHaDevice(cfg, dev);
    snprintf(topic, sizeof(topic), "homeassistant/binary_sensor/rf433_%s/battery/config", dev.uid);
    publishJson(topic, cfg, true);
    dev.batteryAnnounced = true;
  }

  if (dev.isAlert && !dev.alertAnnounced) {
    JsonDocument cfg;
    cfg["name"] = "Alert";
    cfg["unique_id"] = String("rf433_") + dev.uid + "_alert";
    cfg["state_topic"] = String("rf433/") + dev.uid + "/alert";
    cfg["json_attributes_topic"] = stateTopic;
    cfg["device_class"] = "motion";
    addHaDevice(cfg, dev);
    snprintf(topic, sizeof(topic), "homeassistant/binary_sensor/rf433_%s/alert/config", dev.uid);
    publishJson(topic, cfg, true);
    dev.alertAnnounced = true;
  }
}

void publishDevice(Device& dev) {
  if (!mqtt.connected()) return;
  announce(dev);
  char topic[96];
  snprintf(topic, sizeof(topic), "rf433/%s/state", dev.uid);
  publishJson(topic, dev.data, !dev.isAlert);
}

void publishAlert(Device& dev) {
  if (!mqtt.connected()) return;
  char topic[96];
  snprintf(topic, sizeof(topic), "rf433/%s/alert", dev.uid);
  mqtt.publish(topic, dev.alertOn ? "ON" : "OFF", true);
}

// Reconnects at most every 30 seconds so a missing broker never holds up the radio.
void mqttLoop() {
  if (!mqttEnabled() || WiFi.status() != WL_CONNECTED) return;
  if (!mqttStarted) {
    mqtt.setServer(MQTT_HOST, MQTT_PORT);
    mqtt.setBufferSize(1024);
    mqtt.setSocketTimeout(3);
    mqttStarted = true;
  }
  if (!mqtt.connected()) {
    if (lastMqttTry != 0 && millis() - lastMqttTry < 30000) return;
    lastMqttTry = millis();
    const char* user = MQTT_USER[0] ? MQTT_USER : nullptr;
    const char* pass = MQTT_USER[0] ? MQTT_PASSWORD : nullptr;
    if (!mqtt.connect(gatewayId, user, pass, availTopic, 0, true, "offline")) {
      Serial.printf("MQTT connect failed (state %d)\n", mqtt.state());
      return;
    }
    Serial.println("MQTT connected");
    mqtt.publish(availTopic, "online", true);
    // Home Assistant may have restarted: announce everything again.
    for (int i = 0; i < deviceCount; i++) {
      Device& dev = devices[i];
      dev.announced = 0;
      dev.alertAnnounced = dev.batteryAnnounced = false;
      publishDevice(dev);
      if (dev.isAlert) publishAlert(dev);
    }
  }
  mqtt.loop();
}

// ---------- Handling decoded messages ----------

void updateLed() {
  bool any = false;
  for (int i = 0; i < deviceCount; i++) any |= devices[i].alertOn;
  digitalWrite(LED_PIN, any ? HIGH : LOW);
}

void handleMessage(const char* text) {
  JsonDocument msg;
  if (deserializeJson(msg, text)) return;
  if (msg["model"].isNull()) return;  // status messages and undecoded noise

  Serial.printf("RF: %s\n", text);

  char uid[56];
  makeUid(msg, uid, sizeof(uid));

  // The same transmission usually arrives 2-3 times in a row; keep only the first.
  JsonDocument payload;
  bool hasMeasure = false;
  for (JsonPair kv : msg.as<JsonObject>()) {
    if (isRadioInfo(kv.key().c_str())) continue;
    payload[kv.key()] = kv.value();
    if (measureIndex(kv.key().c_str()) >= 0) hasMeasure = true;
  }
  String payloadText;
  serializeJson(payload, payloadText);

  Device* dev = findOrAddDevice(uid);
  bool isRepeat = dev->count > 0 && payloadText == dev->lastPayload &&
                  millis() - dev->lastSeen < REPEAT_WINDOW_MS;
  dev->lastSeen = millis();
  dev->rssi = msg["rssi"] | 0;
  if (isRepeat) return;

  dev->count++;
  dev->lastPayload = payloadText;
  // Some stations (Acurite 5-in-1) alternate between two message types with
  // different readings, so merge instead of replacing.
  for (JsonPair kv : payload.as<JsonObject>()) dev->data[kv.key()] = kv.value();
  if (hasMeasure) dev->isAlert = false;
  else if (dev->count == 1) dev->isAlert = true;

  publishDevice(*dev);

  if (dev->isAlert) {
    const char* nice = friendlyName(uid);
    Serial.printf("ALERT: %s\n", nice ? nice : uid);
    dev->lastAlert = millis();
    if (!dev->alertOn) {
      dev->alertOn = true;
      publishAlert(*dev);
    }
    updateLed();
  }
}

// Turns alerts back off 30 seconds after the last signal.
void checkAlerts() {
  for (int i = 0; i < deviceCount; i++) {
    Device& dev = devices[i];
    if (dev.alertOn && millis() - dev.lastAlert > ALERT_HOLD_MS) {
      dev.alertOn = false;
      publishAlert(dev);
      updateLed();
    }
  }
}

// ---------- Web page ----------

WebServer server(80);

const char PAGE[] PROGMEM = R"rawliteral(<!doctype html>
<html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>433 MHz Receiver</title>
<style>
:root{--bg:#f2f4f7;--card:#fff;--ink:#17202b;--muted:#5d6878;--line:#d9dee6;
--accent:#1f6feb;--alert:#d1242f;--alert-bg:#ffebe9;--ok:#1a7f37}
@media (prefers-color-scheme:dark){:root{--bg:#0f141a;--card:#18202a;--ink:#e6ebf1;
--muted:#97a3b3;--line:#2b3542;--accent:#58a6ff;--alert:#ff6b6b;--alert-bg:#3a1618;--ok:#3fb950}}
*{box-sizing:border-box}
body{margin:0;background:var(--bg);color:var(--ink);font:15px/1.45 -apple-system,system-ui,"Segoe UI",Roboto,sans-serif}
main{max-width:760px;margin:0 auto;padding:16px}
h1{font-size:20px;margin:4px 0 2px}
.sub{color:var(--muted);font-size:13px;margin-bottom:16px}
h2{font-size:13px;text-transform:uppercase;letter-spacing:.06em;color:var(--muted);margin:22px 0 8px}
.banner{border-radius:10px;padding:14px 16px;font-weight:600;background:var(--card);border:1px solid var(--line)}
.banner.on{background:var(--alert-bg);border-color:var(--alert);color:var(--alert);animation:pulse 1s ease-in-out infinite alternate}
@keyframes pulse{to{opacity:.65}}
@media (prefers-reduced-motion:reduce){.banner.on{animation:none}}
.grid{display:grid;grid-template-columns:repeat(auto-fill,minmax(220px,1fr));gap:10px}
.card{background:var(--card);border:1px solid var(--line);border-radius:10px;padding:12px 14px}
.name{font-weight:600}
.uid{color:var(--muted);font-size:12px;word-break:break-all}
.rows{display:grid;grid-template-columns:auto 1fr;gap:2px 12px;margin-top:8px;font-variant-numeric:tabular-nums}
.rows span:nth-child(odd){color:var(--muted)}
.rows span:nth-child(even){text-align:right;font-weight:600}
.card.alerting{border-color:var(--alert)}
.pill{display:inline-block;font-size:12px;font-weight:600;padding:1px 8px;border-radius:99px;background:var(--alert);color:#fff;margin-left:6px}
.empty{color:var(--muted)}
footer{color:var(--muted);font-size:12px;margin-top:24px}
</style></head><body><main>
<h1>433 MHz Receiver</h1>
<div class="sub" id="sub">Listening on 433.92 MHz…</div>
<div class="banner" id="banner">No alerts</div>
<h2>Weather and sensors</h2><div class="grid" id="sensors"></div>
<h2>Alarms and remotes</h2><div class="grid" id="alerts"></div>
<footer>Updates every 3 seconds. To name a device, copy the gray ID under it into the NAMES list in main.cpp.</footer>
</main><script>
const LABELS={temperature_C:["Temperature",v=>(v*9/5+32).toFixed(1)+" °F"],
temperature_F:["Temperature",v=>(+v).toFixed(1)+" °F"],humidity:["Humidity",v=>v+" %"],
wind_avg_km_h:["Wind",v=>(v/1.609).toFixed(1)+" mph"],wind_max_km_h:["Gust",v=>(v/1.609).toFixed(1)+" mph"],
wind_avg_m_s:["Wind",v=>(v*2.237).toFixed(1)+" mph"],wind_max_m_s:["Gust",v=>(v*2.237).toFixed(1)+" mph"],
wind_avg_mi_h:["Wind",v=>(+v).toFixed(1)+" mph"],wind_max_mi_h:["Gust",v=>(+v).toFixed(1)+" mph"],
wind_dir_deg:["Wind from",v=>v+"° "+["N","NE","E","SE","S","SW","W","NW"][Math.round(v/45)%8]],
rain_mm:["Rain total",v=>(v/25.4).toFixed(2)+" in"],rain_in:["Rain total",v=>(+v).toFixed(2)+" in"],
rain_rate_mm_h:["Rain rate",v=>(v/25.4).toFixed(2)+" in/h"],rain_rate_in_h:["Rain rate",v=>(+v).toFixed(2)+" in/h"],
pressure_hPa:["Pressure",v=>(v*0.02953).toFixed(2)+" inHg"],pressure_kPa:["Pressure",v=>(v*0.2953).toFixed(2)+" inHg"],
light_lux:["Light",v=>Math.round(v)+" lx"],uv:["UV index",v=>v],uvi:["UV index",v=>v],
moisture:["Soil moisture",v=>v+" %"],storm_dist:["Storm",v=>(v/1.609).toFixed(0)+" mi"],
strike_count:["Strikes",v=>v],battery_ok:["Battery",v=>v?"OK":"LOW"]};
function ago(s){return s<60?s+" s ago":s<3600?Math.floor(s/60)+" min ago":Math.floor(s/3600)+" h ago"}
function esc(t){return String(t).replace(/[&<>"]/g,c=>({"&":"&amp;","<":"&lt;",">":"&gt;",'"':"&quot;"}[c]))}
function card(d){let rows="";
if(d.alert){rows+=`<span>Signals</span><span>${d.count}</span>`;
for(const k of ["cmd","button","code","state","event"])if(d.data[k]!==undefined)rows+=`<span>${k}</span><span>${esc(d.data[k])}</span>`}
else for(const k in LABELS)if(d.data[k]!==undefined)rows+=`<span>${LABELS[k][0]}</span><span>${esc(LABELS[k][1](d.data[k]))}</span>`;
rows+=`<span>Heard</span><span>${ago(d.ago)}</span><span>Signal</span><span>${d.rssi} dBm</span>`;
return `<div class="card${d.on?" alerting":""}"><div class="name">${esc(d.name)}${d.on?'<span class="pill">ALERT</span>':""}</div>
<div class="uid">${esc(d.uid)}</div><div class="rows">${rows}</div></div>`}
async function refresh(){try{const r=await fetch("/api");const j=await r.json();
const s=j.devices.filter(d=>!d.alert),a=j.devices.filter(d=>d.alert);
document.getElementById("sensors").innerHTML=s.map(card).join("")||'<div class="empty">Nothing heard yet. Weather stations send about once a minute.</div>';
document.getElementById("alerts").innerHTML=a.map(card).join("")||'<div class="empty">Nothing heard yet. Walk past the driveway sensor to test it.</div>';
const on=a.filter(d=>d.on);const b=document.getElementById("banner");
b.className="banner"+(on.length?" on":"");
b.textContent=on.length?"ALERT: "+on.map(d=>d.name).join(", "):"No alerts";
document.getElementById("sub").textContent=`Listening on 433.92 MHz · ${j.devices.length} devices heard · up ${ago(j.uptime).replace(" ago","")}`}
catch(e){document.getElementById("sub").textContent="Lost connection to the receiver, retrying…"}}
refresh();setInterval(refresh,3000);
</script></body></html>)rawliteral";

void handleApi() {
  JsonDocument out;
  out["uptime"] = millis() / 1000;
  JsonArray list = out["devices"].to<JsonArray>();
  // Newest first.
  bool used[MAX_DEVICES] = {false};
  for (int n = 0; n < deviceCount; n++) {
    int best = -1;
    for (int i = 0; i < deviceCount; i++)
      if (!used[i] && (best < 0 || devices[i].lastSeen > devices[best].lastSeen)) best = i;
    used[best] = true;
    Device& dev = devices[best];
    JsonObject o = list.add<JsonObject>();
    const char* nice = friendlyName(dev.uid);
    o["uid"] = dev.uid;
    o["name"] = nice ? nice : dev.data["model"].as<const char*>();
    o["alert"] = dev.isAlert;
    o["on"] = dev.alertOn;
    o["ago"] = (millis() - dev.lastSeen) / 1000;
    o["count"] = dev.count;
    o["rssi"] = dev.rssi;
    o["data"] = dev.data;
  }
  String body;
  serializeJson(out, body);
  server.send(200, "application/json", body);
}

// ---------- Main ----------

bool wasConnected = false;

bool connectWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.setHostname(HOSTNAME);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < WIFI_TIMEOUT_MS) delay(250);
  return WiFi.status() == WL_CONNECTED;
}

void setup() {
  Serial.begin(115200);
  delay(500);
  pinMode(LED_PIN, OUTPUT);
  Log.begin(LOG_LEVEL, &Serial);
  Serial.println("\n433 MHz receiver starting");

  uint8_t mac[6];
  WiFi.macAddress(mac);
  snprintf(gatewayId, sizeof(gatewayId), "rf433_%02x%02x%02x", mac[3], mac[4], mac[5]);
  snprintf(availTopic, sizeof(availTopic), "%s/status", gatewayId);

  // Decoding works without Wi-Fi; you just only see it on the serial monitor.
  wasConnected = connectWiFi();
  if (wasConnected) {
    Serial.printf("WiFi connected. Open http://%s.local or http://%s\n",
                  HOSTNAME, WiFi.localIP().toString().c_str());
    MDNS.begin(HOSTNAME);
    MDNS.addService("http", "tcp", 80);
  } else {
    Serial.println("WiFi failed, will keep trying. Decoding still works.");
  }
  server.on("/", [] { server.send_P(200, "text/html", PAGE); });
  server.on("/api", handleApi);
  server.begin();

  rfQueue = xQueueCreate(6, MSG_SIZE);
  rf.initReceiver(RF_MODULE_RECEIVER_GPIO, RF_MODULE_FREQUENCY);
  rf.setCallback(onRadioMessage, rfBuffer, MSG_SIZE);
  rf.setRawPulsesCallback(onRawPulses);
  rf.enableReceiver();
  rf.getModuleStatus();
  Serial.println("Listening on 433.92 MHz");

  // Two quick blinks: the radio is running.
  for (int i = 0; i < 2; i++) {
    digitalWrite(LED_PIN, HIGH); delay(150);
    digitalWrite(LED_PIN, LOW);  delay(150);
  }
}

void loop() {
  rf.loop();

  static char text[MSG_SIZE];
  while (xQueueReceive(rfQueue, text, 0) == pdTRUE) handleMessage(text);

  checkAlerts();
  server.handleClient();
  mqttLoop();

  // Wi-Fi came back after an outage: restart the .local name.
  bool connected = WiFi.status() == WL_CONNECTED;
  if (connected && !wasConnected) {
    Serial.printf("WiFi up: http://%s\n", WiFi.localIP().toString().c_str());
    MDNS.begin(HOSTNAME);
  }
  wasConnected = connected;
  delay(2);
}
