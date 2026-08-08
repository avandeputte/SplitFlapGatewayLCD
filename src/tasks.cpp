#include "gateway.h"
#include "sse.h"   // taskWeb is the SSE push pump (v3.0)
#include "sensor.h"   // SHTC3 temp/humidity, polled from taskRTC (v3.7)
#include "panel.h"    // panelSetBrightness: the brightness schedule (v3.13)
#include "timer.h"    // alarmTick: daily alarms fire from taskRTC (v3.14)
#include "audio.h"    // audio capture for the reactive effects
#include "touch.h"    // GT911 touchscreen taps (LCD Gateway); I2C stays on taskRTC
#include "settings_ui.h" // settingsActive(): poll touch faster while the shade is open
#include "sdcard.h"   // sdLog: gesture-dismissal audit trail (v3.15)



// tasks.cpp -- the long-lived FreeRTOS task loops.
// taskFrames (core 0): sends scheduled batch frames when they fall due.
// taskRTC  (core 0): refreshes the wall clock once a second.
// taskWeb  (core 1, v0.2): SSE pump + canvas-stream pump + deferred-OTA supervisor (httpd
//                    serves requests on its own task since v3.0 -- see below).
// taskNetwork (core 1): WiFi reconnect and config persistence.
// taskDisplay (core 1, in display.cpp): steps the reels and repaints the panel.
// Each feeds its watchdog timestamp; loop() in main.cpp reboots if one stalls.
/* ----------------------------------------------------------
   FreeRTOS tasks
---------------------------------------------------------- */

void taskFrames(void* pv) {
  // Nothing arrives here any more: the modules stopped replying when the
  // 'v'/'A' queries were removed (v1.24), so this task's whole job is the
  // scheduled-batch drain below.
  while (true) {
    // Stand down while a firmware image is streaming: no frame traffic, no flash
    // contention. Feed the watchdog so the stall check stays happy.
    if (gOtaInProgress) { wdgFramesMs = millis(); vTaskDelay(pdMS_TO_TICKS(100)); continue; }

    // Send any scheduled batch frames now due. This is where /api/frames/batch's
    // cascade pacing actually happens -- moved off taskWeb so the HTTP server never
    // blocks on delay() (see frames.h). At most one 5 ms tick of latency per frame.
    framePollScheduled(millis());

    wdgFramesMs = millis();
    vTaskDelay(pdMS_TO_TICKS(5));
  }
}

// v3.0: evaluate the Quiet-Time schedule and toggle Quiet Time when local time
// crosses a window boundary. Transition-based (only acts on a change), so a
// manual Quiet toggle within a window is respected until the next boundary.
// True if the schedule is enabled and the current user-local time is in-window.
// The schedule (start/end/days) is entered in the browser's local time, and the
// browser sends its UTC offset with it, so we apply that offset and read the
// result as UTC fields -- independent of the gateway posixTZ (which defaults to
// UTC), and consistent with how the web UI renders every other timestamp.
bool quietSchedInWindow() {
  if (!cfg.quietSchedEnabled) return false;
  time_t utc = (time_t)rtcEpochNow();
  if (!utc) return false;            // RTC/NTP not valid yet
  time_t local = utc + (time_t)cfg.quietTzOffsetMin * 60;
  struct tm lt;
  gmtime_r(&local, &lt);             // offset already applied -> read fields directly
  int cur = lt.tm_hour * 60 + lt.tm_min;
  int sh = 22, sm = 0, eh = 7, em = 0;
  sscanf(cfg.quietStart, "%d:%d", &sh, &sm);
  sscanf(cfg.quietEnd,   "%d:%d", &eh, &em);
  int s = sh * 60 + sm, e = eh * 60 + em;
  bool dayOn = (cfg.quietDays >> lt.tm_wday) & 1;
  bool inWin = (s <= e) ? (cur >= s && cur < e) : (cur >= s || cur < e);  // overnight ok
  return dayOn && inWin;
}

