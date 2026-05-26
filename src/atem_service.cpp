#include <atem_service.h>
#include <ATEM.h>
#include <gpio_logic.h>

#include <WiFi.h>
#if defined(USE_ETHERNET_PORT)
#include <ETH.h>
#endif

namespace {

Preferences *sPrefs = nullptr;
bool sEn = false;
String sIp;
ATEM *sAtem = nullptr;
constexpr uint16_t kAtemLocalUdp = 9912;
constexpr uint32_t kAtemRetryMs = 10000; // minimum ms between connection attempts
constexpr uint8_t  kAtemMeAll   = 0xFF;  // sentinel: combined tally across all M/Es
uint32_t sLastConnectAttemptMs = 0;

#if defined(CONFIG_IDF_TARGET_ESP32P4)
constexpr int kMaxAtemMaps = 24;
#else
constexpr int kMaxAtemMaps = 12;
#endif
uint8_t sMapMe[kMaxAtemMaps] = {};
uint16_t sMapIn[kMaxAtemMaps] = {};
uint8_t sMapTally[kMaxAtemMaps] = {};
int8_t sLastAppliedHi[kMaxAtemMaps] = {};

void loadFromNvs() {
  if (!sPrefs)
    return;
  sEn = sPrefs->getBool("atem_en", false);
  sIp = sPrefs->getString("atem_ip", "");
  for (int ch = 0; ch < kMaxAtemMaps; ch++) {
    char km[16];
    char ki[16];
    char ki2[16];
    char kt[16];
    snprintf(km, sizeof(km), "atem_m%d_me", ch);
    snprintf(ki, sizeof(ki), "atem_m%d_in", ch);   // legacy uint8 input
    snprintf(ki2, sizeof(ki2), "atem_m%d_i2", ch); // new uint16 input
    snprintf(kt, sizeof(kt), "atem_m%d_tt", ch);
    sMapMe[ch] = sPrefs->getUChar(km, 0);
    // Prefer the new uint16 key; fall back to legacy uint8 key.
    uint16_t v16 = sPrefs->getUShort(ki2, 0);
    if (v16 == 0)
      v16 = sPrefs->getUChar(ki, 0);
    sMapIn[ch] = v16;
    sMapTally[ch] = sPrefs->getUChar(kt, 0);
  }
  /** Backward compat from older 2-slot keys. */
  if (sMapMe[0] == 0 && sMapIn[0] == 0 && sMapTally[0] == 0) {
    sMapMe[0] = sPrefs->getUChar("atem_g0_me", 0);
    sMapIn[0] = sPrefs->getUChar("atem_g0_in", 0);
    sMapTally[0] = sPrefs->getUChar("atem_g0_tt", 0);
  }
  if (sMapMe[1] == 0 && sMapIn[1] == 0 && sMapTally[1] == 0) {
    sMapMe[1] = sPrefs->getUChar("atem_g1_me", 0);
    sMapIn[1] = sPrefs->getUChar("atem_g1_in", 0);
    sMapTally[1] = sPrefs->getUChar("atem_g1_tt", 0);
  }
  for (int ch = 0; ch < kMaxAtemMaps; ch++)
    sLastAppliedHi[ch] = -1;
}

void destroyAtem() {
  if (sAtem) {
    delete sAtem;
    sAtem = nullptr;
  }
}

bool protocolLinkReady() {
#if defined(USE_ETHERNET_PORT)
  return ETH.linkUp() && ETH.localIP() != IPAddress(0, 0, 0, 0);
#else
  return WiFi.status() == WL_CONNECTED;
#endif
}

void ensureClient() {
  if (!sEn || sIp.length() == 0 || !protocolLinkReady()) {
    destroyAtem();
    return;
  }
  if (sAtem)
    return;
  // Rate-limit reconnect attempts — connect() blocks up to 7s, so we must
  // not call it every loop tick when the ATEM is unreachable.
  const uint32_t now = millis();
  if (now - sLastConnectAttemptMs < kAtemRetryMs)
    return;
  sLastConnectAttemptMs = now;
  IPAddress ip;
  if (!ip.fromString(sIp))
    return;
  sAtem = new ATEM(ip, kAtemLocalUdp);
  sAtem->serialOutput(false);
  sAtem->connect();
  if (sAtem->sessionId() == 0) {
    delete sAtem;
    sAtem = nullptr;
  }
}

void pumpUdp() {
  if (!sEn) {
    destroyAtem();
    return;
  }
  ensureClient();
  if (!sAtem)
    return;
  for (int i = 0; i < 16; i++)
    sAtem->runLoop();
}

void applyGpoFromTally() {
  if (!sEn || !sAtem || !sAtem->hasInitialized())
    return;
  const int chCount = gpioRuntimeChannelCount();
  for (int ch = 0; ch < chCount && ch < kMaxAtemMaps; ch++) {
    if (!gpioRuntimeIsGpo(ch))
      continue;
    if (sMapTally[ch] == 0 || sMapIn[ch] == 0) {
      sLastAppliedHi[ch] = -1;
      continue;
    }
    bool hi = false;
    const uint8_t me = sMapMe[ch];
    if (me == kAtemMeAll || me == 0) {
      // ALL M/E or legacy 0: use combined TlFl/TlIn tally
      if (sMapTally[ch] == 1)
        hi = sAtem->getProgramTally(sMapIn[ch]);
      else if (sMapTally[ch] == 2)
        hi = sAtem->getPreviewTally(sMapIn[ch]);
    } else {
      // Specific M/E (1-based in UI, 0-based in library)
      const uint8_t meIdx = me - 1;
      if (sMapTally[ch] == 1)
        hi = sAtem->getProgramTallyMe(sMapIn[ch], meIdx);
      else if (sMapTally[ch] == 2)
        hi = sAtem->getPreviewTallyMe(sMapIn[ch], meIdx);
    }
    const int8_t next = hi ? 1 : 0;
    if (sLastAppliedHi[ch] != next) {
      Serial.printf("[ATEM TALLY] ch=%d in=%u me=%u type=%u -> %d\n",
                    ch, sMapIn[ch], sMapMe[ch], sMapTally[ch], next);
      sLastAppliedHi[ch] = next;
      gpioLogicalWriteChannel(ch, next);
    }
  }
}

void jsonAppendEscaped(String &j, const String &s) {
  for (unsigned i = 0; i < s.length(); i++) {
    const char c = s[i];
    if (c == '\\' || c == '"')
      j += '\\';
    j += c;
  }
}

} // namespace

