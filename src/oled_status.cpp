#include <oled_status.h>
#include <network_status.h>
#include <pins.h>

#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <WiFi.h>
#include <Wire.h>
#if defined(USE_ETHERNET_PORT)
#include <ETH.h>
#endif

namespace {

#if defined(CONFIG_IDF_TARGET_ESP32P4)
struct OledPinPair {
  int sda;
  int scl;
};
/** Waveshare P4: many breakouts wire GPIO8→SSD1306 SDA, GPIO7→SCL — try that first; then swapped; then 21/22. */
constexpr OledPinPair kPinCandidates[] = {{8, 7}, {7, 8}, {21, 22}};
#else
struct OledPinPair {
  int sda;
  int scl;
};
constexpr OledPinPair kPinCandidates[] = {{21, 22}};
#endif
constexpr uint8_t kAddrCandidates[] = {0x3C, 0x3D};
constexpr uint32_t kRefreshMs = 800;
constexpr uint32_t kSplashMinMs = 2200;
/** ~6 px/char at text size 1 on 128 px wide SSD1306 */
constexpr int kMaxChars = 21;

Adafruit_SSD1306 g_disp(128, 64, &Wire, -1);
bool g_ok = false;
uint32_t g_last;
uint32_t g_bootShownAt = 0;
bool g_ready = false;
int g_usedSda = -1;
int g_usedScl = -1;
uint8_t g_usedAddr = 0;

static bool isAddrPresent(uint8_t addr) {
  Wire.beginTransmission(addr);
  return Wire.endTransmission() == 0;
}

#if defined(CONFIG_IDF_TARGET_ESP32P4)
static void logI2cQuickScan() {
  Serial.print(F("[OLED]   bus scan:"));
  bool any = false;
  for (uint8_t a = 8; a < 0x79; a++) {
    Wire.beginTransmission(a);
    if (Wire.endTransmission() != 0)
      continue;
    Serial.print(any ? F(", ") : F(" "));
    any = true;
    Serial.print(F("0x"));
    if (a < 16)
      Serial.print('0');
    Serial.print(a, HEX);
  }
  if (!any)
    Serial.println(F(" (none)"));
  else
    Serial.println();
}
#endif

static bool tryBeginDisplay(uint8_t addr) {
  if (g_disp.begin(SSD1306_SWITCHCAPVCC, addr, true, false))
    return true;
  // Some modules are wired for external VCC setup.
  if (g_disp.begin(SSD1306_EXTERNALVCC, addr, true, false))
    return true;
  return false;
}

static String fitLine(const String &s) {
  if ((int)s.length() <= kMaxChars)
    return s;
  return s.substring(0, kMaxChars - 1) + ".";
}

static void drawCenteredLine(int y, const String &text) {
  int16_t x1 = 0, y1 = 0;
  uint16_t w = 0, h = 0;
  g_disp.getTextBounds(text, 0, y, &x1, &y1, &w, &h);
  int x = (128 - (int)w) / 2;
  if (x < 0)
    x = 0;
  g_disp.setCursor(x, y);
  g_disp.print(text);
}

static void drawDinoIcon(int x, int y) {
  // Body
  g_disp.fillRoundRect(x + 8, y + 12, 18, 12, 4, SSD1306_WHITE);
  // Head + snout
  g_disp.fillCircle(x + 26, y + 10, 5, SSD1306_WHITE);
  g_disp.fillRoundRect(x + 28, y + 9, 6, 4, 2, SSD1306_WHITE);
  // Eye
  g_disp.drawPixel(x + 27, y + 9, SSD1306_BLACK);
  // Small arm
  g_disp.fillRoundRect(x + 18, y + 16, 4, 3, 1, SSD1306_BLACK);
  // Tail
  g_disp.fillTriangle(x + 8, y + 15, x + 0, y + 11, x + 8, y + 20, SSD1306_WHITE);
  // Legs + feet
  g_disp.fillRect(x + 12, y + 24, 4, 6, SSD1306_WHITE);
  g_disp.fillRect(x + 20, y + 24, 4, 6, SSD1306_WHITE);
  g_disp.fillRect(x + 11, y + 30, 6, 2, SSD1306_WHITE);
  g_disp.fillRect(x + 19, y + 30, 6, 2, SSD1306_WHITE);
}

static void drawBootSplash() {
  g_disp.clearDisplay();
  g_disp.setTextColor(SSD1306_WHITE);
  drawDinoIcon((128 - 34) / 2, 1);
  g_disp.setTextSize(2);
  drawCenteredLine(40, F("IONIX"));
  g_disp.display();
}

static String hotspotPassword() { return String(F("ionixpass")); }

} // namespace