/* ---- brightness schedule (v3.13) ---------------------------------------------------
   Auto-dim in a daily local-time window. Same clock/offset mechanics as the quiet
   schedule above. EDGE-triggered: brightness changes only on a window boundary (or on
   the first evaluation), so a manual brightness set inside the window sticks until the
   next boundary. Quiet Time wins: while quiet is on nothing is touched, and the state
   resets so the right level is re-applied the moment quiet ends. */
static bool dimSchedInWindow() {
  if (!cfg.dimEnabled) return false;
  time_t utc = (time_t)rtcEpochNow();
  if (!utc) return false;
  time_t local = utc + (time_t)cfg.quietTzOffsetMin * 60;
  struct tm lt;
  gmtime_r(&local, &lt);
  int cur = lt.tm_hour * 60 + lt.tm_min;
  int sh = 21, sm = 0, eh = 7, em = 0;
  sscanf(cfg.dimStart, "%d:%d", &sh, &sm);
  sscanf(cfg.dimEnd,   "%d:%d", &eh, &em);
  int s = sh * 60 + sm, e = eh * 60 + em;
  return (s <= e) ? (cur >= s && cur < e) : (cur >= s || cur < e);   // overnight ok
}

static void dimScheduleTick() {
  static int prevWant = -1;                    // -1 = re-evaluate and (re)apply
  if (gOtaInProgress) return;
  if (gQuietTime) { prevWant = -1; return; }   // quiet blanks the panel; re-apply after
  int want;
  if (!cfg.dimEnabled) {
    want = 0;
  } else {
    if (!rtcEpochNow()) return;                // no valid clock yet -- don't guess
    want = dimSchedInWindow() ? 1 : 0;
  }
  if (want != prevWant) {
    panelSetBrightness(want ? cfg.dimLevel : cfg.panelBright);
    if (prevWant != -1)
      DBG("[DIM] schedule: %s (brightness %u)\n", want ? "window start" : "window end",
          (unsigned)(want ? cfg.dimLevel : cfg.panelBright));
    prevWant = want;
  }
}

static void quietScheduleTick() {
  static int prevWant = -1;
  if (gOtaInProgress) return;        // never touch quiet/resync mid-flash (OTA safety)

  // Does the schedule currently want Quiet ON? A DISABLED schedule counts as
  // want=0 (not a hard early-return) so that disabling an active schedule flows
  // through the same falling-edge turn-off below and RELEASES Quiet Time, rather
  // than leaving it stuck on.
  int want;
  if (!cfg.quietSchedEnabled) {
    want = 0;
  } else {
    if (!rtcEpochNow()) return;      // enabled but no valid time yet -- don't change state
    want = quietSchedInWindow() ? 1 : 0;
  }

  // Self-healing, not edge-only: inside the window keep Quiet ON, re-asserting if
  // anything turned it off (external OFF is also refused mid-window; see the
  // MQTT/REST guards). On the falling edge -- window end OR the schedule being
  // disabled -- turn OFF, but only if the schedule was the one holding it
  // (prevWant==1), so a manual Quiet Time outside the schedule is left alone.
  if (want) {
    if (!gQuietTime) { DBG("[QUIET] schedule: in window, asserting ON\n"); sfSetQuietTime(true); }
  } else if (prevWant == 1 && gQuietTime) {
    DBG("[QUIET] schedule: released (window ended or disabled), turning OFF\n");
    sfSetQuietTime(false);
  }
  prevWant = want;
}

