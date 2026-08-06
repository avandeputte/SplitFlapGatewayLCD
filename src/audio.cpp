#include "gateway.h"
#include "audio.h"
#include "effects.h"

#include <Wire.h>
#include <driver/i2s_std.h>
#include <math.h>

// audio.cpp -- see audio.h. Mic capture + the DSP that feeds the audio-reactive
// effects. On this board the mic is the ES8311 codec's own ADC (sound.cpp configures
// the chip); the DSP (FFT, bands, beat, scope) is board-independent.

/* ---- board wiring ---- */
// Waveshare P4 board wiring (BSP-confirmed): one duplex I2S port serves the ES8311,
// DOUT feeds its DAC (speaker), DIN returns its ADC (the onboard SMD mic).
#define AUD_PIN_MCLK  GPIO_NUM_13
#define AUD_PIN_BCLK  GPIO_NUM_12
#define AUD_PIN_WS    GPIO_NUM_10
#define AUD_PIN_DIN   GPIO_NUM_11
#define AUD_PIN_DOUT  GPIO_NUM_9

#define AUD_RATE      16000
#define AUD_HOP       128          // samples per DSP hop (also the FFT size): 8 ms

static bool gAudioPresent   = false;
static volatile bool gAudioRun = false;   // capture task alive
static i2s_chan_handle_t rxChan = NULL;
static i2s_chan_handle_t txChan = NULL;   // speaker path (sound.cpp) -- shared clocks

// The board wires one clock set to the ES8311, so the mic (its ADC -> our DIN) and
// the speaker (its DAC <- our DOUT) are one full-duplex I2S port: both channels are
// allocated TOGETHER on first need and then never deleted --
// enable/disable per side only. (Deleting one side and re-creating it later fights
// the shared clock; the idle cost is ~6 KB of DMA buffers, cheap post-v3.3.)
bool audioAcquireI2S() {
  if (rxChan && txChan) return true;
  i2s_chan_config_t chanCfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
  chanCfg.dma_desc_num  = 4;
  chanCfg.dma_frame_num = AUD_HOP;
  if (i2s_new_channel(&chanCfg, &txChan, &rxChan) != ESP_OK) {
    printf("[AUDIO] i2s_new_channel (duplex) failed\n");
    txChan = rxChan = NULL;
    return false;
  }
  i2s_std_config_t std = {
    .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(AUD_RATE),      // MCLK = 256 x fs = 4.096 MHz
    .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                    I2S_SLOT_MODE_STEREO),
    .gpio_cfg = {
      .mclk = AUD_PIN_MCLK,
      .bclk = AUD_PIN_BCLK,
      .ws   = AUD_PIN_WS,
      .dout = AUD_PIN_DOUT,
      .din  = AUD_PIN_DIN,
      .invert_flags = { .mclk_inv = false, .bclk_inv = false, .ws_inv = false },
    },
  };
  if (i2s_channel_init_std_mode(rxChan, &std) != ESP_OK ||
      i2s_channel_init_std_mode(txChan, &std) != ESP_OK) {
    printf("[AUDIO] i2s duplex init failed\n");
    // Tear down fully: leaving non-null handles makes every later acquire
    // early-return "success" on channels that were never initialised, and
    // i2s reads/writes on those fail INSTANTLY -- turning the audio/synth
    // task loops into core-0 busy spins (TASK_WDT).
    i2s_del_channel(rxChan); i2s_del_channel(txChan);
    rxChan = txChan = NULL;
    return false;
  }
  // EMPIRICAL DUPLEX FACT (found the hard way, 2026-07-27): the TX side only clocks
  // while RX is enabled -- a tone played with the mic channel down produced enable=OK
  // yet every write timed out (0x107) and silence; the identical tone with RX up
  // played. So RX stays enabled for the life of the port as the clock heartbeat.
  // This does NOT run the microphone pipeline: capture (read + DSP) happens only in
  // audioTask, which starts and stops with its consumers as before; an unread RX DMA
  // ring just overruns harmlessly.
  if (i2s_channel_enable(rxChan) != ESP_OK) {
    printf("[AUDIO] rx enable failed\n");
    i2s_del_channel(rxChan); i2s_del_channel(txChan);
    rxChan = txChan = NULL;
    return false;
  }
  return true;
}
i2s_chan_handle_t audioTxChan() { return txChan; }

