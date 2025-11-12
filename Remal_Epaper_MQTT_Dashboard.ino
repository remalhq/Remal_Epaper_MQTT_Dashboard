/**
 * @file      Remal_Epaper_MQTT_Dashboard.ino
 * @author 		Khalid Mansoor AlAwadhi, Remal <khalid@remal.io>
 * @date      Nov 11 2025
 * 
 * @brief     A simple MQTT dashboard for Remal Shabakah v4 (ESP32-C3) with 4.2" e-paper display.
 *            It connects to Wi-Fi, subscribes to topic defined by SUB_TOPIC, and displays incoming 
 *            messages on the e-paper display with soft-wrapping. It also logs messages to FFat storage
 *            with a persistent index.
**/
#include <WiFi.h>
#include <time.h>
#include "WifiCreds.h"
#include <PubSubClient.h>
#include "FS.h"
#include "FFat.h"
#include "Remal_SHT3X.h"
#include <GxEPD2_BW.h>
#include <Adafruit_GFX.h>

// ---------- E-paper driver (4.2" SSD1683) ----------
#define GxEPD2_DRIVER_CLASS GxEPD2_420_GDEY042T81

// ---------- Shabakah v4 (ESP32-C3) e-paper pins ----------
static const uint8_t EP_CS   = 7;
static const uint8_t EP_DC   = 0;
static const uint8_t EP_RST  = 21;
static const uint8_t EP_BUSY = 20;
// SPI MOSI = GPIO6, SCK = GPIO4 (MISO unused)

GxEPD2_BW<GxEPD2_DRIVER_CLASS, GxEPD2_DRIVER_CLASS::HEIGHT> display(
  GxEPD2_DRIVER_CLASS(EP_CS, EP_DC, EP_RST, EP_BUSY)
);

// ---------- Layout ----------
const int titleH = 40;
const int leftX  = 10;
const int rightMargin = 10;
const int contentLineStep = 18;
const int baseY0 = 56;
int yPos = baseY0;
int partialsSinceClean = 0;

// ---------- Timing ----------
unsigned long lastTitleUpdateMs  = 0;
unsigned long lastSensorUpdateMs = 0;
const unsigned long TITLE_UPDATE_MS  = 60000;
const unsigned long SENSOR_UPDATE_MS = 10000;

// ---------- SHT30 ----------
#define SHT30_ADDR 0x44
SHT3x SHT30_Sensor(SHT30_ADDR);
float CurrTemp = NAN, CurrHumd = NAN;

// ---------- MQTT ----------
#define REMAL_MQTT_SERVER "mqtt.remal.io"
#define REMAL_MQTT_PORT   8885
static const char* SUB_TOPIC = "remal/test/#";

WiFiClient netClient;
PubSubClient mqtt(netClient);

// ---------- Persistent log/index (FFat) ----------
#define LOG_PATH "/MQTT_log.txt"
#define IDX_PATH "/MQTT_index.txt"

uint32_t gMsgIndex = 0;  // will be loaded from IDX_PATH

// ---------- Message queue (visual lines) ----------
#define MSGQ_CAP 96
String msgQ[MSGQ_CAP];
int qHead = 0, qTail = 0;
inline bool qEmpty(){ return qHead == qTail; }
inline bool qFull(){  return ((qHead + 1) % MSGQ_CAP) == qTail; }
void qPush(const String& s){ if(qFull()) qTail=(qTail+1)%MSGQ_CAP; msgQ[qHead]=s; qHead=(qHead+1)%MSGQ_CAP; }
bool qPop(String& o){ if(qEmpty()) return false; o=msgQ[qTail]; qTail=(qTail+1)%MSGQ_CAP; return true; }

// ---------- Helpers ----------
inline int alignDown(int v,int m){ return (v/m)*m; }
inline int alignUp  (int v,int m){ return ((v+m-1)/m)*m; }
int contentMaxWidth() { return display.width() - leftX - rightMargin; }

