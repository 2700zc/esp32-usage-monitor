#include "hw/net.h"
#include <Preferences.h>

static Preferences _prefs;

int netScan(NetScanResult* out, int max) {
  int n = WiFi.scanNetworks();
  if (n < 0) return 0;
  if (n > max) n = max;
  for (int i = 0; i < n; i++) {
    strncpy(out[i].ssid, WiFi.SSID(i).c_str(), 32);
    out[i].rssi = WiFi.RSSI(i);
    out[i].encryption = WiFi.encryptionType(i);
  }
  return n;
}

bool netConnect(const char* ssid, const char* pass) {
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, pass);
  for (int i = 0; i < 50; i++) {
    if (WiFi.status() == WL_CONNECTED) {
      netSaveCred(ssid, pass);
      return true;
    }
    delay(200);
  }
  return WiFi.status() == WL_CONNECTED;
}

void netDisconnect() {
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
}

bool netConnected() {
  return WiFi.status() == WL_CONNECTED;
}

const char* netSSID() {
  return WiFi.SSID().c_str();
}

IPAddress netIP() {
  return WiFi.localIP();
}

int8_t netRSSI() {
  return WiFi.RSSI();
}

void netSaveCred(const char* ssid, const char* pass) {
  _prefs.begin("buddy", false);
  _prefs.putString("wifi_ssid", ssid);
  _prefs.putString("wifi_pass", pass);
  _prefs.end();
}

bool netLoadCred(char* ssid, size_t ssidLen, char* pass, size_t passLen) {
  _prefs.begin("buddy", true);
  String s = _prefs.getString("wifi_ssid", "");
  String p = _prefs.getString("wifi_pass", "");
  _prefs.end();
  if (s.length() == 0) return false;
  strncpy(ssid, s.c_str(), ssidLen - 1); ssid[ssidLen - 1] = 0;
  strncpy(pass, p.c_str(), passLen - 1); pass[passLen - 1] = 0;
  return true;
}
