// C3 AdBlock — DNS sinkhole + web dashboard for the ESP32-C3 (no PSRAM).
// Blocklist = sorted 40-bit FNV-1a hashes in flash, binary-searched.
// Dashboard at http://c3adblock.local : per-client stats, system info,
// ban clients, add custom block domains. All control state persisted to flash.

#include <Arduino.h>
#include <WiFi.h>
#include <LittleFS.h>
#include <ESPmDNS.h>
#include <WebServer.h>
#include <Update.h>            // firmware OTA
#include <HTTPClient.h>        // remote blocklist fetch
#include <WiFiClientSecure.h>  // https fetch
#include <ArduinoOTA.h>        // network firmware flashing (pio run over wifi)
#include <DNSServer.h>         // captive-portal catch-all DNS
#include <Preferences.h>       // NVS store for provisioned WiFi creds
#include "lwip/etharp.h"
#include "lwip/netif.h"
#include "lwip/sockets.h"
#include "esp_task.h"
#include <freertos/message_buffer.h>
#include "pending.h"
#include "esp_private/wifi.h"
#include "secrets.h"   // WIFI_SSID / WIFI_PASS — used only as a FALLBACK if no creds
                       // have been provisioned via the captive portal (copy secrets.example.h)

// ---- config ----
#ifndef UPSTREAM_IP
#define UPSTREAM_IP 9, 9, 9, 9                    // Quad9
#endif
#ifndef UPSTREAM_PORT
#define UPSTREAM_PORT 53
#endif
#ifndef UPSTREAM2_IP
#define UPSTREAM2_IP 149, 112, 112, 112
#endif
static const IPAddress UPSTREAM(UPSTREAM_IP);
static const IPAddress UPSTREAM2(UPSTREAM2_IP);
static void pinDns() { WiFi.setDNS(UPSTREAM, UPSTREAM2); }
static const uint16_t DNS_PORT = 53;
static const char* BLOCKLIST_PATH = "/blocklist.bin";
static const int HASH_BYTES = 5;
static const uint64_t HASH_MASK = (1ULL << (HASH_BYTES * 8)) - 1;
static const int INDEX_ENTRIES = 4096;   // 20 KB first-level flash index
static const int CACHE_SIZE = 256;       // must be power of 2
static const int MAX_RANGE = 256;        // max hashes per index bucket (fine up to ~1M hashes; flash holds far fewer)

// ---- globals ----
WebServer web(80);
File blocklist;
uint32_t numHashes = 0, totalBlocked = 0, totalAllowed = 0;
uint8_t buf[1536];   // fits any non-fragmented UDP reply (EDNS answers can exceed 512)

// first-level flash index (sorted sample hashes) + small direct-mapped cache
static uint8_t blIndex[INDEX_ENTRIES][HASH_BYTES];
static uint8_t cacheKey[CACHE_SIZE][HASH_BYTES];
static uint8_t cacheRes[CACHE_SIZE];
static uint8_t cacheValid[CACHE_SIZE];
static uint8_t rangeBuf[MAX_RANGE * HASH_BYTES];

struct Dev { uint32_t ip; uint8_t mac[6]; uint32_t blocked, allowed, lastSeen, macTry; bool banned; String label; };
static const int MAX_CLIENTS = 96;
Dev clients[MAX_CLIENTS]; int numClients = 0;

static const int MAX_CUSTOM = 200;
String customDom[MAX_CUSTOM]; uint64_t customHash[MAX_CUSTOM]; int numCustom = 0;

static const int MAX_BAN = 32;
uint32_t bannedIP[MAX_BAN]; int numBanned = 0;

// remote blocklist auto-update
String updateUrl = "";              // URL of a prebuilt blocklist.bin (e.g. GitHub release asset)
uint32_t updateIntervalH = 24;      // hours between auto-fetches
uint32_t lastCheckMs = 0;
uint8_t  updateDays = 0;            // bit0 Sun..bit6 Sat; 0 = use updateIntervalH instead
uint8_t  updateHour = 3;
int      lastSchedDay = -1;
String updateStatus = "Never";

// WiFi provisioning (captive portal)
Preferences prefs;
DNSServer   dnsPortal;
String      portalOpts;             // <option> list of scanned networks, built once at portal start

// blocking pause (Pi-hole-style "disable for a while")
bool     blockingOn = true;
uint32_t resumeAt   = 0;            // millis() to auto-resume; 0 = paused indefinitely / not paused
uint8_t  nightMode  = 0;
uint8_t  nightFrom  = 22, nightTo = 7;
int8_t   nightTz    = 7;            // hours from UTC

// ---------- hashing / matching ----------
static uint64_t fnv40(const char* s, size_t n) {
  uint64_t h = 0xcbf29ce484222325ULL;
  for (size_t i = 0; i < n; i++) { h ^= (uint8_t)s[i]; h *= 0x100000001b3ULL; }
  return h & HASH_MASK;
}
static inline uint64_t unpackHash(const uint8_t* b) {
  uint64_t v = 0;
  for (int k = 0; k < HASH_BYTES; k++) v |= (uint64_t)b[k] << (8 * k);
  return v;
}
static inline void packHash(uint64_t h, uint8_t* b) {
  for (int k = 0; k < HASH_BYTES; k++) { b[k] = (uint8_t)h; h >>= 8; }
}

static void buildFlashIndex() {
  if (!blocklist || numHashes == 0) return;
  for (int i = 0; i < INDEX_ENTRIES; i++) {
    uint32_t pos = (uint32_t)((uint64_t)i * (numHashes - 1) / (INDEX_ENTRIES - 1));
    blocklist.seek((uint32_t)pos * HASH_BYTES);
    blocklist.read(blIndex[i], HASH_BYTES);
  }
  for (int i = 0; i < CACHE_SIZE; i++) cacheValid[i] = 0;
}

