# Latency research (ESP32-C3 adblock)

Scope: per-query DNS latency under Wi-Fi modem sleep, the power-save knobs that affect it, and whether a DNS response cache is worth adding. Read-only research; no source file was changed and nothing was built or flashed. Follows on from `docs/research/performance.md` (Q3 there covers the basic modem-sleep docs; not repeated here).

Legend: **[S]** measured in the session (cited, not re-derived), **[L]** read from a local file of the installed toolchain (path given), **[P]** read from a primary online source (URL given), **[I]** my inference from [S]/[L]/[P], **[U]** unverified or could not confirm.

Stack: `platform-espressif32` 8.0.0, Arduino-ESP32 3.3.x libs built from ESP-IDF v5.5.5. `LIBS` below means `~/.platformio/packages/framework-arduinoespressif32-esp32c3-libs/`, `ARD` means `~/.platformio/packages/framework-arduinoespressif32/`. IDF docs and source are the v5.5.5 ones.

## Outcome (measured 2026-10-26, after this research)

Ran E0, E2, E4 and E5 on the device over one hour of household use [S], switching settings at runtime through `/ps` (20 minutes each):

| Setting | Idle die temp | Local answer, 1-2 s gap | Local answer, 0.2-0.4 s gap | Forwarded, 1-2 s gap |
|---|---|---|---|---|
| Modem sleep, 50 ms awake window (old default) | 42-43 C | 97 ms | 61 ms | 126 ms |
| Modem sleep, 300 ms awake window | 44-47 C | 68-84 ms | 21-26 ms | 111-121 ms |
| `WIFI_PS_NONE` | rising to 65 C | 12 ms | 11 ms | 53 ms |