uint16_t measureWidth(const String& s) {
  int16_t bx, by; uint16_t bw, bh;
  display.setTextSize(1);
  display.setFont(NULL);
  display.getTextBounds(s.c_str(), 0, 0, &bx, &by, &bw, &bh);
  return bw;
}

String makeSpaces(size_t n){
  String s; s.reserve(n);
  for (size_t i=0;i<n;i++) s += ' ';
  return s;
}

// ---------- Payload cleanup (for display) ----------
String sanitizePayload(const String& in) {
  String out; out.reserve(in.length());
  for (size_t i=0;i<in.length();++i) {
    char c = in[i];
    if (c == '\r') continue;
    if (c == '\t') { out += ' '; continue; }
    if ((c >= 32 && c <= 126) || c == '\n') out += c; // ASCII + LF
  }
  // collapse consecutive spaces
  String out2; out2.reserve(out.length());
  bool lastSpace=false;
  for (size_t i=0;i<out.length();++i){
    char c = out[i];
    if (c==' ') { if (!lastSpace){ out2+=' '; lastSpace=true; } }
    else { out2 += c; lastSpace=false; }
  }
  return out2;
}

// ---------- Wrapping (continuations use full width, no indent) ----------
void wrapAndEnqueue(const String& firstVisual,
                    const String& payload,
                    const String& firstPayloadPrefix,
                    const String& contPrefix /* pass "" */)
{
  const int maxW = contentMaxWidth();

  // 1) index - time line
  qPush(firstVisual);

  // 2) payload (may be empty or multi-line)
  if (payload.length() == 0) {
    qPush(firstPayloadPrefix);
    return;
  }

  String line;
  String prefix = firstPayloadPrefix;   // only on FIRST payload line

  auto pushLine = [&](){
    if (line.length()==0) qPush(prefix);
    else qPush(line);
  };

  line = prefix;

  for (size_t i=0; i<payload.length(); ++i) {
    char c = payload[i];

    if (c == '\n') {
      pushLine();
      // continuation lines: NO prefix → full width
      prefix = "";
      line   = prefix;
      continue;
    }

    // avoid leading spaces after a break
    if (c==' ' && line.length()==(int)prefix.length()) continue;

    String trial = line; trial += c;
    if (measureWidth(trial) <= (uint16_t)maxW) {
      line += c;
    } else {
      pushLine();
      prefix = "";
      line   = prefix;
      if (c!=' ') {
        String trial2 = line; trial2 += c;
        if (measureWidth(trial2) > (uint16_t)maxW) {
          qPush(String(prefix) + c);
          line = prefix;
        } else {
          line += c;
        }
      }
    }
  }
  pushLine();
}

// ---------- Time helpers ----------
String nowString() {
  struct tm t; if (!getLocalTime(&t,200)) return "--:--";
  char b[48]; strftime(b,sizeof(b), "%a %b %d '%y %I:%M %p", &t);
  return String(b);
}

// ---------- FFat: index load/save + logging ----------
void ffatInit() {
  if (!FFat.begin(true)) {   // format on fail
    Serial.println("[FFat] mount failed");
  } else {
    Serial.println("[FFat] mounted");
  }
}

uint32_t ffatLoadIndex() {
  File f = FFat.open(IDX_PATH, FILE_READ);
  if (!f) {
    Serial.println("[FFat] index file not found; starting at 0");
    return 0;
  }
  String s = f.readStringUntil('\n');
  f.close();
  uint32_t v = s.toInt(); // toInt handles leading spaces; 0 if invalid
  Serial.printf("[FFat] loaded index=%lu\n", (unsigned long)v);
  return v;
}

void ffatSaveIndex(uint32_t idx) {
  File f = FFat.open(IDX_PATH, FILE_WRITE); // truncate
  if (!f) {
    Serial.println("[FFat] index save open failed");
    return;
  }
  f.printf("%lu\n", (unsigned long)idx);
  f.flush();
  f.close();
}