static bool inFlash(uint64_t h) {
  if (numHashes == 0) return false;
  uint64_t first = unpackHash(blIndex[0]);
  uint64_t last  = unpackHash(blIndex[INDEX_ENTRIES - 1]);
  if (h < first || h > last) return false;

  int lo = 0, hi = INDEX_ENTRIES - 2, seg = 0;
  while (lo <= hi) {
    int mid = (lo + hi) >> 1;
    uint64_t midv = unpackHash(blIndex[mid]);
    if (midv < h) {
      seg = mid;
      lo = mid + 1;
    } else if (midv > h) {
      hi = mid - 1;
    } else {
      return true;
    }
  }

  uint32_t startPos = (uint32_t)((uint64_t)seg * (numHashes - 1) / (INDEX_ENTRIES - 1));
  uint32_t endPos   = (uint32_t)((uint64_t)(seg + 1) * (numHashes - 1) / (INDEX_ENTRIES - 1));
  if (endPos >= numHashes) endPos = numHashes - 1;
  uint32_t rangeCount = endPos - startPos + 1;
  if (rangeCount > (uint32_t)MAX_RANGE) rangeCount = MAX_RANGE;

  blocklist.seek((uint32_t)startPos * HASH_BYTES);
  blocklist.read(rangeBuf, (uint32_t)rangeCount * HASH_BYTES);

  for (uint32_t i = 0; i < rangeCount; i++) {
    uint64_t v = unpackHash(rangeBuf + i * HASH_BYTES);
    if (v == h) return true;
    if (v > h) break;
  }
  return false;
}

static bool inCustom(uint64_t h) { for (int i = 0; i < numCustom; i++) if (customHash[i] == h) return true; return false; }

// Only flash results are cached: the flash list only changes via reopenBlocklist(), which
// rebuilds the index and clears the cache. Custom domains change at runtime, so they're
// checked uncached (a short linear scan) to avoid serving stale answers.
static bool isBlockedHash(uint64_t h) {
  if (inCustom(h)) return true;
  uint32_t slot = h & (CACHE_SIZE - 1);
  if (cacheValid[slot]) {
    uint8_t want[HASH_BYTES]; packHash(h, want);
    bool same = true;
    for (int k = 0; k < HASH_BYTES; k++) if (cacheKey[slot][k] != want[k]) { same = false; break; }
    if (same) return cacheRes[slot] != 0;
  }
  bool res = inFlash(h);
  cacheValid[slot] = 1;
  cacheRes[slot] = res ? 1 : 0;
  packHash(h, cacheKey[slot]);
  return res;
}

static bool isBlocked(const char* domain) {
  const char* p = domain;
  while (p && *p) {
    uint64_t h = fnv40(p, strlen(p));
    if (isBlockedHash(h)) return true;
    const char* dot = strchr(p, '.'); if (!dot) break;
    const char* next = dot + 1; if (!strchr(next, '.')) break; p = next;
  }
  return false;
}

// ---------- persistence ----------
static void loadCustom() {
  numCustom = 0; File f = LittleFS.open("/custom.txt", "r"); if (!f) return;
  while (f.available() && numCustom < MAX_CUSTOM) {
    String l = f.readStringUntil('\n'); l.trim(); l.toLowerCase();
    if (l.length() && l.indexOf('.') > 0) { customDom[numCustom] = l; customHash[numCustom] = fnv40(l.c_str(), l.length()); numCustom++; }
  }
  f.close();
}
static void saveCustom() { File f = LittleFS.open("/custom.txt", "w"); if (!f) return; for (int i = 0; i < numCustom; i++) f.println(customDom[i]); f.close(); }
static bool addCustom(String d) {
  d.trim(); d.toLowerCase(); if (d.startsWith("www.")) d = d.substring(4);
  if (!d.length() || d.indexOf('.') < 0 || numCustom >= MAX_CUSTOM) return false;
  for (int i = 0; i < numCustom; i++) if (customDom[i] == d) return false;
  customDom[numCustom] = d; customHash[numCustom] = fnv40(d.c_str(), d.length()); numCustom++; saveCustom(); return true;
}
static void removeCustom(String d) {
  d.toLowerCase();
  for (int i = 0; i < numCustom; i++) if (customDom[i] == d) {
    for (int j = i; j < numCustom - 1; j++) { customDom[j] = customDom[j+1]; customHash[j] = customHash[j+1]; }
    numCustom--; saveCustom(); return;
  }
}
static bool isBannedIP(uint32_t ip) { for (int i = 0; i < numBanned; i++) if (bannedIP[i] == ip) return true; return false; }
static void loadBanned() {
  numBanned = 0; File f = LittleFS.open("/banned.txt", "r"); if (!f) return;
  while (f.available() && numBanned < MAX_BAN) { String l = f.readStringUntil('\n'); l.trim(); IPAddress ip; if (l.length() && ip.fromString(l)) bannedIP[numBanned++] = (uint32_t)ip; }
  f.close();
}
static void saveBanned() {
  numBanned = 0;
  for (int i = 0; i < numClients && numBanned < MAX_BAN; i++) if (clients[i].banned) bannedIP[numBanned++] = clients[i].ip;
  File f = LittleFS.open("/banned.txt", "w"); if (!f) return;
  for (int i = 0; i < numBanned; i++) { IPAddress ip(bannedIP[i]); f.println(ip.toString()); }
  f.close();
}

// ---------- client table ----------
static void getMac(uint32_t ip, uint8_t* mac) {
  memset(mac, 0, 6); ip4_addr_t ipa; ipa.addr = ip;
  struct eth_addr* eth = nullptr; const ip4_addr_t* ipret = nullptr;
  for (struct netif* nif = netif_list; nif; nif = nif->next)
    if (etharp_find_addr(nif, &ipa, &eth, &ipret) >= 0 && eth) { memcpy(mac, eth->addr, 6); return; }
}
static bool macUnknown(const uint8_t* m) { return !(m[0] | m[1] | m[2] | m[3] | m[4] | m[5]); }
static Dev* getClient(uint32_t ip) {
  for (int i = 0; i < numClients; i++) if (clients[i].ip == ip) {
    uint32_t now = millis(); clients[i].lastSeen = now;
    if (macUnknown(clients[i].mac) && now - clients[i].macTry >= 10000) { clients[i].macTry = now; getMac(ip, clients[i].mac); }
    return &clients[i];
  }
  if (numClients < MAX_CLIENTS) {
    Dev* c = &clients[numClients++];
    c->ip = ip; c->blocked = c->allowed = 0; c->lastSeen = c->macTry = millis(); c->banned = isBannedIP(ip); c->label = "";
    getMac(ip, c->mac); return c;
  }
  return nullptr;
}

