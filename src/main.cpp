#include "gateway.h"
#include "ttf.h"     // scalable AA TrueType text faces (v0.2)
#include "audio.h"
#include "sound.h"
#include "sensor.h"
#include "battery.h"
#include "sdcard.h"
#include "backup.h"
#include "touch.h"
#include "canvas.h"   // canvasAnimLoadPlay: the boot animation
#include <ETH.h>      // LCD Gateway: IP101 PHY behind the PoE jack
#include <esp_ota_ops.h>   // esp_ota_get_running_partition(): which slot are we actually running?

// After this many crash/watchdog reboots in a row, the boot logic reformats FATFS -- a corrupt
// filesystem is the one thing that can crash a flash write and boot-loop the board. See setup().
#define PANIC_REFORMAT_THRESHOLD 3
// Consecutive crash/watchdog reboots, kept in RTC memory: it survives a reboot but is garbage on
// a cold power-up. Written in setup(), cleared in loop() once the board has run healthy for 60s.
RTC_NOINIT_ATTR static uint32_t sfPanicBoots;
// Reset-cause history (v3.13.1): the last 8 reset reasons + uptime-at-death, kept in RTC
// memory so they survive warm reboots (a cold power-up zeroes them, which reads as
// POWERON -- correct). Exposed in /api/status as "resets" so a crash self-reports even
// with nothing attached to USB: the one that got away on 2026-07-30 taught us that.
RTC_NOINIT_ATTR uint8_t  sfResetLog[8];       // esp_reset_reason_t values, newest first
RTC_NOINIT_ATTR uint32_t sfResetUpMin[8];     // minutes of uptime when each reset hit
RTC_NOINIT_ATTR static uint32_t sfResetMagic; // 0x52534C47 = the ring is initialised
static uint32_t sfBootMs = 0;                 // set in setup(); loop() stamps uptime
static char     sfBootNote[160];              // boot line, written to the SD log after mount

// TWDT breadcrumb (v0.4.5): a task-watchdog panic names its culprit on serial only --
// useless on a wall. This strong override of IDF's weak hook runs inside the TWDT
// interrupt, before the panic: snapshot what each core was running into RTC memory,
// and the next boot's BOOT line carries it into the SD log. The 2026-08-05 TASK_WDT
// reboot (core-0 idle starved by an unidentified spinner) is why this exists.
RTC_NOINIT_ATTR static char     sfWdtTask[2][20];
RTC_NOINIT_ATTR static uint32_t sfWdtMagic;             // 0x57444754 = snapshot valid
extern "C" void esp_task_wdt_isr_user_handler(void) {
  for (int c = 0; c < 2; c++) {
    TaskHandle_t t = xTaskGetCurrentTaskHandleForCore(c);
    const char* n = t ? pcTaskGetName(t) : NULL;
    strlcpy(sfWdtTask[c], n ? n : "?", sizeof(sfWdtTask[c]));
  }
  sfWdtMagic = 0x57444754;
}

// main.cpp -- boot sequence and supervisor.
// setup() brings the system up in dependency order (mutexes, config, clock,
// filesystem, virtual modules, panel, frame link, WiFi, servers, then tasks).
// loop() is the watchdog supervisor: it logs periodic telemetry and reboots if a
// task stalls or the heap runs critically low.
//
// Ordering constraints worth knowing:
//   * cfg must be loaded before dispPlan(), which decides the wall's real size.
//   * dispPlan() must precede vmInit(): the plan says how many modules exist,
//     because a cell too small to hold a glyph is not a module.
//   * sfFsInit() must precede vmInit(), which restores /vmods.dat.
//   * vmInit() must precede dispInit(), which reads vmCount.
//   * dispInit() runs before WiFi mostly for a lit panel early in boot; the DSI
//     framebuffer is in PSRAM, so it does not compete with WiFi for internal SRAM.

