#pragma once

#include <Arduino.h>
#include <Preferences.h>

void atemServiceBegin(Preferences &prefs);
void atemServiceLoop();
/** Call from the GPIO long-poll wait loop so tally updates wake waiters. */
void atemServiceOnLongPollTick();
uint32_t atemLiveSig();
String atemStatusJson();

void atemSetEnabledSave(bool enabled);
void atemSetIpSave(const String &ip);
void atemSaveGpoMap(int channel, uint8_t me03, uint16_t inputId, uint8_t tally012);

bool atemIsEnabled();
/** Configured mixer IP from NVS (may be empty). */
String atemGetConfiguredIp();
