// audio.h -- the microphone frontend: the ES8311 codec's ADC -> I2S -> DSP.
//
// The Waveshare P4 board carries a single onboard mic on the ES8311 codec's own ADC
// (I2C control at 0x18 on the shared 7/8 bus, audio on the duplex I2S port: MCLK=13
// BCLK=12 WS=10, mic in on DIN=11, speaker out on DOUT=9 -- the same chip and port
// drive the speaker). This module feeds the audio-reactive effects (spectrum,
// soundwall, and the "audio":true modulation of fire/matrix/plasma) with a small
// per-frame feature block; raw samples never leave the device and are never stored.
//
// Split lifecycle, deliberately:
//   * audioInit() -- called ONCE from setup(), BEFORE tasks exist: confirms the ES8311
//     answers (sound.cpp writes its registers, ADC included). rtcHwInit brings up Wire
//     with no bus lock, so all of audio's I2C happens while the system is still
//     single-threaded; after boot this module never touches I2C again.
//   * capture starts on demand (an audio effect starting) and self-stops a few
//     seconds after the last consumer leaves -- the I2S channel and its DMA buffers
//     exist only while something is actually listening.
//
// The DSP (audioTask, core 0): 16 kHz 16-bit stereo, 128-sample hops. Per hop:
// DC-removed mono mix, RMS level with slow auto-gain, a 128-point windowed FFT folded
// into AUDIO_BANDS log-spaced bands (each with its own slow normaliser), and a bass
// beat detector (energy vs its ~1 s average, 150 ms refractory).

#pragma once
#include <stdint.h>
#include <driver/i2s_std.h>   // i2s_chan_handle_t (the shared duplex port, below)

#define AUDIO_BANDS 16
#define AUDIO_SCOPE 128        // time-domain waveform samples exposed for the oscilloscope

struct AudioFrame {
  float    level;               // 0..1 overall loudness (auto-gained RMS)
  float    peak;                // 0..1 peak with ~1 s decay
  bool     beat;                // true on the hop a beat fired (latched until read)
  float    bands[AUDIO_BANDS];  // 0..1 per log-spaced band, per-band normalised
  float    bassRaw;             // un-normalised bass energy (beat/debug)
  uint32_t seq;                 // increments per DSP hop; stale if it stops moving
};

void audioInit();                    // confirm the ES8311 codec answers (setup() only)
bool audioAvailable();               // ES8311 mic ADC available
void audioMaybeStart();              // a consumer exists: ensure capture is running
void audioRead(AudioFrame& out);     // latest features (zeroed when not capturing)
void audioReadScope(int8_t* out, int n);  // latest DC-removed mono waveform, auto-gain-scaled to ±127
bool audioCapturing();               // I2S currently running (diagnostics)

// The shared full-duplex I2S port (one wired clock set serves both codecs): sound.cpp
// acquires the same pair and drives the TX side. Channels persist once created.
bool audioAcquireI2S();
i2s_chan_handle_t audioTxChan();