void setup() {
  // 1. Mutexes first -- must exist before any task touches shared data
  msgMutex   = xSemaphoreCreateMutexStatic(&msgMutexBuf);
  timeMutex  = xSemaphoreCreateMutexStatic(&timeMutexBuf);
  txMutex    = xSemaphoreCreateMutexStatic(&txMutexBuf);
  txQMutex   = xSemaphoreCreateMutexStatic(&txQMutexBuf);
  vmMutex    = xSemaphoreCreateMutexStatic(&vmMutexBuf);
  psramAllocInit();   // monitor ring + TX queue -> PSRAM

  // Debug output over native USB CDC (the board boots with CDC on).
  Serial.begin(115200);
  // This board has no USB-UART bridge: Serial IS the native USB CDC (HWCDC), so
  // CDC-on-boot cannot be turned off without losing the serial monitor entirely.
  // The hazard is that HWCDC::write() blocks on its TX ring for tx_timeout_ms
  // (100 ms by default) whenever a host has enumerated the port but is not
  // draining it -- a monitor left paused, or a USB power brick. DBG() is called
  // from inside web handlers, so that blocks taskWeb. 0 = drop instead of block.
  { unsigned long t = millis(); while (!Serial && millis() - t < 3000) delay(10); }
  delay(200);
  printf("\n[Boot] %s v%s\n", PRODUCT_NAME, FW_VERSION);
  // Which app slot is actually running, and when it was built. A serial reflash writes ota_0 and
  // resets otadata, but an OTA leaves the selector on the other slot -- so this is the first thing
  // to check when a board seems to be running firmware you did not just flash.
  { const esp_partition_t* rp = esp_ota_get_running_partition();
    printf("[Boot] running slot=%s, built %s %s\n", rp ? rp->label : "?", __DATE__, __TIME__); }
  bool fatfsRecover = false;   // set by the panic-recovery check inside the block below
  {
    esp_reset_reason_t rr = esp_reset_reason();
    const char* rs = "OTHER";
    switch (rr) {
      case ESP_RST_POWERON:  rs = "POWERON";  break;
      case ESP_RST_SW:       rs = "SW";       break;
      case ESP_RST_PANIC:    rs = "PANIC";    break;
      case ESP_RST_INT_WDT:  rs = "INT_WDT";  break;
      case ESP_RST_TASK_WDT: rs = "TASK_WDT"; break;
      case ESP_RST_WDT:      rs = "WDT";      break;
      case ESP_RST_BROWNOUT: rs = "BROWNOUT"; break;
      case ESP_RST_DEEPSLEEP:rs = "DEEPSLEEP";break;
      default: break;
    }
    printf("[Boot] reset=%s heap=%u psram=%u flash=%uKB sdk=%s\n",
           rs, (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getPsramSize(),
           (unsigned)ESP.getFlashChipSize()/1024, ESP.getSdkVersion());

    // Panic-recovery safeguard. sfPanicBoots lives in RTC memory: it survives a reboot -- even a
    // crash reboot -- and is only garbage on a cold power-up, which POWERON zeroes below. A
    // corrupted FATFS can crash a flash write and boot-loop the board (which is exactly what
    // happened once). After PANIC_REFORMAT_THRESHOLD crash/watchdog reboots IN A ROW -- each
    // before the board runs healthy for 60s, at which point loop() clears the count -- reformat
    // FATFS on this boot to break the loop: one self-healing reboot instead of a brick.
    const bool crashReset = (rr == ESP_RST_PANIC || rr == ESP_RST_INT_WDT ||
                             rr == ESP_RST_TASK_WDT || rr == ESP_RST_WDT);
    if (rr == ESP_RST_POWERON || rr == ESP_RST_BROWNOUT) sfPanicBoots = 0;   // cold boot: init RTC
    else if (crashReset)                                  sfPanicBoots++;
    else                                                  sfPanicBoots = 0;   // clean SW reset/OTA
    // Reset-cause ring (v3.13.1): shift in this boot's cause. Cold boot re-inits.
    if (rr == ESP_RST_POWERON || sfResetMagic != 0x52534C47) {
      memset(sfResetLog, 0, sizeof(sfResetLog));
      memset((void*)sfResetUpMin, 0, sizeof(sfResetUpMin));
      sfResetMagic = 0x52534C47;
    }
    for (int i = 7; i > 0; i--) { sfResetLog[i] = sfResetLog[i-1]; sfResetUpMin[i] = sfResetUpMin[i-1]; }
    sfResetLog[0] = (uint8_t)rr;
    sfResetUpMin[0] = 0;                       // this boot's death-uptime; stamped by loop()
    sfBootMs = millis();
    // The post-mortem line (v3.13.2): sdInit() has not run yet, so remember it and
    // write it once the card is up -- see the sdLog call after sdInit() below.
    snprintf(sfBootNote, sizeof(sfBootNote),
             "BOOT %s v%s reset=%s prev-uptime=%lum panicBoots=%lu",
             PRODUCT_NAME, FW_VERSION, rs, (unsigned long)sfResetUpMin[1],
             (unsigned long)sfPanicBoots);
    if (rr == ESP_RST_TASK_WDT && sfWdtMagic == 0x57444754) {
      sfWdtTask[0][sizeof(sfWdtTask[0]) - 1] = 0;       // RTC RAM is trusted-but-verified
      sfWdtTask[1][sizeof(sfWdtTask[1]) - 1] = 0;
      const size_t l = strlen(sfBootNote);
      snprintf(sfBootNote + l, sizeof(sfBootNote) - l, " wdt-running: c0=%s c1=%s",
               sfWdtTask[0], sfWdtTask[1]);
    }
    sfWdtMagic = 0;    // one-shot: a stale snapshot must never attach to a later reset
    if (sfPanicBoots >= PANIC_REFORMAT_THRESHOLD) {
      printf("[RECOVERY] %u crash reboots in a row -- reformatting FATFS to break the loop\n",
             (unsigned)sfPanicBoots);
      sfPanicBoots = 0;
      fatfsRecover = true;
    }
  }

  // 2. Config
  cfgSetDefaults();
  loadConfig();

  // 3. Clock (before WiFi so timestamps work from boot; invalid until NTP)
  rtcHwInit();
  rtcRead();

  // 3b. Audio codec (ES8311, duplex mic+speaker) register bring-up -- HERE, single-threaded, because
  //     rtc.cpp's raw Wire access holds no bus lock; after this the audio module
  //     never touches I2C again (capture start/stop is I2S-only). See audio.h.
  audioInit();
  soundInit();   // ES8311 speaker DAC -- same single-threaded I2C window
  sensorInit();  // SHTC3 temp/humidity -- same single-threaded I2C window
  batteryInit(); // configure the battery-sense ADC pin (7B; no-op on boards without a battery)
  touchInit();   // GT911 capacitive touch -- same single-threaded I2C window (LCD)

  // 4. Plan the panel geometry. The DSI panel is a fixed size; the module grid is
  //    clamped to what fits it, and the wall IS the module list, so the module count
  //    comes from the plan rather than the raw grid config.
  gPanel = dispPlan(DEFAULT_PANEL_W, DEFAULT_PANEL_H, cfg.gridCols, cfg.gridRows);
  if (gPanel.cols != cfg.gridCols || gPanel.rows != cfg.gridRows)
    printf("[PANEL] wall %ux%u does not fit the %ux%u panel -- using %ux%u\n",
           cfg.gridCols, cfg.gridRows, DEFAULT_PANEL_W, DEFAULT_PANEL_H, gPanel.cols, gPanel.rows);

  // 5. Filesystem, then the thing that restores from it: the virtual modules' own
  //    state (/vmods.dat). Nothing else on this board is sticky.
  sfFsInit(fatfsRecover);
  sdInit();           // microSD (v3.10): mount the TF card if one is fitted (setup() only)
  sdLog("%s", sfBootNote);   // the post-mortem line: why THIS boot happened (v3.13.2)
  // Self-restore (v3.16): a freshly formatted FATFS -- panic-recovery or first
  // boot -- gets the card's mirror copied back BEFORE anything reads assets
  // (boot animation below, atlas/vmods later). Data loss becomes a log line.
  backupRestoreIfNeeded(sfFsFormatted);
  gTransType = cfg.transType; gTransMs = cfg.transMs;   // restore persisted transition (v3.7.2)
  vmBuildReel();      // the shared reel: every CP1252 glyph, then the colours
  vmInit((int)gPanel.cols * (int)gPanel.rows);

  // 6. Panel. Before WiFi: the default (internal-SRAM) framebuffer must be claimed
  //    before the WiFi stack, the other big claimant on that pool. (fbPsram moves the
  //    framebuffer to octal PSRAM instead -- v3.11.)
  dispInit();
  dispMarkDirty();
  ttfBegin();          // scalable AA TrueType faces (v0.2): parse the bundled TTF; cache is lazy

  // 7. Boot animation (v2.1): if a library animation is configured, play it now --
  //    before WiFi -- so the wall is alive seconds after power-on. The first
  //    split-flap command (or canvas/effect start) supersedes it, as always.
  if (cfg.bootAnim[0] && gPanel.ready) {
    int rc = canvasAnimLoadPlay(cfg.bootAnim);
    if (rc) printf("[ANIM] boot animation '%s' failed (%d)\n", cfg.bootAnim, rc);
    else    printf("[ANIM] boot animation '%s' playing\n", cfg.bootAnim);
  }

  // 8a. Ethernet (LCD Gateway): the P4 carries an IP101 PHY behind the PoE jack --
  // wired is this board's born deployment mode, and it needs no C6 co-processor.
  // Runs alongside WiFi; DHCP on either interface serves the gateway. Pin facts:
  // SMI MDC=31 MDIO=52, PHY reset=51, addr 1, RMII data pins via build flags
  // (IDF-default P4 wiring, matching Waveshare's own ethernetbasic example).
#if BOARD_HAS_ETH
  Network.onEvent([](arduino_event_id_t ev, arduino_event_info_t info) {
    if (ev == ARDUINO_EVENT_ETH_GOT_IP) {
      printf("[ETH] up: %s\n", ETH.localIP().toString().c_str());
      gEthUp = true;    // taskNetwork powers WiFi down: wired is the reliable native path
    } else if (ev == ARDUINO_EVENT_ETH_DISCONNECTED || ev == ARDUINO_EVENT_ETH_LOST_IP) {
      printf("[ETH] link down\n");
      gEthUp = false;   // taskNetwork brings WiFi back as the fallback
    }
  });
  ETH.setHostname(cfgHostname());
  if (!ETH.begin(ETH_PHY_IP101, 1, 31, 52, 51, EMAC_CLK_EXT_IN))
    printf("[ETH] init failed (no PHY?)\n");
#else
  // No Ethernet PHY on this board (e.g. the 7B): WiFi-only. gEthUp stays false, so
  // taskNetwork never powers WiFi down. (Skipping ETH.begin also drops the emac
  // reset-timeout errors the missing PHY logged.)
#endif

  // 8. WiFi -- MUST be initialised here, on the main Arduino task.
  // The SoftAP is a FALLBACK only: start in station mode and connect to the
  // configured network. With no network configured, bring the AP up immediately
  // so the gateway is reachable for first-time setup.
  // Must precede WiFi.mode()/begin(): the DHCP client latches the name at association.
  WiFi.setHostname(cfgHostname());
  printf("[WiFi] hostname %s (http://%s.local)\n", cfgHostname(), cfgHostname());
  WiFi.mode(WIFI_STA);
  // Modem sleep OFF. Upstream leaves it on and gets away with it, but on this board
  // it correlates with packets going missing at an RSSI of -45: outbound SYNs never
  // answered (the SYN-ACK arrives while the radio is parked and the AP does not
  // re-deliver it), and established sockets reset by the peer. Inbound traffic hides
  // it, because serving a request keeps the radio awake -- which is why the web UI
  // limped while every outbound TCP connect failed.
  //
  // The saving it buys is irrelevant here: this board is mains-powered and is
  // driving a HUB75 panel that dwarfs the radio's draw. Reliability wins. If a
  // future board is battery-powered, make this a config flag rather than flipping
  // it back blindly.
  WiFi.setSleep(false);
  if (strlen(cfg.wifiSSID)) {
    WiFi.begin(cfg.wifiSSID, cfg.wifiPass);
    staDownSince = millis();
    printf("[WiFi] STA connecting to %s...\n", cfg.wifiSSID);
  } else {
    wifiSetApActive(true);
    printf("[WiFi] No network configured -- fallback AP only\n");
  }

  // 9. Servers
  otaInit();
  webInit();

  // 10. Tasks. Display sits on core 1 with the network stack, leaving core 0 for
  //    frames, the web server and the clock -- the same split the physical
  //    gateway uses, with the panel taking the slot the OTA task shares.
  // 3072 (was 2048, v3.13.2): the brightness schedule added sscanf+gmtime_r to this
  // task's tick and the soak flagged a 496-byte minimum -- the thinnest margin of any
  // task, and a plausible cause of the unexplained 2026-07-30 reboot. 1 KB well spent.
  xTaskCreatePinnedToCore(taskRTC,     "RTC",     4096, NULL, 2, &hTaskRTC,   0);
  xTaskCreatePinnedToCore(taskFrames,  "Frames",  5120, NULL, 3, &hTaskFrames, 0);
  // v3.0: HTTP requests are served by esp_http_server's own task (httpx.h); taskWeb
  // is just the SSE pump + supervisor now, so its stack shrank from the handler-era 8 KB.
  // CORE 1 (LCD, v0.2, was core 0): taskWeb runs the canvas STREAM pump -- the heavy
  // per-frame render of a busy canvas app (aquarium, games) at 1280x800. On core 0 it
  // starved the HTTP server (core_id=0) and the WiFi-over-SDIO RX task that share it, so
  // the board went HTTP-unresponsive while a heavy app streamed and control requests
  // (an app switch) timed out. Core 1 hosts taskDisplay -- which PARKS while a canvas app
  // owns the panel -- and the low-priority taskNetwork poller, so the render has core 1
  // largely to itself and core 0 stays responsive. The real WiFi RX/TX is in high-prio
  // IDF tasks that preempt this regardless of core; the present is PPA-serialised (gShowMutex).
  // 12 KB, not 4 KB: the canvas STREAM pump runs on this task and executes the full canvas
  // ops path -- including the deep stb_truetype gtext rasterizer. 4 KB overflowed and tripped
  // the Core-1 stack-protection panic (boot-loop when a client streamed gtext). The one-shot
  // ops path already gets 10 KB on the httpd worker (httpx.cpp); the stream pump must match,
  // with margin, since it is a notch deeper (pump -> record dispatch -> canvasOpsRun -> ttf).
  xTaskCreatePinnedToCore(taskWeb,     "Web",     12288, NULL, 2, &hTaskWeb,   1);
  xTaskCreatePinnedToCore(taskNetwork, "Network", 6144, NULL, 1, &hTaskNet,   1);
  // 12 KB, NOT 4 KB: taskDisplay runs the stb_truetype rasterizer since the scalable
  // exclusive ticker (v0.4.5) -- the same stack class that boot-looped taskWeb at 4 KB
  // on stream+gtext. A cold glyph-cache miss mid-ticker rasterizes on THIS stack.
  xTaskCreatePinnedToCore(taskDisplay, "Display", 12288, NULL, 2, &hTaskDisp,  1);

  printf("[Boot] Ready\n");
}

void loop() {
  // Stamp this boot's running time into the reset ring (v3.13.1): after a crash, the
  // NEXT boot reports how long this one lived. Cheap: one RTC-mem write per pass.
  sfResetUpMin[0] = (uint32_t)((millis() - sfBootMs) / 60000UL);
  // RTC chip-service diagnostics (v3.15): the service runs on stack-lean taskRTC, so
  // it mails its log lines here -- this task has the stack for FatFs.
  // FATFS->SD backup (v3.16): manual trigger, nightly pass, first-run pass. Runs
  // here because this task has FatFs stack headroom and no latency budget.
  backupTick();
  // Heap heartbeat to the SD log every 10 min (v3.13.2): a crashed board's log then
  // shows its health right up to the end, without anything on USB.
  { static uint32_t lastHb = 0;
    if (millis() - lastHb > 600000UL) {
      lastHb = millis();
      sdLog("hb up=%lum heap=%u minheap=%u", (unsigned long)(millis() / 60000UL),
            (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMinFreeHeap());
    } }
  static unsigned long lastWdgCheck = 0;
  unsigned long now = millis();
  // Panic-recovery: once we have run healthy for a minute, this boot plainly was not a crash
  // loop, so clear the RTC crash counter -- only RAPID consecutive crashes (each before 60s)
  // accumulate toward the FATFS reformat in setup().
  static bool panicCounterCleared = false;
  if (!panicCounterCleared && now > 60000UL) { sfPanicBoots = 0; panicCounterCleared = true; }
  if (now - lastWdgCheck >= 30000UL) {
    lastWdgCheck = now;
    // Rich periodic telemetry. No 'frag' percentage: 100 - maxblk/heap is a ratio of two
    // independently noisy numbers, and it IMPROVES as the free heap shrinks -- at heap
    // 82908/maxblk 31732 it read 62%, and at heap 62276 with the SAME maxblk it read 50%.
    // The two numbers that actually predict an allocation failure are the watermarks:
    // min (lowest free heap ever) and minblk (smallest largest-block ever).
    //
    // heap/min/maxblk are INTERNAL RAM only -- that is what
    // (unsigned)ESP.getFreeHeap() reports -- and the panel's DMA framebuffer is the big claimant
    // on that pool, so a modest maxblk is expected. psram is counted separately; the
    // monitor ring and the TX queue live there. The virtual modules do
    // NOT -- they are pinned to internal RAM, see vmInit().
    // Heap + min-ever heap + largest free block
    // (fragmentation: a big gap between freeHeap and maxAlloc is a common
    // pre-crash signature). Per-task stack high-water marks catch the
    // canary-overflow class before it fires. rx/tx/drop counters surface
    // frame traffic.
    unsigned freeHeap = (unsigned)ESP.getFreeHeap();
    unsigned minHeap  = (unsigned)ESP.getMinFreeHeap();
    unsigned maxBlk   = ESP.getMaxAllocHeap();
    unsigned sFrm = hTaskFrames   ? uxTaskGetStackHighWaterMark(hTaskFrames)   : 0;
    unsigned sWeb = hTaskWeb   ? uxTaskGetStackHighWaterMark(hTaskWeb)   : 0;
    unsigned sNet = hTaskNet   ? uxTaskGetStackHighWaterMark(hTaskNet)   : 0;
    TaskHandle_t hHttpd = xTaskGetHandle("httpd");   // esp_http_server's worker
    unsigned sHtp = hHttpd     ? uxTaskGetStackHighWaterMark(hHttpd)     : 0;
    unsigned sRtc = hTaskRTC   ? uxTaskGetStackHighWaterMark(hTaskRTC)   : 0;
    unsigned sDsp = hTaskDisp  ? uxTaskGetStackHighWaterMark(hTaskDisp)  : 0;
    static unsigned minBlkEver = 0xFFFFFFFFu;
    if (maxBlk < minBlkEver) minBlkEver = maxBlk;
    printf("[WDG] up=%lus heap=%u min=%u maxblk=%u minblk=%u "
           "stk(frames/web/net/httpd/rtc/disp)=%u/%u/%u/%u/%u/%u "
           "tx=%lu psram=%u panel=%d "
           "wifi=%d eth=%d ap=%d rssi=%d mods=%d\n",
           now/1000, freeHeap, minHeap, maxBlk, minBlkEver,
           sFrm, sWeb, sNet, sHtp, sRtc, sDsp,
           txCount,
           (unsigned)ESP.getFreePsram(), (int)gPanel.ready,
           (int)(WiFi.status()==WL_CONNECTED),
           (int)gEthUp,
           (int)gApActive,
           (WiFi.status()==WL_CONNECTED) ? (int)WiFi.RSSI() : 0,
           vmCount);

    // Boot grace period: skip stall detection for the first 60 s. The first boot
    // after flashing formats the FATFS partition (a long blocking flash
    // operation), and WiFi bring-up can briefly skew task scheduling.
    if (now >= 60000UL) {
      // A heartbeat in the future (wdg > now) can only come from transient boot
      // skew -- treat it as healthy rather than letting the unsigned subtraction
      // underflow into a 49-day "stall".
      bool okFrm  = (wdgFramesMs == 0 || wdgFramesMs > now || now - wdgFramesMs < 30000UL);
      bool okWeb  = (wdgWebMs   == 0 || wdgWebMs   > now || now - wdgWebMs  < 120000UL);
      bool okNet  = (wdgNetMs   == 0 || wdgNetMs   > now || now - wdgNetMs  < 30000UL);
      bool okDisp = (wdgDispMs  == 0 || wdgDispMs  > now || now - wdgDispMs < 30000UL);
      if (!okFrm || !okWeb || !okNet || !okDisp) {
        printf("[WDG] STALL: Frames=%d Web=%d Net=%d Disp=%d (heap=%u) -- rebooting\n",
               okFrm, okWeb, okNet, okDisp, (unsigned)ESP.getFreeHeap());
        sdLog("WDG STALL Frames=%d Web=%d Net=%d Disp=%d heap=%u -- rebooting",
              okFrm, okWeb, okNet, okDisp, (unsigned)ESP.getFreeHeap());
        delay(200);
        ESP.restart();
      }

      // WiFi-over-SDIO/C6 transport-wedge recovery (v1.3.7). The link to the C6 can silently
      // wedge under sustained load: the RPC transport dies (100% packet loss, endless
      // "rpc_core: Response not received" timeouts) while the host keeps WiFi.status() cached at
      // WL_CONNECTED -- so taskNetwork, which only ever tests WL_CONNECTED, never reconnects and
      // the board stays unreachable forever. The tell is RSSI==0 while "connected": 0 dBm is
      // physically impossible, so a read of 0 means the RSSI RPC (WifiStaGetApInfo) timed out.
      // A sustained run of it = a dead transport; reboot to re-init SDIO + reset the C6 (the slave
      // is reset on every host boot). WiFi-only boards only -- a wired board runs with WiFi off.
      static uint8_t rpcDead = 0;
      if (!gEthUp && WiFi.status() == WL_CONNECTED && WiFi.RSSI() == 0) {
        if (++rpcDead >= 3) {          // ~3 x 30 s: the C6 RPC has been dead ~90 s
          printf("[WDG] C6/SDIO transport wedged (RSSI=0 while connected x%u) -- rebooting\n", rpcDead);
          sdLog("WDG C6 transport wedged -- rebooting");
          delay(200);
          ESP.restart();
        }
      } else {
        rpcDead = 0;
      }
    }
    // Emergency reboot if heap falls critically low -- EXCEPT during a web OTA.
    // A fast sender fills the TCP receive window (~95 KB on this build's lwIP
    // config) faster than flash writes drain it, which transiently dives below
    // this floor on a strong link; the OTA handler's own throttle keeps it
    // survivable, the transient ends with the upload, and rebooting mid-flash
    // is the one genuinely destructive response. Every observed "mystery OTA
    // reboot" on the bench was this check firing. Outside an upload the floor
    // keeps its original meaning: a leak has won, reboot before malloc chaos.
    if ((unsigned)ESP.getFreeHeap() < 20000 && !gOtaInProgress) {
      printf("[WDG] CRITICAL: heap=%u -- rebooting\n", (unsigned)ESP.getFreeHeap());
      sdLog("WDG heap floor: heap=%u -- rebooting", (unsigned)ESP.getFreeHeap());
      delay(200);
      ESP.restart();
    }
  }
  vTaskDelay(pdMS_TO_TICKS(1000));
}
