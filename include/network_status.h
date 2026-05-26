#pragma once

#include <Arduino.h>

struct NetworkStatusSnapshot {
  String primaryIp;
  String physicalIp;
  String wifiIp;
  String hotspotIp;
  String mac;
};

NetworkStatusSnapshot readNetworkStatus();
