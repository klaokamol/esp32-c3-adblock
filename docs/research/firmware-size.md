# Firmware size research (ESP32-C3 adblock)

Target: app image under 1,376,256 bytes (partitions.csv, `app0`/`app1` = 0x150000). Baseline 1,403,227.

Legend: **[M]** measured from `.pio/build/c3/firmware.map` / `firmware.elf` / objects, **[C]** measured by compiling the real sources with the real PlatformIO flags into a scratch dir (not a project rebuild), **[E]** estimated, **[U]** unverified (needs a real link to confirm).

Note: the map in `.pio/build/c3` is from the `-fno-exceptions` experiment build (`.eh_frame` is 0x2d14 = 11,536 B, `.flash.text` 0x113220, `.flash.rodata` 0x2a158).

## Headline finding: the app and every Arduino library are compiled with NO optimization (-O0)

- [M] `strings -a .pio/build/c3/src/main.cpp.o` shows the compiler producer string `GNU C++20 14.2.0 ... -fno-rtti -ffunction-sections ... -fno-exceptions ... -flto -ffat-lto-objects` with **no `-O` option**. GCC records `-Os` there when given (verified with the same toolchain on a test file: producer ends in `-g -Os`). The same holds for all 15 source-built archives (FrameworkArduino, WebServer, HTTPClient, Network, WiFi, ...).
- [M] Disassembly of `unpackHash()` in `firmware.elf` is classic -O0 code (`addi s0,sp,48`, every variable spilled to the frame).
- Cause [M]: `~/.platformio/packages/framework-arduinoespressif32-esp32c3-libs/flags/{c_flags,cpp_flags}` contain no `-O`. `~/.platformio/packages/framework-arduinoespressif32/tools/platformio-build.py` only adds `-Wall -Wextra -Werror=return-type`. PlatformIO core never adds `-Os` (only `piomisc.py:95` removes it for debug builds). The Arduino IDE path gets `-Os` from `framework-arduinoespressif32/platform.txt:34` (`compiler.optimization_flags=-Os`) and `:51-53`, but PlatformIO does not read platform.txt. The prebuilt IDF libs are already -Os (`sdkconfig`: `CONFIG_COMPILER_OPTIMIZATION_SIZE=y`).
- [C] Same sources and flags, `-O0` vs `-Os` (object text+rodata, before GC): main.cpp 51,184 -> 28,044; WebServer.cpp 46,625 -> 19,898; HTTPClient.cpp 40,437 -> 15,752; NetworkClient.cpp 14,295 -> 4,552; WString.cpp 13,271 -> 7,269.
- [C] Applied to exactly the input sections that survive `--gc-sections` in the map (276,138 B source-built flash/IRAM/data across 16 archives + main): -Os version of the same sections is ~129.8 KB vs ~306.5 KB for my -O0 recompile of the same sections (my recompile is ~11% above the map because of merged string sections). Scaled: **saving ~159 KB [E]**. Take a conservative 100 KB as the planning number. Result: roughly 1.24-1.30 MB, under the 1,376,256 limit by itself.

Fix: add `-Os` to `[env:c3] build_flags` (upstream default is -Os per platform.txt:34, so the Arduino libs are tested with it).

## Ranked table