- Query gaps (1518 queries): under 50 ms 43%, 50-300 ms 11%, 0.3-1 s 10%, 1-10 s 28%, over 10 s 7%.
- Response cache: of 1433 forwarded queries, 28 (about 2%) asked for a name and type already answered with a TTL still running. Client caches absorb nearly all repeats. **Cache not built.**
- `esp_wifi_set_sleep_min_active_time()` works at runtime after connect (Q2's open question). **Shipped:** 300 ms window set after each connect (`WIFI_AWAKE_MS` in `src/main.cpp`). PS_NONE rejected for heat.

## Summary

Measured today [S], 40 sequential queries each (medians): Mac to router 38 ms; Mac to 9.9.9.9 40 ms; device answering a blocked name locally 81 ms (p90 128, min 22); device forwarding to Quad9 124 ms (p90 184). Firmware runs `WIFI_PS_MIN_MODEM` [L] `src/main.cpp:613`. The spacing of those 40 queries relative to the 50 ms Wi-Fi active window (Q2) is not recorded **[U]**; repeat runs should space queries at least 200 ms apart so each one meets a sleeping radio.

Decomposition [I]: router 38, then +43 ms to reach a sleeping device (81), then +43 ms for the upstream leg (124), which matches the Mac's own 40 ms to Quad9. These are two separate levers of about the same size, and they stack:

- **Wake lever (~43 ms median, ~90 ms p90):** the AP holds frames for a dozing station until the next DTIM beacon. With a typical 102.4 ms beacon interval and DTIM 1, the expected wait is about half of that, which fits +43 median / +90 p90 [I]. The router's actual DTIM is **[U]**. Only power-save changes touch this.
- **Upstream lever (~40 ms):** only a response cache removes it, and only on a hit. A cache hit lands near 81 ms (the local-answer figure), not near 38 ms.

Correction to `performance.md` Q3: it says the `CONFIG_ESP_WIFI_SLP_DEFAULT_*` values are compile-time only with no runtime API. Not quite: setters for them are declared in an IDF private header and linked in the prebuilt libs (Q2). They are unsupported, but callable at runtime.

| # | Option | Value | Effort | Risk |
|---|--------|-------|--------|------|
| 0 | Measure first: PS_NONE single-query median, inter-query gap histogram, would-have-hit counter (experiments below) | High: bounds the prize of every other row and decides between 2 and 4 | Low (two counters and one toggle, OTA flash) | None: nothing is served differently |
| 1 | Check the router's DTIM is 1 (router setting) | Probably zero: DTIM 3 would put the median wake near 150 ms, not +43, so the data already points to DTIM 1 [I]. A free check | Very low, no firmware | None |
| 2 | Raise Wi-Fi `min_active_time` at runtime (private setter, 50 ms to 100-300 ms first) | Medium: keeps the radio awake through the upstream reply tail and close follow-up queries | Very low (one call after connect) | Medium: private API, value range and post-init effect **[U]**; if LAN broadcasts (mDNS, SSDP, ARP) refresh the timer **[U]**, a long window keeps the radio awake almost always, with PS_NONE heat |
| 3 | DNS response cache, 32-64 entries, TTL-aware | Medium on hits (~40 ms each); hit rate unknown, clients already cache (Q4) | Medium (~80-120 lines) | Medium: correctness rules in Q4 |
| 4 | Adaptive PS: `WiFi.setSleep(WIFI_PS_NONE)` for H seconds after the last query, then back to MIN_MODEM | Conditional: the triggering query has already paid the wake; only queries 50 ms to H after the previous one gain. On a chatty LAN it stays on nearly always (back to 69 C) | Low | Low-medium: switch latency and AP notification undocumented (Q1) |
| 5 | Always `WIFI_PS_NONE` | High for latency (-43 ms median [I]) | Trivial | Thermal: 69 C vs 48 C [S] |
| - | `listen_interval` / MAX_MODEM, `esp_wifi_set_inactive_time`, light sleep | None or negative for latency (Q2, `performance.md` Q3) | | |

Do first: #0 and #1. Then #2 if the gap histogram shows clustered queries (page loads), #3 if the would-have-hit counter is above about 20%, #4 only if the histogram shows long quiet periods between short bursts.

## Q1. Switching power save at runtime

Docs and headers:

- "Call `esp_wifi_set_ps(WIFI_PS_MIN_MODEM)` ... after calling `esp_wifi_init()`. When station connects to AP, Modem-sleep will start. When station disconnects from AP, Modem-sleep will stop." `WIFI_PS_NONE` "increases power consumption, but minimizes the delay in receiving Wi-Fi data in real time. When Modem-sleep mode is enabled, the delay in receiving Wi-Fi data may be the same as the DTIM cycle". [P] https://docs.espressif.com/projects/esp-idf/en/v5.5.5/esp32c3/api-guides/wifi.html (section "Station Sleep"; source text https://raw.githubusercontent.com/espressif/esp-idf/v5.5.5/docs/en/api-guides/wifi.rst lines 2146-2148).
- The header's only constraint is "Default power save type is WIFI_PS_MIN_MODEM"; no restriction on calling it while connected, no documented cost or latency of the switch. [L] `LIBS/include/esp_wifi/include/esp_wifi.h:671-680`. The IDF docs do not say either way **[U]**.
- Arduino calls it at runtime itself: `WiFiGenericClass::setSleep(wifi_ps_type_t)` calls `esp_wifi_set_ps()` immediately when the STA interface is started, otherwise stores the value; the stored value is re-applied on every `ARDUINO_EVENT_WIFI_STA_START`, so a runtime toggle survives reconnects. [L] `ARD/libraries/WiFi/src/WiFiGeneric.cpp:779-797`, `ARD/libraries/WiFi/src/STA.cpp:122`. That the wrapper does this while connected is the best evidence that runtime switching is an intended use [I].
- Inside the closed Wi-Fi blob the call goes through a symbol named `wifi_set_ps_process`, and the PM code has `pm_send_sleep_null_cb`. [L] `strings LIBS/lib/libnet80211.a`. My reading: the request is queued to the Wi-Fi task (asynchronous), and entering doze is announced to the AP with a null-data frame carrying the PM bit, as 802.11 legacy power save requires **[I]** (symbol names only). How fast the AP sees the change after `esp_wifi_set_ps()` returns is **[U]**: not documented. Experiment E3 measures it.
- Coexistence: the Wi-Fi guide says "in coexist mode, Wi-Fi will remain active only during Wi-Fi time slice", i.e. even PS_NONE does not keep the radio on while Bluetooth holds the slice [P] wifi.html above. The coexistence guide has no rule that forbids PS_NONE [P] https://docs.espressif.com/projects/esp-idf/en/v5.5.5/esp32c3/api-guides/coexist.html . BT is compiled into the libs (`CONFIG_BT_ENABLED=y`, NimBLE) [L] `LIBS/sdkconfig:714-718`, but `src/main.cpp` never includes or starts Bluetooth (grep for BLE/NimBLE/esp_bt finds nothing) [L], so coexistence time-slicing should not apply [I].

Verdict: runtime switching is supported by the Arduino wrapper and not forbidden by the docs. Its cost and propagation time are undocumented and must be measured (E3).

## Q2. Knobs that shorten modem-sleep wake latency

| Knob | Where | Runtime? | Effect on latency |
|------|-------|----------|-------------------|
| AP DTIM period | Router | n/a (router setting) | MIN_MODEM "wakes up to receive beacon every DTIM period" [L] `LIBS/include/esp_wifi/include/esp_wifi_types_generic.h:383`. DTIM 1 is the floor |
| `min_active_time` (`CONFIG_ESP_WIFI_SLP_DEFAULT_MIN_ACTIVE_TIME=50`) | [L] `LIBS/sdkconfig:1866` | **Yes, via private API**: `void esp_wifi_set_sleep_min_active_time(uint32_t)` [L] `LIBS/include/esp_wifi/include/esp_private/wifi.h:647`, symbol present in `LIBS/lib/libnet80211.a` [L] (`nm`). IDF calls it at init with the Kconfig value in microseconds (`CONFIG_..._MIN_ACTIVE_TIME * 1000`) [P] https://raw.githubusercontent.com/espressif/esp-idf/v5.5.5/components/esp_wifi/src/wifi_init.c lines 374-375 | Kconfig: range 8-60 ms, "Only for station in WIFI_PS_MIN_MODEM or WIFI_PS_MAX_MODEM", the station stays active at least this long and the timer is refreshed on send or receive [P] https://raw.githubusercontent.com/espressif/esp-idf/v5.5.5/components/esp_wifi/Kconfig . Whether the blob accepts values above 60 ms and whether a call after init takes effect: **[U]**. The private header's comment says "min active time for wifi to enter the sleep state when light sleep", which conflicts with the Kconfig's MIN/MAX_MODEM scope; IDF calls it unconditionally, so E5 decides |
| `wait_broadcast_data_time` (`=15`) | [L] `LIBS/sdkconfig:1869` | Yes, private: `esp_wifi_set_sleep_wait_broadcast_data_time` [L] `esp_private/wifi.h:665` | Only for broadcast after DTIM; DNS is unicast, so no effect [I] |
| keep-alive (`MAX_ACTIVE_TIME=10` s) | [L] `LIBS/sdkconfig:1868` | Yes, private: `esp_wifi_set_keep_alive_time` [L] `esp_private/wifi.h:654` | Null-data keep-alive interval, not a latency knob [P] Kconfig |
| `CONFIG_ESP_WIFI_SLP_BEACON_LOST_OPT` | not set [L] `LIBS/sdkconfig:1877` | Compile-time | Sleeps sooner after a lost beacon (power, not latency) [P] Kconfig |
| `CONFIG_ESP_WIFI_SLP_IRAM_OPT` | not set [L] `LIBS/sdkconfig:1865` | Compile-time | Puts TBTT/beacon RX in IRAM to cut power-save current [P] Kconfig; latency effect **[U]** |
| `listen_interval` | `wifi_sta_config_t` | Before connect only | Applies only to `WIFI_PS_MAX_MODEM` ("Listen interval for ESP32 station to receive beacon when WIFI_PS_MAX_MODEM is set") [L] `esp_wifi_types_generic.h:565`. Irrelevant to MIN_MODEM; with MAX_MODEM it makes latency worse |
| `esp_wifi_set_inactive_time` | runtime | Yes | Beacon-loss disconnect timer ("If the station does not receive a beacon frame ... during the inactive time, disconnect", default 6 s) [L] `esp_wifi.h:1378-1396`. Not a latency knob |

Why `min_active_time` matters for this workload [I]: on a forwarded query the device transmits upstream, which refreshes the 50 ms timer; Quad9's reply takes about 40 ms [S], so it usually lands inside the window, but a slower reply (p90 tail) arrives after the radio dozes and waits for the next DTIM. A 200-300 ms window would cover the upstream reply tail, and a 1 s window would also cover the follow-up queries a browser fires during one page load. This is the in-blob version of adaptive PS (#4) without any app-level state, with the same caveat: the timer is refreshed by received packets, and this firmware runs ESPmDNS on a LAN with mDNS/SSDP/ARP broadcasts, so with DTIM 1 a long window may never expire [I]. Start E5 small and watch temperature. The private header is on the include path already [I] (`LIBS/include/esp_wifi/include` is where `esp_wifi.h` comes from).

## Q3. Power and temperature

Documented currents (3.3 V):

| State | Current | Source |
|-------|---------|--------|
| Radio receiving (what PS_NONE holds between frames) | 84 mA peak RX (802.11b/g/n HT20), 87 mA (HT40); the table column is "Peak (mA)" | [P] ESP32-C3 Series Datasheet v2.4, Table 5-7, https://documentation.espressif.com/esp32-c3_datasheet_en.pdf |
| Modem sleep, Wi-Fi clock gated, 160 MHz CPU idle | 16 mA (peripheral clocks off) / 21 mA (on) | [P] datasheet Table 5-8 |
| Modem sleep + Wi-Fi connected, DTIM1, 160 MHz, average | 21.47 mA | [P] https://docs.espressif.com/projects/esp-idf/en/v5.5.5/esp32c3/api-guides/low-power-mode/low-power-mode-wifi.html |

So PS_NONE keeps the receiver on continuously at up to about 4x the modem-sleep average (at most 84 vs 21 mA, i.e. about 0.28 W vs 0.07 W at 3.3 V; 84 is a peak figure, so this is an upper bound) [I], which fits the 69 C vs 48 C idle readings [S]. Idle PS_NONE current for this board was not measured **[U]**.

Adaptive scheme estimate [I]: with hold time H, the fraction of time in PS_NONE is d = 1 - (sum over inter-query gaps g of max(0, g - H)) / T. Average current is at most about 21 + 63 d mA (upper bound, from the peak RX figure). Examples: one client query every 60 s and H = 30 s gives d about 0.5 (about 53 mA, temperature roughly midway); a phone or TV querying every few seconds gives d near 1 (back to PS_NONE heat). The latency gain only applies to queries that arrive between 50 ms and H after the previous one; the first query of each quiet period still pays the full wake. The gap histogram (E2) gives d and the gain directly; do not pick H before seeing it.

## Q4. DNS response cache

What clients already cache (repeats from the same client within TTL never reach the device):

| Client | Behaviour | Source |
|--------|-----------|--------|
| Android (DnsResolver module, shared by apps on a network) | Default 640 entries (`MAX_ENTRIES_DEFAULT = 64 * 2 * 5`); TTL = minimum answer TTL; negative answers cached with min(SOA TTL, SOA MINIMUM) per RFC 2308 | [P] https://android.googlesource.com/platform/packages/modules/DnsResolver/+/refs/heads/main/res_cache.cpp (lines 158, 715-790) |
| Chrome / Chromium host cache | System-resolver results cached 60 s (`kCacheEntryTTLSeconds = 60`), failures 0 s; built-in resolver results get a 60 s minimum TTL (`kMinimumTTLSeconds`) | [P] https://chromium.googlesource.com/chromium/src/+/refs/heads/main/net/dns/host_resolver_manager_job.cc (lines 55-61, 801, 970) |
| Chrome entry cap | `kDefaultMaxEntries` 1000 if `ENABLE_BUILT_IN_DNS`, else 100 (tag 99.0.4774.0) | [P] https://chromium.googlesource.com/chromium/src/+/refs/tags/99.0.4774.0/net/dns/host_cache.cc (`HostCache::CreateDefaultCache`, lines 958-964); current value **[U]** |
| Apple mDNSResponder (macOS/iOS) | Caches unicast answers; negative entries without a usable SOA start at 60 s and double on repeat, capped at 3600 s | [P] https://github.com/apple-oss-distributions/mDNSResponder/blob/main/mDNSCore/mDNS.c (around line 9438-9450). Entry cap **[U]** |
| Windows DNS Client | Windows 2000 docs: `Dnscache\Parameters\MaxCacheEntryTtlLimit` default 86400 s, a ceiling on answer TTLs (otherwise the answer's TTL is used) | [P] https://learn.microsoft.com/en-us/previous-versions/windows/it-pro/windows-2000-server/cc959517(v=technet.10) ; negative-cache time and current Windows defaults **[U]** |
| dnsmasq (reference home forwarder) | Default cache 150 names, negative caching on | [P] https://thekelleys.org.uk/dnsmasq/docs/dnsmasq-man.html (`--cache-size`, `--no-negcache`) |

Hit rate evidence: no primary measurement of home-network forwarder hit rates found **[U]**. The only primary data is 2002 campus traces (MIT, KAIST): "little benefit is obtained from sharing a forwarding DNS cache among more than 10 or 20 clients", and cutting A-record TTLs to a few hundred seconds "has little adverse effect on hit rates" [P] Jung et al., IEEE/ACM ToN 10(5), 2002, https://pdos.csail.mit.edu/papers/dns:ton.pdf . For a home LAN this suggests [I]: hits come from (a) several devices asking for the same popular names, (b) separate caches on one device (Chrome's own cache vs the OS cache vs apps with their own resolver), (c) devices with no cache. Chrome's own repeats do not produce hits: its 60 s re-ask of a system-resolved name is answered by the OS cache, and on the built-in path its 60 s floor holds entries at least as long as a TTL-honouring forwarder would. Measure with E4 before building.

Minimal-cache correctness checklist:

- Key on (qname, qtype, qclass), qname compared case-insensitively. [P] RFC 4343 https://www.rfc-editor.org/rfc/rfc4343.txt . Serve with the client's own question bytes and txid.
- Store the minimum TTL of the answer RRs; on serve, write the remaining TTL into every RR. TTL is "the time interval that the resource record may be cached before the source of the information should again be consulted"; zero means do not cache. [P] RFC 1035 section 3.2.1, https://www.rfc-editor.org/rfc/rfc1035.txt . Treat TTLs with the top bit set as 0; an upper bound is allowed. [P] RFC 2181 section 8, https://www.rfc-editor.org/rfc/rfc2181.txt
- Negative answers (NXDOMAIN, NODATA): TTL = min(SOA TTL, SOA MINIMUM) from the authority section; "Negative responses without SOA records SHOULD NOT be cached". [P] RFC 2308 section 5, https://www.rfc-editor.org/rfc/rfc2308.txt
- SERVFAIL: MAY be cached, at most 5 minutes; simplest is not to cache it. [P] RFC 2308 section 7.1
- TC=1: do not cache ("should not cache a possibly partial set of RRs"). [P] RFC 1035 section 7.4
- EDNS: "OPT RRs MUST NOT be cached"; and "If an OPT record is present in a received request, compliant responders MUST include an OPT record". [P] RFC 6891 section 6.1.1, https://www.rfc-editor.org/rfc/rfc6891.txt . So strip OPT before storing and add a fresh one per client query that had one [I]. Do not serve a cached answer larger than the client's limit (512 bytes without EDNS) [I].
- Put the DO bit in the key, or do not cache when the query sets DO, so DNSSEC-aware clients never get an answer stripped of RRSIGs [I].
- Cache only rcode NOERROR/NXDOMAIN with the same question as asked; blocked names stay answered by the blocklist path, never from the cache [I].

## Experiments (OTA flash; USB flashing disturbs the keyboard on the same hub)

Put E0/E3 (PS toggle), E2, E4 and E5 behind web endpoints in one build, so one OTA flash runs all of them.

- **E0, PS_NONE baseline:** temporarily `WiFi.setSleep(WIFI_PS_NONE)`, 40 sequential blocked-name queries at 200-500 ms spacing; compare the median with 81 ms. This is the ceiling for every power-save option (expected near 38 ms if the decomposition is right [I]).
- **E1, router DTIM:** read the router's DTIM setting, or capture beacons on the Mac (Wireless Diagnostics sniffer) and read the TIM element.
- **E2, gap histogram:** in the `dnsrx` task, record `millis()` between consecutive client queries into ~8 log-scale buckets (<50 ms, <200 ms, <1 s, <5 s, <30 s, <120 s, more); show on the stats page. Run for a day. Gives d (Q3) and how many queries would gain from #2 or #4.
- **E3, switch cost:** web endpoint that calls `WiFi.setSleep()`; from the Mac, query at 300 ms spacing, flip the mode mid-run, plot per-query latency to see how many queries it takes to change. Also read temperature after 10 min in each mode.
- **E4, would-have-hit counter:** ring of 64 (name hash, qtype, expiry from the upstream answer's min TTL) for forwarded answers; count forwarded queries whose key is present and unexpired. Serves nothing, so no correctness risk. Hits / forwarded = the cache's hit rate.
- **E5, `min_active_time`:** after connect, `esp_wifi_set_sleep_min_active_time(100000)` then `300000` (microseconds) via `#include "esp_private/wifi.h"`; repeat the E0 run at 100 ms spacing and the forwarded run; watch temperature. If nothing changes, the call is ignored post-init or the value is clamped **[U]**.

## Not verified / could not confirm

- Propagation time of `esp_wifi_set_ps()` to the AP, and whether it sends a PM null frame on each switch (closed blob).
- Accepted range of the private sleep setters after init.
- The router's DTIM; idle PS_NONE current on this board; the spacing used in today's 40-query runs.
- Current Windows and Chrome cache caps; any home-network forwarder hit-rate study.
