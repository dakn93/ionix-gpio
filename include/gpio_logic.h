#pragma once

#include <Arduino.h>
#include <Preferences.h>

/** Call after prefs.begin("ionix-gpio"). Loads pins/labels; invert for all NVS-backed channels is refreshed in gpioSetRuntimeChannelCount. */
void gpioLogicBegin(Preferences &prefs);

void gpioApplyOutputs();
int gpioRuntimeChannelCount();
bool gpioRuntimeIsGpo(int ch);
void gpioSetRuntimeChannelCount(int count);
void gpioSetChannelType(int ch, bool isGpo);

bool gpioGetInvertGpi(int i); // 0..1
bool gpioGetInvertGpo(int i);
bool gpioGetInvertChannel(int ch);
void gpioToggleInvertGpi(int i);
void gpioToggleInvertGpo(int i);
void gpioSetInvertGpi(int i, bool invert);
void gpioSetInvertGpo(int i, bool invert);
void gpioSetInvertChannel(int ch, bool invert);

/** User label for channel 0..3 (empty NVS → default GPI 1 / GPO 2 …). */
String gpioChannelName(int ch);
/** Sanitize and store label (max 24 chars, [A-Za-z0-9 _.-]); empty clears to default. */
bool gpioSetChannelName(int ch, const String &name);
int gpioChannelPin(int ch);
bool gpioSetChannelPin(int ch, int pin);

/** UI / test: channel 0=GPI1 .. 3=GPO2 — while held, logical level is forced to 1 (GPO drives pin accordingly). */
void gpioUiHoldSet(int ch, bool on);

int gpioDisplayedLogicalGpi(int i);
int gpioDisplayedLogicalGpo(int i);
int gpioDisplayedLogicalChannel(int ch);

/** App / OSC: desired logical GPO (0/1), respects invert and hold. */
void gpioLogicalWriteGpo(int gpoIdx01, int logical01);
void gpioLogicalWriteChannel(int ch, int logical01);

/** Packed dashboard state for long-poll (display levels, invert, UI hold). */
uint32_t gpioDashboardStateSig(void);