// ---------- DNS ----------
static size_t parseQuery(const uint8_t* pkt, int len, char* out, uint16_t* qtype, int* qend) {
  if (len < 13) return 0; int i = 12; size_t o = 0;
  while (i < len) { uint8_t l = pkt[i++]; if (l == 0) break; if (l & 0xC0) return 0;
    if (o + l + 1 >= 250 || i + l > len) return 0; if (o) out[o++] = '.';
    for (uint8_t k = 0; k < l; k++) out[o++] = tolower(pkt[i++]); }
  out[o] = 0; if (i + 4 > len) return 0; *qtype = (pkt[i] << 8) | pkt[i + 1]; *qend = i + 4;
  if (o > 4 && strncmp(out, "www.", 4) == 0) { memmove(out, out + 4, o - 3); o -= 4; }
  return o;
}
static int buildBlocked(int qend, uint16_t qtype) {
  buf[2] = 0x81; buf[3] = 0x80; buf[6] = 0; buf[7] = (qtype == 1) ? 1 : 0; buf[8] = 0; buf[9] = 0; buf[10] = 0; buf[11] = 0;
  if (qtype != 1) return qend;
  const uint8_t ans[] = {0xC0,0x0C, 0,1, 0,1, 0,0,1,0x2C, 0,4, 0,0,0,0};
  memcpy(buf + qend, ans, sizeof(ans)); return qend + sizeof(ans);
}
// Forward to upstream without waiting: each query takes a pending slot under a fresh
// random txid, and pollUpstream() relays the reply that actually belongs to it.
// Issue #10: after one timeout the late reply used to sit in the socket and get relayed
// to the next client (every answer shifted by one). Now a reply is only accepted when
// its txid, question section, and source address/port match a pending query; anything
// else is dropped. The client's own txid is restored on the way back.
static uint8_t upPref = 0;
static const int PEND_SLOTS = 16;
static const uint32_t UPSTREAM_TIMEOUT_MS = 500;
static PendingQuery pending[PEND_SLOTS];
struct RxHdr { uint32_t ip; uint16_t port; };
static int dnsFd = -1, upFd = -1;
static MessageBufferHandle_t dnsRx, upRx;
static volatile uint32_t rxDropped = 0;
static void rxTask(void* arg) {
  const int fd = (int)(intptr_t)arg;
  MessageBufferHandle_t mb = fd == dnsFd ? dnsRx : upRx;
  uint8_t m[sizeof(RxHdr) + sizeof(buf) + 1];
  for (;;) {
    sockaddr_in from; socklen_t fl = sizeof(from);
    int n = recvfrom(fd, m + sizeof(RxHdr), sizeof(buf) + 1, 0, (sockaddr*)&from, &fl);
    if (n < 0) { vTaskDelay(1); continue; }
    if (n > (int)sizeof(buf)) continue;
    RxHdr h = { from.sin_addr.s_addr, ntohs(from.sin_port) };
    memcpy(m, &h, sizeof h);
    if (!xMessageBufferSend(mb, m, sizeof h + n, 0)) rxDropped++;
  }
}
static int udpOpen(uint16_t port) {
  int fd = socket(AF_INET, SOCK_DGRAM, 0);
  sockaddr_in a = {}; a.sin_family = AF_INET; a.sin_port = htons(port); a.sin_addr.s_addr = htonl(INADDR_ANY);
  if (fd >= 0 && bind(fd, (sockaddr*)&a, sizeof a) == 0) return fd;
  Serial.printf("[dns] socket on :%u failed\n", port);
  if (fd >= 0) close(fd);
  return -1;
}
static void dnsBegin() {
  dnsFd = udpOpen(DNS_PORT); upFd = udpOpen(0);
  dnsRx = xMessageBufferCreate(4096); upRx = xMessageBufferCreate(8192);
  if (dnsFd >= 0) xTaskCreate(rxTask, "dnsrx", 4096, (void*)(intptr_t)dnsFd, ESP_TASK_TCPIP_PRIO + 1, nullptr);
  if (upFd >= 0) xTaskCreate(rxTask, "uprx", 4096, (void*)(intptr_t)upFd, ESP_TASK_TCPIP_PRIO + 1, nullptr);
}
static int rxNext(MessageBufferHandle_t mb, uint32_t* ip, uint16_t* port) {
  static uint8_t m[sizeof(RxHdr) + sizeof(buf)];
  size_t n = xMessageBufferReceive(mb, m, sizeof m, 0);
  if (n < sizeof(RxHdr)) return -1;
  RxHdr h; memcpy(&h, m, sizeof h); *ip = h.ip; *port = h.port;
  memcpy(buf, m + sizeof h, n - sizeof h);
  return n - sizeof h;
}
static void udpSend(int fd, uint32_t ip, uint16_t port, const uint8_t* d, int n) {
  sockaddr_in a = {}; a.sin_family = AF_INET; a.sin_port = htons(port); a.sin_addr.s_addr = ip;
  sendto(fd, d, n, 0, (sockaddr*)&a, sizeof a);
}
static const uint32_t GAP_EDGES_MS[] = {50, 100, 300, 1000, 3000, 10000, 30000};
static const int GAP_BUCKETS = sizeof(GAP_EDGES_MS) / sizeof(GAP_EDGES_MS[0]) + 1;
static uint32_t gapHist[GAP_BUCKETS], lastQueryMs = 0;
static void countGap(uint32_t now) {
  if (lastQueryMs) {
    int b = 0;
    while (b < GAP_BUCKETS - 1 && now - lastQueryMs >= GAP_EDGES_MS[b]) b++;
    gapHist[b]++;
  }
  lastQueryMs = now ? now : 1;
}
static void forwardUpstream(uint32_t cip, uint16_t cport, int qlen, int qend) {
  const int i = pendFree(pending, PEND_SLOTS);
  if (i < 0) return;
  PendingQuery& p = pending[i];
  uint16_t wid;
  do wid = (uint16_t)esp_random(); while (pendWidUsed(pending, PEND_SLOTS, wid));
  p.cid = (uint16_t)(buf[0] << 8 | buf[1]); p.wid = wid;
  buf[0] = wid >> 8; buf[1] = wid & 0xFF;
  p.len = qlen < PEND_PKT ? qlen : PEND_PKT;
  p.qend = qend < p.len ? qend : p.len;
  memcpy(p.pkt, buf, p.len);
  p.tries = qlen <= PEND_PKT ? 1 : 0;
  p.cip = cip; p.cport = cport; p.up = upPref; p.sentTo = 1 << p.up;
  udpSend(upFd, p.up ? (uint32_t)UPSTREAM2 : (uint32_t)UPSTREAM, UPSTREAM_PORT, buf, qlen);
  p.sentAt = millis(); p.used = true;
}
static bool pollUpstream() {
  bool did = false;
  for (int budget = 0; budget < 16; budget++) {
    uint32_t from; uint16_t port;
    int n = rxNext(upRx, &from, &port); if (n < 0) break;
    did = true;
    const int up = port != UPSTREAM_PORT ? -1 : from == (uint32_t)UPSTREAM ? 0 : from == (uint32_t)UPSTREAM2 ? 1 : -1;
    if (up < 0) continue;
    const int i = pendMatch(pending, PEND_SLOTS, (uint8_t)up, buf, n);
    if (i < 0) continue;
    PendingQuery& p = pending[i];
    buf[0] = p.cid >> 8; buf[1] = p.cid & 0xFF;
    udpSend(dnsFd, p.cip, p.cport, buf, n);
    upPref = (uint8_t)up; p.used = false;
  }
  for (int i; (i = pendExpired(pending, PEND_SLOTS, millis(), UPSTREAM_TIMEOUT_MS)) >= 0; ) {
    PendingQuery& p = pending[i];
    if (!p.tries) { p.used = false; continue; }
    p.tries--; p.up ^= 1; p.sentTo |= 1 << p.up;
    udpSend(upFd, p.up ? (uint32_t)UPSTREAM2 : (uint32_t)UPSTREAM, UPSTREAM_PORT, p.pkt, p.len);
    p.sentAt = millis();
  }
  return did;
}
// Drain a whole RX burst per call (capped, so web/OTA still get a turn) instead of
// one packet per loop iteration. Returns true if any query was handled this call.
static bool handleDns() {
  bool did = false;
  for (int budget = 0; budget < 16 && pendFree(pending, PEND_SLOTS) >= 0; budget++) {
    if (pollUpstream()) did = true;
    uint32_t cip; uint16_t cport;
    int qlen = rxNext(dnsRx, &cip, &cport); if (qlen < 0) break;
    did = true;
    if (qlen < 13) continue;
    if ((buf[2] & 0xF8) != 0 || ((buf[4] << 8) | buf[5]) != 1) continue;
    countGap(millis());
    char domain[256]; uint16_t qtype = 0; int qend = qlen;
    size_t dl = parseQuery(buf, qlen, domain, &qtype, &qend);
    Dev* c = getClient(cip);
    bool ban = c && c->banned;
    bool blocked = ban || (blockingOn && dl && numHashes && isBlocked(domain));
    if (!blocked) { forwardUpstream(cip, cport, qlen, dl ? qend : 12); totalAllowed++; if (c) c->allowed++; continue; }
    int rlen = buildBlocked(qend, qtype); totalBlocked++; if (c) c->blocked++;
    udpSend(dnsFd, cip, cport, buf, rlen);
  }
  return pollUpstream() || did;
}

