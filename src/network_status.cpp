#include <network_status.h>

#include <WiFi.h>
#if defined(USE_ETHERNET_PORT)
#include <ETH.h>
#endif

namespace {

bool hasUsableIp(const IPAddress &ip) { return ip != IPAddress(0, 0, 0, 0); }

String normalizedMac(String mac) {
  mac.toUpperCase();
  if (mac.length() == 0)
    return String(F("-"));
  return mac;
}

} // namespace

NetworkStatusSnapshot readNetworkStatus() {
  NetworkStatusSnapshot snapshot;
  snapshot.primaryIp = F("-");
  snapshot.physicalIp = F("-");
  snapshot.wifiIp = F("-");
  snapshot.hotspotIp = F("-");
  snapshot.mac = normalizedMac(WiFi.macAddress());

#if defined(USE_ETHERNET_PORT)
  if (ETH.linkUp()) {
    const IPAddress ethIp = ETH.localIP();
    if (hasUsableIp(ethIp)) {
      snapshot.physicalIp = ethIp.toString();
      snapshot.primaryIp = snapshot.physicalIp;
    }
    snapshot.mac = normalizedMac(ETH.macAddress());
  }
#endif

  if (WiFi.status() == WL_CONNECTED) {
    const IPAddress wifiIp = WiFi.localIP();
    if (hasUsableIp(wifiIp))
      snapshot.wifiIp = wifiIp.toString();
  }

  const IPAddress hotspotIp = WiFi.softAPIP();
  if (hasUsableIp(hotspotIp))
    snapshot.hotspotIp = hotspotIp.toString();

  if (snapshot.primaryIp == F("-")) {
    if (snapshot.wifiIp != F("-"))
      snapshot.primaryIp = snapshot.wifiIp;
    else if (snapshot.hotspotIp != F("-"))
      snapshot.primaryIp = snapshot.hotspotIp;
  }

  return snapshot;
}