void oledStatusBegin() {
  bool found = false;
  for (unsigned pi = 0; pi < sizeof(kPinCandidates) / sizeof(kPinCandidates[0]) && !found; pi++) {
    const int sda = kPinCandidates[pi].sda;
    const int scl = kPinCandidates[pi].scl;
    Wire.begin(sda, scl);
    Wire.setClock(100000); // 100 kHz — more tolerant with dupont wiring than default 400 kHz+
#if defined(CONFIG_IDF_TARGET_ESP32P4)
    delay(40);
#else
    delay(10);
#endif
    bool sawOledAddr = false;
    bool displayOk = false;
    for (unsigned ai = 0; ai < sizeof(kAddrCandidates) / sizeof(kAddrCandidates[0]); ai++) {
      if (!isAddrPresent(kAddrCandidates[ai]))
        continue;
      sawOledAddr = true;
      if (!tryBeginDisplay(kAddrCandidates[ai]))
        continue;
      g_usedSda = sda;
      g_usedScl = scl;
      g_usedAddr = kAddrCandidates[ai];
      displayOk = true;
      found = true;
      break;
    }
#if defined(CONFIG_IDF_TARGET_ESP32P4)
    if (!displayOk) {
      Serial.print(F("[OLED] try SDA=GPIO"));
      Serial.print(sda);
      Serial.print(F(" SCL=GPIO"));
      Serial.print(scl);
      if (sawOledAddr)
        Serial.print(F(": I2C OK but SSD1306 init rejected → check 3V3/power/GND."));
      else
        Serial.print(F(": "));
      Serial.println();
      if (!sawOledAddr)
        logI2cQuickScan();
    }
#endif
  }
  if (!found) {
    g_ok = false;
    Serial.println(F("[OLED] init failed on all pin/address candidates."));
    return;
  }
  Serial.print(F("[OLED] ready SDA="));
  Serial.print(g_usedSda);
  Serial.print(F(" SCL="));
  Serial.print(g_usedScl);
  Serial.print(F(" ADDR=0x"));
  Serial.println(g_usedAddr, HEX);
  g_ready = false;
  g_bootShownAt = millis();
  drawBootSplash();
  g_ok = true;
  g_last = 0;
}

void oledStatusSetReady(bool ready) { g_ready = ready; }

void oledStatusLoop() {
  if (!g_ok)
    return;
  const uint32_t now = millis();
  if (!g_ready || (now - g_bootShownAt < kSplashMinMs)) {
    if (now - g_last >= kRefreshMs / 2) {
      g_last = now;
      drawBootSplash();
    }
    return;
  }
  if (now - g_last < kRefreshMs)
    return;
  g_last = now;
  const NetworkStatusSnapshot net = readNetworkStatus();
  String mac = net.mac;
  if (mac.length() == 0)
    mac = F("-");

  String wifiUi = net.wifiIp;
  if (wifiUi == F("-"))
    wifiUi = net.hotspotIp;
  if (wifiUi.length() == 0)
    wifiUi = F("-");

#if defined(USE_ETHERNET_PORT)
  const String topIpLine = String(F("ETH:")) + net.physicalIp;
#else
  const String topIpLine = String(F("IP:")) + net.primaryIp;
#endif

  String apSsid = WiFi.softAPSSID();
  if (apSsid.length() == 0)
    apSsid = F("-");
  const String apPass = hotspotPassword();

  g_disp.clearDisplay();
  g_disp.setTextSize(1);
  g_disp.setTextColor(SSD1306_WHITE);
  drawCenteredLine(0, F("IONIX I GPIO"));
  drawCenteredLine(8, fitLine(topIpLine));
  drawCenteredLine(16, fitLine(String(F("MAC:")) + mac));
  drawCenteredLine(24, fitLine(String(F("WLAN:")) + wifiUi));
  drawCenteredLine(32, fitLine(String(F("SSID:")) + apSsid));
  drawCenteredLine(40, fitLine(String(F("PW:")) + apPass));
  g_disp.display();
}
