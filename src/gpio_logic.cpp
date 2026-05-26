#include <gpio_logic.h>
#include <pins.h>

#include <Arduino.h>
#include <cstdio>

namespace {

#if defined(CONFIG_IDF_TARGET_ESP32P4)
constexpr int kMaxRuntimeChannels = 24;
#else
constexpr int kMaxRuntimeChannels = 12;
#endif
Preferences *s_prefs = nullptr;
int s_channelCount = 4;
bool s_isGpo[kMaxRuntimeChannels] = {false, false, true, true};
bool s_inv[kMaxRuntimeChannels] = {};
volatile bool s_uiHold[kMaxRuntimeChannels] = {};
uint8_t s_gpoDesired[kMaxRuntimeChannels] = {};
String s_lbl[kMaxRuntimeChannels];
int s_pinMap[kMaxRuntimeChannels] = {PIN_GPI_1, PIN_GPI_2, PIN_GPO_1, PIN_GPO_2};
#if defined(CONFIG_IDF_TARGET_ESP32P4)
// P4: strictly use the validated pin list provided by wiring plan.
// Reserved and intentionally excluded: 7(SDA), 8(SCL), 37(TXD), 38(RXD), ETH RMII pins.
// GPIO0 (BOOT strap) and GPIO45 are excluded due board-level conflicts.
static const int kAllowedPins[] = {22, 5, 4, 1, 36, 32, 25, 54, 46, 27, 23, 21, 20, 6, 3, 2, 24, 33, 26, 48, 53, 47};
#else
static const int kAllowedPins[] = {4, 5, 12, 13, 14, 15, 16, 17, 18, 19, 21, 22, 23, 25, 26, 27, 32, 33};
#endif

static uint32_t hashString(const String &s) {
  uint32_t h = 2166136261u;
  for (unsigned i = 0; i < s.length(); i++) {
    h ^= (uint8_t)s[i];
    h *= 16777619u;
  }
  return h;
}

static bool isValidLabelChar(char c) {
  if (c >= 'a' && c <= 'z')
    return true;
  if (c >= 'A' && c <= 'Z')
    return true;
  if (c >= '0' && c <= '9')
    return true;
  /** Common in user channel names (broadcast / routing text). */
  if (c == ' ' || c == '_' || c == '.' || c == '-' || c == '/' || c == '(' || c == ')' || c == ',' || c == ':' || c == '+')
    return true;
  return false;
}

static String defaultLabelForChannel(int ch) {
  int ordinal = 0;
  if (s_isGpo[ch]) {
    for (int i = 0; i <= ch && i < s_channelCount; i++)
      if (s_isGpo[i])
        ordinal++;
    return String("GPO ") + String(ordinal);
  }
  for (int i = 0; i <= ch && i < s_channelCount; i++)
    if (!s_isGpo[i])
      ordinal++;
  return String("GPI ") + String(ordinal);
}

static void loadLabelsFromNvs() {
  if (!s_prefs)
    return;
  for (int i = 0; i < s_channelCount; i++) {
    char k[12];
    snprintf(k, sizeof(k), "lbl%d", i);
    s_lbl[i] = s_prefs->getString(k, "");
    s_lbl[i].trim();
    if (s_lbl[i].length() > 24)
      s_lbl[i] = s_lbl[i].substring(0, 24);
  }
}

static void saveLabelNvs(int ch) {
  if (!s_prefs || ch < 0 || ch >= s_channelCount)
    return;
  char k[12];
  snprintf(k, sizeof(k), "lbl%d", ch);
  if (s_lbl[ch].length() == 0)
    s_prefs->remove(k);
  else
    s_prefs->putString(k, s_lbl[ch]);
}

static int pinForChannelIndex(int ix) {
  return (ix >= 0 && ix < s_channelCount) ? s_pinMap[ix] : -1;
}

static int logicalFromPhysical(bool invert, int physIsHigh) {
  const bool p = physIsHigh != 0;
  return (p ^ invert) ? 1 : 0;
}

static int physicalFromLogical(bool invert, int logical01) {
  const bool lo = logical01 != 0;
  return (lo ^ invert) ? HIGH : LOW;
}

static void saveInvertNvs() {
  if (!s_prefs)
    return;
  for (int i = 0; i < s_channelCount; i++) {
    char k[12];
    snprintf(k, sizeof(k), "inv%d", i);
    s_prefs->putBool(k, s_inv[i]);
  }
}

static bool isAllowedPin(int pin) {
  for (unsigned i = 0; i < sizeof(kAllowedPins) / sizeof(kAllowedPins[0]); i++) {
    if (kAllowedPins[i] == pin)
      return true;
  }
  return false;
}

static bool pinUsedByOtherChannel(int ch, int pin) {
  for (int i = 0; i < s_channelCount; i++) {
    if (i == ch)
      continue;
    if (s_pinMap[i] == pin)
      return true;
  }
  return false;
}

static void loadPinsFromNvs() {
  if (!s_prefs)
    return;
  for (int ch = 0; ch < s_channelCount; ch++) {
    char k[12];
    snprintf(k, sizeof(k), "pin%d", ch);
    int p = s_prefs->getInt(k, s_pinMap[ch]);
    if (isAllowedPin(p))
      s_pinMap[ch] = p;
  }
  for (int ch = 0; ch < s_channelCount; ch++) {
    if (!isAllowedPin(s_pinMap[ch]) || pinUsedByOtherChannel(ch, s_pinMap[ch])) {
      s_pinMap[0] = PIN_GPI_1;
      s_pinMap[1] = PIN_GPI_2;
      s_pinMap[2] = PIN_GPO_1;
      s_pinMap[3] = PIN_GPO_2;
      for (int i = 4; i < s_channelCount; i++)
        s_pinMap[i] = kAllowedPins[i % (sizeof(kAllowedPins) / sizeof(kAllowedPins[0]))];
      break;
    }
  }
}

static void configurePinModes() {
  for (int i = 0; i < s_channelCount; i++)
    pinMode(s_pinMap[i], s_isGpo[i] ? OUTPUT : INPUT_PULLUP);
}

static void savePinToNvs(int ch) {
  if (!s_prefs || ch < 0 || ch >= s_channelCount)
    return;
  char k[12];
  snprintf(k, sizeof(k), "pin%d", ch);
  s_prefs->putInt(k, s_pinMap[ch]);
}

} // namespace