// ---------- web ----------
static String macStr(const uint8_t* m) { if (macUnknown(m)) return "—"; char s[18]; snprintf(s, sizeof(s), "%02x:%02x:%02x:%02x:%02x:%02x", m[0],m[1],m[2],m[3],m[4],m[5]); return String(s); }
static String jesc(const String& s) { String o; for (char ch : s) { if (ch == '"' || ch == '\\') o += '\\'; o += ch; } return o; }
// HTML text/attribute escaping for the setup portal. jesc() covers JSON (stats
// endpoint); the portal builds HTML, and its inputs — a scanned SSID, the
// submitted WiFi name — are attacker-controllable during provisioning (the
// portal AP is open, and a nearby attacker can also broadcast an SSID of their
// choosing). Without escaping both, a crafted SSID/name is reflected script
// into the setup page, the same class as the dashboard XSS fixed earlier.
static String htmlEscape(const String& s) {
  String o; o.reserve(s.length());
  for (char ch : s) {
    switch (ch) {
      case '&':  o += "&amp;";  break;
      case '<':  o += "&lt;";   break;
      case '>':  o += "&gt;";   break;
      case '"':  o += "&quot;"; break;
      case '\'': o += "&#39;";  break;
      default:   o += ch;
    }
  }
  return o;
}

#include "page.h"   // dashboard HTML (PROGMEM) — see issue #6
#include "board_ui.h"
#include "hashlist.h"
static_assert(HASH_BYTES == 5, "hashlist.h assumes 5-byte hashes");

static bool localTime(int tzHours, time_t& l) {
  time_t t = time(nullptr);
  if (t < 1700000000) return false;
  l = t + tzHours * 3600;
  return true;
}
static bool localClock(int tzHours, int& day, int& hour) {
  time_t l;
  if (!localTime(tzHours, l)) return false;
  day = (int)(l / 86400); hour = (int)(l / 3600 % 24);
  return true;
}
static String grouped(uint32_t n) { String s(n); for (int i = s.length() - 3; i > 0; i -= 3) s = s.substring(0, i) + "," + s.substring(i); return s; }
static String clockStamp() {
  time_t l; if (!localTime(nightTz, l)) return "";
  char b[20]; strftime(b, sizeof(b), ", %b %d %H:%M", gmtime(&l)); return b;
}
static int nightLocalHour(int tzHours) {
  int d, h;
  return localClock(tzHours, d, h) ? h : -1;
}

