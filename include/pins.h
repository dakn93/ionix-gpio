#pragma once

// Inputs (GPIO13/14 — avoid 32/33 which are noisy / strapped on many boards)
#define PIN_GPI_1 13
#define PIN_GPI_2 14

// Outputs
#define PIN_GPO_1 18
#define PIN_GPO_2 19

// BOOT = GPIO0: hold at power-on / reset to force config hotspot
#define PIN_FORCE_CONFIG 0

/** SoftAP WPA2 password (captive portal / QR WIFI string). Same as Web UI login optional convention. */
#define IONIX_SOFTAP_PASSWORD "pass"