void gpioLogicBegin(Preferences &prefs) {
  s_prefs = &prefs;
  for (int i = 0; i < s_channelCount; i++) {
    char k[12];
    snprintf(k, sizeof(k), "inv%d", i);
    s_inv[i] = prefs.getBool(k, false);
  }
  loadPinsFromNvs();
  configurePinModes();
  loadLabelsFromNvs();
}

int gpioRuntimeChannelCount() { return s_channelCount; }
bool gpioRuntimeIsGpo(int ch) { return (ch >= 0 && ch < s_channelCount) ? s_isGpo[ch] : false; }
bool gpioGetInvertChannel(int ch) { return (ch >= 0 && ch < s_channelCount) ? s_inv[ch] : false; }

bool gpioGetInvertGpi(int i) {
  int ord = 0;
  for (int ch = 0; ch < s_channelCount; ch++) {
    if (s_isGpo[ch])
      continue;
    if (ord == i)
      return s_inv[ch];
    ord++;
  }
  return false;
}

bool gpioGetInvertGpo(int i) {
  int ord = 0;
  for (int ch = 0; ch < s_channelCount; ch++) {
    if (!s_isGpo[ch])
      continue;
    if (ord == i)
      return s_inv[ch];
    ord++;
  }
  return false;
}

void gpioSetRuntimeChannelCount(int count) {
  if (count < 1)
    count = 1;
  if (count > kMaxRuntimeChannels)
    count = kMaxRuntimeChannels;
  s_channelCount = count;
  /** gpioLogicBegin runs before user channel count is known; reload all invN keys now. */
  if (s_prefs) {
    for (int i = 0; i < s_channelCount; i++) {
      char k[12];
      snprintf(k, sizeof(k), "inv%d", i);
      s_inv[i] = s_prefs->getBool(k, false);
    }
  }
  configurePinModes();
}

void gpioSetChannelType(int ch, bool isGpo) {
  if (ch < 0 || ch >= s_channelCount)
    return;
  s_isGpo[ch] = isGpo;
  configurePinModes();
}

void gpioToggleInvertGpi(int i) {
  int ord = 0;
  for (int ch = 0; ch < s_channelCount; ch++) {
    if (s_isGpo[ch])
      continue;
    if (ord++ == i) {
      s_inv[ch] = !s_inv[ch];
      saveInvertNvs();
      return;
    }
  }
}

void gpioToggleInvertGpo(int i) {
  int ord = 0;
  for (int ch = 0; ch < s_channelCount; ch++) {
    if (!s_isGpo[ch])
      continue;
    if (ord++ == i) {
      s_inv[ch] = !s_inv[ch];
      saveInvertNvs();
      gpioApplyOutputs();
      return;
    }
  }
}

void gpioSetInvertGpi(int i, bool invert) {
  int ord = 0;
  for (int ch = 0; ch < s_channelCount; ch++) {
    if (s_isGpo[ch])
      continue;
    if (ord++ == i) {
      gpioSetInvertChannel(ch, invert);
      return;
    }
  }
}