static String gapList() { String g; for (int i = 0; i < GAP_BUCKETS; i++) { g += (i ? "," : ""); g += gapHist[i]; } return g; }
static void handleStats() {
  uint32_t up = millis() / 1000;
  char ut[24];
  if (up >= 86400) snprintf(ut, sizeof(ut), "%lud %luh", up/86400, (up%86400)/3600);
  else if (up >= 3600) snprintf(ut, sizeof(ut), "%luh %lum", up/3600, (up%3600)/60);
  else if (up >= 60) snprintf(ut, sizeof(ut), "%lum", up/60);
  else snprintf(ut, sizeof(ut), "%lus", up);
  int rssi = WiFi.RSSI(); String temp = String(temperatureRead(), 1); uint32_t heap = ESP.getFreeHeap();
  String sys = "Wi-Fi " + String(rssi) + " dBm · chip " + temp + " °C · " + String(heap / 1024) + " KB RAM free · up " + ut;
  sys.replace(" ", "\u00a0"); sys.replace("\u00a0\u00b7\u00a0", " \u00b7 ");
  String j = "{\"ip\":\"" + WiFi.localIP().toString() + "\",\"blocked\":" + totalBlocked + ",\"allowed\":" + totalAllowed +
             ",\"domains\":" + numHashes + ",\"rssi\":" + rssi + ",\"temp\":" + temp +
             ",\"heap\":" + heap + ",\"rxdrop\":" + rxDropped + ",\"ps\":" + (int)WiFi.getSleep() + ",\"gaps\":[" + gapList() + "]" + ",\"uptime\":\"" + ut + "\",\"sys\":\"" + sys + "\"" +
             ",\"upurl\":\"" + jesc(updateUrl) + "\",\"upiv\":" + updateIntervalH + ",\"updays\":" + updateDays + ",\"uphour\":" + updateHour + ",\"upstat\":\"" + jesc(updateStatus) + "\"" +
             ",\"nmode\":" + nightMode + ",\"nfrom\":" + nightFrom + ",\"nto\":" + nightTo + ",\"ntz\":" + nightTz + ",\"nhour\":" + nightLocalHour(nightTz) +
             ",\"blocking\":" + (blockingOn ? "true" : "false") +
             ",\"resumeIn\":" + (uint32_t)(!blockingOn && resumeAt ? (resumeAt - millis()) / 1000 : 0) +
             ",\"defcreds\":" + ((strcmp(WEB_PASS, "CHANGE_ME_WEB_PASSWORD") == 0 || strcmp(OTA_PASS, "CHANGE_ME_OTA_PASSWORD") == 0) ? "true" : "false") +
             ",\"clients\":[";
  for (int i = 0; i < numClients; i++) { Dev& c = clients[i]; IPAddress ip(c.ip);
    j += (i ? "," : ""); j += "{\"ip\":\"" + ip.toString() + "\",\"mac\":\"" + macStr(c.mac) + "\",\"blocked\":" + c.blocked + ",\"allowed\":" + c.allowed + ",\"banned\":" + (c.banned?"true":"false") + "}"; }
  j += "],\"custom\":[";
  for (int i = 0; i < numCustom; i++) { j += (i ? "," : ""); j += "\"" + jesc(customDom[i]) + "\""; }
  j += "]}";
  web.send(200, "application/json", j);
}
// Upstream shipped every state-changing/OTA endpoint with zero authentication —
// anyone on the LAN could reflash firmware or rewrite the blocklist. Gate them.
//
// Basic Auth alone isn't enough here: these are GET endpoints with side effects,
// and browsers auto-attach cached Basic Auth credentials to *any* request to an
// already-authenticated origin — including one triggered by a completely
// unrelated page the victim's browser visits later (e.g. <img src="http://
// c3adblock.local/forgetwifi">). That's CSRF, and it defeats the LAN-attacker
// threat model entirely: the attacker doesn't need network access, just to get
// the victim's browser to fire one request. A custom header can't be attached
// by a plain <img>/<form> CSRF vector (only same-origin fetch() can set it, and
// that's exactly what the dashboard's own JS does), so requiring one blocks the
// drive-by case without needing TLS, cookies, or a token endpoint.
static const char* CSRF_HEADER = "X-Requested-With";
static const char* CSRF_VALUE  = "c3-adblock";
static bool requireAuth() {
  if (web.header(CSRF_HEADER) != CSRF_VALUE) { web.send(403, "text/plain", "missing CSRF header"); return false; }
  if (web.authenticate(WEB_USER, WEB_PASS)) return true;
  web.requestAuthentication();
  return false;
}
static void handleBan() {
  if (!requireAuth()) return;
  IPAddress ip; if (ip.fromString(web.arg("ip"))) { Dev* c = getClient((uint32_t)ip); if (c) { c->banned = !c->banned; saveBanned(); } }
  web.send(200, "text/plain", "ok");
}

// ---------- blocklist swap (shared by upload + remote fetch) ----------
// The partition holds one list, so we free the old one before writing the new.
// While swapping, numHashes=0 -> device fail-opens (forwards, no blocking).
static void reopenBlocklist() {
  blocklist = LittleFS.open(BLOCKLIST_PATH, "r");
  numHashes = blocklist ? blocklist.size() / HASH_BYTES : 0;
  buildFlashIndex();
}
static const size_t SWAP_SLACK = 16 * 4096;
static void beginBlocklistSwap(int incoming) {
  LittleFS.remove("/blocklist.new");
  if (incoming > 0 && LittleFS.totalBytes() - LittleFS.usedBytes() >= (size_t)incoming + SWAP_SLACK) return;
  if (blocklist) blocklist.close();
  numHashes = 0;
  LittleFS.remove(BLOCKLIST_PATH);
}
static bool blobSorted(File& f) {
  uint64_t prev = 0; int n;
  while ((n = f.read(rangeBuf, sizeof(rangeBuf))) > 0) if (!hashesAscending(rangeBuf, n, &prev)) return false;
  return true;
}
static bool landed(size_t n) {
  File f = LittleFS.open("/blocklist.new", "r");
  size_t sz = f ? f.size() : 0;
  if (f) f.close();
  return sz == n;
}
static bool commitNewBlocklist() {                  // /blocklist.new -> live (validated)
  File f = LittleFS.open("/blocklist.new", "r");
  size_t sz = f ? f.size() : 0;
  bool ok = sz > 0 && (sz % HASH_BYTES) == 0;       // sorted hash blob -> 5-byte multiple
  ok = ok && blobSorted(f);
  if (f) f.close();
  if (ok) { if (blocklist) blocklist.close(); LittleFS.remove(BLOCKLIST_PATH); LittleFS.rename("/blocklist.new", BLOCKLIST_PATH); }
  else    LittleFS.remove("/blocklist.new");
  reopenBlocklist();
  return ok;
}

// ---------- OTA blocklist update (browser upload) ----------
static bool upOk = false;
static bool upFull = false;
static bool upAuthOk = false;
static File upFile;
static void handleUploadDone() {
  if (!upAuthOk) { web.requestAuthentication(); return; }
  web.send(upOk ? 200 : 500, "text/plain",
           upOk ? "ok" : upFull ? "Rejected: blocklist does not fit the filesystem"
                       : "Rejected: empty or size not a multiple of 5 (not a blocklist.bin?)");
}
static void handleUpload() {
  HTTPUpload& u = web.upload();
  switch (u.status) {
    case UPLOAD_FILE_START:
      upAuthOk = web.header(CSRF_HEADER) == CSRF_VALUE && web.authenticate(WEB_USER, WEB_PASS);
      if (!upAuthOk) { Serial.println("[ota] blocklist upload: auth/CSRF check failed"); break; }
      upOk = false; upFull = false; beginBlocklistSwap(web.clientContentLength());
      upFile = LittleFS.open("/blocklist.new", "w");
      Serial.printf("[ota] receiving %s\n", u.filename.c_str());
      break;
    case UPLOAD_FILE_WRITE:
      if (upAuthOk && upFile) upFile.write(u.buf, u.currentSize);
      break;
    case UPLOAD_FILE_END:
      if (!upAuthOk) break;
      if (upFile) upFile.close();
      upFull = !landed(u.totalSize);
      if (upFull) LittleFS.remove("/blocklist.new");
      upOk = commitNewBlocklist();
      Serial.printf("[ota] %s -> %u domains\n", upOk ? "OK" : "REJECTED", numHashes);
      break;
    case UPLOAD_FILE_ABORTED:
      if (!upAuthOk) break;
      if (upFile) upFile.close();
      LittleFS.remove("/blocklist.new"); reopenBlocklist();
      Serial.println("[ota] aborted");
      break;
  }
}