void atemServiceBegin(Preferences &prefs) {
  sPrefs = &prefs;
  loadFromNvs();
}

String atemGetConfiguredIp() { return sIp; }

bool atemIsEnabled() { return sEn; }

void atemServiceLoop() {
  pumpUdp();
  applyGpoFromTally();
}

void atemServiceOnLongPollTick() {
  pumpUdp();
  applyGpoFromTally();
}

uint32_t atemLiveSig() {
  if (!sEn)
    return 0;
  uint32_t x = 0x13579BDFu;
  if (!sAtem || !sAtem->hasInitialized())
    return x ^ 0x11111111u;
  x ^= 0x042u;
  const int chCount = gpioRuntimeChannelCount();
  for (int ch = 0; ch < chCount && ch < kMaxAtemMaps; ch++) {
    if (!gpioRuntimeIsGpo(ch))
      continue;
    if (sMapTally[ch] == 0 || sMapIn[ch] == 0)
      continue;
    const bool pr = sAtem->getProgramTally(sMapIn[ch]);
    const bool pv = sAtem->getPreviewTally(sMapIn[ch]);
    x ^= (uint32_t)(pr ? 3 : 0) << (ch % 24);
    x ^= (uint32_t)(pv ? 5 : 0) << ((ch + 7) % 24);
  }
  return x;
}