void ffatAppendLog(uint32_t idx, const String& ts, const String& topic, const String& rawPayload) {
  File f = FFat.open(LOG_PATH, FILE_APPEND);
  if (!f) {
    Serial.println("[FFat] log open failed");
    return;
  }
  // Write block:
  // <idx> - <ts>\r\n
  // [topic]: <payload with \n→\r\n>\r\n
  // \r\n
  f.printf("%lu - %s\r\n", (unsigned long)idx, ts.c_str());
  f.print("[");
  f.print(topic);
  f.print("]: ");

  // Convert lone '\n' to CRLF for readability
  for (size_t i=0; i<rawPayload.length(); ++i) {
    char c = rawPayload[i];
    if (c == '\r') continue;         // normalize CRLF → LF → CRLF out
    if (c == '\n') { f.print("\r\n"); continue; }
    f.write((uint8_t)c);
  }
  f.print("\r\n\r\n");
  f.flush();
  f.close();
}

// ---------- Title Bar ----------
void drawTitleBar(const String& timeText){
  display.setPartialWindow(0, 0, display.width(), titleH);
  display.firstPage();
  do {
    display.fillRect(0, 0, display.width(), titleH, GxEPD_BLACK);
    display.setTextColor(GxEPD_WHITE);
    display.setTextSize(1);
    display.setCursor(leftX, 14); display.print("Remal MQTT Dashboard");
    display.setCursor(leftX, 30); display.print(timeText);

    if(!isnan(CurrTemp) && !isnan(CurrHumd)){
      char t[16]; snprintf(t,sizeof(t),"%.1f C", CurrTemp);
      char h[16]; snprintf(h,sizeof(h),"%.0f %%RH", CurrHumd);
      int16_t bx,by; uint16_t bw,bh;
      display.getTextBounds(t,0,0,&bx,&by,&bw,&bh);
      int rx = display.width() - rightMargin - bw;
      display.setCursor(rx, 14); display.print(t);
      display.getTextBounds(h,0,0,&bx,&by,&bw,&bh);
      rx = display.width() - rightMargin - bw;
      display.setCursor(rx, 30); display.print(h);
    }
  } while (display.nextPage());
}

void fullClearWithTitle(){
  display.setFullWindow();
  display.firstPage(); do { display.fillScreen(GxEPD_WHITE); } while (display.nextPage());
  drawTitleBar(nowString());
  partialsSinceClean  = 0;
  lastTitleUpdateMs   = millis();
  lastSensorUpdateMs  = millis();
}

// ---------- Draw one visual line (partial stripe) ----------
void drawOneLine(const String& text){
  display.setTextSize(1);
  int16_t bx,by; uint16_t bw,bh;
  display.getTextBounds(text.c_str(), leftX, yPos, &bx, &by, &bw, &bh);
  const int padTop = 2, padBot = 3;
  int top = by - padTop;
  int h   = (int)bh + padTop + padBot;
  if (top < titleH) top = titleH;
  if (top + h > display.height()) h = display.height() - top;
  top = alignDown(top, 2);
  h   = alignUp  (h,   2);
  display.setPartialWindow(0, top, display.width(), h);
  display.firstPage();
  do {
    display.fillRect(0, top, display.width(), h, GxEPD_WHITE);
    display.setTextColor(GxEPD_BLACK);
    display.setCursor(leftX, yPos);
    display.print(text);
  } while (display.nextPage());
  yPos += contentLineStep;
  if (++partialsSinceClean >= 25) {
    fullClearWithTitle();
    yPos = baseY0;
  }
}

// ---------- Wi-Fi + Time ----------
void connectWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.printf("[WiFi] Connecting to %s", WIFI_SSID);
  while (WiFi.status() != WL_CONNECTED) { delay(300); Serial.print("."); }
  Serial.printf("\n[WiFi] OK  RSSI=%d dBm\n", WiFi.RSSI());
}