void taskRTC(void* pv) {
  // 100 ms base tick: the touch poll wants sub-second latency, and this task is where
  // ALL runtime I2C must live (the bus has no lock). Everything else keeps its slower
  // cadence via the ms-timers below.
  uint32_t lastSched = 0, lastEnv = 0, lastSec = 0;
  while (true) {
    if (cfg.touchEnabled) touchTick();   // GT911 poll (taskRTC: the I2C rule)
    panelBacklightService();   // pending backlight writes land here (the I2C rule)
    if (lastSec == 0 || millis() - lastSec >= 1000UL) {
      lastSec = millis();
      rtcRead();
      alarmTick();             // daily alarms, ~1/s (v3.14; overrides quiet by design)
    }
    if (lastSched == 0 || millis() - lastSched > 5000UL) {
      lastSched = millis();
      quietScheduleTick();     // evaluate the quiet window every 5s (prompt flip)
      dimScheduleTick();       // brightness schedule rides the same cadence (v3.13)
    }
    // Environment sensor (v3.7): runtime I2C stays on THIS task (the bus has no lock);
    // temp/humidity move slowly, so every 10 s is ample. First read soon after boot.
    if (lastEnv == 0 || millis() - lastEnv > 10000UL) {
      lastEnv = millis();
      sensorPoll();
    }
    // 20 ms while the settings shade is open, so touch tracks slider drags smoothly; 100 ms
    // otherwise. (touchTick is the only runtime I2C, so the poll rate lives here.)
    vTaskDelay(pdMS_TO_TICKS(settingsActive() ? 20 : 100));
  }
}

// v3.0: requests are served by esp_http_server's own task (see httpx.h), so taskWeb
// no longer pumps handleClient. It is now (a) the SSE push pump behind /api/events --
// the ONE task that writes to those parked sockets -- (b) the web watchdog's cover while
// the server idles, and (c) the deferred-OTA-reboot runner it always was.
void taskWeb(void* pv) {
  uint32_t      lastHash   = 0;   // last wall state pushed to the SSE streams
  unsigned long lastPush   = 0;   // last display event (rate floor: one per 150 ms)
  unsigned long lastKa     = 0;   // last keepalive comment
  unsigned long lastListen = 0;   // last listener health check
  while (true) {
    const unsigned long now = millis();

    // Web watchdog: the httpd task has no loop of its own to feed it, so cover it from
    // here -- but only while no request has been stuck in a handler for ~2 minutes.
    // A genuinely wedged handler stops the cover, wdgWebMs goes stale, and loop()'s
    // 120 s stall check reboots, exactly as it did when handlers ran on this task.
    // (Long uploads stay healthy: the shim stamps wdgWebMs on every body chunk.)
    const unsigned long busy = httpxBusySince();
    if (!busy || now - busy < 110000UL) wdgWebMs = now;

    // Gesture events: the touch detector publishes, this pump delivers. A live
    // timer/alarm CONSUMES a gesture as its stop/dismiss (double-tap the screen);
    // only otherwise does it reach the companion over SSE.
    { uint8_t gc; uint32_t gs;
      // Dismissal requires a DOUBLE gesture: one stray touch must never kill a timer.
      // Singles still reach the companion, which sets its own bar. Two single events
      // within the pair window also count as a double (human rhythm, not the sensor's).
      static unsigned long lastTouchEvMs = 0;
      // Pair window 1.0 s (was 1.5): deliberate doubles land ~0.3-0.6 s apart, but desk
      // activity (typing bursts) could pair two accidental singles inside 1.5 s -- the
      // 2026-08-01 false dismissal. Every dismissal is SD-logged with its channel so a
      // surprise dismissal is diagnosable from the log, not from guesswork.
      // Touchscreen tap: a DOUBLE-tap dismisses a timer/alarm, a single reaches the
      // companion as a "touch" event with {count,seq}.
      if (touchPoll(&gc, &gs)) {
        const bool dbl = (gc >= 2) || (millis() - lastTouchEvMs < 1000);
        lastTouchEvMs = millis();
        if (dbl && timerAlarmGestureDismiss()) sdLog("gesture dismiss: touch x%u", (unsigned)(gc >= 2 ? gc : 2));
        else { sseBroadcastGesture("touch", gc, gs); panelGestureBlip(0, 255, 120); }  // green = touch
      } }
    panelBlipService();

    // Self-heal a dead port-80 server (boot-time httpd_start failure): a ground-truth
    // check every 20s, acted on only when the server is genuinely down.
    if (now - lastListen >= 20000UL) {
      lastListen = now;
      webEnsureListening();
    }

    // Canvas stream channel (v3.2): drain and execute whatever records have arrived.
    canvasStreamPump();

    // Deferred companion-settings flush (v0.3.1): write the pending blob to FATFS only
    // once saves go quiet, so the flash write's scan-out glitch stays off app switches.
    companionSettingsFlushMaybe();

    // SSE live preview: when the wall changes, push the display state to every open
    // /api/events stream -- at most ~7 events/s so a flip cascade streams as motion
    // without flooding the sockets. The hash reads the reels under vmMutex; a miss
    // (busy lock) just retries next tick.
    if (sseClientCount() && !gOtaInProgress) {
      // Heap-graded (v3.2.0): one display event is ~6 KB of internal-heap TX pbufs per
      // client, and a long flip cascade (a full 160-module page) sustains that at 7/s
      // for seconds -- bisected as the deepest watermark driver on the 256x64 board
      // (wall pages + 2 SSE alone: -37 KB from a clean boot). Under pressure the
      // preview drops frames rather than the heap; the next healthy tick resyncs.
      if (now - lastPush >= 150 && ESP.getFreeHeap() >= 40000) {
        uint32_t h = 2166136261u;                     // FNV-1a over the visible state
        if (vmMutex && xSemaphoreTake(vmMutex, pdMS_TO_TICKS(20)) == pdTRUE) {
          for (int i = 0; i < vmCount; i++) {
            h ^= (uint32_t)(uint16_t)vmods[i].curIndex;
            h *= 16777619u;
          }
          xSemaphoreGive(vmMutex);
          // A mode flip (wall <-> canvas/effect/anim/ticker) must push an event even
          // though no reel moved: the preview switches renderer on it (v3.0.1).
          if (dispPixelsMode()) h ^= 0x9E3779B9u;
          if (h != lastHash) {
            lastHash = h;
            lastPush = now;
            sseBroadcastDisplay();
            lastKa = now;                             // a data frame is also a keepalive
          }
        }
      }
      // Status every 5 s (v3.2): the dashboard drops its 3 s /api/status poll -- less
      // connection churn, which measurably contributed to heap dips.
      static unsigned long lastStatus = 0;
      if (now - lastStatus >= 5000UL) {
        lastStatus = now;
        sseBroadcastStatus();
        lastKa = now;                                 // a status frame is also a keepalive
      }
      if (now - lastKa >= 15000UL) {
        lastKa = now;
        sseKeepalive();
      }
    }

    // The OTA 200 was queued by the httpd task; give lwIP a moment to flush it to the
    // browser, then boot the image we just flashed.
    if (gOtaRebootPending) {
      printf("[OTA] response delivered -- rebooting into the new image\n");
      vTaskDelay(pdMS_TO_TICKS(500));
      ESP.restart();
    }
    // A tick is 50 ms idle; 15 ms while a canvas stream is open (drain cadence sets
    // the stream's throughput ceiling: 64 KB budget / tick).
    vTaskDelay(pdMS_TO_TICKS(canvasStreamActive() ? 15 : 50));
  }
}