// Published features. A spinlock keeps reads untorn; the DSP writes ~60/s.
static AudioFrame     gFrame = {};
static portMUX_TYPE   gFrameMux = portMUX_INITIALIZER_UNLOCKED;
// Oscilloscope tap (v3.10): the DC-removed mono hop, auto-gain-scaled to ±127, published
// each hop under gFrameMux alongside gFrame. Separate from AudioFrame so the hot per-frame
// audioRead() copy (spectrum/soundwall) stays lean -- only the scope effect pays for it.
static int8_t         gScope[AUDIO_SCOPE] = {0};
static volatile bool  gBeatLatch = false;

bool audioAvailable() { return gAudioPresent; }
bool audioCapturing() { return gAudioRun; }

void audioRead(AudioFrame& out) {
  taskENTER_CRITICAL(&gFrameMux);
  out = gFrame;
  out.beat = gBeatLatch;
  gBeatLatch = false;                     // beat is consume-once per reader cycle
  taskEXIT_CRITICAL(&gFrameMux);
}






void audioReadScope(int8_t* out, int n) {
  if (n > AUDIO_SCOPE) n = AUDIO_SCOPE;
  taskENTER_CRITICAL(&gFrameMux);
  for (int i = 0; i < n; i++) out[i] = gScope[i];
  taskEXIT_CRITICAL(&gFrameMux);
}


void audioInit() {
  // The ES8311 is a COMBINED codec: sound.cpp drives its DAC (speaker), and its ADC
  // captures the board's single onboard mic onto the same duplex I2S port (our DIN).
  // There is no separate ES7210 here -- just confirm the codec answers on I2C; the
  // register config (DAC and ADC both) is done by sound.cpp's es8311Configure() right
  // after this, in the same single-threaded boot window. Wire is already begun.
  Wire.beginTransmission(0x18);
  gAudioPresent = (Wire.endTransmission(true) == 0);
  printf("[AUDIO] ES8311 mic %s (codec ADC, 16 kHz)\n",
         gAudioPresent ? "available" : "NOT FOUND -- audio effects disabled");
}

/* ---- DSP: 128-point real FFT via iterative radix-2, precomputed tables ---- */
static float fftRe[AUD_HOP], fftIm[AUD_HOP];
static float hann[AUD_HOP];
static float twidRe[AUD_HOP / 2], twidIm[AUD_HOP / 2];
static uint8_t bitrev[AUD_HOP];
static bool  tablesReady = false;

static void fftTables() {
  for (int i = 0; i < AUD_HOP; i++)
    hann[i] = 0.5f * (1.0f - cosf(2.0f * (float)M_PI * i / (AUD_HOP - 1)));
  for (int i = 0; i < AUD_HOP / 2; i++) {
    twidRe[i] = cosf(-2.0f * (float)M_PI * i / AUD_HOP);
    twidIm[i] = sinf(-2.0f * (float)M_PI * i / AUD_HOP);
  }
  for (int i = 0; i < AUD_HOP; i++) {
    uint8_t r = 0;
    for (int b = 0; b < 7; b++) if (i & (1 << b)) r |= 1 << (6 - b);   // 128 = 2^7
    bitrev[i] = r;
  }
  tablesReady = true;
}