bool syncTimeDubai() {
  configTzTime("GST-4", "pool.ntp.org", "time.nist.gov", "time.google.com");
  struct tm t; for(int i=0;i<50;i++){ if(getLocalTime(&t,200)) return true; delay(200); }
  return false;
}

// ---------- Enqueue message (display) ----------
void enqueueWrappedMessage(uint32_t index, const String& topic, const String& rawPayload, const String& ts) {
  // First visual line: "<index> - <local time>"
  String firstVisual = String(index) + " - " + ts;

  // Build prefixes: show header ONLY on first payload line; continuations = ""
  String payloadDisp = sanitizePayload(rawPayload);
  String head        = "[" + topic + "]: ";

  wrapAndEnqueue(firstVisual, payloadDisp, head, "" /* no indent for continuations */);
}

// ---------- MQTT callback ----------
void mqttCallback(char* topic, byte* payload, unsigned int len){
  // Build raw payload (keep original for log)
  String pay; pay.reserve(len);
  for (unsigned int i=0; i<len; i++) pay += (char)payload[i];

  // Assign next index and timestamp (once)
  uint32_t idx = gMsgIndex + 1;
  String    ts = nowString();

  // 1) Append to FFat log first (best effort)
  ffatAppendLog(idx, ts, String(topic), pay);

  // 2) Persist the index (so reboot continues)
  gMsgIndex = idx;
  ffatSaveIndex(gMsgIndex);

  // 3) Enqueue for on-screen rendering (sanitized/soft-wrapped)
  enqueueWrappedMessage(idx, String(topic), pay, ts);
}

bool connectMQTT(){
  mqtt.setServer(REMAL_MQTT_SERVER, REMAL_MQTT_PORT);
  mqtt.setCallback(mqttCallback);
  char cid[48]; snprintf(cid, sizeof(cid), "ShabakahC3-%lu", millis());
  Serial.printf("[MQTT] Connecting to %s:%d ...\n", REMAL_MQTT_SERVER, REMAL_MQTT_PORT);
  if (mqtt.connect(cid)){
    Serial.println("[MQTT] Connected");
    mqtt.subscribe(SUB_TOPIC);
    return true;
  }
  Serial.printf("[MQTT] Failed rc=%d\n", mqtt.state());
  return false;
}

// ---------- Setup ----------
void setup(){
  Serial.begin(115200);
  Serial.println("\n[BOOT] Remal MQTT Dashboard (FFat logging + persistent index)");

  display.init(115200);
  display.setRotation(1);

  ffatInit();
  // Load last index (continue counting)
  gMsgIndex = ffatLoadIndex();

  connectWiFi();
  syncTimeDubai();

  SHT30_Sensor.Initialize();
  SHT30_Sensor.SetRepeatability(e_high);
  if (SHT30_Sensor.IsConnected()) {
    CurrTemp = SHT30_Sensor.GetTemperatureCelsius();
    CurrHumd = SHT30_Sensor.GetHumidity();
  }

  fullClearWithTitle();
  connectMQTT();
}

// ---------- Loop ----------
void loop(){
  unsigned long now = millis();
  if (WiFi.status() != WL_CONNECTED) connectWiFi();
  if (!mqtt.connected()) connectMQTT(); else mqtt.loop();

  if (now - lastTitleUpdateMs >= TITLE_UPDATE_MS) {
    drawTitleBar(nowString());
    lastTitleUpdateMs = now;
  }
  if (now - lastSensorUpdateMs >= SENSOR_UPDATE_MS) {
    if (SHT30_Sensor.IsConnected()) {
      CurrTemp = SHT30_Sensor.GetTemperatureCelsius();
      CurrHumd = SHT30_Sensor.GetHumidity();
    }
    drawTitleBar(nowString());
    lastSensorUpdateMs = now;
  }

  String line;
  if (qPop(line)) {
    if (yPos + 20 > display.height()) { yPos = baseY0; fullClearWithTitle(); }
    drawOneLine(line);
  }
  delay(10);
}