String atemStatusJson() {
  String j;
  j.reserve(8192);
  j = F("{\"ok\":true,\"en\":");
  j += sEn ? '1' : '0';
  j += F(",\"wifi\":");
  j += (WiFi.status() == WL_CONNECTED) ? '1' : '0';
  j += F(",\"net\":");
  j += protocolLinkReady() ? '1' : '0';
  j += F(",\"ip\":\"");
  jsonAppendEscaped(j, sIp);
  j += F("\",\"init\":");
  j += (sAtem && sAtem->hasInitialized()) ? '1' : '0';
  j += F(",\"slots\":");
  if (sAtem && sAtem->hasInitialized())
    j += String(sAtem->getTallySlotCount());
  else
    j += '0';
  j += F(",\"maps\":[");
  bool firstMap = true;
  const int chCount = gpioRuntimeChannelCount();
  for (int ch = 0; ch < chCount && ch < kMaxAtemMaps; ch++) {
    if (!gpioRuntimeIsGpo(ch))
      continue;
    if (!firstMap)
      j += ',';
    firstMap = false;
    j += F("{\"ch\":");
    j += String(ch);
    j += F(",\"me\":");
    j += String(sMapMe[ch]);
    j += F(",\"in\":");
    j += String(sMapIn[ch]);
    j += F(",\"tally\":");
    j += String(sMapTally[ch]);
    j += '}';
  }
  j += F("],\"inputs\":[");
  if (sAtem && sAtem->hasInitialized()) {
    const int count = sAtem->getInPrCount();
    bool first = true;
    for (int idx = 0; idx < count; idx++) {
      uint16_t srcId = 0;
      const char *nm = nullptr;
      if (!sAtem->getInPrEntry(idx, srcId, nm)) continue;
      String label = String(F("Input ")) + String(srcId);
      if (nm && nm[0]) label = String(nm);
      if (!first) j += ',';
      first = false;
      j += F("{\"id\":");
      j += String(srcId);
      j += F(",\"n\":\"");
      jsonAppendEscaped(j, label);
      j += F("\"}");
      if (j.length() > 7800) break;
    }
  }
  j += F("]}");
  return j;
}

void atemSetEnabledSave(bool enabled) {
  sEn = enabled;
  for (int ch = 0; ch < kMaxAtemMaps; ch++)
    sLastAppliedHi[ch] = -1;
  if (!sPrefs)
    return;
  sPrefs->putBool("atem_en", sEn);
  destroyAtem();
}

void atemSetIpSave(const String &ip) {
  sIp = ip;
  sIp.trim();
  for (int ch = 0; ch < kMaxAtemMaps; ch++)
    sLastAppliedHi[ch] = -1;
  if (!sPrefs)
    return;
  sPrefs->putString("atem_ip", sIp);
  destroyAtem();
}

void atemSaveGpoMap(int channel, uint8_t me03, uint16_t inputId, uint8_t tally012) {
  if (channel < 0 || channel >= gpioRuntimeChannelCount() || channel >= kMaxAtemMaps)
    return;
  if (!gpioRuntimeIsGpo(channel))
    return;
  // me: 0xFF = ALL M/E, 1-4 = specific M/E
  sMapMe[channel] = (me03 == kAtemMeAll || me03 == 0) ? kAtemMeAll : (me03 > 4 ? 4 : me03);
  sMapIn[channel] = inputId;
  sMapTally[channel] = tally012 > 2 ? 0 : tally012;
  sLastAppliedHi[channel] = -1;
  if (!sPrefs)
    return;
  char km[16];
  char ki[16];
  char ki2[16];
  char kt[16];
  snprintf(km, sizeof(km), "atem_m%d_me", channel);
  snprintf(ki, sizeof(ki), "atem_m%d_in", channel);
  snprintf(ki2, sizeof(ki2), "atem_m%d_i2", channel);
  snprintf(kt, sizeof(kt), "atem_m%d_tt", channel);
  sPrefs->putUChar(km, sMapMe[channel]);
  sPrefs->putUShort(ki2, sMapIn[channel]);
  sPrefs->remove(ki); // ensure legacy uint8 key cannot mask new uint16 value
  sPrefs->putUChar(kt, sMapTally[channel]);
}