static void fft128() {
  for (int i = 0; i < AUD_HOP; i++) {
    if (bitrev[i] > i) {
      float t = fftRe[i]; fftRe[i] = fftRe[bitrev[i]]; fftRe[bitrev[i]] = t;
      t = fftIm[i]; fftIm[i] = fftIm[bitrev[i]]; fftIm[bitrev[i]] = t;
    }
  }
  for (int len = 2; len <= AUD_HOP; len <<= 1) {
    const int half = len >> 1, step = AUD_HOP / len;
    for (int i = 0; i < AUD_HOP; i += len) {
      for (int j = 0; j < half; j++) {
        const float wr = twidRe[j * step], wi = twidIm[j * step];
        const int a = i + j, b = a + half;
        const float tr = fftRe[b] * wr - fftIm[b] * wi;
        const float ti = fftRe[b] * wi + fftIm[b] * wr;
        fftRe[b] = fftRe[a] - tr; fftIm[b] = fftIm[a] - ti;
        fftRe[a] += tr;           fftIm[a] += ti;
      }
    }
  }
}

// Log-spaced band edges over bins 1..63 (125 Hz .. 7.9 kHz at 16 kHz/128).
static uint8_t bandEdge[AUDIO_BANDS + 1];
static void bandTables() {
  const float lo = 1.0f, hi = 63.0f;
  for (int b = 0; b <= AUDIO_BANDS; b++) {
    float e = lo * powf(hi / lo, (float)b / AUDIO_BANDS);
    bandEdge[b] = (uint8_t)(e + 0.5f);
    if (b && bandEdge[b] <= bandEdge[b - 1]) bandEdge[b] = bandEdge[b - 1] + 1;
  }
  bandEdge[AUDIO_BANDS] = 64;
}

/* ---- capture task ---- */
// Any audio consumer? The dedicated effects, or "audio":true modulating another one.
static bool audioHasConsumer() {
  if (gEffect == EFFECT_SPECTRUM || gEffect == EFFECT_SOUNDWALL ||
      gEffect == EFFECT_RIPPLE  || gEffect == EFFECT_SCOPE ||
      gEffect == EFFECT_SPECTRO) return true;
  return gEffectAudioMod && gEffect != EFFECT_NONE;
}

