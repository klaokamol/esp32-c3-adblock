# Performance research (ESP32-C3 adblock)

Scope: DNS latency, burst handling, CPU/flash/heap cost, thermal/power. Read-only research; no source file was changed and nothing was built or flashed.

Legend: **[S]** measured in the session (cited, not re-derived), **[L]** read from a local file of the installed toolchain (path given), **[P]** read from a primary online source (URL given), **[I]** my inference from [S]/[L]/[P], **[U]** unverified or could not confirm.

Installed stack (from local files): `platform-espressif32` 8.0.0 (`~/.platformio/platforms/espressif32/platform.json`), Arduino-ESP32 libs built from `esp-idf: v5.5.5` (`~/.platformio/packages/framework-arduinoespressif32-esp32c3-libs/versions.txt`). IDF docs below are the v5.5.5 ones to match. `platformio.ini` already has `-Os` (the firmware-size report's top item is applied).

## Summary and ranked recommendations

Findings in one paragraph: the ~6 answered queries of a 16-query burst is very probably the lwIP UDP receive mailbox (default 6, [L]/[P] below) filling while `loop()` is stuck inside a blocking `forwardUpstream()`. That mailbox cannot be enlarged on the official `espressif32@8.0.0` + prebuilt-libs setup; the practical fix is to drain the socket faster (async forward), not to resize it. Latency is dominated by modem-sleep wake (up to one DTIM interval, documented), which the device cannot tune below the AP's DTIM.

**Correction after measuring on the device (2026-10-26) [S]:** the mailbox is the cap, but the blocking forward is not what fills it. Bursts of **blocked** names, which never touch upstream, were capped at 6-7 of 16 on both the blocking and the async-forward firmware. With modem sleep the AP buffers frames and delivers them in one batch when the radio wakes; lwIP (tcpip task, priority 18) posts the whole batch into the 6-slot mailbox before `loopTask` (priority 1) runs, so the rest is dropped. Fix shipped: two receive tasks at `ESP_TASK_TCPIP_PRIO + 1` block in `recvfrom` on the client and upstream sockets and copy each datagram into a FreeRTOS message buffer, so the mailbox never holds more than one datagram. Result with modem sleep on: 16 and 32 queries at 0-20 ms spacing, forwarded or blocked, all answered (5 runs each, `tools/dns_burst.py`); idle temperature unchanged (48C). #1 (async forward) still helps on its own: forwarded bursts at 10-20 ms spacing went from 7-11 to 11-16 of 16 answered, slowest reply from about 1-1.5 s to 0.3-0.5 s. #5 is no longer needed.

| # | Recommendation | Value | Effort | Risk |
|---|----------------|-------|--------|------|
| 1 | Non-blocking upstream forward: pending-query table keyed by wire txid, `loop()` never waits on upstream (Q2) | High: removes head-of-line blocking (up to 2 s per query) and lets the 6-slot mailbox drain in microseconds | Medium (about 60-100 lines in `main.cpp`) | Medium: must keep the issue #10 guarantees (txid, question, source match) |
| 2 | Cap the blocking wait now as a stop-gap: upstream timeout 1000 ms to roughly 250-400 ms, and a hold-down on a dead upstream (Q2) | Medium: bounds worst case from 2 s to under 1 s per query for 3 lines of change | Very low | Low: slow upstreams get retried by the client instead |
| 3 | DNS response cache, 32-64 entries, TTL-aware (Q2) | Medium: removes the upstream leg (about 25-50 ms of the 67-141 ms forwarded figure [I]) for repeat names; cuts upstream traffic | Medium | Medium: TTL/rcode/EDNS rules (RFC 2308, 6891) must be followed or stale/wrong answers get served |
| 4 | Do nothing about modem sleep beyond what is done; optionally lower CPU to 80 MHz with `setCpuFrequencyMhz(80)` (Q3) | Low-medium: datasheet says about 6 mA less while CPU runs [P] | Very low | Low-medium: Wi-Fi behaviour at 80 MHz not confirmed in a primary source [U] |
| 5 | Enlarge the UDP mailbox (up to 64) via a custom IDF build (pioarduino `custom_sdkconfig`, or lib-builder) (Q1) | Medium for bursts, but redundant after #1 | High: leaves the official platform, rebuilds libs | Medium-high |
| 6 | Light sleep / DFS for a large power drop (Q3) | High for power (documented 1.4 mA vs 21 mA averages) | High: needs `CONFIG_PM_ENABLE` and tickless idle, not in the prebuilt libs | High: latency and CPU suspension, unmeasured for this workload |
| 7 | Bloom filter in RAM (Q4) | Low: flash lookup already index-bounded and cached | Medium | Medium: false positives block real sites |
| 8 | `-O2` for `main.cpp` only, IRAM tweaks (Q5) | Very low: CPU is not the bottleneck | Low | Low |

Do first: #2 (minutes), then #1. Measure again before touching #3 to #8. A cheap experiment that confirms or refutes the mailbox theory without any rebuild is in Q1.

## Q1. Is the UDP recvmbox the cause of ~6 answered?

**Verdict: the mailbox is the cap (confirmed on the device [S]), but item 5 below is wrong about what fills it. See the correction in the Summary: batched delivery after modem sleep plus task priority, not the blocking forward.**

Evidence chain (each link traced):

1. The prebuilt libs for this board set `CONFIG_LWIP_UDP_RECVMBOX_SIZE=6` (and `CONFIG_LWIP_TCPIP_RECVMBOX_SIZE=32`, `CONFIG_LWIP_TCP_RECVMBOX_SIZE=6`, `CONFIG_LWIP_MAX_UDP_PCBS=16`, `CONFIG_LWIP_SO_RCVBUF=y`). [L] `~/.platformio/packages/framework-arduinoespressif32-esp32c3-libs/sdkconfig` lines 2176, 2223, 2238, 2239, 2160. Upstream default and range: default 6, range 6 to 64, help "The recommended value is 6." [P] https://raw.githubusercontent.com/espressif/esp-idf/v5.5.5/components/lwip/Kconfig (entry `LWIP_UDP_RECVMBOX_SIZE`).
2. The config feeds lwIP's `DEFAULT_UDP_RECVMBOX_SIZE`. [L] `.../include/lwip/port/include/lwipopts.h:877` (`#define DEFAULT_UDP_RECVMBOX_SIZE CONFIG_LWIP_UDP_RECVMBOX_SIZE`); opt.h doc at `.../include/lwip/lwip/src/include/lwip/opt.h:1932`.
3. At socket creation, `netconn_alloc()` creates the receive mailbox with that size for UDP (`size = DEFAULT_UDP_RECVMBOX_SIZE; ... sys_mbox_new(&conn->recvmbox, size)`). On arrival, `recv_udp()` does `sys_mbox_trypost(&conn->recvmbox, buf)` and on failure `netbuf_delete(buf)`, i.e. the datagram is silently dropped. [P] https://raw.githubusercontent.com/espressif/esp-lwip/master/src/api/api_msg.c (functions `netconn_alloc`, `recv_udp`).
4. One mailbox slot is one datagram; `lwip_recvfrom` removes at most one datagram per call. [P] https://raw.githubusercontent.com/espressif/esp-lwip/master/src/api/sockets.c (`lwip_recvfrom_udp_raw`).
5. The firmware only reads the socket from `loop()` via `handleDns()`, and `forwardUpstream()` can block for up to 1 s per upstream, 2 upstreams per query. [L] `src/main.cpp:252-285` (loop at 1000 ms deadline, `tries` up to 2). While one forwarded query is waiting, new datagrams pile into the 6 slots and overflow. 16 queries sent at once with only ~6 answered (+1 or 2 already pulled by the app) matches [S].
6. Other queues are larger than the observed limit, so they are not the cap: tcpip input mailbox 32, Wi-Fi dynamic RX buffers 32, static RX 8 (`CONFIG_ESP_WIFI_DYNAMIC_RX_BUFFER_NUM=32`, `CONFIG_ESP_WIFI_STATIC_RX_BUFFER_NUM=8`). [L] same sdkconfig, lines 2176, 1840, 1839.

Why the count is the same in PS_NONE and MIN_MODEM [S]: for a simultaneous burst the frames arrive back to back either way, so the mailbox overflows before `loopTask` runs. At 2-10 ms spacing the modes differ [S]: MIN_MODEM still capped blocked-name bursts at 6, PS_NONE answered 16 in most runs, because modem sleep regroups spaced frames into one batch.

**Can it be changed from PlatformIO without rebuilding the framework? No, not on this platform.**

- `CONFIG_LWIP_UDP_RECVMBOX_SIZE` is a compile-time IDF option baked into `liblwip.a` of the prebuilt libs. The Arduino-ESP32 docs say settings are changed with `idf.py menuconfig` when using Arduino as an IDF component, and recommend recompiling the core with the Arduino Static Library Builder to reuse modified settings. [P] https://docs.espressif.com/projects/arduino-esp32/en/latest/esp-idf_component.html
- The installed official `platform-espressif32` 8.0.0 only reads `sdkconfig.<env>` / `sdkconfig.defaults` for the **espidf** framework (`builder/frameworks/espidf.py:101,131`); nothing for `framework = arduino`. [L] `~/.platformio/platforms/espressif32/builder/frameworks/espidf.py`. `build_flags = -DCONFIG_LWIP_UDP_RECVMBOX_SIZE=...` does nothing for already-compiled `liblwip.a`. [I]
- pioarduino (a fork, not the official platform) reads a `custom_sdkconfig` option in `platformio.ini` and a board-level `espidf.custom_sdkconfig`, and reinstalls/rebuilds the Arduino libs when it changes. [P] https://github.com/pioarduino/platform-espressif32/blob/main/builder/frameworks/arduino.py (lines around `custom_sdkconfig`; fetched copy, about lines 262-300). Its README lists stable as "espressif Arduino 3.3.12 and IDF v5.5.5" and does not document ESP32-C3 support or the option syntax [P] https://github.com/pioarduino/platform-espressif32 ; whether the C3 target works with `custom_sdkconfig` in practice: **[U], not tested**. The syntax of the option's value (sdkconfig lines, or `file://` path) is visible in the builder source only.
- Cost of a bigger mailbox: slots hold pointers, but every queued datagram holds its pbuf in heap until read. Exact per-pbuf overhead: **[U]**.

**Real options, ranked for this project:**

| Option | Does it raise the 6-datagram cap? | Source / verdict |
|--------|-----------------------------------|------------------|
| Drain faster (async upstream, #1 above) | No, but makes the cap rarely reached: the app empties the mailbox between packets | [I] from items 3 and 5 above |
| `setsockopt(SO_RCVBUF)` | **No.** `LWIP_SO_RCVBUF` only bounds bytes: `recv_udp` drops if `recv_avail + p->tot_len > conn->recv_bufsize`, default `RECV_BUFSIZE_DEFAULT = INT_MAX`; the slot count stays `DEFAULT_UDP_RECVMBOX_SIZE`. [P] api_msg.c above; [L] `opt.h:2117-2121`. IDF lists `SO_RCVBUF` as available if `CONFIG_LWIP_SO_RCVBUF` is set [P] https://docs.espressif.com/projects/esp-idf/en/v5.5.5/esp32c3/api-guides/lwip.html ("Socket Options"). Setting it would only lower the cap |
| Raw lwIP `udp_recv()` callback with an own ring buffer | Yes in principle: the callback receives each pbuf directly in the tcpip thread (`udp_recv_fn(arg, pcb, p, addr, port)`, [L] `.../lwip/udp.h:77`), bypassing the mailbox | **Not supported by Espressif**: "The lwIP Raw API is designed for single-threaded devices and is not supported in ESP-IDF." [P] lwip.html above ("Netconn API" section). `CONFIG_LWIP_TCPIP_CORE_LOCKING=y` is set in this build [L] sdkconfig:2145, so it could be made to work, but it is unsupported, adds thread-safety work, and pbuf ownership rules apply. Not recommended |
| Netconn API | Same 6-slot mailbox (it is the netconn's) | "Espressif does not test the Netconn API in ESP-IDF." [P] lwip.html. No benefit |
| Second socket / `SO_REUSEADDR` to split load | No: `SO_REUSE_RXTOALL` copies **broadcast/multicast** to all matching pcbs only, unicast is not load-balanced. [L] `opt.h:2137-2143` | Dead end |
| Custom IDF build (pioarduino or lib-builder) | Yes, up to 64 [P] Kconfig above | High effort, see table row 5 |
| Receive task above tcpip priority, copying into an own buffer | Effectively yes: the task preempts tcpip on each post, so the mailbox never fills | **Shipped**, see the correction in the Summary [S] |

**Cheapest confirming experiment (no rebuild, needs a temporary source edit, your call):** add `delay(500)` at the top of `loop()`, send 16 simultaneous queries for **blocked** domains. If the mailbox is the cap, about 6-8 get answered after the delay regardless of upstream; if it is something else (e.g. rate of `parsePacket`, Wi-Fi RX buffers) the number will differ. Also: `NetworkUDP::parsePacket()` allocates `malloc(1460)` plus a `cbuf` per datagram and frees both after (per-query heap churn). [L] `~/.platformio/packages/framework-arduinoespressif32/libraries/Network/src/NetworkUdp.cpp:304-380`. A direct `recvfrom` loop on a raw socket would avoid that but adds code; value unmeasured **[U]**.

## Q2. Lowering per-query service time

### 2a. Non-blocking upstream forward (highest value)

Today (`src/main.cpp:252-285`): flush stale replies, send, then spin on `parsePacket()` with `delay(1)` until 1 s, then retry the second upstream. `handleDns()` loops up to 16 queries serially, so worst case one loop call can block about 32 s [I] (16 queries x 2 s), during which web, OTA and the OLED also stall.

Design (minimal): a pending table of N entries (N = 8 to 16): `{clientIP, clientPort, clientTxid, wireTxid, deadline, tries, qlen, query bytes}`. On a forwardable query: allocate entry, assign fresh random wire txid, send, return immediately. Each `loop()` pass: read the upstream socket (`parsePacket` loop), match reply to entry by wire txid + question section + source address/port (same checks as today), restore client txid, reply; then sweep entries past their deadline and resend to the other upstream once, then drop. Memory: 16 entries x ~300 B (query copy up to 260 B as today) is about 5 KB static. [I]

Primary-source rules to keep:
- Match by ID, question, source address and port, destination port. RFC 5452 section 9.1 ("A resolver implementation MUST match responses to all of the following attributes of the query") and 4.2. [P] https://www.rfc-editor.org/rfc/rfc5452.txt . The current code already does this.
- RFC 5452 section 9.2 also recommends an unpredictable source port per outstanding query and full-range random IDs. The current code uses one socket (`upstreamCli.begin(0)`, one ephemeral port) and `esp_random()` txids. A single-socket async design keeps that same weaker property; per-query ports would need a socket per pending entry (heap/FD cost, `CONFIG_LWIP_MAX_SOCKETS=16` [L] sdkconfig:2155). Accept or not is a design call, **[I]**.
- RFC 1035 section 4.1.1: the ID "can be used by the requester to match up replies to outstanding queries." [P] https://www.rfc-editor.org/rfc/rfc1035.txt

This also lets the 6-slot mailbox (Q1) drain fast, which is the real burst fix. It does not fix the radio wake latency (Q3).

### 2b. Shorter timeout and circuit breaker (stop-gap, 3 lines)

- Timeout: RFC 1035 section 4.2.1 says "the minimum retransmission interval should be 2-5 seconds" for a resolver. [P] RFC 1035. That is a rule for the original resolver; a forwarder that wants to fail over inside the client's own retry window needs a shorter per-hop timeout. The current 1 s x 2 can exceed a typical client retry; the exact timeout of common stub resolvers is **[U]** (not traced to a primary source here). Measured upstream RTT is small compared with modem-sleep delay [S] (forwarded 67-141 ms vs local 17-113 ms), so a 250-400 ms cap loses almost nothing on a healthy path. [I]
- Hold-down: remember when an upstream last timed out and skip it for a few seconds (the code already prefers the last good one via `upPref`). RFC 2308 section 7.2 limits "dead server" marking to "not longer than five (5) minutes" [P] https://www.rfc-editor.org/rfc/rfc2308.txt ; a seconds-scale hold-down is well inside that.
- Upstream selection: `upPref` already sticks to the last upstream that answered. Parallel race to both upstreams would halve tail latency but doubles upstream traffic; not recommended on 2 public resolvers without need. **[I]**

### 2c. Response cache

Today only the block verdict is cached (256 entries, 5 bytes + flags each [L] `src/main.cpp:41,52-54`), not the upstream answers.

What the RFCs require:
- TTL: "a 32 bit signed integer that specifies the time interval that the resource record may be cached"; zero means use only for the transaction in progress. [P] RFC 1035 section 3.2.1. So a cache must decrement TTLs on the way out (or rewrite them to remaining time) and never serve past expiry. Rewriting every RR TTL requires walking the answer section; a simpler option is to store the minimum TTL, serve while `now < expiry`, and rewrite all RR TTLs to remaining seconds, **[I]**.
- Negative answers: TTL is "the minimum of the SOA.MINIMUM field and SOA's TTL"; "Values of one to three hours have been found to work well." [P] RFC 2308 section 5. Server failure responses must not be cached for more than 5 minutes (section 7.1); simplest is not to cache SERVFAIL/REFUSED at all. [P] https://www.rfc-editor.org/rfc/rfc2308.txt
- EDNS: OPT records "MUST NOT be cached, forwarded, or stored" (RFC 6891 section 6.1.1) and the requestor's advertised payload size "MUST NOT be cached for use beyond the transaction" (6.2.3). [P] https://www.rfc-editor.org/rfc/rfc6891.txt . Consequence: a cache key must include qname, qtype, qclass; a stored reply must not blindly include the OPT RR of the original client, and responses larger than the next client's advertised size must not be sent (truncate or skip). Safest: cache only replies that fit in 512 bytes (the RFC 1035 UDP limit, [P] section 4.2.1), do not cache TC=1 replies, and strip OPT or only serve cached answers to queries with the same OPT state. This is the main correctness risk. **[I]**
- Blocked names already answered locally; the cache is only for forwarded names.

Memory cost on the C3 (no PSRAM): arithmetic only, **[I]**. Entry = 8 B key hash (or the 5-byte hash used already) + 4 B expiry + 2 B length + reply (typical A/AAAA answers are on the order of 60-150 B, up to 512). 64 entries x 192 B = 12 KB heap or static; 32 entries x 128 B = 4 KB. Free heap on the device was not measured here **[U]**; check `ESP.getFreeHeap()` after Wi-Fi, TLS fetch and OTA before choosing the size. Note the 20 KB `blIndex` and other statics already exist [L] firmware-size.md section 4.

Expected gain: only the upstream leg (the difference between forwarded and local [S], about 25-50 ms by my reading of the ranges, not a controlled measurement **[I]**) because the modem-sleep wake on the client-to-board and board-to-client path still applies. Value is therefore moderate; it also reduces upstream load and timeouts. Do #1 and #2b first.

## Q3. Modem sleep tuning

Documented behaviour (all [P] unless noted):

- `WIFI_PS_MIN_MODEM`: "station wakes up to receive beacon every DTIM period"; `WIFI_PS_MAX_MODEM`: interval set by `listen_interval`. https://raw.githubusercontent.com/espressif/esp-idf/v5.5.5/components/esp_wifi/include/esp_wifi_types_generic.h
- "In minimum power-saving mode, station wakes up every DTIM to receive beacon", "it cannot save much more power if DTIM is short for DTIM is determined by AP." Receive delay "may be the same as the DTIM cycle (minimum power-saving mode)". Default mode is `WIFI_PS_MIN_MODEM`. https://docs.espressif.com/projects/esp-idf/en/v5.5.5/esp32c3/api-guides/wifi.html (section "Station Sleep"); also `esp_wifi_set_ps` docs https://raw.githubusercontent.com/espressif/esp-idf/v5.5.5/components/esp_wifi/include/esp_wifi.h
- `listen_interval` default 3 (units: AP beacon intervals) and applies only with `WIFI_PS_MAX_MODEM`; MAX_MODEM "may lead to missed DTIMs and broadcast data." [P] types header above; https://docs.espressif.com/projects/esp-idf/en/v5.5.5/esp32c3/api-guides/low-power-mode/low-power-mode-wifi.html
- Arduino: `WiFi.setSleep(bool)` maps true to `MIN_MODEM`, false to `NONE`; `WiFi.setSleep(wifi_ps_type_t)` calls `esp_wifi_set_ps`. The Arduino default for ESP32-C3 is already `WIFI_PS_MIN_MODEM` (only ESP32-S2 defaults to NONE). [L] `~/.platformio/packages/framework-arduinoespressif32/libraries/WiFi/src/WiFiGeneric.cpp:386-388, 770-795`; `STA.cpp:122`. So the earlier `setSleep(false)` line in `main.cpp` was an explicit opt-out; commit 58f18f1 returned to the platform default. [L] `git show 58f18f1`.
- Sleep timing knobs in the prebuilt libs: `CONFIG_ESP_WIFI_SLP_DEFAULT_MIN_ACTIVE_TIME=50` ms ("it will work for at least ... If a data packet is received or sent during this period, the time will be refreshed"), `..._WAIT_BROADCAST_DATA_TIME=15` ms, `..._MAX_ACTIVE_TIME=10` s (keep-alive null data). [L] sdkconfig:1866-1869; semantics [P] https://raw.githubusercontent.com/espressif/esp-idf/v5.5.5/components/esp_wifi/Kconfig . These are compile-time only; Arduino has no runtime API for them. [I] **Practical meaning:** after any packet the radio stays awake at least 50 ms, so a query arriving within 50 ms of previous traffic skips the wake delay. That fits the wide 17 to 113 ms spread in [S], **[I]**.

What can and cannot be tuned:

| Knob | Available at runtime on this stack? | Effect |
|------|--------------------------------------|--------|
| DTIM period | No, set on the AP/router | Sets the MIN_MODEM latency floor. Router DTIM=1 is the best case; changing it is a router setting, not a firmware one |
| `listen_interval` | Only via `esp_wifi_set_config` before connect and only with `MAX_MODEM`; Arduino does not expose it **[U]** (no setter found in `STA.cpp`) | Would make latency worse, not better. Not useful here |
| `esp_wifi_set_ps` modes | Yes (`WiFi.setSleep`) | Already at the best trade-off the user accepted (about 69 C to 49 C, +~100 ms [S]) |
| `esp_pm_configure` DFS / auto light sleep | **No** with the prebuilt libs: `# CONFIG_PM_ENABLE is not set`; `esp_pm_configure()` returns `ESP_ERR_NOT_SUPPORTED` without it, and also without `CONFIG_FREERTOS_USE_TICKLESS_IDLE` for light sleep. [L] sdkconfig:1727; [P] https://docs.espressif.com/projects/esp-idf/en/v5.5.5/esp32c3/api-reference/system/power_management.html ; `TICKLESS` does not appear in the sdkconfig at all [L] | Needs a custom IDF build (same path as Q1 row 5) |
| CPU frequency | Yes: `setCpuFrequencyMhz(80)` exists in the core. [L] `cores/esp32/esp32-hal-cpu.h:67`. Libs default is 160 MHz (`CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ=160`, [L] sdkconfig:1757) | Datasheet: Modem-sleep, all peripheral clocks disabled: 160 MHz CPU running 23 mA / idle 16 mA; 80 MHz running 17 mA / idle 13 mA. [P] ESP32-C3 Series Datasheet v2.4, Table 5-8 https://documentation.espressif.com/esp32-c3_datasheet_en.pdf |
| TX power | Yes (`WiFi.setTxPower`) | Not in the source; not researched. **[U]** whether lowering helps heat meaningfully |

Documented current figures (all average currents from Espressif's shielded-box tests, 160 MHz, [P] https://docs.espressif.com/projects/esp-idf/en/v5.5.5/esp32c3/api-guides/low-power-mode/low-power-mode-wifi.html):

| Mode | DTIM1 avg | DTIM3 avg | DTIM10 avg |
|------|-----------|-----------|------------|
| Modem-sleep, DFS off (what this firmware runs) | 21.47 mA | 20.82 mA | 20.67 mA |
| Modem-sleep, DFS on | 11.35 mA | 10.71 mA | 10.32 mA |
| Auto light sleep + Wi-Fi | 1.4 mA | 0.62 mA | 0.31 mA |

Datasheet peak RX current: 84 mA (HT20), 87 mA (HT40); light-sleep (Wi-Fi powered down) 130 uA. [P] datasheet Tables 5-7 and 5-9. These explain the heat reduction from PS_NONE (radio on continuously) to MIN_MODEM; with DTIM1 the radio still wakes every beacon.

Honest assessment:
- The device cannot go below one DTIM interval of added latency and keep sleeping. The IDF page states the trade-off: sleep "results in slower response times, affecting communication timeliness". [P] low-power-mode-wifi.html.
- Auto light sleep (about 1.4 mA vs 21 mA averaged) is the one big lever, but the CPU is suspended ("CPU operation is suspended") so the Arduino loop runs only when woken; it needs the PM/tickless config rebuild, and a DNS server on the LAN would still wake each DTIM. Whether the first query after sleep would suffer more than MIN_MODEM latency is **[U]**, not measured or documented. Do not pursue unless power matters more than the existing thermal fix.
- `ESP_WIFI_SLP_IRAM_OPT` is off in the prebuilt libs [L] sdkconfig:1865; its Kconfig says it reduces "Wi-Fi power-save mode average current". [P] Kconfig above. Compile-time only. **[U]** size of the effect.
- The documented "recommended" modem-sleep configuration is MIN_MODEM + `CONFIG_PM_ENABLE` + DFS with `max_freq_mhz` = max CPU, `min_freq_mhz` = XTAL, `light_sleep_enable` = false, at tick 1000 Hz. [P] low-power-mode-wifi.html. Not reachable without the rebuild above.

## Q4. Flash/LittleFS lookup cost in `inFlash`

Facts from the code [L] `src/main.cpp:97-150`: 4096-entry sampled index in RAM (20 KB), binary search in RAM (12 steps, no flash), then one `seek` + one `read` of up to `MAX_RANGE` x 5 = 1280 B, linear scan in RAM. Cached (256 entries direct-mapped) per hash. `isBlocked()` hashes each parent suffix of the name (so a 4-label name can cost up to 3 lookups on a cold cache).

Flash reads per uncached lookup: one LittleFS file read per candidate hash. With the library config `CONFIG_LITTLEFS_READ_SIZE=128`, `CACHE_SIZE=512` [L] sdkconfig:3468-3471, a read of at most 1280 B is issued as a small number of flash reads (the exact number of block-device calls per file read depends on LittleFS internals, file layout and its metadata traversal): **[U]** not traced to the esp_littlefs/LittleFS source or measured. Per-lookup time on this board: **[U]**, not measured. The blocklist today is about 584 KB (`data/blocklist.bin`, ~117k hashes at 5 B [L]) so each bucket averages about 29 hashes (117k / 4096), far below `MAX_RANGE` 256, i.e. average reads are about 145 B rather than 1280 B. [I]

Is there a cheaper structure? A Bloom filter is the textbook candidate:
- The false-positive rate is `(1 - e^(-kn/m))^k` for `n` items, `m` bits, `k` hashes. For 117k domains at about 1% FP you need about 9.6 bits per item = 140 KB RAM, `k` about 7. That RAM does not fit the C3's heap budget alongside TLS (about 300 KB total usable DRAM is a common figure but **[U]** here) and the existing 20 KB index. [I]; formula is standard (Bloom, CACM 1970) but I did not fetch a primary copy: **[U]** as a cited source.
- A blocker's false positive means a legitimate domain is wrongly sinkholed with no recourse except the custom list; an index-then-verify scheme (what the code does: exact 40-bit hash match) already has a collision rate of about `n / 2^40` per lookup (117k / 1.1e12 about 1e-7). A Bloom prefilter in front would only save the flash read for names that are not blocked (the majority); a negative Bloom answer is exact, a positive one still goes to flash, so correctness is preserved if the filter is used only as a **pre-check** (no new false positives). Saving: the flash read for most allowed names. Cost: RAM above. Gain not measured **[U]**.
- Cheaper alternative with no new structure: raise the block-result cache from 256 entries (1.8 KB) or make it 2-way. RAM cost tiny, benefit depends on traffic pattern **[U]**. Existing hit rate is not instrumented; add a counter before deciding.
- Smaller index trade: 4096 entries x 5 B = 20 KB; doubling to 8192 halves the average bucket read for +20 KB. Marginal. **[I]**

Recommendation: measure first (add counters for `inFlash` calls and microseconds). Nothing here is justified by a documented problem. Not recommended now.

## Q5. Other documented levers

| Item | Source | Impact for this workload | Cost / risk |
|------|--------|---------------------------|-------------|
| `-O2` instead of `-Os` for app code | IDF speed guide: "Optimize for performance (-O2) ... may slightly increase binary size but usually helps speed"; UB may surface. https://docs.espressif.com/projects/esp-idf/en/v5.5.5/esp32c3/api-guides/performance/speed.html (section "Compiler optimization") | Negligible: service time is dominated by radio wake and RTT [S], not CPU. Do not trade the flash headroom (see firmware-size.md) | Low risk, low value |
| QIO flash mode | Same page: QIO/QOUT "almost double the speed at which code is loaded or executed from flash" | The libs sdkconfig shows `FLASHMODE_QIO=y` with 80 MHz flash [L] sdkconfig:455-465, but the string `CONFIG_ESPTOOLPY_FLASHMODE="dio"` also appears at line 460; the effective mode depends on the `board.json` / `board_build.flash_mode` PlatformIO passes to esptool, not traced **[U]**. Check `board_build.flash_mode = qio` and that the module supports it | Wrong flash wiring or chip makes the board fail to boot [P] same page; verify the module first |
| `CONFIG_LWIP_IRAM_OPTIMIZATION` | Kconfig: "can improve UDP/TCP throughput by >10% for single core mode" [P] Kconfig URL in Q1. Currently `# ... is not set` [L] sdkconfig:2150 | Throughput, not latency; DNS is tiny packets. Needs lib rebuild | Uses IRAM; rebuild path only |
| `CONFIG_ESP_WIFI_IRAM_OPT` / `ESP_WIFI_RX_IRAM_OPT` | Kconfig: place frequently called Wi-Fi / RX functions in IRAM [P] esp_wifi Kconfig URL in Q3; both off in the prebuilt libs [L] sdkconfig:1857,1859 | Fewer flash cache misses on the Wi-Fi path; magnitude **[U]** (no figure in the source) | Rebuild required; IRAM use |
| `CONFIG_LWIP_IPV6` off | "can save about 39 KB for firmware size and 2 KB RAM" [P] lwip.html ("Minimum RAM Usage"). `ESP_MLDV6_REPORT` is on in the libs [L] sdkconfig:2174 | Frees flash/RAM, may speed packet path slightly (**[U]**); also the firmware-size report notes `nd6.c` 10.7 KB | Rebuild required |
| Task priorities | lwIP task priority 18, Wi-Fi 23; app tasks doing network work should stay below 18. [P] speed.html ("Task priorities"). The Arduino loop task runs at priority 1 on core 0 (`CONFIG_ARDUINO_RUNNING_CORE=0`, [L] sdkconfig:510) | No change needed | n/a |
| Float math | "On ESP32-C3 these calculations are emulated in software and are very slow." [P] speed.html | The DNS path has no float; `temperatureRead()` in the stats handler only | None |
| Logging level | Lower `CONFIG_LOG_DEFAULT_LEVEL` reduces CPU on string formatting. [P] speed.html. `CORE_DEBUG_LEVEL=0` is already set by the builder [L] firmware-size.md section 2 | Already done | None |
| OLED flush on the loop thread | `board_ui.h:51` carries a `ponytail:` note: dirty-page flush blocks I2C about 2 ms per changed page [L] | Up to a few ms every second when the screen changes, small next to the 100 ms wake delay | Move to a task only if measurements show it |
| Heap churn per datagram | `NetworkUDP::parsePacket` does `malloc(1460)` + `new cbuf` + frees [L] NetworkUdp.cpp:304-380 | Fragmentation risk long-term, cost per query small **[U]** | A custom `recvfrom` wrapper; only if heap fragmentation is observed |

Single-core note: the C3 is single core [S], so the lwIP task (priority 18) and Wi-Fi task (23) preempt the Arduino loop (1). When `loop()` runs 100% (busy path, no `delay(1)`), tasks above it still run; lower-priority housekeeping does not. [I]

## Not verified / could not confirm

- That the 6-answered cap is the recvmbox: strongly supported by code and defaults, not proven on the device (experiment in Q1).
- pioarduino `custom_sdkconfig` on ESP32-C3 with this core (syntax and success): not tested; README does not document it.
- How many SPI flash reads one `inFlash` costs and how long it takes: no source or measurement.
- Whether `WiFi` is fully supported/stable at `setCpuFrequencyMhz(80)` on C3: no primary source found in the fetched pages. The IDF doc's DFS section says that with max 80 MHz "holding either lock keeps both at 80 MHz" [P] power_management.html, which implies 80 MHz is a supported max, but I did not find a statement about Wi-Fi at fixed 80 MHz without PM.
- The common default timeouts of client stub resolvers (drives the choice of upstream timeout).
- Current figures for this exact board and a DNS workload (all numbers above are Espressif's lab averages, not this device).
- RFC 2308 section 5 text: tool output called it "Caching Negative Answers"; the RFC has no positive caching section. RFC 1035 sections 7.2/7.3 (resolver cache) were truncated in the fetch and not quoted.
- lwIP docs at nongnu.org (`udp_recv`, multithreading) returned HTTP 429; the `udp_recv_fn` signature was taken from the local `udp.h` instead.
- A Bloom filter primary source (Bloom 1970); the formula is stated from general knowledge.