// ---------- remote blocklist auto-update ----------
static void loadUpdateCfg() {
  File f = LittleFS.open("/update.cfg", "r"); if (!f) return;
  updateUrl = f.readStringUntil('\n'); updateUrl.trim();
  String iv = f.readStringUntil('\n'); iv.trim(); if (iv.length()) updateIntervalH = iv.toInt();
  f.close(); if (updateIntervalH < 1) updateIntervalH = 1;
}
static void saveUpdateCfg() {
  File f = LittleFS.open("/update.cfg", "w"); if (!f) return;
  f.println(updateUrl); f.println(updateIntervalH); f.close();
}
static bool fetchBlocklist(String url) {
  url.trim(); if (!url.length()) { updateStatus = "No URL set"; return false; }
  Serial.printf("[remote] GET %s\n", url.c_str());
  pinDns();
  WiFiClientSecure cs; cs.setInsecure();            // blocklist isn't secret -> skip cert pinning
  WiFiClient cl;
  HTTPClient http; http.setTimeout(20000);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);  // GitHub release -> CDN redirect
  bool https = url.startsWith("https");
  if (!(https ? http.begin(cs, url) : http.begin(cl, url))) { updateStatus = "Begin failed"; return false; }
  int code = http.GET();
  if (code != HTTP_CODE_OK) { updateStatus = "HTTP " + String(code) + " " + HTTPClient::errorToString(code) + ", heap " + String(ESP.getFreeHeap() / 1024) + "K"; http.end(); Serial.printf("[remote] %s\n", updateStatus.c_str()); return false; }
  int len = http.getSize();
  beginBlocklistSwap(len);
  File f = LittleFS.open("/blocklist.new", "w");
  if (!f) { http.end(); updateStatus = "FS open failed"; reopenBlocklist(); return false; }
  WiFiClient* stream = http.getStreamPtr();
  uint8_t b[1024]; size_t total = 0; uint32_t idle = millis(); bool stalled = false, full = false;
  while (http.connected() && (len < 0 || (int)total < len)) {
    bool dns = handleDns();
    size_t avail = stream->available();
    if (avail) { int n = stream->readBytes(b, avail > sizeof(b) ? sizeof(b) : avail); if (n > 0) { if (f.write(b, n) != (size_t)n) { full = true; break; } total += n; idle = millis(); } }
    else { if (millis() - idle > 15000) { stalled = true; break; } if (!dns) delay(2); }
  }
  f.close(); http.end();
  full = full || !landed(total);
  if (stalled || full || (len > 0 && total != (size_t)len)) {
    LittleFS.remove("/blocklist.new"); reopenBlocklist();
    updateStatus = (full ? "FS full (" : "Truncated (") + String(total) + "/" + String(len) + "B)";
    Serial.printf("[remote] %s\n", updateStatus.c_str());
    return false;
  }
  bool ok = commitNewBlocklist();
  updateStatus = ok ? ("OK, " + grouped(numHashes) + " domains" + clockStamp()) : ("Bad data (" + String(total) + "B)");
  Serial.printf("[remote] %s\n", updateStatus.c_str());
  return ok;
}

// ---------- firmware OTA (browser upload of firmware.bin -> reboot) ----------
static bool fwAuthOk = false;
static void handleFwUpdateDone() {
  if (!fwAuthOk) { web.requestAuthentication(); return; }
  bool ok = !Update.hasError();
  web.send(ok ? 200 : 500, "text/plain", ok ? "ok, rebooting" : "Firmware update failed");
  if (ok) { delay(300); ESP.restart(); }
}
static void handleFwUpload() {
  HTTPUpload& u = web.upload();
  if (u.status == UPLOAD_FILE_START) {
    fwAuthOk = web.header(CSRF_HEADER) == CSRF_VALUE && web.authenticate(WEB_USER, WEB_PASS);
    if (!fwAuthOk) { Serial.println("[fw-ota] auth/CSRF check failed, rejecting flash"); return; }
    Serial.printf("[fw-ota] %s\n", u.filename.c_str());
    if (!Update.begin(UPDATE_SIZE_UNKNOWN)) Update.printError(Serial);
  } else if (u.status == UPLOAD_FILE_WRITE) {
    if (!fwAuthOk) return;
    if (Update.write(u.buf, u.currentSize) != u.currentSize) Update.printError(Serial);
  } else if (u.status == UPLOAD_FILE_END) {
    if (!fwAuthOk) return;
    if (Update.end(true)) Serial.printf("[fw-ota] %u bytes OK\n", u.totalSize);
    else Update.printError(Serial);
  } else if (u.status == UPLOAD_FILE_ABORTED) {
    if (!fwAuthOk) return;
    Update.abort(); Serial.println("[fw-ota] aborted");
  }
}

// ---------- WiFi provisioning (captive portal) ----------
// Try provisioned NVS creds first, then the compile-time secrets.h creds as a
// fallback (so the maintainer's own device + source builders keep working). If
// neither connects, fall through to the config portal.
static bool hasCreds() {
  prefs.begin("wifi", true); bool nvs = prefs.getString("ssid", "").length() > 0; prefs.end();
  return nvs || (WIFI_SSID && *WIFI_SSID && strcmp(WIFI_SSID, "YOUR_WIFI_SSID") != 0);
}
static const uint32_t WIFI_AWAKE_MS = 300;
static bool connectWiFi() {
  prefs.begin("wifi", true);
  String ss = prefs.getString("ssid", "");
  String pw = prefs.getString("pass", "");
  prefs.end();
  const char* ssid = ss.length() ? ss.c_str() : WIFI_SSID;
  const char* pass = ss.length() ? pw.c_str() : WIFI_PASS;
  if (!ssid || !*ssid || strcmp(ssid, "YOUR_WIFI_SSID") == 0) return false;  // unconfigured
  Serial.printf("WiFi: connecting to \"%s\"%s\n", ssid, ss.length() ? " (provisioned)" : " (secrets.h)");
  WiFi.onEvent([](arduino_event_id_t, arduino_event_info_t) {
    pinDns();
    esp_wifi_set_sleep_min_active_time(WIFI_AWAKE_MS * 1000);  // ponytail: private IDF API (esp_private/wifi.h), may vanish on an IDF bump; then set CONFIG_ESP_WIFI_SLP_DEFAULT_MIN_ACTIVE_TIME in a custom sdkconfig build
  }, ARDUINO_EVENT_WIFI_STA_GOT_IP);
  WiFi.mode(WIFI_STA); WiFi.setSleep(WIFI_PS_MIN_MODEM); WiFi.begin(ssid, pass);
  configTime(0, 0, "pool.ntp.org");
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 30000) { delay(250); Serial.print("."); }
  Serial.println();
  return WiFi.status() == WL_CONNECTED;
}

