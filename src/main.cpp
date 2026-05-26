/**
 * IONIX I GPIO — ESP32
 * IONIX-Connect-style UI, login admin/admin, 2× GPI + 2× GPO, Wi-Fi captive portal
 * (double-reset on EN or hold BOOT at power-on).
 */
#include <Arduino.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <WebServer.h>
#include <WiFi.h>
#include <ESPmDNS.h>
#include <Update.h>
#include <cstdlib>
#if defined(USE_ETHERNET_PORT)
#include <ETH.h>
#endif

#include <atem_service.h>
#include <gpio_logic.h>
#include <network_status.h>
#include <oled_status.h>
#include <pins.h>

#ifndef IONIX_FW_VERSION
#define IONIX_FW_VERSION "0.1.0"
#endif

namespace {

// Double-reset: press EN/RST twice within ~1s during early boot → config portal (RTC SRAM counter)
struct RtcDrd {
  uint32_t mark;
  uint32_t cnt;
};
RTC_NOINIT_ATTR RtcDrd g_rtcDrd;
constexpr uint32_t kRtcMark = 0xB007C0DEu;

WebServer g_server(80);
DNSServer g_dns;
Preferences g_prefs;

String g_staSsid;
String g_staPass;
String g_sessionCookieValue; // empty = not logged in
String g_deviceName;
bool g_mdnsStarted = false;
bool g_apMode = false;
bool g_protoWebApi = true;
bool g_ipDhcp = true;
String g_staticIp;
String g_staticMask;
static constexpr const char *kFirmwareVersion = IONIX_FW_VERSION;
static constexpr const char *kFirmwareReleaseRepo = "dakn93/ionix-gpio";
static constexpr const char *kCompanionReleaseRepo = "dakn93/ionix-gpio-companion-module";
static constexpr const char *kFirmwareReleasePageUrl = "https://github.com/dakn93/ionix-gpio/releases/latest";
static constexpr const char *kCompanionModuleDownloadUrl = "https://github.com/dakn93/ionix-gpio-companion-module/releases/latest";
#if defined(CONFIG_IDF_TARGET_ESP32P4)
static constexpr const char *kFirmwareTargetId = "waveshare_esp32_p4_poe";
static constexpr const char *kFirmwareAssetSuffix = "waveshare_esp32_p4_poe-ota.bin";
#else
static constexpr const char *kFirmwareTargetId = "esp32dev";
static constexpr const char *kFirmwareAssetSuffix = "esp32dev-ota.bin";
#endif

#if defined(CONFIG_IDF_TARGET_ESP32P4)
constexpr int kMaxUserChannels = 22;
constexpr int kDefaultUserChannels = 22;
#else
constexpr int kMaxUserChannels = 12;
constexpr int kDefaultUserChannels = 4;
#endif
struct UserChannel {
  String name;
  bool isGpo;
  int pin;
};
UserChannel g_userChannels[kMaxUserChannels];
int g_userChannelCount = 0;

/** If false, GET / and GET /api/gpio work without session cookie (no login page). */
constexpr bool kRequireWebLogin = false;
constexpr const char *kWebUser = "ionix";
constexpr const char *kWebPass = "pass";
constexpr const char *kHotspotPass = "ionixpass";

void applyUserChannelsToRuntime();
bool parseIpv4(const String &s, IPAddress &out);
IPAddress guessGatewayFromIp(const IPAddress &ip);
static bool bringUpSoftAp();

#if defined(USE_ETHERNET_PORT)
void beginEthernetDhcp() {
  // Board-specific PHY settings must be provided via IONIX_ETH_* build flags.
#if !defined(IONIX_ETH_PHY_TYPE) || !defined(IONIX_ETH_PHY_ADDR) || !defined(IONIX_ETH_PHY_POWER) || !defined(IONIX_ETH_PHY_MDC) || !defined(IONIX_ETH_PHY_MDIO) || !defined(IONIX_ETH_CLK_MODE)
  Serial.println(F("[NET] USE_ETHERNET_PORT set, but IONIX_ETH_* flags are incomplete."));
  Serial.println(F("[NET] Required: PHY_TYPE/ADDR/POWER/MDC/MDIO/CLK_MODE."));
  return;
#else
#if defined(CONFIG_IDF_TARGET_ESP32P4)
  const bool ok = ETH.begin((eth_phy_type_t)IONIX_ETH_PHY_TYPE, IONIX_ETH_PHY_ADDR, IONIX_ETH_PHY_MDC, IONIX_ETH_PHY_MDIO,
                            IONIX_ETH_PHY_POWER, (eth_clock_mode_t)IONIX_ETH_CLK_MODE);
#else
  const bool ok = ETH.begin(IONIX_ETH_PHY_ADDR, IONIX_ETH_PHY_POWER, IONIX_ETH_PHY_MDC, IONIX_ETH_PHY_MDIO,
                            (eth_phy_type_t)IONIX_ETH_PHY_TYPE, (eth_clock_mode_t)IONIX_ETH_CLK_MODE);
#endif
  if (!ok) {
    Serial.println(F("[NET] Ethernet init failed."));
    return;
  }
  if (!g_ipDhcp) {
    IPAddress ip;
    IPAddress mask;
    if (parseIpv4(g_staticIp, ip) && parseIpv4(g_staticMask, mask)) {
      const IPAddress gw = guessGatewayFromIp(ip);
      ETH.config(ip, gw, mask, gw, gw);
    }
  }
  Serial.println(F("[NET] Ethernet started (DHCP)."));
#endif
}
#endif

String apIpStr() { return WiFi.softAPIP().toString(); }

String mdnsHostname() {
#if defined(USE_ETHERNET_PORT)
  String mac = ETH.macAddress();
#else
  String mac = WiFi.macAddress();
#endif
  mac.replace(":", "");
  mac.toLowerCase();
  String last4 = mac.substring(mac.length() - 4);
  String host = "ionix-gpio-" + last4;
  if (g_deviceName.length()) {
    String dn = g_deviceName;
    dn.toLowerCase();
    for (unsigned i = 0; i < dn.length(); i++) {
      char c = dn[i];
      if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'))) dn[i] = '-';
    }
    while (dn.length() && dn[0] == '-') dn = dn.substring(1);
    while (dn.length() && dn[dn.length()-1] == '-') dn = dn.substring(0, dn.length()-1);
    if (dn.length()) host += "-" + dn;
  }
  return host;
}

void startMdnsIfReady() {
  if (g_mdnsStarted) return;
  IPAddress ip;
#if defined(USE_ETHERNET_PORT)
  ip = (ETH.linkUp() && ETH.localIP() != IPAddress(0,0,0,0)) ? ETH.localIP() : WiFi.localIP();
#else
  ip = WiFi.localIP();
#endif
  if (ip == IPAddress(0,0,0,0)) return;
  String host = mdnsHostname();
  if (MDNS.begin(host.c_str())) {
    MDNS.addService("http", "tcp", 80);
    MDNS.addServiceTxt("http", "tcp", "ionix", "gpio");
    Serial.printf("[mDNS] %s.local\n", host.c_str());
    g_mdnsStarted = true;
  }
}

bool parseIpv4(const String &s, IPAddress &out) {
  IPAddress t;
  if (!t.fromString(s))
    return false;
  out = t;
  return true;
}

IPAddress guessGatewayFromIp(const IPAddress &ip) { return IPAddress(ip[0], ip[1], ip[2], 1); }

bool detectDoubleReset() {
  if (g_rtcDrd.mark != kRtcMark) {
    g_rtcDrd.mark = kRtcMark;
    g_rtcDrd.cnt = 0;
  }
  g_rtcDrd.cnt++;
  delay(1000);
  const bool drd = g_rtcDrd.cnt > 1;
  g_rtcDrd.cnt = 0;
  return drd;
}

void loadWifiCreds() {
  g_staSsid = g_prefs.getString("ssid", "");
  g_staPass = g_prefs.getString("pass", "");
}

void saveWifiCreds(const String &ssid, const String &pass) {
  g_prefs.putString("ssid", ssid);
  g_prefs.putString("pass", pass);
  g_staSsid = ssid;
  g_staPass = pass;
}

void loadProtocolPrefs() {
  g_deviceName = g_prefs.getString("dev_name", "");
  g_protoWebApi = g_prefs.getBool("proto_web", true);
  g_ipDhcp = g_prefs.getBool("ip_dhcp", true);
  g_staticIp = g_prefs.getString("ip_addr", "");
  g_staticMask = g_prefs.getString("ip_mask", "255.255.255.0");
}

static int defaultChannelPinForIndex(int i) {
#if defined(CONFIG_IDF_TARGET_ESP32P4)
  // P4 default preset: GPIO0/45 removed due board-level conflicts.
  // Fixed layout: 10x GPI + 12x GPO.
  static const int kP4PresetPins[kDefaultUserChannels] = {
      // User-provided usable pins (excluding reserved SDA/SCL/UART pins).
      22, 5, 4, 1, 36, 32, 25, 54, 46, 27, // GPI 1..10
      23, 21, 20, 6, 3, 2, 24, 33, 26, 48, 53, 47 // GPO 1..12
  };
  if (i >= 0 && i < kDefaultUserChannels)
    return kP4PresetPins[i];
#endif
  return i == 0 ? gpioChannelPin(0) : (i == 1 ? gpioChannelPin(1) : (i == 2 ? gpioChannelPin(2) : gpioChannelPin(3)));
}

static bool defaultChannelIsGpoForIndex(int i) {
#if defined(CONFIG_IDF_TARGET_ESP32P4)
  return i >= 10;
#else
  return i >= 2;
#endif
}

static String defaultChannelNameForIndex(int i) {
  const bool isGpo = defaultChannelIsGpoForIndex(i);
#if defined(CONFIG_IDF_TARGET_ESP32P4)
  const int ord = isGpo ? (i - 9) : (i + 1);
  return String(isGpo ? "GPO " : "GPI ") + String(ord);
#else
  return String(i < 2 ? "GPI " : "GPO ") + String((i % 2) + 1);
#endif
}

void loadUserChannelsFromNvs() {
  g_userChannelCount = g_prefs.getUChar("uc_cnt", kDefaultUserChannels);
  if (g_userChannelCount < 0)
    g_userChannelCount = 0;
  if (g_userChannelCount > kMaxUserChannels)
    g_userChannelCount = kMaxUserChannels;
  if (g_userChannelCount == 0)
    g_userChannelCount = kDefaultUserChannels;
  for (int i = 0; i < g_userChannelCount; i++) {
    char nk[12];
    char tk[12];
    char pk[12];
    snprintf(nk, sizeof(nk), "ucn%d", i);
    snprintf(tk, sizeof(tk), "uct%d", i);
    snprintf(pk, sizeof(pk), "ucp%d", i);
    g_userChannels[i].name = g_prefs.getString(nk, defaultChannelNameForIndex(i));
    g_userChannels[i].isGpo = g_prefs.getBool(tk, defaultChannelIsGpoForIndex(i));
    g_userChannels[i].pin = g_prefs.getInt(pk, defaultChannelPinForIndex(i));
  }
}

void saveUserChannelsToNvs() {
  g_prefs.putUChar("uc_cnt", (uint8_t)g_userChannelCount);
  for (int i = 0; i < g_userChannelCount; i++) {
    char nk[12];
    char tk[12];
    char pk[12];
    snprintf(nk, sizeof(nk), "ucn%d", i);
    snprintf(tk, sizeof(tk), "uct%d", i);
    snprintf(pk, sizeof(pk), "ucp%d", i);
    g_prefs.putString(nk, g_userChannels[i].name);
    g_prefs.putBool(tk, g_userChannels[i].isGpo);
    g_prefs.putInt(pk, g_userChannels[i].pin);
  }
}

void applyDefaultUserChannelsPreset() {
  g_userChannelCount = kDefaultUserChannels;
  if (g_userChannelCount < 1)
    g_userChannelCount = 1;
  if (g_userChannelCount > kMaxUserChannels)
    g_userChannelCount = kMaxUserChannels;
  for (int i = 0; i < g_userChannelCount; i++) {
    g_userChannels[i].name = defaultChannelNameForIndex(i);
    g_userChannels[i].isGpo = defaultChannelIsGpoForIndex(i);
    g_userChannels[i].pin = defaultChannelPinForIndex(i);
  }
  saveUserChannelsToNvs();
  applyUserChannelsToRuntime();
}

void applyUserChannelsToRuntime() {
  gpioSetRuntimeChannelCount(g_userChannelCount);
  for (int i = 0; i < g_userChannelCount; i++) {
    gpioSetChannelType(i, g_userChannels[i].isGpo);
    gpioSetChannelName(i, g_userChannels[i].name);
    gpioSetChannelPin(i, g_userChannels[i].pin);
  }
}

String apSsid() {
  String mac = WiFi.macAddress();
#if defined(USE_ETHERNET_PORT)
  const String ethMac = ETH.macAddress();
  if (ethMac.length())
    mac = ethMac;
#endif
  mac.toUpperCase();
  String tail4;
  for (int i = 0; i < (int)mac.length(); i++) {
    const char c = mac[i];
    if ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F'))
      tail4 += c;
  }
  if (tail4.length() >= 4)
    tail4 = tail4.substring(tail4.length() - 4);
  else if (tail4.length() == 0)
    tail4 = F("0000");
  char buf[40];
  snprintf(buf, sizeof(buf), "IONIX GPIO %s", tail4.c_str());
  return String(buf);
}

String apPassword() { return String(kHotspotPass); }

static constexpr const char *kBrandTitle = "IONIX I GPIO";

String htmlTextEscape(const String &s) {
  String o;
  o.reserve(s.length() + 8);
  for (unsigned i = 0; i < s.length(); i++) {
    const char c = s[i];
    if (c == '&')
      o += F("&amp;");
    else if (c == '<')
      o += F("&lt;");
    else if (c == '>')
      o += F("&gt;");
    else if (c == '"')
      o += F("&quot;");
    else
      o += c;
  }
  return o;
}

String htmlAttrEscape(const String &s) {
  String o;
  o.reserve(s.length() + 8);
  for (unsigned i = 0; i < s.length(); i++) {
    const char c = s[i];
    if (c == '&')
      o += F("&amp;");
    else if (c == '"')
      o += F("&quot;");
    else if (c == '<')
      o += F("&lt;");
    else
      o += c;
  }
  return o;
}

String urlDecode(const String &s) {
  String out;
  out.reserve(s.length());
  for (size_t i = 0; i < s.length(); i++) {
    const char ch = s[i];
    if (ch == '+') {
      out += ' ';
      continue;
    }
    if (ch == '%' && i + 2 < s.length()) {
      const char h1 = s[i + 1];
      const char h2 = s[i + 2];
      auto hex = [](char c) -> int {
        if (c >= '0' && c <= '9')
          return c - '0';
        if (c >= 'a' && c <= 'f')
          return 10 + (c - 'a');
        if (c >= 'A' && c <= 'F')
          return 10 + (c - 'A');
        return -1;
      };
      const int a = hex(h1);
      const int b = hex(h2);
      if (a >= 0 && b >= 0) {
        out += char((a << 4) | b);
        i += 2;
        continue;
      }
    }
    out += ch;
  }
  return out;
}

String formField(const String &body, const char *key) {
  const String pat = String(key) + "=";
  const int p = body.indexOf(pat);
  if (p < 0)
    return String();
  const int start = p + pat.length();
  const int amp = body.indexOf('&', start);
  const String raw = (amp < 0) ? body.substring(start) : body.substring(start, amp);
  return urlDecode(raw);
}

void parseFormFieldsUp(const String &body, String &uOut, String &pOut) {
  uOut = "";
  pOut = "";
  int pos = 0;
  while (pos < (int)body.length()) {
    int amp = body.indexOf('&', pos);
    if (amp < 0)
      amp = (int)body.length();
    const String pair = body.substring(pos, amp);
    pos = amp + 1;
    const int eq = pair.indexOf('=');
    if (eq <= 0)
      continue;
    const String key = urlDecode(pair.substring(0, eq));
    const String val = urlDecode(pair.substring(eq + 1));
    if (key == F("u"))
      uOut = val;
    else if (key == F("p"))
      pOut = val;
  }
}

String readPostBodyRaw(size_t maxLen) {
  String s;
  if (!g_server.hasHeader(F("Content-Length")))
    return s;
  const int len = g_server.header(F("Content-Length")).toInt();
  if (len <= 0 || (size_t)len > maxLen)
    return s;
  s.reserve((unsigned)len);
  WiFiClient cl = g_server.client();
  const uint32_t t0 = millis();
  while ((int)s.length() < len && millis() - t0 < 4000) {
    while (cl.available() && (int)s.length() < len) {
      s += (char)cl.read();
    }
    delay(1);
  }
  return s;
}

/** Prefer parsed form fields (ESP32 WebServer); only read raw body if needed. */
String postFormField(const char *key, size_t maxBodyFallback) {
  const String k(key);
  if (g_server.hasArg(k))
    return g_server.arg(k);
  const String raw = readPostBodyRaw(maxBodyFallback);
  return formField(raw, key);
}

String buildWifiScanOptionsHtml() {
  String html;
  html += F("<option value=\"\">— Select a network —</option>");
  WiFi.mode(WIFI_AP_STA);
  delay(50);
  const int n = WiFi.scanNetworks(false /* async */, true /* show hidden */, false /* passive */, 220);
  if (n <= 0) {
    html += F("<option value=\"\" disabled>(No networks found — try Rescan)</option>");
  } else {
    for (int i = 0; i < n; i++) {
      const String ssid = WiFi.SSID(i);
      if (!ssid.length())
        continue;
      html += F("<option value=\"");
      html += htmlAttrEscape(ssid);
      html += F("\">");
      html += htmlTextEscape(ssid);
      html += F(" (");
      html += String(WiFi.RSSI(i));
      html += F(" dBm)</option>");
    }
  }
  WiFi.scanDelete();
  return html;
}

// --- IONIX-Connect-style CSS (Cairo, colours, header, login, cards) ---
const char kCss[] = R"CSS(
:root {
  --bg-dark: #4a4d4e;
  --header-bg: #333333;
  --accent-green: #3eb368;
  --accent-dark-green: #2d824c;
  --input-bg: #3b3e3f;
  --text-white: #ffffff;
  --text-gray: #b0b0b0;
  --row-bg: #404040;
  --error-red: #ff4d4d;
  --shadow: 0 4px 15px rgba(0,0,0,0.4);
  --led-off: #4a4a4a;
  --led-on: #e53935;
}
* { box-sizing: border-box; margin: 0; padding: 0; font-family: 'Cairo', sans-serif; }
body { background: var(--bg-dark); color: var(--text-white); min-height: 100vh; display: flex; flex-direction: column; overflow-x: hidden; }

.app-header {
  display: flex; flex-direction: column; width: 100%; flex-shrink: 0;
  background: var(--header-bg); border-bottom: 1px solid #3d4041;
}
.header-main {
  height: 60px; display: flex; align-items: center; justify-content: space-between;
  padding: 0 25px; width: 100%;
}
.header-left { display: flex; align-items: center; gap: 12px; font-weight: 700; letter-spacing: 2px; font-size: 16px; }
.header-dino-icon { height: 32px; filter: drop-shadow(0 2px 3px rgba(0,0,0,0.3)); }
.header-right { display: flex; align-items: center; gap: 12px; }
.btn-header { background: rgba(0,0,0,0.2); border: 1px solid #555; color: #ddd; padding: 8px 14px; border-radius: 6px; font-size: 11px; font-weight: 700; cursor: pointer; letter-spacing: 0.5px; text-decoration: none; display: inline-block; }
.btn-header:hover { background: rgba(255,255,255,0.08); color: #fff; }
.btn-test-on { background: #b26b1a !important; border-color: #c57b23 !important; color: #fff !important; }

.page { display: none; flex: 1; flex-direction: column; width: 100%; }
.page.active { display: flex; }

.login-container { flex: 1; display: flex; flex-direction: column; align-items: center; justify-content: center; padding: 24px; }
.dino-wrapper { width: 240px; height: 200px; margin-bottom: 30px; filter: drop-shadow(0 10px 20px rgba(0,0,0,0.5)); }
.trex-leg-back { animation: walkBack 1s infinite linear; transform-origin: 45px 65px; }
.trex-leg-front { animation: walkFront 1s infinite linear; transform-origin: 55px 65px; }
.trex-body { animation: bodyMove 1s infinite ease-in-out; transform-origin: 50px 70px; }
.trex-jaw { animation: talk 0.25s infinite alternate; transform-origin: 72px 42px; }
@keyframes walkBack { 0% { transform: rotate(15deg) translateY(-2px); } 50% { transform: rotate(-15deg) translateY(0); } 100% { transform: rotate(15deg) translateY(-2px); } }
@keyframes walkFront { 0% { transform: rotate(-15deg) translateY(0); } 50% { transform: rotate(15deg) translateY(-2px); } 100% { transform: rotate(-15deg) translateY(0); } }
@keyframes bodyMove { 0%, 100% { transform: rotate(0deg) translateY(0); } 50% { transform: rotate(1deg) translateY(2px); } }
@keyframes talk { 0% { transform: rotate(0deg); } 100% { transform: rotate(8deg); } }

.form-group { width: 100%; max-width: 320px; margin-bottom: 15px; }
.form-group input, .form-group select {
  width: 100%; height: 45px; background: var(--input-bg); border: 1px solid #444; border-radius: 8px; color: white;
  padding: 0 15px; font-size: 16px; outline: none;
}
.form-group input:focus, .form-group select:focus { border-color: var(--accent-green); box-shadow: 0 0 10px rgba(62,179,104,0.3); }
.login-btn {
  width: 100%; max-width: 320px; background: var(--accent-green); color: white; border: none; padding: 12px; border-radius: 8px;
  font-weight: 700; cursor: pointer; letter-spacing: 1px; font-size: 14px;
}
.login-btn:hover { background: #4ec77d; }
#login-error { color: var(--error-red); margin-top: 12px; font-size: 13px; display: none; }

.main-scroll { flex: 1; overflow-y: auto; padding: 20px 24px 40px; width: 100%; max-width: 1120px; margin: 0 auto; }
.section-title { font-size: 11px; font-weight: 700; color: #888; text-transform: uppercase; letter-spacing: 1px; margin-bottom: 12px; border-bottom: 1px solid #444; padding-bottom: 8px; }
.card {
  background: var(--input-bg); border: 1px solid #555; border-radius: 10px; padding: 16px 18px; margin-bottom: 18px;
  box-shadow: var(--shadow);
}
.msg { font-size: 12px; color: var(--text-gray); margin-top: 8px; }
.msg.ok { color: #8ee0aa; }
.msg.err { color: #ff9b9b; }

.led-grid { display: flex; flex-direction: column; gap: 8px; align-items: center; }
.gpio-table-head { width: 100%; display: grid; grid-template-columns: 42px 1.4fr 0.65fr 0.7fr 0.9fr 0.9fr 0.8fr 0.9fr; gap: 8px; margin-bottom: 4px; }
.gpio-table-head div { text-align: center; font-size: 10px; color: #a7a7a7; text-transform: uppercase; letter-spacing: 0.45px; font-weight: 700; }
.gpio-group-gap { height: 16px; width: 100%; }
.led-tile {
  width: 100%;
  background: #2a2a2a; border: 1px solid #444; border-radius: 9px; padding: 7px 8px;
  display: grid; grid-template-columns: 42px 1.4fr 0.65fr 0.7fr 0.9fr 0.9fr 0.8fr 0.9fr; gap: 8px;
  align-items: center;
  cursor: pointer; user-select: none; touch-action: none;
}
.led-tile.holding { border-color: #3eb368; box-shadow: 0 0 0 1px rgba(62,179,104,0.35); }
.led-tile .label { font-size: 12px; font-weight: 700; text-align: center; overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
.type-chip { text-align: center; font-size: 10px; border: 1px solid #555; border-radius: 6px; padding: 7px 6px; color: #d3d3d3; background: #232323; font-weight: 700; letter-spacing: 0.4px; }
.led-bulb {
  width: 22px; height: 22px; border-radius: 7px;
  background: #523b3b; border: 1px solid #2e2e2e;
  margin: 0 auto; opacity: 0.7;
}
.led-bulb.on {
  background: radial-gradient(circle at 35% 35%, #ff6b63, #d43730);
  border-color: #d64f4a; opacity: 1;
  box-shadow: 0 0 10px rgba(229, 57, 53, 0.65);
}
button.primary { background: var(--accent-dark-green); color: #fff; border: none; padding: 10px 18px; border-radius: 8px; font-weight: 700; font-size: 12px; letter-spacing: 0.5px; cursor: pointer; }
button.primary:hover { background: var(--accent-green); }

.status-led { display: inline-block; width: 14px; height: 14px; border-radius: 50%; background: #555; margin-left: 10px; vertical-align: middle; border: 1px solid #333; }
.status-led.ok { background: radial-gradient(circle at 30% 30%, #a5f5c4, #3eb368); box-shadow: 0 0 10px rgba(62,179,104,0.7); }
.status-led.wait { background: #c9a227; box-shadow: 0 0 8px rgba(201,162,39,0.5); }

.led-col { display: contents; }
.row-select, .row-btn {
  width: 100%; height: 30px; border-radius: 6px; border: 1px solid #4f4f4f; background: #202224; color: #f0f0f0;
  font-size: 10px; font-weight: 700; letter-spacing: 0.35px; text-transform: uppercase; padding: 0 6px; text-align: center;
}
.row-btn { cursor: pointer; }
.row-btn:hover { border-color: #6c6c6c; background: #2a2b2c; }
.row-btn:disabled, .row-select:disabled { opacity: 0.5; cursor: default; }
.inv-btn.high { border-color: #2f8f53; color: #c5f7d5; }
.inv-btn.low { border-color: #9b6d27; color: #ffe2b4; }
.dashboard-title { text-align: center; font-size: 34px; font-weight: 700; letter-spacing: 2px; margin-top: 22px; }
.dashboard-sub { text-align: center; font-size: 11px; color: #b8b8b8; letter-spacing: 1.3px; margin-bottom: 22px; }
.atem-map-badge { display: inline-block; margin-left: 8px; padding: 2px 7px; border-radius: 999px; font-size: 9px; letter-spacing: 0.4px; border: 1px solid #555; color: #bbb; background: #2c2c2c; }
.atem-map-badge.mapped { border-color: #2f8f53; color: #bff4d0; background: #1f3528; }
.atem-map-badge.unmapped { border-color: #785b1c; color: #f2d99a; background: #3a311a; }

.header-ip-bar {
  padding: 9px 25px 11px;
  background: #1a1c1d;
  color: #dff5ea;
  font-size: 13px;
  font-weight: 700;
  letter-spacing: 0.35px;
  border-bottom: 1px solid #333;
}
#test_mode_banner {
  display: none;
  width: 100%;
  background: #b26b1a;
  color: #fff;
  text-align: center;
  font-weight: 800;
  font-size: 12px;
  letter-spacing: 0.8px;
  padding: 8px 10px;
  border-bottom: 1px solid #8f5514;
}
#test_mode_banner.show { display: block; }
.section-title-row { display: flex; align-items: center; justify-content: space-between; gap: 10px; flex-wrap: wrap; margin-bottom: 10px; border-bottom: 1px solid #444; padding-bottom: 8px; }
.section-title-row .section-title { margin-bottom: 0; border-bottom: none; padding-bottom: 0; }
.info-icon-btn {
  width: 26px; height: 26px; border-radius: 50%;
  border: 1px solid #666; background: #2a2c2d; color: #ccc;
  font-size: 12px; font-weight: 800; cursor: pointer; line-height: 1; padding: 0;
  flex-shrink: 0;
}
.info-icon-btn:hover { border-color: var(--accent-green); color: #fff; }
.info-panel {
  display: none;
  font-size: 12px; color: var(--text-gray); line-height: 1.45;
  margin: 0 0 12px 0; padding: 12px; background: #2a2c2d; border: 1px solid #444; border-radius: 8px;
}
.info-panel.show { display: block; }
.set-head { display:grid; grid-template-columns: 1.4fr 0.7fr 0.7fr 44px; gap:8px; margin-bottom:6px; }
.set-head div { text-align:center; font-size:10px; color:#a7a7a7; text-transform:uppercase; letter-spacing:0.45px; font-weight:700; }
.set-row { display:grid; grid-template-columns: 1.4fr 0.7fr 0.7fr 44px; gap:8px; align-items:center; margin-bottom:8px; background:#2a2a2a; border:1px solid #444; border-radius:9px; padding:7px 8px; }
.set-row .del-btn { width:34px; height:30px; border-radius:6px; border:1px solid #555; background:#2a2c2d; color:#bbb; cursor:pointer; }
.set-row .del-btn:hover { border-color:#ff4d4d; color:#ff4d4d; box-shadow:0 0 8px rgba(255,77,77,0.35); }
.set-row input, .set-row select { width:100%; height:30px; background:#202224; border:1px solid #4f4f4f; border-radius:6px; color:#fff; padding:0 8px; font-size:11px; font-weight:700; letter-spacing:0.25px; }
.settings-toolbar { display:flex; justify-content:flex-end; margin-bottom:10px; }
.btn-add-green { width:34px; height:34px; border-radius:50%; border:1px solid #2f9b59; background:#3eb368; color:#fff; font-weight:900; font-size:20px; line-height:1; cursor:pointer; }
.btn-add-green:hover { background:#4ec77d; box-shadow:0 0 8px rgba(62,179,104,0.45); }
.btn-cancel-red { background:#b43c3c !important; border-color:#9a3333 !important; color:#fff !important; }
.btn-cancel-red:hover { background:#cc4a4a !important; }
#hdr_settings_btn, #hdr_test_btn {
  width: 54px;
  height: 32px;
  padding: 0;
  line-height: 1;
  text-align: center;
  display: inline-flex;
  align-items: center;
  justify-content: center;
}
#hdr_settings_btn { font-size: 18px; }
#hdr_test_btn { font-size: 12px; }

.modal-overlay { display: none; position: fixed; inset: 0; background: rgba(0,0,0,0.55); z-index: 200; align-items: center; justify-content: center; padding: 16px; }
.modal-overlay.show { display: flex; }
.modal-box { background: #2f3233; border: 1px solid #555; border-radius: 12px; padding: 20px; max-width: 400px; width: 100%; box-shadow: var(--shadow); }
.modal-overlay#settings_modal .modal-box { max-height: calc(100vh - 24px); overflow-y: auto; overflow-x: hidden; }
.modal-box h3 { font-size: 14px; margin-bottom: 12px; }
.modal-actions { display: flex; gap: 10px; margin-top: 16px; justify-content: flex-end; flex-wrap: wrap; }
)CSS";

const char kSvgDino[] =
    "<svg class=\"header-dino-icon\" viewBox=\"0 0 120 100\"><g><path d=\"M45,65 Q40,80 40,90 H52 Q50,80 55,65 Z\" "
    "fill=\"#2d824c\"/><path d=\"M40,90 L38,93 L54,93 L52,90 Z\" fill=\"#2d824c\"/></g><g><path d=\"M30,60 C10,55 5,45 "
    "20,40 C35,35 50,45 60,45 L70,45 C75,55 70,65 55,75 C45,80 30,65 30,60 Z\" fill=\"#3eb368\"/><path d=\"M60,45 "
    "Q70,25 75,25 L85,25 Q80,45 70,50 Z\" fill=\"#3eb368\"/><path d=\"M75,25 H95 Q105,25 105,32 Q105,40 95,40 H80 "
    "Q75,40 75,25 Z\" fill=\"#3eb368\"/><path d=\"M78,40 H95 Q100,40 100,43 Q100,48 95,48 H80 Q75,48 78,40 Z\" "
    "fill=\"#3eb368\"/><circle cx=\"88\" cy=\"29\" r=\"1.8\" fill=\"#222\"/><circle cx=\"89\" cy=\"28.5\" r=\"0.6\" "
    "fill=\"white\"/><rect x=\"70\" y=\"26\" width=\"6\" height=\"11\" rx=\"2\" fill=\"#222\"/><rect x=\"71\" y=\"26\" "
    "width=\"4\" height=\"11\" rx=\"1\" fill=\"none\" stroke=\"#333\" stroke-width=\"0.5\"/><path d=\"M73,26 C73,20 "
    "85,20 88,23\" stroke=\"#222\" stroke-width=\"1.8\" fill=\"none\" stroke-linecap=\"round\"/><path d=\"M73,33 "
    "Q80,44 98,42\" stroke=\"#222\" stroke-width=\"1.5\" fill=\"none\" stroke-linecap=\"round\"/><rect x=\"96\" y=\"40\" "
    "width=\"5\" height=\"4\" rx=\"1\" fill=\"#111\"/><rect x=\"99\" y=\"40\" width=\"1.5\" height=\"4\" "
    "fill=\"#3eb368\"/><g><path d=\"M62,55 Q65,65 72,62\" stroke=\"#3eb368\" stroke-width=\"3\" fill=\"none\" "
    "stroke-linecap=\"round\"/><path d=\"M72,62 L74,60 M72,62 L74,64\" stroke=\"#3eb368\" stroke-width=\"2\" fill=\"none\" "
    "stroke-linecap=\"round\"/></g></g><g><path d=\"M60,65 Q55,80 55,90 H67 Q65,80 70,65 Z\" fill=\"#3eb368\"/><path "
    "d=\"M55,90 L53,93 L69,93 L67,90 Z\" fill=\"#3eb368\"/></g></svg>";

String htmlShell(const String &title, const String &bodyInner, bool showHeader, bool showLogout, const String &belowHeaderStrip = String()) {
  String h;
  h.reserve(12000);
  h += F("<!DOCTYPE html><html lang=\"en\"><head><meta charset=\"UTF-8\"/>");
  h += F("<meta name=\"viewport\" content=\"width=device-width, initial-scale=1.0\"/>");
  h += F("<title>");
  if (g_deviceName.length()) {
    String t = title;
    t.replace(kBrandTitle, (String(kBrandTitle) + " [ " + g_deviceName + " ]").c_str());
    h += t;
  } else {
    h += title;
  }
  h += F("</title>");
  h += F("<link rel=\"icon\" type=\"image/svg+xml\" href=\"data:image/svg+xml,"
         "<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 100 100'>"
         "<path d='M20,55 C8,50 5,38 18,33 C30,28 45,38 55,38 L62,38 C67,48 62,58 48,65 C38,70 20,60 20,55Z' fill='%233eb368'/>"
         "<path d='M55,38 Q62,20 67,20 L74,20 Q71,38 62,42Z' fill='%233eb368'/>"
         "<circle cx='63' cy='27' r='3' fill='%23000'/>"
         "<path d='M44,68 Q40,80 40,90 H50 Q49,80 52,68Z' fill='%232d824c'/>"
         "<path d='M51,66 Q47,78 47,90 H57 Q56,78 59,66Z' fill='%233eb368'/>"
         "</svg>\"/>");
  h += F("<link href=\"https://fonts.googleapis.com/css2?family=Cairo:wght@300;400;600;700&display=swap\" rel=\"stylesheet\">");
  h += F("<style>");
  h += kCss;
  h += F("</style></head><body>");

  if (showHeader) {
    h += F("<header class=\"app-header\"><div class=\"header-main\"><div class=\"header-left\">");
    h += kSvgDino;
    h += F("<span>");
    h += kBrandTitle;
    h += F("</span></div><div class=\"header-right\">");
    if (!g_apMode) {
      h += F("<button type=\"button\" class=\"btn-header\" id=\"hdr_test_btn\" title=\"Test mode\">TEST</button>");
      h += F("<button type=\"button\" class=\"btn-header\" id=\"hdr_settings_btn\" title=\"Settings\">&#9881;</button>");
    }
    if (showLogout) {
      h += F("<a class=\"btn-header\" href=\"/logout\">LOGOUT</a>");
    }
    h += F("</div></div>");
    if (belowHeaderStrip.length())
      h += belowHeaderStrip;
    h += F("</header>");
    if (!g_apMode)
      h += F("<div id=\"test_mode_banner\">TEST MODE IS ACTIVATED!</div>");
  }

  h += bodyInner;
  h += F("</body></html>");
  return h;
}

String pageLogin(bool showAuthError) {
  String inner;
  inner += F("<main class=\"page active\"><div class=\"login-container\">");
  inner += F("<div class=\"dino-wrapper\"><svg viewBox=\"0 0 120 100\">");
  inner += F("<g class=\"trex-leg-back\"><path d=\"M45,65 Q40,80 40,90 H52 Q50,80 55,65 Z\" fill=\"#2d824c\"/><path "
             "d=\"M40,90 L38,93 L54,93 L52,90 Z\" fill=\"#2d824c\"/></g>");
  inner += F("<g class=\"trex-body\"><path d=\"M30,60 C10,55 5,45 20,40 C35,35 50,45 60,45 L70,45 C75,55 70,65 55,75 "
             "C45,80 30,65 30,60 Z\" fill=\"#3eb368\"/><path d=\"M60,45 Q70,25 75,25 L85,25 Q80,45 70,50 Z\" "
             "fill=\"#3eb368\"/><path d=\"M75,25 H95 Q105,25 105,32 Q105,40 95,40 H80 Q75,40 75,25 Z\" "
             "fill=\"#3eb368\"/><g class=\"trex-jaw\"><path d=\"M78,40 H95 Q100,40 100,43 Q100,48 95,48 H80 Q75,48 "
             "78,40 Z\" fill=\"#3eb368\"/></g><circle cx=\"88\" cy=\"29\" r=\"1.8\" fill=\"#222\"/><circle cx=\"89\" "
             "cy=\"28.5\" r=\"0.6\" fill=\"white\"/><rect x=\"70\" y=\"26\" width=\"6\" height=\"11\" rx=\"2\" "
             "fill=\"#222\"/><rect x=\"71\" y=\"26\" width=\"4\" height=\"11\" rx=\"1\" fill=\"none\" stroke=\"#333\" "
             "stroke-width=\"0.5\"/><path d=\"M73,26 C73,20 85,20 88,23\" stroke=\"#222\" stroke-width=\"1.8\" "
             "fill=\"none\" stroke-linecap=\"round\"/><path d=\"M73,33 Q80,44 98,42\" stroke=\"#222\" stroke-width=\"1.5\" "
             "fill=\"none\" stroke-linecap=\"round\"/><rect x=\"96\" y=\"40\" width=\"5\" height=\"4\" rx=\"1\" "
             "fill=\"#111\"/><rect x=\"99\" y=\"40\" width=\"1.5\" height=\"4\" fill=\"#3eb368\"/><g "
             "class=\"trex-arm\"><path d=\"M62,55 Q65,65 72,62\" stroke=\"#3eb368\" stroke-width=\"3\" fill=\"none\" "
             "stroke-linecap=\"round\"/><path d=\"M72,62 L74,60 M72,62 L74,64\" stroke=\"#3eb368\" stroke-width=\"2\" "
             "fill=\"none\" stroke-linecap=\"round\"/></g></g>");
  inner += F("<g class=\"trex-leg-front\"><path d=\"M60,65 Q55,80 55,90 H67 Q65,80 70,65 Z\" fill=\"#3eb368\"/><path "
             "d=\"M55,90 L53,93 L69,93 L67,90 Z\" fill=\"#3eb368\"/></g></svg></div>");
  inner += F("<form method=\"post\" action=\"/login\" autocomplete=\"on\">");
  inner += F("<div class=\"form-group\"><input name=\"u\" type=\"text\" placeholder=\"Username\" "
             "autocomplete=\"username\"/></div>");
  inner += F("<div class=\"form-group\"><input name=\"p\" type=\"password\" placeholder=\"Password\" "
             "autocomplete=\"current-password\"/></div>");
  inner += F("<button class=\"login-btn\" type=\"submit\">LOGIN</button></form>");
  if (showAuthError) {
    inner += F("<p id=\"login-error\" style=\"display:block;\">Invalid username or password. Use <strong>ionix</strong> / "
               "<strong>pass</strong>.</p>");
  } else {
    inner += F("<p id=\"login-error\"></p>");
  }
  inner += F("<p class=\"msg\" style=\"margin-top:18px;font-size:10px;color:#777;\">Default login: ionix / pass · GPI ");
  inner += String(gpioChannelPin(0));
  inner += F("/");
  inner += String(gpioChannelPin(1));
  inner += F(" · GPO ");
  inner += String(gpioChannelPin(2));
  inner += F("/");
  inner += String(gpioChannelPin(3));
  inner += F("</p></div></main>");
  return htmlShell(F("IONIX I GPIO — Login"), inner, /*header*/ true, false);
}

uint32_t dashboardStateSig() { return gpioDashboardStateSig() ^ (atemLiveSig() * UINT32_C(2654435761)); }

String buildDashboardHeaderStrip() {
  const NetworkStatusSnapshot net = readNetworkStatus();
  String wifiDisplayIp = net.wifiIp;
  if (wifiDisplayIp == F("-"))
    wifiDisplayIp = net.hotspotIp;

  String strip;
  strip.reserve(192);
  strip += F("<div class=\"header-ip-bar\">Adapter IP: <span id=\"hdr-physical-ip\">");
  strip += htmlTextEscape(net.physicalIp);
  strip += F("</span> &nbsp;|&nbsp; WIFI IP: <span id=\"hdr-wifi-ip\">");
  strip += htmlTextEscape(wifiDisplayIp);
  strip += F("</span></div>");
  return strip;
}

String pageDashboard() {
  String inner;
  inner += F("<main class=\"page active\"><div class=\"main-scroll\">");
  inner += F("<div class=\"dashboard-title\">IONIX I GPIO<span id=\"dn-disp\" style=\"color:#3eb368\">");
  if (g_deviceName.length()) {
    inner += F(" [ ");
    inner += htmlTextEscape(g_deviceName);
    inner += F(" ]");
  }
  inner += F("</span></div>");
  inner += F("<div class=\"dashboard-sub\" id=\"dn-btn\" style=\"cursor:pointer;user-select:none\" title=\"Click to set a custom device name\">CHANGE DEVICE NAME &#9998;</div>");
  inner += F("<div id=\"dn-form\" style=\"display:none;margin:8px auto 0;max-width:300px\">");
  inner += F("<input id=\"dn-inp\" type=\"text\" maxlength=\"32\" placeholder=\"Device name (e.g. MAIN)\" style=\"width:100%;height:36px;background:#1e2a22;border:1px solid #3eb368;border-radius:6px;color:#fff;padding:0 10px;font-size:13px;outline:none;font-family:inherit\"/>");
  inner += F("<div style=\"margin-top:5px;font-size:11px;color:#888;text-align:center\">Press Enter to confirm &nbsp;&middot;&nbsp; Click outside to cancel</div>");
  inner += F("</div>");
  inner += F("<div class=\"gpio-table-head\"><div></div><div>User Label</div><div>Type</div><div>ATEM ME</div><div>ATEM IN</div><div>ATEM TALLY</div><div>REST API</div><div>Invert Logic</div></div>");
  inner += F("<div class=\"led-grid\">");

  auto gpioCard = [&](int userIdx, int runtimeCh, bool isGpo, int pinNum, const String &name) {
    const bool active = runtimeCh >= 0 && runtimeCh < gpioRuntimeChannelCount();
    const bool inv = active ? gpioGetInvertChannel(runtimeCh) : false;
    const String bulbId = String("l_ch_") + String(active ? runtimeCh : userIdx);
    inner += F("<div class=\"led-tile\" data-ch=\"");
    inner += String(runtimeCh);
    inner += F("\" data-active=\"");
    inner += active ? F("1") : F("0");
    inner += F("\"><div class=\"led-bulb\" id=\"");
    inner += bulbId;
    inner += F("\"></div><div class=\"label\" id=\"ttl_");
    inner += String(active ? runtimeCh : userIdx + 200);
    inner += F("\">");
    inner += htmlTextEscape(name);
    inner += F("</div><div class=\"type-chip\">");
    inner += isGpo ? F("GPO") : F("GPI");
    inner += F("</div>");

    if (isGpo) {
      inner += F("<select class=\"row-select row-control\" id=\"atem_me");
      inner += String(runtimeCh);
      inner += F("\"><option value=\"255\">ALL ME</option><option value=\"1\">ME 1</option><option value=\"2\">ME 2</option><option value=\"3\">ME 3</option><option value=\"4\">ME 4</option></select>");
      inner += F("<select class=\"row-select row-control\" id=\"atem_in");
      inner += String(runtimeCh);
      inner += F("\"><option value=\"0\">INPUT 0</option></select>");
      inner += F("<select class=\"row-select row-control\" id=\"atem_tt");
      inner += String(runtimeCh);
      inner += F("\"><option value=\"0\">OFF</option><option value=\"1\">RED</option><option value=\"2\">GREEN</option></select>");
    } else {
      inner += F("<button type=\"button\" class=\"row-btn\" disabled>-</button><button type=\"button\" class=\"row-btn\" disabled>-</button><button type=\"button\" class=\"row-btn\" disabled>-</button>");
    }

    if (g_protoWebApi)
      inner += F("<button type=\"button\" class=\"row-btn row-control proto-copy-btn\" data-copy-text=\"/api/gpio\">COPY PATH</button>");
    else
      inner += F("<button type=\"button\" class=\"row-btn\" disabled>-</button>");

    inner += F("<button type=\"button\" class=\"row-btn row-control inv-btn ");
    inner += inv ? F("low") : F("high");
    inner += F("\" id=\"inv_btn_");
    inner += String(runtimeCh);
    inner += F("\" data-ch=\"");
    inner += String(runtimeCh);
    inner += F("\">");
    inner += inv ? F("LOW") : F("HIGH");
    inner += F("</button></div>");
  };

  for (int i = 0; i < g_userChannelCount; i++) {
    if (g_userChannels[i].isGpo)
      continue;
    const int runtimeCh = i;
    gpioCard(i, runtimeCh, false, g_userChannels[i].pin, g_userChannels[i].name);
  }
  bool hasGpi = false;
  bool hasGpo = false;
  for (int i = 0; i < g_userChannelCount; i++) {
    if (g_userChannels[i].isGpo)
      hasGpo = true;
    else
      hasGpi = true;
  }
  if (hasGpi && hasGpo)
    inner += F("<div class=\"gpio-group-gap\"></div>");
  for (int i = 0; i < g_userChannelCount; i++) {
    if (!g_userChannels[i].isGpo)
      continue;
    const int runtimeCh = i;
    gpioCard(i, runtimeCh, true, g_userChannels[i].pin, g_userChannels[i].name);
  }

  inner += F("</div><p class=\"msg\" id=\"poll-msg\"></p>");
  inner += F("<div class=\"modal-overlay\" id=\"settings_modal\"><div class=\"modal-box\" style=\"max-width:880px;\"><h3>Settings</h3>");
  inner += F("<p class=\"msg\" style=\"margin-bottom:12px;\">GPIO list is saved persistently in NVS.</p>");
  inner += F("<div class=\"card\" style=\"margin-bottom:12px;padding:12px;\"><div class=\"section-title\">Protocol</div>");
  inner += F("<div style=\"display:flex;align-items:center;flex-wrap:wrap;gap:12px;margin-bottom:4px;\">");
  inner += F("<label style=\"cursor:pointer;font-size:13px;\"><input type=\"checkbox\" id=\"proto_webapi\" style=\"margin-right:8px;\"/> Enable Web API</label>");
  inner += F("<label style=\"cursor:pointer;font-size:13px;\"><input type=\"checkbox\" id=\"atem_en\" style=\"margin-right:8px;\"/> Enable BMD ATEM PROTOCOL</label>");
  inner += F("<input type=\"text\" id=\"atem_ip_in\" placeholder=\"ATEM IP (e.g. 192.168.1.240)\" autocomplete=\"off\" style=\"max-width:220px;\"/>");
  inner += F("<span class=\"status-led\" id=\"atem_led\"></span></div><p class=\"msg\" id=\"atem_msg\" style=\"margin-top:8px;\"></p></div>");
  inner += F("<div class=\"card\" style=\"margin-bottom:12px;padding:12px;\"><div class=\"section-title\">Companion Module</div>");
  inner += F("<p class=\"msg\" style=\"margin-bottom:10px;\">Download the latest IONIX Companion package (.tgz) from the public GitHub release.</p>");
  inner += F("<p class=\"msg\" id=\"companion_release_status\" style=\"margin-bottom:10px;\">Checking GitHub release status...</p>");
  inner += F("<div style=\"display:flex;align-items:center;gap:10px;flex-wrap:wrap;\">");
  inner += F("<button type=\"button\" class=\"btn-header\" id=\"companion_dl_btn\">DOWNLOAD LATEST</button>");
  inner += F("<button type=\"button\" class=\"btn-header\" id=\"companion_release_btn\">OPEN RELEASE</button>");
  inner += F("</div></div>");
  inner += F("<div class=\"card\" style=\"margin-bottom:12px;padding:12px;\"><div class=\"section-title\">Firmware Update (OTA)</div>");
  inner += F("<p class=\"msg\" style=\"margin-bottom:6px;\">Installed firmware: <code id=\"ota_cur_ver\">");
  inner += String(kFirmwareVersion);
  inner += F("</code></p>");
  inner += F("<p class=\"msg\" id=\"ota_release_status\" style=\"margin-bottom:6px;\">Checking GitHub release status...</p>");
  inner += F("<p class=\"msg\" id=\"ota_release_meta\" style=\"margin-bottom:10px;display:none;\"></p>");
  inner += F("<div style=\"display:flex;align-items:center;gap:10px;flex-wrap:wrap;margin-bottom:10px;\">");
  inner += F("<button type=\"button\" class=\"btn-header\" id=\"ota_check_btn\">CHECK NOW</button>");
  inner += F("<button type=\"button\" class=\"btn-header\" id=\"ota_download_btn\" style=\"display:none;\">DOWNLOAD UPDATE</button>");
  inner += F("<button type=\"button\" class=\"btn-header\" id=\"ota_release_btn\">OPEN RELEASE</button>");
  inner += F("</div>");
  inner += F("<p class=\"msg\" style=\"margin-bottom:10px;\">Download the newest OTA file, then select the <code>.bin</code> file below to flash over the network. The device will reboot automatically.</p>");
  inner += F("<p class=\"msg\" style=\"margin-bottom:10px;\">mDNS hostname: <code id=\"ota_mdns_host\">-</code>.local</p>");
  inner += F("<div style=\"display:flex;align-items:center;gap:10px;flex-wrap:wrap;\">");
  inner += F("<input type=\"file\" id=\"ota_file\" accept=\".bin\" style=\"color:#ccc;font-size:13px;\"/>");
  inner += F("<button type=\"button\" class=\"btn-header\" id=\"ota_flash_btn\">FLASH SELECTED FILE</button>");
  inner += F("</div>");
  inner += F("<div style=\"margin-top:8px;display:none\" id=\"ota_progress_wrap\">");
  inner += F("<div style=\"background:#222;border-radius:4px;height:8px;width:100%;overflow:hidden;\">");
  inner += F("<div id=\"ota_progress_bar\" style=\"background:#3eb368;height:100%;width:0%;transition:width 0.3s;\"></div></div>");
  inner += F("<p class=\"msg\" id=\"ota_msg\" style=\"margin-top:6px;\"></p></div></div>");
  inner += F("<div class=\"card\" style=\"margin-bottom:12px;padding:12px;\"><div class=\"section-title\">IP Configuration</div>");
  inner += F("<div class=\"msg\" style=\"margin-bottom:4px;\">Primary IP: <span id=\"net_cur_ip\">-</span> | Adapter IP: <span id=\"net_phys_ip\">-</span></div>");
  inner += F("<div class=\"msg\" style=\"margin-bottom:8px;\">WIFI IP: <span id=\"net_wifi_ip\">-</span> | MAC: <span id=\"net_cur_mac\">-</span></div>");
  inner += F("<div style=\"display:flex;align-items:center;gap:12px;flex-wrap:wrap;\">");
  inner += F("<label style=\"cursor:pointer;font-size:13px;\"><input type=\"radio\" name=\"ip_mode\" id=\"ip_mode_dhcp\" checked style=\"margin-right:6px;\"/>DHCP</label>");
  inner += F("<label style=\"cursor:pointer;font-size:13px;\"><input type=\"radio\" name=\"ip_mode\" id=\"ip_mode_manual\" style=\"margin-right:6px;\"/>Manual</label>");
  inner += F("<input type=\"text\" id=\"net_ip_in\" placeholder=\"IP (e.g. 192.168.1.120)\" style=\"max-width:220px;\"/>");
  inner += F("<input type=\"text\" id=\"net_mask_in\" placeholder=\"Subnet (e.g. 255.255.255.0)\" style=\"max-width:220px;\"/>");
  inner += F("<button type=\"button\" class=\"btn-header\" id=\"net_save_btn\">SAVE IP</button>");
  inner += F("</div><p class=\"msg\" id=\"net_msg\" style=\"margin-top:8px;\"></p></div>");
  inner += F("<form method=\"post\" action=\"/settings/channels\" id=\"set_form\">");
  inner += F("<input type=\"hidden\" name=\"count\" id=\"set_count\" value=\"");
  inner += String(g_userChannelCount);
  inner += F("\"/>");
  inner += F("<div class=\"settings-toolbar\" style=\"gap:10px;\">");
  inner += F("<button type=\"button\" class=\"btn-header\" id=\"set_default_btn\" title=\"Restore default mapping\">DEFAULT</button>");
  inner += F("<button type=\"button\" class=\"btn-add-green\" title=\"Add GPIO\" id=\"set_add_btn\">+</button></div>");
  inner += F("<div class=\"set-head\"><div>User Label</div><div>Type</div><div>Pin</div><div></div></div>");
  inner += F("<div id=\"set_rows\">");
  #if defined(CONFIG_IDF_TARGET_ESP32P4)
  const int setChoices[] = {22, 5, 4, 1, 36, 32, 25, 54, 46, 27, 23, 21, 20, 6, 3, 2, 24, 33, 26, 48, 53, 47};
  #else
  const int setChoices[] = {4, 5, 12, 13, 14, 15, 16, 17, 18, 19, 21, 22, 23, 25, 26, 27, 32, 33};
  #endif
  for (int i = 0; i < g_userChannelCount; i++) {
    inner += F("<div class=\"set-row\" data-row=\"");
    inner += String(i);
    inner += F("\"><div><input type=\"text\" maxlength=\"24\" name=\"name");
    inner += String(i);
    inner += F("\" value=\"");
    inner += htmlAttrEscape(g_userChannels[i].name);
    inner += F("\"/></div><div><select name=\"type");
    inner += String(i);
    inner += F("\"><option value=\"gpi\"");
    if (!g_userChannels[i].isGpo)
      inner += F(" selected");
    inner += F(">GPI</option><option value=\"gpo\"");
    if (g_userChannels[i].isGpo)
      inner += F(" selected");
    inner += F(">GPO</option></select></div><div><select name=\"pin");
    inner += String(i);
    inner += F("\" data-pin-select=\"1\"><option value=\"");
    inner += String(g_userChannels[i].pin);
    inner += F("\" selected>GPIO ");
    inner += String(g_userChannels[i].pin);
    inner += F("</option></select></div><button type=\"button\" class=\"del-btn\" title=\"Delete\" onclick=\"setDelRow(this)\">&#128465;</button></div>");
  }
  inner += F("</div><div style=\"margin-top:14px;display:flex;gap:10px;flex-wrap:wrap;justify-content:flex-end;\">");
  inner += F("<button type=\"button\" class=\"btn-header btn-cancel-red\" id=\"set_cancel_btn\">CANCEL</button>");
  inner += F("<button type=\"submit\" class=\"primary\">OK</button>");
  inner += F("</div></form></div></div>");

  inner += F("</main>");

  inner += F("<script>");
  inner += F("function postForm(u,b){return fetch(u,{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:b,credentials:'same-origin'});}");
  inner += F("var gpioSig=0,evtFails=0,testMode=false,netDirty=false,netSaving=false,updateInfo=null,firmwareRelease=null,firmwareAssetUrl='',companionAssetUrl='',companionReleaseUrl='';");
  inner += F("function setMsg(el,text,kind){if(!el)return;el.textContent=text;el.className='msg'+(kind?' '+kind:'');}");
  inner += F("function normalizeVersion(v){return String(v||'').trim().replace(/^refs\\/tags\\//,'').replace(/^v/i,'');}");
  inner += F("function versionParts(v){var s=normalizeVersion(v);if(!s)return[0];return s.split('.').map(function(part){var m=String(part).match(/\\d+/);return m?parseInt(m[0],10):0;});}");
  inner += F("function compareVersions(a,b){var aa=versionParts(a),bb=versionParts(b),len=Math.max(aa.length,bb.length);for(var i=0;i<len;i++){var av=aa[i]||0,bv=bb[i]||0;if(av!==bv)return av>bv?1:-1;}return 0;}");
  inner += F("function pickFirmwareAsset(rel,target){if(!rel||!rel.assets||!rel.assets.length)return null;var suffix=(String(target||'').toLowerCase()+'-ota.bin');");
  inner += F("var exact=rel.assets.find(function(a){var n=String((a&&a.name)||'').toLowerCase();return suffix&&n.endsWith(suffix);});if(exact)return exact;");
  inner += F("var targeted=rel.assets.find(function(a){var n=String((a&&a.name)||'').toLowerCase();return n.indexOf(String(target||'').toLowerCase())>=0&&n.endsWith('.bin');});if(targeted)return targeted;");
  inner += F("return rel.assets.find(function(a){return String((a&&a.name)||'').toLowerCase().endsWith('.bin');})||null;}");
  inner += F("function pickCompanionAsset(rel){if(!rel||!rel.assets||!rel.assets.length)return null;return rel.assets.find(function(a){var n=String((a&&a.name)||'').toLowerCase();return n.endsWith('.tgz')||n.endsWith('.tar.gz');})||null;}");
  inner += F("async function ensureUpdateInfo(){if(updateInfo)return updateInfo;var r=await fetch('/api/update/info',{credentials:'same-origin',cache:'no-store'});if(!r.ok)throw new Error('device_info');");
  inner += F("updateInfo=await r.json();if(!updateInfo.ok)throw new Error('device_info');var cur=document.getElementById('ota_cur_ver');if(cur)cur.textContent=updateInfo.firmware_version||'-';return updateInfo;}");
  inner += F("async function fetchLatestGithubRelease(repo){var r=await fetch('https://api.github.com/repos/'+repo+'/releases/latest',{cache:'no-store'});if(r.status===404)return null;if(!r.ok)throw new Error('github_'+r.status);return await r.json();}");
  inner += F("async function refreshFirmwareRelease(){var st=document.getElementById('ota_release_status'),meta=document.getElementById('ota_release_meta'),dl=document.getElementById('ota_download_btn');");
  inner += F("setMsg(st,'Checking GitHub release status...','');if(meta){meta.style.display='none';meta.textContent='';meta.className='msg';}if(dl)dl.style.display='none';firmwareAssetUrl='';");
  inner += F("try{var info=await ensureUpdateInfo();var rel=await fetchLatestGithubRelease(info.firmware_repo);if(!rel){setMsg(st,'No firmware release has been published on GitHub yet.','');return;}firmwareRelease=rel;");
  inner += F("var latest=normalizeVersion(rel.tag_name||rel.name||'');var asset=pickFirmwareAsset(rel,info.firmware_target);var newer=compareVersions(latest,info.firmware_version)>0;");
  inner += F("if(asset)firmwareAssetUrl=asset.browser_download_url||rel.html_url||info.firmware_release_page;");
  inner += F("if(newer&&asset){setMsg(st,'Update available: v'+latest+' is newer than the installed v'+info.firmware_version+'.','ok');if(meta){meta.style.display='block';meta.textContent='Asset: '+asset.name;meta.className='msg ok';}if(dl)dl.style.display='inline-block';}");
  inner += F("else if(newer){setMsg(st,'A newer release exists (v'+latest+'), but no matching '+info.firmware_target+' OTA asset was found.','err');if(meta){meta.style.display='block';meta.textContent='Expected asset suffix: '+info.firmware_asset_suffix;meta.className='msg err';}}");
  inner += F("else{setMsg(st,'Firmware is up to date at v'+info.firmware_version+'.','ok');if(meta){meta.style.display='block';meta.textContent='Latest GitHub release: '+(latest?('v'+latest):'unknown');meta.className='msg ok';}}}catch(e){setMsg(st,'Could not check GitHub releases automatically.','err');}}");
  inner += F("async function refreshCompanionRelease(){var st=document.getElementById('companion_release_status');setMsg(st,'Checking GitHub release status...','');companionAssetUrl='';");
  inner += F("try{var info=await ensureUpdateInfo();companionReleaseUrl=info.companion_release_page;var rel=await fetchLatestGithubRelease(info.companion_repo);if(!rel){setMsg(st,'No Companion release has been published on GitHub yet.','');return;}companionReleaseUrl=rel.html_url||info.companion_release_page;");
  inner += F("var asset=pickCompanionAsset(rel);if(asset){companionAssetUrl=asset.browser_download_url||rel.html_url||info.companion_release_page;setMsg(st,'Latest Companion package ready: '+asset.name,'ok');}");
  inner += F("else{setMsg(st,'Latest Companion release found, but no .tgz package asset is attached.','err');}}catch(e){setMsg(st,'Could not query GitHub automatically. Use the release page button instead.','err');}}");
  inner += F("function applyTestModeUi(){var b=document.getElementById('hdr_test_btn');var m=document.getElementById('test_mode_banner');");
  inner += F("if(b)b.classList.toggle('btn-test-on',!!testMode);if(m)m.classList.toggle('show',!!testMode);}");
  inner += F("function markNetDirty(){if(!netSaving)netDirty=true;}");
  inner += F("function clearNetDirty(){netDirty=false;}");
  inner += F("function isNetConfigDirty(){var a=document.activeElement;return netDirty||a===document.getElementById('ip_mode_dhcp')||a===document.getElementById('ip_mode_manual')||a===document.getElementById('net_ip_in')||a===document.getElementById('net_mask_in');}");
  inner += F("function lamp(el,on){if(!el)return;el.className='led-bulb'+(on?' on':'');}");
  inner += F("function setInvertBtn(ch,inv){var b=document.getElementById('inv_btn_'+ch);if(!b)return;b.classList.remove('high','low');b.classList.add(inv?'low':'high');b.textContent=inv?'LOW':'HIGH';}");
  inner += F("function applyFromJ(j){if(j.channels&&j.channels.length){j.channels.forEach(function(c){");
  inner += F("lamp(document.getElementById('l_ch_'+c.ch),!!c.v);");
  inner += F("var tt=document.getElementById('ttl_'+c.ch);if(tt)tt.textContent=c.name||'';");
  inner += F("setInvertBtn(c.ch,!!c.inv);");
  inner += F("});}");
  inner += F("if(j.sig!==undefined&&j.sig!==null)gpioSig=+j.sig;}");
  inner += F("function fillAtemInputs(sel,inputs,cur){if(!sel)return;var v=sel.value;sel.innerHTML='';var o=document.createElement('option');o.value='0';o.textContent='— Off —';sel.appendChild(o);");
  inner += F("if(inputs&&inputs.length){for(var i=0;i<inputs.length;i++){var x=inputs[i];var op=document.createElement('option');op.value=String(x.id);op.textContent=x.n||('Input '+x.id);sel.appendChild(op);}}");
  inner += F("sel.value=String(cur||0);if(sel.value!=='0'&&!sel.querySelector('option[value=\"'+String(cur)+'\"]')){sel.value='0';}}");
  #if defined(CONFIG_IDF_TARGET_ESP32P4)
  inner += F("var setAllPins=[22,5,4,1,36,32,25,54,46,27,23,21,20,6,3,2,24,33,26,48,53,47];");
  #else
  inner += F("var setAllPins=[4,5,12,13,14,15,16,17,18,19,21,22,23,25,26,27,32,33];");
  #endif
  inner += F("function setRenumber(){var wrap=document.getElementById('set_rows');if(!wrap)return;var rows=wrap.querySelectorAll('.set-row');var c=document.getElementById('set_count');if(c)c.value=rows.length;rows.forEach(function(r,i){r.setAttribute('data-row',i);var inp=r.querySelector('input');if(inp)inp.setAttribute('name','name'+i);var s=r.querySelectorAll('select');if(s[0])s[0].setAttribute('name','type'+i);if(s[1])s[1].setAttribute('name','pin'+i);});setRefreshPins();}");
  inner += F("function setRefreshPins(){var wrap=document.getElementById('set_rows');if(!wrap)return;var rows=[].slice.call(wrap.querySelectorAll('.set-row'));var used={};rows.forEach(function(r){var s=r.querySelector('select[name^=\"pin\"]');if(s)used[s.value]=1;});rows.forEach(function(r){var s=r.querySelector('select[name^=\"pin\"]');if(!s)return;var cur=s.value;var html='';setAllPins.forEach(function(p){var ps=String(p);if(ps===cur||!used[ps])html+='<option value=\"'+ps+'\">GPIO '+ps+'</option>';});s.innerHTML=html;s.value=cur;if(s.value!==cur&&s.options.length>0)s.selectedIndex=0;});}");
  inner += F("function setDelRow(btn){var rows=document.querySelectorAll('#set_rows .set-row');if(rows.length<=1)return;btn.closest('.set-row').remove();setRenumber();}");
  inner += F("function setAddRow(){var wrap=document.getElementById('set_rows');if(!wrap)return;var rows=wrap.querySelectorAll('.set-row');if(rows.length>=");
  inner += String(kMaxUserChannels);
  inner += F(")return;var d=document.createElement('div');d.className='set-row';d.innerHTML='<div><input type=\"text\" maxlength=\"24\" value=\"\"/></div><div><select><option value=\"gpi\">GPI</option><option value=\"gpo\">GPO</option></select></div><div><select data-pin-select=\"1\"></select></div><button type=\"button\" class=\"del-btn\" title=\"Delete\" onclick=\"setDelRow(this)\">&#128465;</button>';wrap.appendChild(d);setRenumber();}");
  inner += F("function openSettingsModal(){clearNetDirty();var m=document.getElementById('settings_modal');if(m)m.classList.add('show');refreshNet();refreshFirmwareRelease();refreshCompanionRelease();}");
  inner += F("function closeSettingsModal(){clearNetDirty();var m=document.getElementById('settings_modal');if(m)m.classList.remove('show');refreshNet();}");
  inner += F("function applyNetModeUi(){var d=document.getElementById('ip_mode_dhcp');var dis=!(d&&d.checked);var i=document.getElementById('net_ip_in');var m=document.getElementById('net_mask_in');if(i)i.disabled=!dis;if(m)m.disabled=!dis;}");
  inner += F("async function refreshNet(){try{var r=await fetch('/api/net/status',{credentials:'same-origin',cache:'no-store'});if(!r.ok)return;var n=await r.json();if(!n.ok)return;");
  inner += F("var wifiIp=(n.wifi_ip&&n.wifi_ip!=='-')?n.wifi_ip:(n.hotspot_ip||'-');");
  inner += F("var ci=document.getElementById('net_cur_ip');if(ci)ci.textContent=n.ip||'-';");
  inner += F("var pi=document.getElementById('net_phys_ip');if(pi)pi.textContent=n.physical_ip||'-';");
  inner += F("var wi=document.getElementById('net_wifi_ip');if(wi)wi.textContent=wifiIp;");
  inner += F("var cm=document.getElementById('net_cur_mac');if(cm)cm.textContent=n.mac||'-';");
  inner += F("var hp=document.getElementById('hdr-physical-ip');if(hp)hp.textContent=n.physical_ip||'-';");
  inner += F("var hw=document.getElementById('hdr-wifi-ip');if(hw)hw.textContent=wifiIp;");
  inner += F("var d=document.getElementById('ip_mode_dhcp');var m=document.getElementById('ip_mode_manual');var ii=document.getElementById('net_ip_in');var im=document.getElementById('net_mask_in');");
  inner += F("if(!netSaving&&!isNetConfigDirty()){if(d)d.checked=!!n.dhcp;if(m)m.checked=!n.dhcp;if(ii)ii.value=n.static_ip||'';if(im)im.value=n.static_mask||'255.255.255.0';}");
  inner += F("applyNetModeUi();}catch(e){}}");
  inner += F("async function saveNet(){var d=document.getElementById('ip_mode_dhcp');var dhcp=d&&d.checked;var ip=((document.getElementById('net_ip_in')||{}).value||'').trim();var mask=((document.getElementById('net_mask_in')||{}).value||'').trim();");
  inner += F("var msg=document.getElementById('net_msg');var b='dhcp='+(dhcp?'1':'0')+'&ip='+encodeURIComponent(ip)+'&mask='+encodeURIComponent(mask);");
  inner += F("netSaving=true;var saved=false;try{var r=await postForm('/api/net/config',b);var j=await r.json();if(!j.ok){if(msg)msg.textContent='Save failed';return;}saved=true;clearNetDirty();if(msg)msg.textContent='Saved. Restart device to apply IP settings.';}catch(e){if(msg)msg.textContent='Save failed';}finally{netSaving=false;}if(saved)refreshNet();}");
  inner += F("async function refreshProtocols(){try{var r=await fetch('/api/protocol/status',{credentials:'same-origin',cache:'no-store'});if(!r.ok)return;var p=await r.json();if(!p.ok)return;");
  inner += F("var w=document.getElementById('proto_webapi');if(w)w.checked=!!p.webapi;");
  inner += F("var a=document.getElementById('atem_en');if(a)a.checked=!!p.atem;}catch(e){}}");
  inner += F("async function refreshAtem(){try{var r=await fetch('/api/atem/status',{credentials:'same-origin',cache:'no-store'});if(!r.ok)return;var a=await r.json();if(!a.ok)return;");
  inner += F("var cb=document.getElementById('atem_en');if(cb)cb.checked=!!a.en;var led=document.getElementById('atem_led');");
  inner += F("if(led){led.className='status-led'+(a.init?' ok':(a.en?' wait':''));led.title=a.init?'Connected + tally':(a.en?'Waiting for tally…':'Disabled');}");
  inner += F("var show=!!a.en;");
  inner += F("var maps={};if(a.maps&&a.maps.length){a.maps.forEach(function(m){maps[String(m.ch)]=m;});}");
  inner += F("document.querySelectorAll('select[id^=\"atem_me\"]').forEach(function(me){var ch=(me.id||'').replace('atem_me','');");
  inner += F("var ins=document.getElementById('atem_in'+ch);var tt=document.getElementById('atem_tt'+ch);var m=maps[String(ch)];");
  inner += F("me.disabled=!show;if(ins)ins.disabled=!show;if(tt)tt.disabled=!show;");
  inner += F("if(!m)return;if(show&&document.activeElement!==me&&document.activeElement!==ins&&document.activeElement!==tt){");
  inner += F("var meVal=(m.me|0)||255;me.value=String(meVal);if(tt)tt.value=String(m.tally|0);");
  inner += F("if(a.inputs&&a.inputs.length>0){var sv=m.in|0;var dv=ins?parseInt(ins.value)||0:0;fillAtemInputs(ins,a.inputs,sv||dv);}}}); ");
  inner += F("var ip=document.getElementById('atem_ip_in');if(ip&&a.ip&&document.activeElement!==ip)ip.value=a.ip;}catch(e){}}");
  inner += F("async function saveAtemIp(){var ip=((document.getElementById('atem_ip_in')||{}).value||'').trim();var msg=document.getElementById('atem_msg');");
  inner += F("var r=await postForm('/api/atem/config','ip='+encodeURIComponent(ip));var j=await r.json();if(msg)msg.textContent=j.ok?'Saved.':'Save failed';if(j.ok){setTimeout(function(){if(msg&&msg.textContent==='Saved.')msg.textContent='';},1200);refreshAtem();}}");
  inner += F("async function snapshot(){");
  inner += F("var r=await fetch('/api/gpio',{credentials:'same-origin',cache:'no-store'});if(!r.ok)throw new Error('http');");
  inner += F("var j=await r.json();if(!j.ok)throw new Error('json');applyFromJ(j);}");
  inner += F("async function metaLoop(){for(;;){try{await refreshAtem();await refreshProtocols();await refreshNet();}catch(e){}");
  inner += F("await new Promise(function(res){setTimeout(res,1500);});}}");
  inner += F("async function eventLoop(){var pm=document.getElementById('poll-msg');for(;;){try{");
  inner += F("await snapshot();evtFails=0;if(pm)pm.textContent='';await new Promise(function(res){setTimeout(res,220);});}");
  inner += F("catch(e){evtFails++;if(pm&&evtFails>=3)pm.textContent='Events: network error (retrying)';");
  inner += F("await new Promise(function(res){setTimeout(res,Math.min(2000,250+evtFails*150));});}}}");
  inner += F("eventLoop().catch(function(e){var pm=document.getElementById('poll-msg');if(pm)pm.textContent='Events: failed to load';});");
  inner += F("metaLoop();");
  inner += F("document.querySelectorAll('.led-tile[data-ch]').forEach(function(tile){");
  inner += F("function endHold(ev,tile){var ch=+tile.getAttribute('data-ch');tile.classList.remove('holding');");
  inner += F("try{if(tile.hasPointerCapture(ev.pointerId))tile.releasePointerCapture(ev.pointerId);}catch(e){}");
  inner += F("if(!(tile.getAttribute('data-active')==='1'))return;");
  inner += F("postForm('/api/gpio/hold','ch='+ch+'&down=0').then(function(r){return r.json();}).then(function(j){if(j.ok)applyFromJ(j);}).catch(function(){});}");
  inner += F("tile.addEventListener('pointerdown',function(ev){if(ev.target.closest('.row-control'))return;if(!testMode)return;if(!(tile.getAttribute('data-active')==='1'))return;ev.preventDefault();");
  inner += F("var ch=+tile.getAttribute('data-ch');tile.classList.add('holding');try{tile.setPointerCapture(ev.pointerId);}catch(e){}");
  inner += F("postForm('/api/gpio/hold','ch='+ch+'&down=1').then(function(r){return r.json();}).then(function(j){if(j.ok)applyFromJ(j);}).catch(function(){});});");
  inner += F("tile.addEventListener('pointerup',function(ev){endHold(ev,tile);});");
  inner += F("tile.addEventListener('pointercancel',function(ev){endHold(ev,tile);});});");
  inner += F("document.querySelectorAll('.inv-btn[data-ch]').forEach(function(btn){btn.addEventListener('click',function(ev){ev.stopPropagation();");
  inner += F("var ch=parseInt(btn.getAttribute('data-ch')||'-1',10);if(isNaN(ch)||ch<0)return;var inv=btn.classList.contains('high')?1:0;");
  inner += F("postForm('/api/gpio/polarity','ch='+ch+'&inv='+inv).then(function(r){return r.json();}).then(function(j){if(j.ok)applyFromJ(j);});});});");
  inner += F("(function(){");
  inner += F("document.querySelectorAll('.proto-copy-btn').forEach(function(b){b.addEventListener('click',function(ev){ev.stopPropagation();");
  inner += F("var t=b.getAttribute('data-copy-text')||'';if(!t)return;");
  inner += F("if(navigator.clipboard&&navigator.clipboard.writeText)navigator.clipboard.writeText(t).catch(function(){});else{var ta=document.createElement('textarea');ta.value=t;document.body.appendChild(ta);ta.select();try{document.execCommand('copy');}catch(e){}document.body.removeChild(ta);}});});");
  inner += F("})();");
  inner += F("(function(){try{var tb=document.getElementById('hdr_test_btn');if(tb)tb.addEventListener('click',function(ev){ev.preventDefault();testMode=!testMode;applyTestModeUi();});");
  inner += F("var sb=document.getElementById('hdr_settings_btn');if(sb)sb.addEventListener('click',function(ev){ev.preventDefault();openSettingsModal();});");
  inner += F("var sa=document.getElementById('set_add_btn');if(sa)sa.addEventListener('click',function(ev){ev.preventDefault();setAddRow();});");
  inner += F("var sc=document.getElementById('set_cancel_btn');if(sc)sc.addEventListener('click',function(ev){ev.preventDefault();closeSettingsModal();});");
  inner += F("var sf=document.getElementById('set_form');if(sf)sf.addEventListener('submit',function(){closeSettingsModal();});");
  inner += F("var sdef=document.getElementById('set_default_btn');if(sdef)sdef.addEventListener('click',function(){");
  inner += F("if(!confirm('Restore default GPIO preset? Current channel list will be replaced.'))return;");
  inner += F("postForm('/settings/default','').then(function(){location.reload();});});");
  inner += F("var cdl=document.getElementById('companion_dl_btn');if(cdl)cdl.addEventListener('click',function(){if(companionAssetUrl)window.open(companionAssetUrl,'_blank');else window.open('/api/companion/module/download','_blank');});");
  inner += F("var crb=document.getElementById('companion_release_btn');if(crb)crb.addEventListener('click',function(){if(companionReleaseUrl)window.open(companionReleaseUrl,'_blank');else window.open('/api/companion/module/download','_blank');});");
  // OTA + mDNS hostname display
  inner += F("(function(){");
  inner += F("var otaHost=document.getElementById('ota_mdns_host');");
  // populate mDNS hostname from /api/net/status
  inner += F("fetch('/api/net/status',{credentials:'same-origin'}).then(function(r){return r.json();}).then(function(j){if(otaHost&&j.mdns)otaHost.textContent=j.mdns;}).catch(function(){});");
  inner += F("var otaFile=document.getElementById('ota_file'),otaBtn=document.getElementById('ota_flash_btn');");
  inner += F("var otaWrap=document.getElementById('ota_progress_wrap'),otaBar=document.getElementById('ota_progress_bar'),otaMsg=document.getElementById('ota_msg');");
  inner += F("var otaCheck=document.getElementById('ota_check_btn');if(otaCheck)otaCheck.addEventListener('click',function(){refreshFirmwareRelease();refreshCompanionRelease();});");
  inner += F("var otaDownload=document.getElementById('ota_download_btn');if(otaDownload)otaDownload.addEventListener('click',function(){if(firmwareAssetUrl)window.open(firmwareAssetUrl,'_blank');});");
  inner += F("var otaRelease=document.getElementById('ota_release_btn');if(otaRelease)otaRelease.addEventListener('click',function(){if(firmwareRelease&&firmwareRelease.html_url)window.open(firmwareRelease.html_url,'_blank');else if(updateInfo&&updateInfo.firmware_release_page)window.open(updateInfo.firmware_release_page,'_blank');});");
  inner += F("if(otaBtn)otaBtn.addEventListener('click',function(){");
  inner += F("if(!otaFile||!otaFile.files||!otaFile.files.length){alert('Please select a firmware.bin file first.');return;}");
  inner += F("var f=otaFile.files[0];if(!f.name.endsWith('.bin')){alert('File must be a .bin firmware file.');return;}");
  inner += F("var fd=new FormData();fd.append('firmware',f);");
  inner += F("if(otaWrap)otaWrap.style.display='block';if(otaMsg)otaMsg.textContent='Uploading '+f.name+' ('+Math.round(f.size/1024)+' KB)...';if(otaBar)otaBar.style.width='0%';");
  inner += F("var xhr=new XMLHttpRequest();xhr.open('POST','/api/ota/upload');xhr.withCredentials=true;");
  inner += F("xhr.upload.onprogress=function(e){if(e.lengthComputable&&otaBar)otaBar.style.width=Math.round(e.loaded/e.total*90)+'%';};");
  inner += F("xhr.onload=function(){if(otaBar)otaBar.style.width='100%';");
  inner += F("try{var j=JSON.parse(xhr.responseText);if(j.ok){if(otaMsg)otaMsg.textContent='Flash successful! Rebooting device...';}");
  inner += F("else{if(otaMsg)otaMsg.textContent='Error: '+(j.err||'unknown');}}catch(e){if(otaMsg)otaMsg.textContent='Upload complete. Rebooting...';}};");
  inner += F("xhr.onerror=function(){if(otaMsg)otaMsg.textContent='Upload failed. Check connection.';};");
  inner += F("xhr.send(fd);});})();");
  inner += F("var sr=document.getElementById('set_rows');if(sr)sr.addEventListener('change',function(ev){if(ev.target&&ev.target.name&&ev.target.name.indexOf('pin')===0)setRefreshPins();});");
  inner += F("var nd=document.getElementById('ip_mode_dhcp');var nm=document.getElementById('ip_mode_manual');if(nd)nd.addEventListener('change',function(){markNetDirty();applyNetModeUi();});if(nm)nm.addEventListener('change',function(){markNetDirty();applyNetModeUi();});");
  inner += F("var nii=document.getElementById('net_ip_in');var nim=document.getElementById('net_mask_in');if(nii)nii.addEventListener('input',markNetDirty);if(nim)nim.addEventListener('input',markNetDirty);");
  inner += F("var ns=document.getElementById('net_save_btn');if(ns)ns.addEventListener('click',function(ev){ev.preventDefault();saveNet();});");
  inner += F("setRenumber();applyTestModeUi();refreshFirmwareRelease();refreshCompanionRelease();}catch(e){}})();");
  inner += F("(function(){");
  inner += F("var dnBtn=document.getElementById('dn-btn'),dnForm=document.getElementById('dn-form'),dnInp=document.getElementById('dn-inp'),dnDisp=document.getElementById('dn-disp');");
  inner += F("function openDn(){var cur=dnDisp?dnDisp.textContent.replace(/^\\s*\\[\\s*/,'').replace(/\\s*\\]\\s*$/,'').trim():'';dnInp.value=cur;dnForm.style.display='block';dnInp.focus();dnInp.select();}");
  inner += F("function closeDn(){dnForm.style.display='none';}");
  inner += F("function saveDn(){var n=(dnInp?dnInp.value.trim():'').substring(0,32);");
  inner += F("postForm('/api/device/name','name='+encodeURIComponent(n)).then(function(r){return r.json();}).then(function(j){if(!j.ok)return;");
  inner += F("if(dnDisp){dnDisp.textContent=n?' [ '+n+' ]':'';}");
  inner += F("document.title='IONIX I GPIO'+(n?' [ '+n+' ]':'');");
  inner += F("closeDn();});}");
  inner += F("if(dnBtn)dnBtn.addEventListener('click',function(ev){ev.stopPropagation();dnForm.style.display==='block'?closeDn():openDn();});");
  inner += F("if(dnForm)dnForm.addEventListener('click',function(ev){ev.stopPropagation();});");
  inner += F("document.addEventListener('click',function(){if(dnForm&&dnForm.style.display==='block')closeDn();});");
  inner += F("if(dnInp)dnInp.addEventListener('keydown',function(ev){if(ev.key==='Enter'){ev.preventDefault();saveDn();}else if(ev.key==='Escape'){closeDn();}});");
  inner += F("})();");
  inner += F("(function(){function postProto(k,en){return postForm('/api/protocol/config','key='+encodeURIComponent(k)+'&en='+(en?'1':'0')).then(function(r){return r.json();});}");
  inner += F("var pw=document.getElementById('proto_webapi');if(pw)pw.addEventListener('change',function(){postProto('webapi',pw.checked).then(function(){return refreshProtocols();});});");
  inner += F("var en=document.getElementById('atem_en');if(en)en.addEventListener('change',function(){postProto('atem',en.checked).then(function(){return refreshAtem();}).then(function(){return refreshProtocols();});});");
  inner += F("var ai=document.getElementById('atem_ip_in');if(ai)ai.addEventListener('keydown',function(ev){if(ev.key!=='Enter')return;ev.preventDefault();saveAtemIp();});");
  inner += F("if(ai)ai.addEventListener('blur',function(){if(ai.value.trim())saveAtemIp();});");
  inner += F("function postMap(ch){var me=document.getElementById('atem_me'+ch);var ins=document.getElementById('atem_in'+ch);var tt=document.getElementById('atem_tt'+ch);");
  inner += F("if(!me||!ins||!tt)return;var b='ch='+encodeURIComponent(ch)+'&me='+encodeURIComponent(me.value)+'&in='+encodeURIComponent(ins.value)+'&tally='+encodeURIComponent(tt.value);");
  inner += F("postForm('/api/atem/map',b).then(function(r){return r.json();}).then(function(j){if(j.ok)refreshAtem();});}");
  inner += F("document.querySelectorAll('select[id^=\"atem_me\"],select[id^=\"atem_in\"],select[id^=\"atem_tt\"]').forEach(function(el){");
  inner += F("el.addEventListener('change',function(){var id=el.id||'';var ch=id.replace('atem_me','').replace('atem_in','').replace('atem_tt','');if(ch==='')return;postMap(ch);});});})();");
  inner += F("</script>");

  return htmlShell(F("IONIX I GPIO"), inner, true, kRequireWebLogin, buildDashboardHeaderStrip());
}

String pagePortal(const String &apIp, const String &err, const String &scanOptionsHtml) {
  String inner;
  inner += F("<main class=\"page active\"><div class=\"main-scroll\">");
  inner += F("<div class=\"card\"><div class=\"section-title\">Wi-Fi setup</div>");
  inner += F("<p class=\"msg\" style=\"margin-bottom:12px;\">Hotspot: <strong>");
  inner += apSsid();
  inner += F("</strong> · Open <strong>http://");
  inner += apIp;
  inner += F("/</strong> in your browser.</p>");
  if (err.length()) {
    inner += F("<p class=\"msg err\">");
    inner += htmlTextEscape(err);
    inner += F("</p>");
  }
  inner += F("<form method=\"post\" action=\"/portal/save\" style=\"margin-top:12px;\">");
  inner += F("<div class=\"form-group\"><label class=\"section-title\" style=\"display:block;border:none;padding:0;margin-bottom:8px;\">");
  inner += F("Pick a network</label><select name=\"ssid_pick\" id=\"ssid_pick\">");
  inner += scanOptionsHtml;
  inner += F("</select></div>");
  inner += F("<div class=\"form-group\"><label class=\"section-title\" style=\"display:block;border:none;padding:0;margin-bottom:8px;\">");
  inner += F("Or type SSID manually</label><input name=\"ssid_manual\" type=\"text\" placeholder=\"Network name (SSID)\" "
             "autocomplete=\"off\"/></div>");
  inner += F("<div class=\"form-group\"><input name=\"pass\" type=\"password\" placeholder=\"Wi-Fi password (leave empty if open)\"/></div>");
  inner += F("<button class=\"primary\" type=\"submit\">SAVE &amp; CONNECT</button></form>");
  inner += F("<p class=\"msg\" style=\"margin-top:12px;\"><a class=\"btn-header\" href=\"/\" style=\"cursor:pointer;\">Rescan networks</a></p>");
  inner += F("<p class=\"msg\" style=\"margin-top:14px;\">Tip: press <strong>EN/RST</strong> twice quickly during the first second of boot to open "
             "this portal again, or hold <strong>BOOT (GPIO0)</strong> at power-on.</p>");
  inner += F("</div></div></main>");
  return htmlShell(F("IONIX I GPIO — Setup"), inner, true, false);
}

String pageSuccess(const String &staIp) {
  String inner;
  inner += F("<main class=\"page active\"><div class=\"main-scroll\">");
  inner += F("<div class=\"card\"><div class=\"section-title\">Connected</div>");
  inner += F("<p class=\"msg ok\" style=\"font-size:14px;font-weight:800;margin-bottom:10px;\">Your device IP:</p>");
  inner += F("<p style=\"font-size:22px;font-weight:900;letter-spacing:0.5px;margin-bottom:12px;\">");
  inner += staIp;
  inner += F("</p>");
  inner += F("<p class=\"msg\" style=\"margin-bottom:12px;\">Open in the browser:</p>");
  inner += F("<p><a class=\"btn-header\" style=\"padding:12px 16px;font-size:13px;\" href=\"http://");
  inner += staIp;
  inner += F("/\">http://");
  inner += staIp;
  inner += F("/</a></p>");
  inner += F("<p class=\"msg\" style=\"margin-top:14px;\">The hotspot will close shortly. Reconnect your phone to your home Wi-Fi.</p></div></div></main>");
  inner += F("<script>setTimeout(function(){location.href='http://");
  inner += staIp;
  inner += F("/';}, 8000);</script>");
  return htmlShell(F("IONIX I GPIO — Done"), inner, true, false);
}

String pageSettings() {
  String inner;
  inner.reserve(9000);
  inner += F("<main class=\"page active\">");
  inner += F("<div class=\"modal-overlay show\" style=\"position:static;inset:auto;background:transparent;padding:24px;\">");
  inner += F("<div class=\"modal-box\" style=\"max-width:880px;\">");
  inner += F("<h3>Settings</h3>");
  inner += F("<p class=\"msg\" style=\"margin-bottom:12px;\">GPIO list is saved persistently in NVS. Used pins are hidden from other rows.</p>");
  inner += F("<form method=\"post\" action=\"/settings/channels\" id=\"set_form\">");
  inner += F("<input type=\"hidden\" name=\"count\" id=\"set_count\" value=\"");
  inner += String(g_userChannelCount);
  inner += F("\"/>");
  inner += F("<div class=\"settings-toolbar\"><button type=\"button\" class=\"btn-add-green\" title=\"Add GPIO\" onclick=\"addRow()\">+</button></div>");
  inner += F("<div id=\"set_rows\">");
  #if defined(CONFIG_IDF_TARGET_ESP32P4)
  const int choices[] = {22, 5, 4, 1, 36, 32, 25, 54, 46, 27, 23, 21, 20, 6, 3, 2, 24, 33, 26, 48, 53, 47};
  #else
  const int choices[] = {4, 5, 12, 13, 14, 15, 16, 17, 18, 19, 21, 22, 23, 25, 26, 27, 32, 33};
  #endif
  for (int i = 0; i < g_userChannelCount; i++) {
    inner += F("<div class=\"set-row\" data-row=\"");
    inner += String(i);
    inner += F("\"><div><label class=\"mini\">Name</label><input type=\"text\" maxlength=\"24\" name=\"name");
    inner += String(i);
    inner += F("\" value=\"");
    inner += htmlAttrEscape(g_userChannels[i].name);
    inner += F("\"/></div><div><label class=\"mini\">Type</label><select name=\"type");
    inner += String(i);
    inner += F("\"><option value=\"gpi\"");
    if (!g_userChannels[i].isGpo)
      inner += F(" selected");
    inner += F(">GPI</option><option value=\"gpo\"");
    if (g_userChannels[i].isGpo)
      inner += F(" selected");
    inner += F(">GPO</option></select></div><div><label class=\"mini\">Pin</label><select name=\"pin");
    inner += String(i);
    inner += F("\" data-pin-select=\"1\"><option value=\"");
    inner += String(g_userChannels[i].pin);
    inner += F("\" selected>GPIO ");
    inner += String(g_userChannels[i].pin);
    inner += F("</option></select></div><button type=\"button\" class=\"del-btn\" title=\"Delete\" onclick=\"delRow(this)\">&#128465;</button></div>");
  }
  inner += F("</div><div style=\"margin-top:14px;display:flex;gap:10px;flex-wrap:wrap;justify-content:flex-end;\">");
  inner += F("<a class=\"btn-header btn-cancel-red\" href=\"/\">CANCEL</a>");
  inner += F("<button type=\"submit\" class=\"primary\">OK</button>");
  inner += F("</div></form></div></div>");
  inner += F("<script>var maxRows=");
  inner += String(kMaxUserChannels);
  inner += F(";var pinOpts='");
  for (unsigned p = 0; p < sizeof(choices) / sizeof(choices[0]); p++) {
    inner += "<option value=\\\"";
    inner += String(choices[p]);
    inner += "\\\">GPIO ";
    inner += String(choices[p]);
    inner += "</option>";
  }
  inner += F("';var allPins=[");
  for (unsigned p = 0; p < sizeof(choices) / sizeof(choices[0]); p++) {
    if (p) inner += ",";
    inner += String(choices[p]);
  }
  inner += F("];function renumber(){var rows=document.querySelectorAll('#set_rows .set-row');document.getElementById('set_count').value=rows.length;rows.forEach(function(r,i){r.setAttribute('data-row',i);r.querySelector('input').setAttribute('name','name'+i);var s=r.querySelectorAll('select');s[0].setAttribute('name','type'+i);s[1].setAttribute('name','pin'+i);});refreshPinOptions();}");
  inner += F("function delRow(btn){var rows=document.querySelectorAll('#set_rows .set-row');if(rows.length<=1)return;btn.closest('.set-row').remove();renumber();}");
  inner += F("function refreshPinOptions(){var rows=[].slice.call(document.querySelectorAll('#set_rows .set-row'));var used={};rows.forEach(function(r){var s=r.querySelector('select[name^=\"pin\"]');if(s)used[s.value]=1;});rows.forEach(function(r){var s=r.querySelector('select[name^=\"pin\"]');if(!s)return;var cur=s.value;var html='';allPins.forEach(function(p){var ps=String(p);if(ps===cur||!used[ps])html+='<option value=\"'+ps+'\">GPIO '+ps+'</option>';});s.innerHTML=html;s.value=cur;if(s.value!==cur&&s.options.length>0)s.selectedIndex=0;});}");
  inner += F("function addRow(){var rows=document.querySelectorAll('#set_rows .set-row');if(rows.length>=maxRows)return;var d=document.createElement('div');d.className='set-row';d.innerHTML='<div><label class=\"mini\">Name</label><input type=\"text\" maxlength=\"24\" value=\"\"/></div><div><label class=\"mini\">Type</label><select><option value=\"gpi\">GPI</option><option value=\"gpo\">GPO</option></select></div><div><label class=\"mini\">Pin</label><select data-pin-select=\"1\"></select></div><button type=\"button\" class=\"del-btn\" title=\"Delete\" onclick=\"delRow(this)\">&#128465;</button>';document.getElementById('set_rows').appendChild(d);renumber();}");
  inner += F("document.getElementById('set_rows').addEventListener('change',function(ev){if(ev.target&&ev.target.name&&ev.target.name.indexOf('pin')===0)refreshPinOptions();});renumber();</script></main>");
  return htmlShell(F("IONIX I GPIO — Settings"), inner, true, kRequireWebLogin);
}

bool cookieMatchesSession() {
  if (!g_server.hasHeader(F("Cookie"))) return false;
  const String c = g_server.header(F("Cookie"));
  if (!g_sessionCookieValue.length()) return false;
  const String needle = String("ionix_sess=") + g_sessionCookieValue;
  return c.indexOf(needle) >= 0;
}

void sendHtml(int code, const String &html) {
  g_server.sendHeader(F("Cache-Control"), F("no-store"));
  g_server.send(code, F("text/html; charset=utf-8"), html);
}

void sendRedirect(const String &loc) {
  g_server.sendHeader(F("Location"), loc);
  g_server.send(302, F("text/plain"), F("Redirect"));
}

void applySessionCookie() {
  if (!g_sessionCookieValue.length()) return;
  // No SameSite: some embedded browsers handle Set-Cookie + redirect more reliably without it.
  String ck = String("ionix_sess=") + g_sessionCookieValue + F("; Path=/; HttpOnly; Max-Age=86400");
  g_server.sendHeader(F("Set-Cookie"), ck);
}

void clearSessionCookie() {
  g_server.sendHeader(F("Set-Cookie"), F("ionix_sess=; Path=/; HttpOnly; Max-Age=0"));
}

void sendRedirectWithCookie(const String &loc) {
  g_server.sendHeader(F("Location"), loc);
  g_server.send(303, F("text/html"), F("<!DOCTYPE html><html><head><meta charset=\"utf-8\"><title>Redirect</title></head>"
                                        "<body><p>Signing in… <a href=\"/\">Continue</a></p></body></html>"));
}

void handleRoot() {
  if (g_apMode) {
    const String scanOpts = buildWifiScanOptionsHtml();
    sendHtml(200, pagePortal(apIpStr(), "", scanOpts));
    return;
  }
  if (kRequireWebLogin && !cookieMatchesSession()) {
    sendHtml(200, pageLogin(false));
    return;
  }
  sendHtml(200, pageDashboard());
}

void handleSettingsPage() {
  sendRedirect("/");
}

void handleSettingsChannelsPost() {
  if (g_apMode) {
    sendRedirect("http://" + apIpStr() + "/");
    return;
  }
  if (kRequireWebLogin && !cookieMatchesSession()) {
    g_server.send(401, F("text/plain"), F("unauthorized"));
    return;
  }
  bool ok = true;
  int cnt = g_server.hasArg(F("count")) ? g_server.arg(F("count")).toInt() : 0;
  if (cnt < 1)
    cnt = 1;
  if (cnt > kMaxUserChannels)
    cnt = kMaxUserChannels;
  UserChannel tmp[kMaxUserChannels];
  for (int i = 0; i < cnt; i++) {
    const String n = g_server.arg(String(F("name")) + String(i));
    const String t = g_server.arg(String(F("type")) + String(i));
    const int p = g_server.arg(String(F("pin")) + String(i)).toInt();
    tmp[i].name = n;
    tmp[i].name.trim();
    if (tmp[i].name.length() == 0)
      tmp[i].name = String(i + 1);
    tmp[i].isGpo = (t == F("gpo"));
    tmp[i].pin = p;
  }
  for (int i = 0; i < cnt; i++) {
    for (int j = i + 1; j < cnt; j++) {
      if (tmp[i].pin == tmp[j].pin)
        ok = false;
    }
  }
  if (ok) {
    g_userChannelCount = cnt;
    for (int i = 0; i < cnt; i++)
      g_userChannels[i] = tmp[i];
    saveUserChannelsToNvs();
    applyUserChannelsToRuntime();
  }
  sendRedirect(ok ? "/" : "/?seterr=1");
}

void handleSettingsDefaultPost() {
  if (g_apMode) {
    sendRedirect("http://" + apIpStr() + "/");
    return;
  }
  if (kRequireWebLogin && !cookieMatchesSession()) {
    g_server.send(401, F("text/plain"), F("unauthorized"));
    return;
  }
  applyDefaultUserChannelsPreset();
  sendRedirect("/");
}

void handleLoginPost() {
  if (g_apMode) {
    sendRedirect("http://" + apIpStr() + "/");
    return;
  }
  String u2;
  String p2;
  if (g_server.hasArg(F("u"))) {
    u2 = g_server.arg(F("u"));
    p2 = g_server.hasArg(F("p")) ? g_server.arg(F("p")) : String();
  } else {
    const String raw = readPostBodyRaw(512);
    parseFormFieldsUp(raw, u2, p2);
  }
  u2.trim();
  p2.trim();

  g_server.sendHeader(F("Cache-Control"), F("no-store"));
  if (u2 == String(kWebUser) && p2 == String(kWebPass)) {
    g_sessionCookieValue = String(esp_random(), HEX) + String(esp_random(), HEX);
    applySessionCookie();
    sendRedirectWithCookie(F("/"));
    return;
  }
  sendHtml(200, pageLogin(true));
}

void handleLogout() {
  g_sessionCookieValue = "";
  clearSessionCookie();
  sendRedirect("/");
}

static void appendJsonQuoted(String &dst, const String &s) {
  dst += '"';
  for (unsigned i = 0; i < s.length(); i++) {
    const char c = s[i];
    if (c == '"' || c == '\\') {
      dst += '\\';
      dst += c;
    } else if ((uint8_t)c < 0x20)
      dst += ' ';
    else
      dst += c;
  }
  dst += '"';
}

static void sendGpioDashboardJson(const char *changedLiteralOrNull) {
  const uint32_t sig = dashboardStateSig();
  const int chCount = gpioRuntimeChannelCount();
  const NetworkStatusSnapshot net = readNetworkStatus();

  String j;
  j.reserve(1800);
  j += F("{\"ok\":true");
  if (changedLiteralOrNull != nullptr) {
    j += F(",\"changed\":");
    j += changedLiteralOrNull;
  }
  j += F(",\"sig\":");
  j += String(static_cast<unsigned long>(sig));
  j += F(",\"channels\":[");
  for (int i = 0; i < chCount; i++) {
    if (i)
      j += ',';
    j += F("{\"ch\":");
    j += String(i);
    j += F(",\"isGpo\":");
    j += gpioRuntimeIsGpo(i) ? '1' : '0';
    j += F(",\"v\":");
    j += String(gpioDisplayedLogicalChannel(i));
    j += F(",\"inv\":");
    j += gpioGetInvertChannel(i) ? '1' : '0';
    j += F(",\"pin\":");
    j += String(gpioChannelPin(i));
    j += F(",\"name\":");
    appendJsonQuoted(j, gpioChannelName(i));
    j += '}';
  }
  j += F("],\"ip\":");
  appendJsonQuoted(j, net.primaryIp);
  j += '}';
  g_server.sendHeader(F("Cache-Control"), F("no-store"));
  g_server.send(200, F("application/json"), j);
}

static void appendCompanionChannelJson(String &j, int ch) {
  j += F("{\"ch\":");
  j += String(ch);
  j += F(",\"name\":");
  appendJsonQuoted(j, gpioChannelName(ch));
  j += F(",\"v\":");
  j += String(gpioDisplayedLogicalChannel(ch));
  j += F(",\"pin\":");
  j += String(gpioChannelPin(ch));
  j += F(",\"inv\":");
  j += gpioGetInvertChannel(ch) ? '1' : '0';
  j += '}';
}

static void sendCompanionJson(const char *changedLiteralOrNull) {
  const uint32_t sig = dashboardStateSig();
  const int chCount = gpioRuntimeChannelCount();

  String j;
  j.reserve(2200);
  j += F("{\"ok\":true");
  if (changedLiteralOrNull != nullptr) {
    j += F(",\"changed\":");
    j += changedLiteralOrNull;
  }
  j += F(",\"sig\":");
  j += String(static_cast<unsigned long>(sig));
  j += F(",\"gpi\":[");
  bool first = true;
  for (int ch = 0; ch < chCount; ch++) {
    if (gpioRuntimeIsGpo(ch))
      continue;
    if (!first)
      j += ',';
    first = false;
    appendCompanionChannelJson(j, ch);
  }
  j += F("],\"gpo\":[");
  first = true;
  for (int ch = 0; ch < chCount; ch++) {
    if (!gpioRuntimeIsGpo(ch))
      continue;
    if (!first)
      j += ',';
    first = false;
    appendCompanionChannelJson(j, ch);
  }
  j += F("]}");
  g_server.sendHeader(F("Cache-Control"), F("no-store"));
  g_server.send(200, F("application/json"), j);
}

void handleNetworkStatus() {
  const NetworkStatusSnapshot net = readNetworkStatus();
  String j;
  j.reserve(320);
  j += F("{\"ok\":true,\"dhcp\":");
  j += g_ipDhcp ? '1' : '0';
  j += F(",\"ip\":");
  appendJsonQuoted(j, net.primaryIp);
  j += F(",\"physical_ip\":");
  appendJsonQuoted(j, net.physicalIp);
  j += F(",\"wifi_ip\":");
  appendJsonQuoted(j, net.wifiIp);
  j += F(",\"hotspot_ip\":");
  appendJsonQuoted(j, net.hotspotIp);
  j += F(",\"mac\":");
  appendJsonQuoted(j, net.mac);
  j += F(",\"static_ip\":");
  appendJsonQuoted(j, g_staticIp);
  j += F(",\"static_mask\":");
  appendJsonQuoted(j, g_staticMask);
  j += F(",\"mdns\":");
  appendJsonQuoted(j, mdnsHostname());
  j += '}';
  g_server.sendHeader(F("Cache-Control"), F("no-store"));
  g_server.send(200, F("application/json"), j);
}

void handleUpdateInfo() {
  if (kRequireWebLogin && !cookieMatchesSession()) {
    g_server.send(401, F("application/json"), F("{\"ok\":false}"));
    return;
  }
  String j;
  j.reserve(384);
  j += F("{\"ok\":true,\"firmware_version\":");
  appendJsonQuoted(j, String(kFirmwareVersion));
  j += F(",\"firmware_repo\":");
  appendJsonQuoted(j, String(kFirmwareReleaseRepo));
  j += F(",\"firmware_release_page\":");
  appendJsonQuoted(j, String(kFirmwareReleasePageUrl));
  j += F(",\"firmware_target\":");
  appendJsonQuoted(j, String(kFirmwareTargetId));
  j += F(",\"firmware_asset_suffix\":");
  appendJsonQuoted(j, String(kFirmwareAssetSuffix));
  j += F(",\"companion_repo\":");
  appendJsonQuoted(j, String(kCompanionReleaseRepo));
  j += F(",\"companion_release_page\":");
  appendJsonQuoted(j, String(kCompanionModuleDownloadUrl));
  j += '}';
  g_server.sendHeader(F("Cache-Control"), F("no-store"));
  g_server.send(200, F("application/json"), j);
}

void handleNetworkConfigPost() {
  if (kRequireWebLogin && !cookieMatchesSession()) {
    g_server.send(401, F("application/json"), F("{\"ok\":false}"));
    return;
  }
  const bool dhcp = postFormField("dhcp", 32).toInt() != 0;
  const String ip = postFormField("ip", 64);
  const String mask = postFormField("mask", 64);
  if (!dhcp) {
    IPAddress a;
    IPAddress m;
    if (!parseIpv4(ip, a) || !parseIpv4(mask, m)) {
      g_server.send(400, F("application/json"), F("{\"ok\":false,\"err\":\"invalid_ip\"}"));
      return;
    }
  }
  g_ipDhcp = dhcp;
  g_staticIp = ip;
  g_staticMask = mask;
  g_prefs.putBool("ip_dhcp", g_ipDhcp);
  g_prefs.putString("ip_addr", g_staticIp);
  g_prefs.putString("ip_mask", g_staticMask);
  g_server.sendHeader(F("Cache-Control"), F("no-store"));
  g_server.send(200, F("application/json"), F("{\"ok\":true,\"reboot\":true}"));
}

void handleApiGpio() {
  if (g_apMode) {
    g_server.send(401, F("application/json"), F("{\"ok\":false}"));
    return;
  }
  if (kRequireWebLogin && !cookieMatchesSession()) {
    g_server.send(401, F("application/json"), F("{\"ok\":false}"));
    return;
  }
  sendGpioDashboardJson(nullptr);
}

void handleApiCompanionGpio() { sendCompanionJson(nullptr); }

void handleApiCompanionWait() {
  if (!g_server.hasArg(F("since"))) {
    sendCompanionJson(nullptr);
    return;
  }
  const uint32_t since = static_cast<uint32_t>(strtoul(g_server.arg(F("since")).c_str(), nullptr, 10));
  uint32_t cur = dashboardStateSig();
  if (cur != since) {
    sendCompanionJson("true");
    return;
  }
  const uint32_t deadline = millis() + 28;
  while (static_cast<int32_t>(millis() - deadline) < 0) {
    atemServiceOnLongPollTick();
    gpioApplyOutputs();
    yield();
    cur = dashboardStateSig();
    if (cur != since) {
      sendCompanionJson("true");
      return;
    }
    delay(1);
  }
  sendCompanionJson("false");
}

void handleApiCompanionGpoSet() {
  const int ch = g_server.hasArg(F("ch")) ? g_server.arg(F("ch")).toInt() : -1;
  const int v = g_server.hasArg(F("v")) ? g_server.arg(F("v")).toInt() : -1;
  if (ch < 0 || ch >= gpioRuntimeChannelCount() || !gpioRuntimeIsGpo(ch) || (v != 0 && v != 1)) {
    g_server.send(400, F("application/json"), F("{\"ok\":false,\"err\":\"bad_request\"}"));
    return;
  }
  gpioLogicalWriteChannel(ch, v);
  gpioApplyOutputs();
  sendCompanionJson("true");
}

/**
 * Bulk GPO set — single request to drive multiple outputs atomically.
 * POST /api/companion/gpo/set/bulk
 * Body (URL-encoded): ch[]=0&v[]=1&ch[]=1&v[]=1&...
 * OR JSON-style flat pairs: ch0=0&v0=1&ch1=1&v1=0&...
 * Applies all writes in one shot, then calls gpioApplyOutputs() once.
 */
void handleApiCompanionGpoBulkSet() {
  bool anyWrite = false;
  // Support up to kMaxUserChannels pairs sent as ch0=X&v0=Y, ch1=X&v1=Y, ...
  for (int i = 0; i < kMaxUserChannels; i++) {
    const String chKey = String("ch") + String(i);
    const String vKey  = String("v")  + String(i);
    if (!g_server.hasArg(chKey) || !g_server.hasArg(vKey))
      break;
    const int ch = g_server.arg(chKey).toInt();
    const int v  = g_server.arg(vKey).toInt();
    if (ch < 0 || ch >= gpioRuntimeChannelCount() || !gpioRuntimeIsGpo(ch) || (v != 0 && v != 1))
      continue;
    gpioLogicalWriteChannel(ch, v);
    anyWrite = true;
  }
  if (!anyWrite) {
    g_server.send(400, F("application/json"), F("{\"ok\":false,\"err\":\"no_valid_pairs\"}"));
    return;
  }
  gpioApplyOutputs(); // single hardware flush for all channels
  sendCompanionJson("true");
}

void handleApiCompanionModuleDownload() { sendRedirect(String(kCompanionModuleDownloadUrl)); }

/** Long-poll: returns when `sig` differs from `since`, or after ~28 ms with `changed:false`; ticks ATEM + GPIO outputs while blocked. */
void handleApiGpioWait() {
  if (g_apMode) {
    g_server.send(401, F("application/json"), F("{\"ok\":false}"));
    return;
  }
  if (kRequireWebLogin && !cookieMatchesSession()) {
    g_server.send(401, F("application/json"), F("{\"ok\":false}"));
    return;
  }
  if (!g_server.hasArg(F("since"))) {
    sendGpioDashboardJson(nullptr);
    return;
  }
  const uint32_t since = static_cast<uint32_t>(strtoul(g_server.arg(F("since")).c_str(), nullptr, 10));
  uint32_t cur = dashboardStateSig();
  if (cur != since) {
    sendGpioDashboardJson("true");
    return;
  }
  /** Keep this short: synchronous WebServer cannot serve POST/clicks while this handler runs. */
  const uint32_t deadline = millis() + 28;
  while (static_cast<int32_t>(millis() - deadline) < 0) {
    atemServiceOnLongPollTick();
    gpioApplyOutputs();
    yield();
    cur = dashboardStateSig();
    if (cur != since) {
      sendGpioDashboardJson("true");
      return;
    }
    delay(1);
  }
  sendGpioDashboardJson("false");
}

void handleGpioHoldPost() {
  if (g_apMode) {
    g_server.send(403, F("application/json"), F("{\"ok\":false}"));
    return;
  }
  if (kRequireWebLogin && !cookieMatchesSession()) {
    g_server.send(401, F("application/json"), F("{\"ok\":false}"));
    return;
  }
  const int ch = g_server.hasArg(F("ch")) ? g_server.arg(F("ch")).toInt() : -1;
  const int down = g_server.hasArg(F("down")) ? g_server.arg(F("down")).toInt() : 0;
  if (ch >= 0 && ch < gpioRuntimeChannelCount())
    gpioUiHoldSet(ch, down != 0);
  gpioApplyOutputs();
  sendGpioDashboardJson("true");
}

void handleAtemStatus() {
  if (g_apMode) {
    g_server.send(401, F("application/json"), F("{\"ok\":false}"));
    return;
  }
  if (kRequireWebLogin && !cookieMatchesSession()) {
    g_server.send(401, F("application/json"), F("{\"ok\":false}"));
    return;
  }
  g_server.sendHeader(F("Cache-Control"), F("no-store"));
  g_server.send(200, F("application/json"), atemStatusJson());
}

void handleAtemEnablePost() {
  if (g_apMode) {
    g_server.send(403, F("application/json"), F("{\"ok\":false}"));
    return;
  }
  if (kRequireWebLogin && !cookieMatchesSession()) {
    g_server.send(401, F("application/json"), F("{\"ok\":false}"));
    return;
  }
  const String en = postFormField("en", 96);
  atemSetEnabledSave(en == "1" || en.equalsIgnoreCase("true"));
  g_server.sendHeader(F("Cache-Control"), F("no-store"));
  g_server.send(200, F("application/json"), F("{\"ok\":true}"));
}

void handleAtemConfigPost() {
  if (g_apMode) {
    g_server.send(403, F("application/json"), F("{\"ok\":false}"));
    return;
  }
  if (kRequireWebLogin && !cookieMatchesSession()) {
    g_server.send(401, F("application/json"), F("{\"ok\":false}"));
    return;
  }
  String ip = postFormField("ip", 512);
  ip.trim();
  atemSetIpSave(ip);
  g_server.sendHeader(F("Cache-Control"), F("no-store"));
  g_server.send(200, F("application/json"), F("{\"ok\":true}"));
}

void handleAtemMapPost() {
  if (g_apMode) {
    g_server.send(403, F("application/json"), F("{\"ok\":false}"));
    return;
  }
  if (kRequireWebLogin && !cookieMatchesSession()) {
    g_server.send(401, F("application/json"), F("{\"ok\":false}"));
    return;
  }
  const int ch = postFormField("ch", 256).toInt();
  const uint8_t me = static_cast<uint8_t>(postFormField("me", 256).toInt());
  const uint16_t inputId = static_cast<uint16_t>(postFormField("in", 256).toInt());
  const uint8_t tally = static_cast<uint8_t>(postFormField("tally", 256).toInt());
  atemSaveGpoMap(ch, me, inputId, tally);
  g_server.sendHeader(F("Cache-Control"), F("no-store"));
  g_server.send(200, F("application/json"), F("{\"ok\":true}"));
}

void handleProtocolStatus() {
  String j;
  j.reserve(128);
  j += F("{\"ok\":true,\"webapi\":");
  j += g_protoWebApi ? '1' : '0';
  j += F(",\"atem\":");
  j += atemIsEnabled() ? '1' : '0';
  j += '}';
  g_server.sendHeader(F("Cache-Control"), F("no-store"));
  g_server.send(200, F("application/json"), j);
}

void handleProtocolConfigPost() {
  if (g_apMode) {
    g_server.send(403, F("application/json"), F("{\"ok\":false}"));
    return;
  }
  if (kRequireWebLogin && !cookieMatchesSession()) {
    g_server.send(401, F("application/json"), F("{\"ok\":false}"));
    return;
  }

  const String k = postFormField("key", 96);
  const bool en = postFormField("en", 96).toInt() != 0;
  if (k == F("webapi")) {
    g_protoWebApi = en;
    g_prefs.putBool("proto_web", g_protoWebApi);
  } else if (k == F("atem")) {
    atemSetEnabledSave(en);
  } else {
    g_server.send(400, F("application/json"), F("{\"ok\":false,\"err\":\"invalid\"}"));
    return;
  }
  handleProtocolStatus();
}

void handleDeviceNameGet() {
  String j = F("{\"name\":\"");
  j += htmlTextEscape(g_deviceName);
  j += F("\"}");
  g_server.sendHeader(F("Cache-Control"), F("no-store"));
  g_server.send(200, F("application/json"), j);
}

void handleDeviceNamePost() {
  if (kRequireWebLogin && !cookieMatchesSession()) {
    g_server.send(401, F("application/json"), F("{\"ok\":false}"));
    return;
  }
  String name = postFormField("name", 64);
  name.replace("[", "");
  name.replace("]", "");
  name.trim();
  if (name.length() > 32) name = name.substring(0, 32);
  g_deviceName = name;
  g_prefs.putString("dev_name", g_deviceName);
  // Restart mDNS with updated hostname
  if (g_mdnsStarted) {
    MDNS.end();
    g_mdnsStarted = false;
  }
  g_server.sendHeader(F("Cache-Control"), F("no-store"));
  g_server.send(200, F("application/json"), F("{\"ok\":true}"));
}

void handleOtaChunk() {
  HTTPUpload &upload = g_server.upload();
  if (upload.status == UPLOAD_FILE_START) {
    Serial.printf("[OTA] Start: %s\n", upload.filename.c_str());
    if (!Update.begin(UPDATE_SIZE_UNKNOWN))
      Serial.printf("[OTA] begin error: %s\n", Update.errorString());
  } else if (upload.status == UPLOAD_FILE_WRITE) {
    if (Update.write(upload.buf, upload.currentSize) != upload.currentSize)
      Serial.printf("[OTA] write error: %s\n", Update.errorString());
  } else if (upload.status == UPLOAD_FILE_END) {
    if (Update.end(true))
      Serial.printf("[OTA] Complete: %u bytes\n", upload.totalSize);
    else
      Serial.printf("[OTA] end error: %s\n", Update.errorString());
  }
}

void handleOtaComplete() {
  if (kRequireWebLogin && !cookieMatchesSession()) {
    g_server.send(401, F("application/json"), F("{\"ok\":false,\"err\":\"unauthorized\"}"));
    return;
  }
  if (Update.hasError()) {
    g_server.send(500, F("application/json"),
                  String(F("{\"ok\":false,\"err\":\"")) + Update.errorString() + F("\"}"));
  } else {
    g_server.send(200, F("application/json"), F("{\"ok\":true}"));
    delay(400);
    ESP.restart();
  }
}

void handleGpioPolarityPost() {
  if (g_apMode) {
    g_server.send(403, F("application/json"), F("{\"ok\":false}"));
    return;
  }
  if (kRequireWebLogin && !cookieMatchesSession()) {
    g_server.send(401, F("application/json"), F("{\"ok\":false}"));
    return;
  }
  const int ch = postFormField("ch", 64).toInt();
  const bool inv = postFormField("inv", 64).toInt() != 0;
  if (ch < 0 || ch >= gpioRuntimeChannelCount()) {
    g_server.send(400, F("application/json"), F("{\"ok\":false}"));
    return;
  }
  gpioSetInvertChannel(ch, inv);
  gpioApplyOutputs();
  sendGpioDashboardJson("true");
}

void handleGpioLabelPost() {
  if (g_apMode) {
    g_server.send(403, F("application/json"), F("{\"ok\":false}"));
    return;
  }
  if (kRequireWebLogin && !cookieMatchesSession()) {
    g_server.send(401, F("application/json"), F("{\"ok\":false}"));
    return;
  }
  const int ch = postFormField("ch", 64).toInt();
  const String name = postFormField("name", 256);
  if (ch < 0 || ch >= gpioRuntimeChannelCount() || !gpioSetChannelName(ch, name)) {
    g_server.send(400, F("application/json"), F("{\"ok\":false,\"err\":\"invalid\"}"));
    return;
  }
  sendGpioDashboardJson("true");
}

void handleGpioInvertPost() {
  if (g_apMode) {
    g_server.send(403, F("application/json"), F("{\"ok\":false}"));
    return;
  }
  if (kRequireWebLogin && !cookieMatchesSession()) {
    g_server.send(401, F("application/json"), F("{\"ok\":false}"));
    return;
  }
  if (!g_server.hasArg(F("which"))) {
    g_server.send(400, F("application/json"), F("{\"ok\":false}"));
    return;
  }
  const String w = g_server.arg(F("which"));
  if (w == F("gpi0"))
    gpioToggleInvertGpi(0);
  else if (w == F("gpi1"))
    gpioToggleInvertGpi(1);
  else if (w == F("gpo0"))
    gpioToggleInvertGpo(0);
  else if (w == F("gpo1"))
    gpioToggleInvertGpo(1);
  else {
    g_server.send(400, F("application/json"), F("{\"ok\":false}"));
    return;
  }
  sendGpioDashboardJson("true");
}

void handlePortalSave() {
  if (!g_apMode) {
    sendRedirect("/");
    return;
  }

  String pick = g_server.hasArg(F("ssid_pick")) ? g_server.arg(F("ssid_pick")) : String();
  String manual = g_server.hasArg(F("ssid_manual")) ? g_server.arg(F("ssid_manual")) : String();
  pick.trim();
  manual.trim();
  String ssid;
  if (manual.length() > 0)
    ssid = manual;
  else if (pick.length() > 0)
    ssid = pick;
  String pass = g_server.hasArg(F("pass")) ? g_server.arg(F("pass")) : String();
  pass.trim();

  if (!ssid.length()) {
    const String scanOpts = buildWifiScanOptionsHtml();
    sendHtml(200, pagePortal(apIpStr(), "Please select a network or type an SSID.", scanOpts));
    return;
  }

  WiFi.mode(WIFI_AP_STA);
  WiFi.begin(ssid.c_str(), pass.c_str());

  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 45000) {
    delay(200);
    g_dns.processNextRequest();
    g_server.handleClient();
  }

  if (WiFi.status() != WL_CONNECTED) {
    WiFi.disconnect(true, false);
    WiFi.mode(WIFI_AP_STA);
    (void)bringUpSoftAp();
    const String scanOpts = buildWifiScanOptionsHtml();
    sendHtml(200, pagePortal(apIpStr(), "Could not connect. Check SSID and password.", scanOpts));
    return;
  }

  const String ip = WiFi.localIP().toString();
  saveWifiCreds(ssid, pass);
  g_rtcDrd.mark = 0;
  g_rtcDrd.cnt = 0;

  sendHtml(200, pageSuccess(ip));

  // Give the client time to render the success page
  uint32_t t0 = millis();
  while (millis() - t0 < 2500) {
    g_dns.processNextRequest();
    g_server.handleClient();
    delay(10);
  }

  // Keep SoftAP always on (same SSID/password as boot) while STA is connected.
  WiFi.mode(WIFI_AP_STA);
  if (!bringUpSoftAp())
    Serial.println(F("[PORTAL] softAP re-up before restart failed"));

  // Clear session after Wi-Fi setup
  g_sessionCookieValue = "";

  delay(200);
  ESP.restart();
}

void handleCaptiveApple() {
  sendRedirect("http://" + apIpStr() + "/");
}

void handleAndroid204() {
  // Some clients expect 204; redirect is fine with captive DNS
  sendRedirect("http://" + apIpStr() + "/");
}

void registerHandlers() {
  g_server.on(F("/"), HTTP_GET, handleRoot);
  g_server.on(F("/settings"), HTTP_GET, handleSettingsPage);
  g_server.on(F("/settings/channels"), HTTP_POST, handleSettingsChannelsPost);
  g_server.on(F("/settings/default"), HTTP_POST, handleSettingsDefaultPost);
  g_server.on(F("/login"), HTTP_POST, handleLoginPost);
  g_server.on(F("/logout"), HTTP_GET, handleLogout);
  g_server.on(F("/api/gpio"), HTTP_GET, handleApiGpio);
  g_server.on(F("/api/gpio/wait"), HTTP_GET, handleApiGpioWait);
  g_server.on(F("/api/companion/gpio"), HTTP_GET, handleApiCompanionGpio);
  g_server.on(F("/api/companion/wait"), HTTP_GET, handleApiCompanionWait);
  g_server.on(F("/api/companion/gpo/set"), HTTP_POST, handleApiCompanionGpoSet);
  g_server.on(F("/api/companion/gpo/set/bulk"), HTTP_POST, handleApiCompanionGpoBulkSet);
  g_server.on(F("/api/companion/module/download"), HTTP_GET, handleApiCompanionModuleDownload);
  g_server.on(F("/api/gpio/hold"), HTTP_POST, handleGpioHoldPost);
  g_server.on(F("/api/gpio/invert"), HTTP_POST, handleGpioInvertPost);
  g_server.on(F("/api/gpio/polarity"), HTTP_POST, handleGpioPolarityPost);
  g_server.on(F("/api/gpio/label"), HTTP_POST, handleGpioLabelPost);
  g_server.on(F("/api/device/name"), HTTP_GET, handleDeviceNameGet);
  g_server.on(F("/api/device/name"), HTTP_POST, handleDeviceNamePost);
  g_server.on(F("/api/ota/upload"), HTTP_POST, handleOtaComplete, handleOtaChunk);
  g_server.on(F("/api/protocol/status"), HTTP_GET, handleProtocolStatus);
  g_server.on(F("/api/protocol/config"), HTTP_POST, handleProtocolConfigPost);
  g_server.on(F("/api/net/status"), HTTP_GET, handleNetworkStatus);
  g_server.on(F("/api/update/info"), HTTP_GET, handleUpdateInfo);
  g_server.on(F("/api/net/config"), HTTP_POST, handleNetworkConfigPost);

  g_server.on(F("/api/atem/status"), HTTP_GET, handleAtemStatus);
  g_server.on(F("/api/atem/enable"), HTTP_POST, handleAtemEnablePost);
  g_server.on(F("/api/atem/config"), HTTP_POST, handleAtemConfigPost);
  g_server.on(F("/api/atem/map"), HTTP_POST, handleAtemMapPost);

  g_server.on(F("/portal/save"), HTTP_POST, handlePortalSave);

  g_server.on(F("/generate_204"), HTTP_GET, handleAndroid204);
  g_server.on(F("/gen_204"), HTTP_GET, handleAndroid204);
  g_server.on(F("/hotspot-detect.html"), HTTP_GET, handleCaptiveApple);
  g_server.on(F("/canonical.html"), HTTP_GET, handleCaptiveApple);
  g_server.on(F("/ncsi.txt"), HTTP_GET, handleCaptiveApple);
  g_server.on(F("/connecttest.txt"), HTTP_GET, handleCaptiveApple);

  g_server.onNotFound([]() {
    if (g_apMode) {
      sendRedirect("http://" + apIpStr() + "/");
      return;
    }
    if (g_server.method() == HTTP_GET) {
      handleRoot();
      return;
    }
    g_server.send(404, F("text/plain"), F("Not found"));
  });
}

/** Always-on SoftAP (192.168.4.x) alongside STA when credentials exist. */
static bool bringUpSoftAp() {
  WiFi.mode(WIFI_AP);
  const String ssid = apSsid();
  const String pass = apPassword();
  return WiFi.softAP(ssid.c_str(), pass.c_str(), 6, 0, 4);
}

bool startSoftApPortal() {
  g_apMode = false;
  return bringUpSoftAp();
}

bool connectStaWithTimeout(uint32_t timeoutMs) {
  if (!g_staSsid.length()) return false;
  WiFi.mode(WIFI_AP_STA);
  WiFi.setSleep(false);
  if (!g_ipDhcp) {
    IPAddress ip;
    IPAddress mask;
    if (parseIpv4(g_staticIp, ip) && parseIpv4(g_staticMask, mask)) {
      const IPAddress gw = guessGatewayFromIp(ip);
      WiFi.config(ip, gw, mask, gw, gw);
    }
  }
  WiFi.begin(g_staSsid.c_str(), g_staPass.c_str());
  const uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < timeoutMs) {
    delay(250);
  }
  return WiFi.status() == WL_CONNECTED;
}

} // namespace

void setup() {
  Serial.begin(115200);
  delay(80);
  Serial.println(F("[IONIX] USB serial 115200 — PlatformIO monitor_speed must match."));

  g_prefs.begin("ionix-gpio", false);
  loadWifiCreds();
  loadProtocolPrefs();
#if defined(USE_ETHERNET_PORT)
  beginEthernetDhcp();
#endif
  gpioLogicBegin(g_prefs);
  loadUserChannelsFromNvs();
  applyUserChannelsToRuntime();
  atemServiceBegin(g_prefs);
  gpioApplyOutputs();

  pinMode(PIN_FORCE_CONFIG, INPUT_PULLUP);
  delay(120);
  (void)digitalRead(PIN_FORCE_CONFIG);
  (void)detectDoubleReset();

  registerHandlers();
  if (!startSoftApPortal())
    Serial.println(F("softAP failed"));

  g_server.begin();
  oledStatusBegin();
  oledStatusSetReady(true);
  Serial.println(F("SoftAP always on; STA if configured"));
  Serial.println(apSsid());
  Serial.println(apIpStr());
}

void loop() {
  oledStatusLoop();
  atemServiceLoop();
  startMdnsIfReady();
  if (g_apMode) {
    g_dns.processNextRequest();
  }
  g_server.handleClient();
}