static void audioTask(void* pv) {
  static int16_t raw[AUD_HOP * 2];              // stereo pairs
  float dcPrev = 0, dcOut = 0;
  float envMax = 0.02f;                          // level auto-gain envelope
  float bandMax[AUDIO_BANDS];
  float bassAvg = 0.001f;
  unsigned long lastBeatMs = 0, idleSince = 0;
  for (int b = 0; b < AUDIO_BANDS; b++) bandMax[b] = 0.05f;

  while (true) {
    size_t got = 0;
    if (i2s_channel_read(rxChan, raw, sizeof(raw), &got, pdMS_TO_TICKS(100)) != ESP_OK ||
        got < sizeof(raw)) {
      // Timeout/short read: check for shutdown, then keep trying. The delay is
      // load-bearing: on a wedged channel the read fails INSTANTLY (no 100 ms
      // block), and this task is pinned to core 0 -- the one core whose idle
      // task the TWDT watches. An unpaced retry loop here is a 5 s reboot.
      if (!audioHasConsumer()) break;
      vTaskDelay(1);
      continue;
    }

    // Mono mix + DC removal (one-pole HPF) + window into the FFT buffer. The un-windowed
    // dcOut is also captured for the oscilloscope, scaled by the PREVIOUS hop's auto-gain
    // envelope so quiet rooms still fill the trace; committed to gScope below with gFrame.
    static_assert(AUD_HOP == AUDIO_SCOPE, "scope buffer must match one DSP hop");
    int8_t sc[AUDIO_SCOPE];
    const float scInv = 127.0f / (envMax * 4.0f + 0.02f);
    float rms = 0;
    for (int i = 0; i < AUD_HOP; i++) {
      const float s = ((int32_t)raw[i * 2] + raw[i * 2 + 1]) * (0.5f / 32768.0f);
      dcOut = s - dcPrev + 0.995f * dcOut;
      dcPrev = s;
      rms += dcOut * dcOut;
      float sv = dcOut * scInv;
      sc[i] = (int8_t)(sv > 127 ? 127 : sv < -127 ? -127 : sv);
      fftRe[i] = dcOut * hann[i];
      fftIm[i] = 0;
    }
    rms = sqrtf(rms / AUD_HOP);

    fft128();

    // Fold magnitudes into log bands.
    float bands[AUDIO_BANDS];
    for (int b = 0; b < AUDIO_BANDS; b++) {
      float acc = 0;
      for (int k = bandEdge[b]; k < bandEdge[b + 1]; k++)
        acc += sqrtf(fftRe[k] * fftRe[k] + fftIm[k] * fftIm[k]);
      bands[b] = acc / (bandEdge[b + 1] - bandEdge[b]);
    }

    // Beat: bass energy vs its ~1 s average (60 hops/s -> 0.984 decay), refractory.
    const float bass = bands[0] + bands[1];
    const bool beat = (bass > 1.7f * bassAvg) && (bass > 0.01f) &&
                      (millis() - lastBeatMs > 150);
    if (beat) lastBeatMs = millis();
    bassAvg = 0.984f * bassAvg + 0.016f * bass;

    // Slow auto-gain: envelopes rise fast, decay slow, so quiet rooms still visualise.
    if (rms > envMax) envMax = rms; else envMax *= 0.9995f;
    if (envMax < 0.004f) envMax = 0.004f;                  // noise floor guard
    AudioFrame f;
    f.level = rms / envMax; if (f.level > 1) f.level = 1;
    for (int b = 0; b < AUDIO_BANDS; b++) {
      if (bands[b] > bandMax[b]) bandMax[b] = bands[b]; else bandMax[b] *= 0.9995f;
      if (bandMax[b] < 0.01f) bandMax[b] = 0.01f;
      f.bands[b] = bands[b] / bandMax[b]; if (f.bands[b] > 1) f.bands[b] = 1;
    }
    f.bassRaw = bass;
    f.seq = gFrame.seq + 1;
    f.peak = (f.level > gFrame.peak) ? f.level : gFrame.peak * 0.985f;
    f.beat = false;                                        // latch carries the flag

    taskENTER_CRITICAL(&gFrameMux);
    const bool latch = gBeatLatch || beat;
    gFrame = f;
    gBeatLatch = latch;
    for (int i = 0; i < AUDIO_SCOPE; i++) gScope[i] = sc[i];
    taskEXIT_CRITICAL(&gFrameMux);

    // Self-stop: no consumer for 3 s ends capture entirely (mic data stops flowing).
    if (audioHasConsumer()) idleSince = 0;
    else if (!idleSince)    idleSince = millis();
    else if (millis() - idleSince > 3000) break;
  }

  // Teardown: the task ends but RX stays ENABLED -- it is the duplex clock heartbeat
  // (see audioAcquireI2S); only the reading/DSP stops here.
  taskENTER_CRITICAL(&gFrameMux);
  gFrame = AudioFrame{};
  for (int i = 0; i < AUDIO_SCOPE; i++) gScope[i] = 0;
  gBeatLatch = false;
  taskEXIT_CRITICAL(&gFrameMux);
  gAudioRun = false;
  // Lost-start race (audit): a consumer arriving between the exit decision and the
  // flag clear saw gAudioRun==true and spawned nothing. Now the flag is down,
  // re-check -- if someone is waiting, hand off to a fresh task before dying.
  if (audioHasConsumer()) audioMaybeStart();
  printf("[AUDIO] capture stopped\n");
  vTaskDelete(NULL);
}

void audioMaybeStart() {
  if (!gAudioPresent || gAudioRun) return;
  if (!tablesReady) { fftTables(); bandTables(); }
  if (!audioAcquireI2S()) return;         // RX is already enabled (the clock heartbeat)
  gAudioRun = true;
  // Core 0 with the other non-render work; ~60 hops/s of float FFT is a light load.
  xTaskCreatePinnedToCore(audioTask, "audio", 4096, NULL, 2, NULL, 0);
  printf("[AUDIO] capture started (16 kHz stereo, %d-band FFT)\n", AUDIO_BANDS);
}