static void handlePortalRoot() {
  String html =
    "<!doctype html><meta charset=utf-8><meta name=viewport content='width=device-width,initial-scale=1'>"
    "<title>C3 AdBlock setup</title>"
    "<meta name=color-scheme content=dark><body style='font:16px system-ui,sans-serif;max-width:420px;margin:36px auto;padding:0 16px;background:#1a1b26;color:#c0caf5'>"
    "<h2>&#128737; C3 AdBlock &mdash; WiFi setup</h2>"
    "<p style='color:#8089b3'>Pick your network and enter its password. The device restarts and joins it.</p>"
    "<form method=POST action=/wifisave>"
    "<input list=nets name=s placeholder='WiFi name' required style='width:100%;box-sizing:border-box;padding:11px;margin:6px 0;border-radius:6px;border:1px solid #3b4261;background:#1f2335;color:#c0caf5'>"
    "<datalist id=nets>" + portalOpts + "</datalist>"
    "<input name=p type=password placeholder='Password' style='width:100%;box-sizing:border-box;padding:11px;margin:6px 0;border-radius:6px;border:1px solid #3b4261;background:#1f2335;color:#c0caf5'>"
    "<button style='width:100%;padding:12px;margin-top:8px;border-radius:6px;border:0;background:#7aa2f7;color:#16161e;font-weight:600;cursor:pointer'>Connect</button>"
    "</form></body>";
  web.send(200, "text/html", html);
}
static void handleWifiSave() {
  String ss = web.arg("s"), pw = web.arg("p");
  if (!ss.length()) { web.send(400, "text/plain", "missing WiFi name"); return; }
  prefs.begin("wifi", false); prefs.putString("ssid", ss); prefs.putString("pass", pw); prefs.end();
  web.send(200, "text/html", "<!doctype html><meta charset=utf-8><meta name=color-scheme content=dark><body style='font:16px system-ui;text-align:center;margin-top:60px;background:#1a1b26;color:#c0caf5'>"
                             "&#9989; Saved. Restarting and joining <b>" + htmlEscape(ss) + "</b>&hellip;<br><br>"
                             "Reconnect your phone to your normal WiFi, then find the box at <b>c3adblock.local</b>.</body>");
  delay(900); ESP.restart();
}
// Never returns — blocks in the portal loop until creds are saved (then reboots).
static void startConfigPortal() {
  int n = WiFi.scanNetworks();                 // scan while still in STA mode (no APSTA)
  portalOpts = "";
  for (int i = 0; i < n && i < 15; i++) portalOpts += "<option value='" + htmlEscape(WiFi.SSID(i)) + "'>";
  const char* ap = "C3-AdBlock";
  WiFi.mode(WIFI_AP); WiFi.softAPConfig(IPAddress(192, 168, 2, 1), IPAddress(192, 168, 2, 1), IPAddress(255, 255, 255, 0)); WiFi.softAP(ap);
  IPAddress apIP = WiFi.softAPIP();
  oledLines(ap, apIP.toString().c_str());
  dnsPortal.start(53, "*", apIP);              // catch-all -> phones pop the captive portal
  web.on("/", handlePortalRoot);
  web.on("/wifisave", HTTP_POST, handleWifiSave);
  web.onNotFound(handlePortalRoot);            // any captive-portal probe -> the form
  web.begin();
  Serial.printf("\n[setup] No WiFi. Join open network \"%s\" and a setup page pops up (or http://%s)\n",
                ap, apIP.toString().c_str());
  // A configured device that merely failed to join (router rebooting, weak signal) must not
  // get stuck here: if nobody is using the portal, reboot and retry WiFi every 3 minutes.
  const bool configured = hasCreds();
  uint32_t t0 = millis();
  while (true) {
    dnsPortal.processNextRequest(); web.handleClient(); delay(2);
    if (WiFi.softAPgetStationNum() > 0) t0 = millis();          // someone is setting it up
    if (configured && millis() - t0 > 180000UL) { Serial.println("[setup] retrying WiFi"); ESP.restart(); }
  }
}