| # | Idea | Expected saving | Risk | How to apply |
|---|------|-----------------|------|--------------|
| 1 | `-Os` for app + Arduino libs | ~100-159 KB [C]/[E] (see above) | Very low: it is the Arduino-ESP32 default (platform.txt:34). Behaviour change only if UB exists in main.cpp | `build_flags = -Os` in `[env:c3]` |
| 2 | `-fno-stack-protector` (flags carry `-fstack-protector`, c_flags/cpp_flags) | 8,600 B pre-GC over the 59 source-built objects at -Os [C]; ~5-6 KB after GC [E] | Medium-low: loses canary detection in WebServer/HTTP parsing code on the LAN. Prebuilt libs keep theirs (unaffected, ABI independent) | `-fno-stack-protector` in build_flags (CCFLAGS come after CXXFLAGS on the command line, so it overrides; the same mechanism made your `-fno-exceptions` work) |
| 3 | Keep your `-fno-exceptions -fno-unwind-tables -fno-asynchronous-unwind-tables` (+ `build_unflags -fexceptions`) | 24 KB measured by you; remaining `.eh_frame` is 11.5 KB from prebuilt libs [M]: nvs_flash 5,216, libstdc++ 3,420, libgcc 2,612 | Low (needs no `throw` in app; `-fexceptions` is in cpp_flags so unflag it) | already known; not removable further without custom libs |
| 4 | Stub out core dump (no coredump partition exists, so it can never write) | 11,678 B (libespcoredump total in map) [M] | Low-medium [U]: loses crash dump to flash (already impossible: partitions.csv has no `coredump` partition). Link must be tried | Define the two externally referenced symbols in an app .cpp (code below) |
| 5 | Stub `mbedtls_ssl_handshake_server_step` (TLS server side is linked, app is client only) | ~7.5 KB (`ssl_tls12_server.c.obj` text+rodata, excl. merged strings) [M] | Low [U]: only external reference to that object is `ssl_tls.c.obj` -> that one symbol (cref in map). Link must be tried | Define the symbol returning an error (code below) |
| 6 | Stub `strptime` + `mktime` to cut HTTPClient cookie chain -> newlib tz/scanf | ~13.5 KB libc members [M]: svfiscanf 7,574 + strptime 2,131 + tzset_r 1,481 + mktime 1,412 + lcltime_r 540 + sccl 164 + siscanf 96 + misc | Low-medium [U]: cookies unused (`setCookie` returns early if `_cookieJar` null, HTTPClient.cpp:1568; `generateCookieString` calls `mktime` first but ignores it without a jar, :1699-1708). Verify nothing else calls them | Define `strptime`/`mktime` stubs (code below). Call chain from disassembly: `HTTPClient::setCookie/generateCookieString -> strptime/mktime -> localtime_r/_tzset_unlocked -> siscanf -> __ssvfiscanf_r` |
| 7 | Drop `ArduinoOTA` (web OTA via `Update` already exists in main.cpp) | ~8.8 KB at -Os [E]: ArduinoOTA 3.1 + SHA2Builder 4.1 + PBKDF2_HMACBuilder 1.6 (cref: only ArduinoOTA.cpp.o pulls those Hash members; SHA1Builder is from WebServer and stays) | Feature loss: no `pio run -t upload` over WiFi (`espota`). Not in the must-keep list | delete `#include <ArduinoOTA.h>`, `ArduinoOTA.*` calls in main.cpp |
| 8 | Replace `HTTPClient` with raw `WiFiClientSecure` GET | HTTPClient ~8.3 KB at -Os [E] + #6 chain 13.5 KB [M] - custom code ~1.5-2 KB [E] = ~19 KB if #6 not done; ~6 KB extra if #6 is done | Medium: must hand-roll status line, chunked decoding, redirects (GitHub release assets 302 to another host), timeouts | only if still short after #1-#7 |
| 9 | gzip the dashboard `PAGE` | `PAGE` is 0x1feb = 8,171 B rodata [M]; ~5 KB saved [E] (HTML compresses ~60%) | Low: serve with `Content-Encoding: gzip` via `web.sendHeader` + `send_P` | pre-compress in a build script; skip unless needed |
| 10 | `-fno-threadsafe-statics` / `-fno-use-cxa-atexit` / `-fjump-tables` | 28 B / 512 B / 756 B pre-GC [C] | Not worth it (`-fno-use-cxa-atexit` changes the ABI vs libs) | do not apply |
| 11 | mbedcrypto camellia + aria stubs | 8.2 KB (camellia 4,274 + aria 3,936) [M] | Higher [U]: 18 symbols referenced from `cipher_wrap.c.obj`; a server picking a Camellia suite would fail the handshake | not recommended |
| 12 | `-flto` | 0 | n/a | do not pursue (see below) |