static bool          staWasUp    = false;
static unsigned long wifiRetryMs = 0;

void taskNetwork(void* pv) {
  // WiFi init done in setup() - this task only polls and reconnects.
  // Seed the retry clock from NOW, not from 0: setup() has just called WiFi.begin()
  // and the association is in flight. An unseeded wifiRetryMs makes the first
  // re-association fire at an absolute millis() of 15000 -- i.e. it interrupts the
  // very association we are waiting on. Give the stack a full interval to succeed.
  wifiRetryMs = millis();
  static bool wifiSuppressed = false;
  static unsigned long ntpRetryMs = 0;   // NTP-over-Ethernet retry clock (0 = never tried)
  while (true) {
    // Wired preferred (LCD Gateway): while Ethernet has an IP, power WiFi OFF -- the IP101 MAC is
    // the reliable native path, so the fragile WiFi-over-SDIO C6 link is not run at all (frees
    // internal RAM + CPU, removes the transport-wedge failure mode). Restored the moment Ethernet
    // drops. WiFi ops stay on this task; the ETH event just flips gEthUp.
    if (gEthUp) {
      if (!wifiSuppressed) {
        wifiSuppressed = true; staWasUp = false;
        if (gApActive) wifiSetApActive(false);
        WiFi.disconnect(true);
        WiFi.mode(WIFI_OFF);
        printf("[WiFi] Ethernet up -- WiFi disabled (wired link preferred)\n");
      }
      // NTP over Ethernet: the WiFi-connect path below (which normally kicks the sync) never
      // runs while wired, so drive it here -- immediately on ETH up, then retried every 60 s
      // until it lands. Without this the clock stays 00:00:00 on a wired-only board.
      if (!ntpSynced && (!ntpRetryMs || millis() - ntpRetryMs > 60000UL)) {
        ntpRetryMs = millis();
        ntpSynced = rtcNTPSync();
      }
      wdgNetMs = millis();
      vTaskDelay(pdMS_TO_TICKS(250));
      continue;
    }
    if (wifiSuppressed) {                       // Ethernet went away -> WiFi back as the fallback
      wifiSuppressed = false;
      if (strlen(cfg.wifiSSID)) {
        WiFi.mode(WIFI_STA); WiFi.setSleep(false);
        WiFi.begin(cfg.wifiSSID, cfg.wifiPass);
        staDownSince = millis(); wifiRetryMs = millis();
        printf("[WiFi] Ethernet down -- WiFi re-enabled\n");
      } else {
        wifiSetApActive(true);
      }
    }
    bool staUp = (WiFi.status() == WL_CONNECTED);
    if (staUp && !staWasUp) {
      staWasUp = true;
      wifiSetApActive(false);   // station is up -> drop the fallback AP
      if (!ntpSynced) ntpSynced = rtcNTPSync();
      { IPAddress _a = WiFi.localIP();
  printf("[WiFi] Connected IP=%d.%d.%d.%d\n", _a[0],_a[1],_a[2],_a[3]); }
    } else if (!staUp && staWasUp) {
      staWasUp = false;
      staDownSince = millis();
      printf("[WiFi] Disconnected\n");
    }
    // Fallback AP: if the station has been down for a grace period (and a
    // network is configured), bring the AP up so the gateway stays reachable.
    // If no network is configured the AP was already raised at boot. Skipped
    // during OTA (the AP is intentionally down to free RAM for the upload).
    if (!staUp && !gApActive && !gOtaInProgress && strlen(cfg.wifiSSID) &&
        staDownSince && millis() - staDownSince > 20000UL) {
      printf("[WiFi] Station down 20s -- raising fallback AP\n");
      wifiSetApActive(true);
    }
    // Manual re-association, as a backstop only. The esp_wifi stack already retries
    // on its own, and WiFi.disconnect() here ABORTS whatever it is doing -- so this
    // must be slow enough that a merely-slow association is never interrupted. It was
    // 15 s from an unseeded (0) wifiRetryMs, which meant the very first attempt fired
    // at millis()==15000 on every boot; an AP that took >10 s to associate got torn
    // down mid-handshake ("Reason: 8 - ASSOC_LEAVE"), re-begun, and could livelock
    // there. 30 s, seeded at task start, leaves the stack alone unless it is truly stuck.
    if (!staUp && strlen(cfg.wifiSSID) && millis() - wifiRetryMs > 30000UL) {
      printf("[WiFi] no association after 30s -- re-associating\n");
      wifiRetryMs = millis();
      WiFi.disconnect();
      WiFi.begin(cfg.wifiSSID, cfg.wifiPass);
    }
    // Persist the companion URL only once it has stopped moving. See the note on
    // gCompanionUrlDirty in common.h: an unconditional save turned two co-resident
    // companions into an NVS write every heartbeat, forever.
    if (gCompanionUrlDirty && millis() - gCompanionUrlDirtyMs > COMPANION_SAVE_DEBOUNCE_MS) {
      gCompanionUrlDirty = false;
      saveConfig();
      DBG("[CFG] companion URL settled -- persisted: %s\n", cfg.companionUrl);
    }

    wdgNetMs = millis();
    vTaskDelay(pdMS_TO_TICKS(100));
  }
}