void setup() {
  Serial.begin(115200); delay(300);
  Serial.println("\n[c3-adblock] booting");
  uiBegin(); oledLines("C3 ADBLOCK", "booting...");
  if (!LittleFS.begin(true)) Serial.println("LittleFS FAILED");
  blocklist = LittleFS.open(BLOCKLIST_PATH, "r");
  if (blocklist) {
    numHashes = blocklist.size() / HASH_BYTES;
    Serial.printf("blocklist: %u domains\n", numHashes);
    buildFlashIndex();
  }
  loadCustom(); loadBanned(); loadUpdateCfg();
  prefs.begin("ui", true); nightMode = prefs.getUChar("night", 0); nightFrom = prefs.getUChar("from", 22); nightTo = prefs.getUChar("to", 7); nightTz = prefs.getChar("tz", 7); updateDays = prefs.getUChar("ud", 0); updateHour = prefs.getUChar("uh", 3); prefs.end();
  Serial.printf("custom: %d, banned: %d\n", numCustom, numBanned);

  // Hold BOOT at power-on to wipe saved WiFi and force the setup portal.
#if CONFIG_IDF_TARGET_ESP32C3
  const int BOOT_PIN = 9;     // C3 BOOT button
#else
  const int BOOT_PIN = 0;     // classic ESP32 BOOT button (GPIO9 is a flash pin there)
#endif
  pinMode(BOOT_PIN, INPUT_PULLUP);
  if (digitalRead(BOOT_PIN) == LOW) { delay(60);
    if (digitalRead(BOOT_PIN) == LOW) { prefs.begin("wifi", false); prefs.clear(); prefs.end();
      Serial.println("[setup] BOOT held -> cleared saved WiFi"); } }

  uiButton(BOOT_PIN);
  oledLines("C3 ADBLOCK", "WiFi...");
  if (!connectWiFi()) startConfigPortal();   // portal blocks + reboots on save; returns only when connected
  Serial.printf("WiFi up: %s\n", WiFi.localIP().toString().c_str());
  oledLines("WiFi up", WiFi.localIP().toString().c_str());
  if (MDNS.begin("c3adblock")) { MDNS.addService("http", "tcp", 80); Serial.println("dashboard: http://c3adblock.local"); }

  if (strcmp(WEB_PASS, "CHANGE_ME_WEB_PASSWORD") == 0 || strcmp(OTA_PASS, "CHANGE_ME_OTA_PASSWORD") == 0)
    Serial.println("[WARN] secrets.h still has placeholder WEB_PASS/OTA_PASS — those are public "
                    "(they're in the repo's example file). Set real values before trusting this "
                    "device on a network you don't fully control.");

  dnsBegin();
  { const char* hdrs[] = { CSRF_HEADER }; web.collectHeaders(hdrs, 1); }  // needed for requireAuth()'s CSRF check
  web.on("/", []() { web.send_P(200, "text/html", PAGE); });
  web.on("/stats.json", handleStats);
  web.on("/ban", handleBan);
  web.on("/addblock", []() { if (!requireAuth()) return; addCustom(web.arg("d")); web.send(200, "text/plain", "ok"); });
  web.on("/unblock", []() { if (!requireAuth()) return; removeCustom(web.arg("d")); web.send(200, "text/plain", "ok"); });
  web.on("/pause", []() {                    // /pause?s=300  (0 or absent = indefinite)
    if (!requireAuth()) return;
    long s = web.hasArg("s") ? web.arg("s").toInt() : 0;
    blockingOn = false; resumeAt = (s > 0) ? millis() + (uint32_t)s * 1000UL : 0;
    web.send(200, "text/plain", "paused");
  });
  web.on("/ps", []() {
    if (!requireAuth()) return;
    if (web.hasArg("m")) WiFi.setSleep(web.arg("m") == "none" ? WIFI_PS_NONE : WIFI_PS_MIN_MODEM);
    if (web.hasArg("a")) esp_wifi_set_sleep_min_active_time((uint32_t)web.arg("a").toInt() * 1000);
    web.send(200, "text/plain", "ok");
  });
  web.on("/night", []() {
    if (!requireAuth()) return;
    if (web.hasArg("m")) nightMode = constrain(web.arg("m").toInt(), 0, 2);
    if (web.hasArg("from")) nightFrom = constrain(web.arg("from").toInt(), 0, 23);
    if (web.hasArg("to")) nightTo = constrain(web.arg("to").toInt(), 0, 23);
    if (web.hasArg("tz")) nightTz = constrain(web.arg("tz").toInt(), -12, 14);
    if (web.args()) { prefs.begin("ui", false); prefs.putUChar("night", nightMode); prefs.putUChar("from", nightFrom); prefs.putUChar("to", nightTo); prefs.putChar("tz", nightTz); prefs.end(); }
    web.send(200, "text/plain", String(nightMode) + " " + nightFrom + "-" + nightTo + " tz" + nightTz + (nightLocalHour(nightTz) < 0 ? " nosync" : ""));
  });
  web.on("/resume", []() { if (!requireAuth()) return; blockingOn = true; resumeAt = 0; web.send(200, "text/plain", "resumed"); });
  web.on("/forgetwifi", []() { if (!requireAuth()) return; web.send(200, "text/plain", "Wi-Fi cleared, rebooting into setup");
    prefs.begin("wifi", false); prefs.clear(); prefs.end(); delay(500); ESP.restart(); });
  web.on("/upload", HTTP_POST, handleUploadDone, handleUpload);      // blocklist OTA (auth inside handleUpload)
  web.on("/update", HTTP_POST, handleFwUpdateDone, handleFwUpload);  // firmware OTA (auth inside handleFwUpload)
  web.on("/fetchnow", []() { if (!requireAuth()) return; fetchBlocklist(updateUrl); web.send(200, "text/plain", updateStatus); });
  web.on("/setupdate", []() {
    if (!requireAuth()) return;
    if (web.hasArg("u")) updateUrl = web.arg("u");
    if (web.hasArg("h")) { updateIntervalH = web.arg("h").toInt(); if (updateIntervalH < 1) updateIntervalH = 1; }
    if (web.hasArg("d")) updateDays = constrain(web.arg("d").toInt(), 0, 127);
    if (web.hasArg("hr")) updateHour = constrain(web.arg("hr").toInt(), 0, 23);
    prefs.begin("ui", false); prefs.putUChar("ud", updateDays); prefs.putUChar("uh", updateHour); prefs.end();
    saveUpdateCfg(); web.send(200, "text/plain", "ok");
  });
  web.begin();
  ArduinoOTA.setHostname("c3adblock");   // pio run -t upload --upload-port c3adblock.local
  ArduinoOTA.setPassword(OTA_PASS);      // network OTA was unauthenticated upstream
  ArduinoOTA.begin();
  Serial.println("DNS :53 + dashboard :80 + OTA up");
}

void loop() {
  ArduinoOTA.handle();
  web.handleClient();
  bool busy = handleDns();
  static uint32_t dnsCheckMs;
  if (millis() - dnsCheckMs >= 60000) { dnsCheckMs = millis(); if (WiFi.isConnected() && WiFi.dnsIP() != UPSTREAM) pinDns(); }
  uiTick(totalBlocked, totalAllowed, numClients, numHashes, !blockingOn, nightActive(nightMode, nightLocalHour(nightTz), nightFrom, nightTo));
  if (!blockingOn && resumeAt && (int32_t)(millis() - resumeAt) >= 0) { blockingOn = true; resumeAt = 0; }
  if (updateUrl.length()) {               // periodic remote blocklist auto-update
    uint32_t now = millis();
    int day, hour;
    if (updateDays) {
      if (localClock(nightTz, day, hour) && updateDue(updateDays, (day + 4) % 7, hour, updateHour, day, lastSchedDay) && now - lastCheckMs >= 600000UL) {
        lastCheckMs = now;
        if (fetchBlocklist(updateUrl) || numHashes > 0) lastSchedDay = day;
      }
    }
    else if (lastCheckMs == 0) lastCheckMs = now;   // skip an immediate fetch on boot
    else if (now - lastCheckMs >= updateIntervalH * 3600000UL) { lastCheckMs = now; if (!fetchBlocklist(updateUrl) && numHashes == 0) lastCheckMs -= updateIntervalH * 3600000UL - 600000UL; }
  }
  if (!busy) delay(1);   // sleep only when idle: full speed under load, cool when quiet
}