Items needing a custom IDF libs build and so out of scope: newlib nano printf (typical 25-50 KB per the IDF size guide, but libc here uses the full `svfprintf`+`vfprintf`+`dtoa`+`mprec` = ~29 KB [M]), IPv6/PPP/NAPT in lwip (`nd6.c` 10.7 KB, `ppp/lcp/ipcp/vj` ~12 KB, `ip4_napt` 4.2 KB [M]), SAE/WPA3 (`sae.c` 10.6 KB + `sae_pk` 3.4 KB + `esp_wpa3` 3.1 KB [M]), enterprise (already GC'd, see below).

## Code for stubs (ideas 4, 5, 6; none were linked or run)

```cpp
extern "C" {
int esp_system_include_coredump_init = 0;
void esp_core_dump_write(void* info) { (void)info; }

int mbedtls_ssl_handshake_server_step(void* ssl) { (void)ssl; return -0x7100; }

char* strptime(const char*, const char*, void*) { return nullptr; }
long long mktime(void*) { return -1; }
}
```

Caveats [U]: the exact prototypes must match (`esp_core_dump_write(panic_info_t*)` is declared at `include/espcoredump/include/esp_core_dump.h:91`; `time_t` is 64-bit in this newlib, check `sizeof(time_t)` and use the real headers instead of the sketch above). A duplicate-symbol link error means the archive member was still pulled by another symbol: then the stub is not viable. `-u esp_system_include_coredump_init` is in `flags/ld_flags` and resolves to our definition.

## Per-claim sources

### 1. Why `-flto` changed nothing
- [M] `framework-arduinoespressif32-esp32c3-libs/flags/ld_flags` contains `-fno-lto`, loaded as LINKFLAGS by `framework-arduinoespressif32/tools/platformio-build.py:394` after user flags. GCC honours the last of `-flto`/`-fno-lto`, so the link ignores the IR from the fat objects and uses their normal code (`-ffat-lto-objects` makes that possible).
- [M] The objects do carry LTO sections (`readelf -S src/main.cpp.o | grep gnu.lto`: present), so compile-time `-flto` took effect, only the link is non-LTO.
- [M] Prebuilt libs hold no IR: `libmbedcrypto.a` (105 members), `liblwip.a` (92), `libnet80211.a` (50): 0 members with `.gnu.lto*` sections. LTO could only ever touch ~255 KB of unoptimized source-built code, and that is dwarfed by the -O0 problem.
- Not verified: removing `-fno-lto` (would need a `ld_flags` override via `extra_scripts`) and whether IDF 5.5 officially supports LTO. The IDF size guide https://docs.espressif.com/projects/esp-idf/en/v5.5/esp32c3/api-guides/performance/size.html does not mention LTO at all (fetched). Recommendation: skip.

### 2. Flags
- `-Os`: see headline. IDF guide recommends `CONFIG_COMPILER_OPTIMIZATION_SIZE` (same URL above); sdkconfig has it for the prebuilt libs.
- Stack protector: `flags/c_flags`, `flags/cpp_flags` contain `-fstack-protector`; `sdkconfig`: `CONFIG_COMPILER_STACK_CHECK_MODE_NORM=y`. Saving measured with [C] recompile of 59 source-built objects at -Os (202,125 -> 193,525 B).
- `--gc-sections`: already in `ld_flags` and active (e.g. `esp_crt_bundle`, enterprise EAP objects are discarded in the map, see 3). `-ffunction-sections -fdata-sections` already in c_flags/cpp_flags. `-fno-rtti` already set. `CORE_DEBUG_LEVEL=0` set in `platformio-build.py`.
- Nano printf: `sdkconfig` has no `CONFIG_LIBC_NEWLIB_NANO_FORMAT` (only `CONFIG_ESP_ROM_HAS_NEWLIB_NANO_FORMAT=y`, a capability). The map shows the full implementation (`libc_a-svfprintf.o` 10,934, `libc_a-vfprintf.o` 11,212, `libc_a-dtoa.o` 4,203, `libc_a-mprec.o` 2,637). Switching needs a libs rebuild: unverified.
- C++ terminate handler: `__verbose_terminate_handler` and `__cxa_demangle` are NOT in the link (not present in the cross-reference table). Nothing to gain.
- `-fno-use-cxa-atexit`: 512 B [C], ABI risk, skipped.

### 3. Library pull-ins
- Cert bundle [M]: `esp_crt_bundle.c.obj` and `x509_crt_bundle.S.obj` (0x10d7b = 69,995 B) appear in "Discarded input sections" and `nm firmware.elf | grep -i crt_bundle` is empty. Already not in the image. `setInsecure()` use is irrelevant to size.
- WiFi enterprise [M]: `esp_eap_client.c.obj` is 18 B in the final image, no `eap*.o`/`tls_mbedtls.o` sections survive. libwpa_supplicant's 90 KB is mostly `sae`, `wpa`, `wpa_auth`, `wpa_common`, `wnm_sta`, `rrm`, `wpa_ft`: needs custom libs.
- network_provisioning: pulled by `WiFiGeneric.cpp.o` but 3,273 B remain, `libjson` 118 B. Not worth it.
- ArduinoOTA vs web OTA: both use `Update` (`libUpdate` 7,580 at -O0 [M], needed anyway). ArduinoOTA brings SHA2Builder/PBKDF2 (see table #7).
- AsyncUDP (6.1 KB at -O0, ~2.7 KB at -Os [E]) is pulled by `DNSServer` (disassembly: `DNSServer::~DNSServer -> AsyncUDP::~AsyncUDP`), needed for the captive portal. Replacing DNSServer with a few lines on `WiFiUDP` (already linked) would drop it: [E] ~2.7 KB + DNSServer ~1.9 KB, minus ~0.5 KB code. Optional.
- mbedTLS: `ssl_tls12_server.c.obj` 7,530 B linked only via `mbedtls_ssl_handshake_step` dispatch (cref), `mbedtls_ssl_handshake_server_step` 0x1964 = 6,500 B in `nm`. `sdkconfig`: `CONFIG_MBEDTLS_TLS_SERVER_AND_CLIENT=y`. `esp_http_client` is NOT linked today (not in link); switching to it would add `libesp_http_client` (504 KB archive) and `esp-tls`, so it is not smaller than Arduino `HTTPClient`+`NetworkClientSecure` [E].
- mbedcrypto biggest members [M]: ecp 8,374, ecp_curves 7,282, rsa 7,228, bignum 6,101, sha512 5,768, error 4,406, camellia 4,274, aria 3,936. All reachable through the prebuilt cipher/suite tables: only trimmable via sdkconfig (custom libs).
- Core dump [M]: libespcoredump 11,678 B; external refs only `esp_core_dump_write` from `libesp_system.a(panic.c.obj)` and the `-u esp_system_include_coredump_init` in `ld_flags`. `partitions.csv` has no coredump partition.

### 4. main.cpp (read `src/main.cpp`, map, `nm -S`)
- [M] main.cpp.o: 34,434 B text + 11,595 B rodata at -O0 (19.7 KB total at -Os [E]). Largest symbols: `PAGE` 8,171 B, `setup()` 1,890, `handleStats()` 1,702, `customDom` 3,200 / `clients` 4,224 / `blIndex` 20,480 (BSS, not in flash image).
- `String`: Arduino `String` is already linked (`WString.cpp` 13 KB at -O0, 7.3 KB at -Os [C]); no iostream is linked (no `libstdc++` stream objects in the map). `std::function` comes from `WebServer::on` (WebServer.h API), unavoidable without changing library.
- `String(temperatureRead(),1)` in `handleStats()` calls float printing, but the full newlib `_dtoa_r` is linked anyway by every `snprintf` (`_svfprintf_r` callers include `snprintf`, `sprintf`, `vsnprintf`), so removing it saves nothing [M].
- The only app-level cheap wins: -Os, gzip PAGE (#9). No avoidable heavy symbol was found in main.cpp itself.

## Not checked

- No project rebuild or link was done (instructed). All stub ideas (#4-#6) and the total -Os saving are not link-verified. Recommended order: apply #1 and relink, then see whether #2-#6 are even needed.
- Whether `PLATFORMIO_BUILD_FLAGS` order overrides `-fstack-protector` in your PlatformIO version was inferred from the existing `-fno-exceptions` result, not tested.
- Platform is unpinned in platformio.ini (`platform = espressif32`); installed is 8.0.0 per `~/.platformio/platforms/espressif32/platform.json`. Pin it so the -O0 default cannot change under you.
- Docs consulted: ESP-IDF v5.5 size guide (URL above); local files `framework-arduinoespressif32/platform.txt`, `tools/platformio-build.py`, `libraries/HTTPClient/src/HTTPClient.cpp`, `libraries/WiFi/src/STA.cpp`, libs `flags/*`, `sdkconfig`, `include/espcoredump/include/esp_core_dump.h`.