void gpioSetInvertGpo(int i, bool invert) {
  int ord = 0;
  for (int ch = 0; ch < s_channelCount; ch++) {
    if (!s_isGpo[ch])
      continue;
    if (ord++ == i) {
      gpioSetInvertChannel(ch, invert);
      return;
    }
  }
}

void gpioSetInvertChannel(int ch, bool invert) {
  if (ch < 0 || ch >= s_channelCount)
    return;
  if (s_inv[ch] == invert)
    return;
  s_inv[ch] = invert;
  saveInvertNvs();
  gpioApplyOutputs();
}

String gpioChannelName(int ch) {
  if (ch < 0 || ch >= s_channelCount)
    return String();
  if (s_lbl[ch].length())
    return s_lbl[ch];
  return defaultLabelForChannel(ch);
}

bool gpioSetChannelName(int ch, const String &name) {
  if (ch < 0 || ch >= s_channelCount)
    return false;
  String t = name;
  t.trim();
  if (t.length() > 24)
    t = t.substring(0, 24);
  if (t.length()) {
    for (unsigned i = 0; i < t.length(); i++) {
      if (!isValidLabelChar(t[i]))
        return false;
    }
  }
  s_lbl[ch] = t;
  saveLabelNvs(ch);
  return true;
}

int gpioChannelPin(int ch) { return (ch >= 0 && ch < s_channelCount) ? s_pinMap[ch] : -1; }

bool gpioSetChannelPin(int ch, int pin) {
  if (ch < 0 || ch >= s_channelCount || !isAllowedPin(pin))
    return false;
  if (pinUsedByOtherChannel(ch, pin))
    return false;
  if (s_pinMap[ch] == pin)
    return true;
  s_pinMap[ch] = pin;
  configurePinModes();
  gpioApplyOutputs();
  savePinToNvs(ch);
  return true;
}

void gpioUiHoldSet(int ch, bool on) {
  if (ch < 0 || ch >= s_channelCount)
    return;
  s_uiHold[ch] = on;
}

int gpioDisplayedLogicalGpi(int i) {
  int ord = 0;
  for (int ch = 0; ch < s_channelCount; ch++) {
    if (s_isGpo[ch])
      continue;
    if (ord++ == i)
      return gpioDisplayedLogicalChannel(ch);
  }
  return 0;
}

int gpioDisplayedLogicalGpo(int i) {
  int ord = 0;
  for (int ch = 0; ch < s_channelCount; ch++) {
    if (!s_isGpo[ch])
      continue;
    if (ord++ == i)
      return gpioDisplayedLogicalChannel(ch);
  }
  return 0;
}

int gpioDisplayedLogicalChannel(int ch) {
  if (ch < 0 || ch >= s_channelCount)
    return 0;
  if (s_uiHold[ch])
    return 1;
  const int pin = pinForChannelIndex(ch);
  const int phys = digitalRead(pin);
  return logicalFromPhysical(s_inv[ch], phys);
}

void gpioApplyOutputs() {
  for (int i = 0; i < s_channelCount; i++) {
    if (!s_isGpo[i])
      continue;
    int logical = s_gpoDesired[i] ? 1 : 0;
    if (s_uiHold[i])
      logical = 1;
    digitalWrite(pinForChannelIndex(i), physicalFromLogical(s_inv[i], logical));
  }
}

void gpioLogicalWriteGpo(int i, int logical01) {
  int ord = 0;
  for (int ch = 0; ch < s_channelCount; ch++) {
    if (!s_isGpo[ch])
      continue;
    if (ord++ == i) {
      gpioLogicalWriteChannel(ch, logical01);
      return;
    }
  }
}

void gpioLogicalWriteChannel(int ch, int logical01) {
  if (ch < 0 || ch >= s_channelCount || !s_isGpo[ch])
    return;
  s_gpoDesired[ch] = logical01 ? 1 : 0;
  gpioApplyOutputs();
}

uint32_t gpioDashboardStateSig(void) {
  uint32_t s = 0;
  for (int i = 0; i < s_channelCount; i++) {
    s ^= (uint32_t)(gpioDisplayedLogicalChannel(i) & 1) << (i % 24);
    s ^= (s_inv[i] ? 1u : 0u) << ((i + 7) % 24);
    s ^= (s_uiHold[i] ? 1u : 0u) << ((i + 13) % 24);
    s ^= (s_isGpo[i] ? 1u : 0u) << ((i + 19) % 24);
  }
  uint32_t lh = 0;
  for (int i = 0; i < s_channelCount; i++)
    lh ^= hashString(s_lbl[i]) ^ (uint32_t)(i * 0x1000193u);
  s ^= lh;
  return s;
}
