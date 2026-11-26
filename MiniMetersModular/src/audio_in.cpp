#include "audio_in.h"

#include <WiFi.h>
#include <WiFiUdp.h>
#include <math.h>
#include <string.h>

// Conductor ingest: 16 kHz PCM over UDP, then a 256-point FFT and a
// K-weighted loudness estimate. Nodes never bind the port.

static const int kRing = 512;
static const int kFftN = 256;
static const int kLoudBlock = 1600;  // 100 ms at 16 kHz
static const int kLoudBlocks = 30;   // 3 s short-term window
static const int kGate = 40;         // abs int16; below this the spectrum is silence

struct Biquad {
  float b0, b1, b2, a1, a2;
  float z1, z2;

  float process(float x) {
    const float y = b0 * x + z1;
    z1 = b1 * x - a1 * y + z2;
    z2 = b2 * x - a2 * y;
    return y;
  }

  void reset() {
    z1 = 0;
    z2 = 0;
  }
};

struct AudioHdr {
  uint32_t magic;
  uint32_t seq;
  uint16_t rate;
  uint16_t channels;
  uint16_t count;
  uint16_t reserved;
} __attribute__((packed));

static WiFiUDP g_udp;
static bool g_udpOn = false;
static bool g_conductor = false;
static uint8_t g_mode = MODE_SPECTRUM;

static int16_t g_mono[kRing];
static int16_t g_left[kRing];
static int16_t g_right[kRing];
static int g_ringPos = 0;
static int g_ringCount = 0;

static Biquad g_hp;
static Biquad g_shelf;
static double g_acc = 0;
static int g_accN = 0;
static float g_blocks[kLoudBlocks];
static int g_blockPos = 0;
static int g_blockFill = 0;
static float g_iMs = 0;
static bool g_iInit = false;

static float g_hann[kFftN];
static bool g_hannReady = false;
static float g_re[kFftN];
static float g_im[kFftN];
static float g_mag[kFftN / 2];
static float g_agcPeak = 1e-4f;
static float g_bandSmooth[kSpecBands];

static VizFrame g_out;
static uint32_t g_takenSeq = 0;
static bool g_have = false;
static uint32_t g_seq = 0;
static uint32_t g_lastStep = 0;
static uint32_t g_lastUdpMs = 0;
static uint32_t g_packets = 0;
static bool g_wasLive = false;
static uint32_t g_silenceSince = 0;
static bool g_rateWarned = false;

static int16_t ringAt(const int16_t *ring, int ageFromEnd) {
  int idx = g_ringPos - ageFromEnd;
  idx %= kRing;
  if (idx < 0) {
    idx += kRing;
  }
  return ring[idx];
}

static Biquad makeHighPass(float fs, float f0, float q) {
  const float w0 = 2.0f * PI * f0 / fs;
  const float c = cosf(w0);
  const float s = sinf(w0);
  const float alpha = s / (2.0f * q);
  const float a0 = 1.0f + alpha;
  Biquad b{};
  b.b0 = ((1.0f + c) * 0.5f) / a0;
  b.b1 = (-(1.0f + c)) / a0;
  b.b2 = ((1.0f + c) * 0.5f) / a0;
  b.a1 = (-2.0f * c) / a0;
  b.a2 = (1.0f - alpha) / a0;
  return b;
}

static Biquad makeHighShelf(float fs, float f0, float dbGain) {
  const float A = powf(10.0f, dbGain / 40.0f);
  const float w0 = 2.0f * PI * f0 / fs;
  const float c = cosf(w0);
  const float s = sinf(w0);
  const float alpha = s * 0.5f * sqrtf((A + 1.0f / A) * 0.0f + 2.0f);
  const float sqrtA = sqrtf(A);
  const float a0 = (A + 1.0f) - (A - 1.0f) * c + 2.0f * sqrtA * alpha;
  Biquad b{};
  b.b0 = (A * ((A + 1.0f) + (A - 1.0f) * c + 2.0f * sqrtA * alpha)) / a0;
  b.b1 = (-2.0f * A * ((A - 1.0f) + (A + 1.0f) * c)) / a0;
  b.b2 = (A * ((A + 1.0f) + (A - 1.0f) * c - 2.0f * sqrtA * alpha)) / a0;
  b.a1 = (2.0f * ((A - 1.0f) - (A + 1.0f) * c)) / a0;
  b.a2 = ((A + 1.0f) - (A - 1.0f) * c - 2.0f * sqrtA * alpha) / a0;
  return b;
}

static void fft256(float *re, float *im) {
  const int n = kFftN;
  for (int i = 1, j = 0; i < n; ++i) {
    int bit = n >> 1;
    for (; j & bit; bit >>= 1) {
      j ^= bit;
    }
    j ^= bit;
    if (i < j) {
      const float tr = re[i];
      re[i] = re[j];
      re[j] = tr;
      const float ti = im[i];
      im[i] = im[j];
      im[j] = ti;
    }
  }
  for (int len = 2; len <= n; len <<= 1) {
    const float ang = 2.0f * PI / (float)len;
    const float wlenRe = cosf(ang);
    const float wlenIm = sinf(ang);
    for (int i = 0; i < n; i += len) {
      float wRe = 1.0f;
      float wIm = 0.0f;
      for (int k = 0; k < len / 2; ++k) {
        const int u = i + k;
        const int v = i + k + len / 2;
        const float tr = wRe * re[v] - wIm * im[v];
        const float ti = wRe * im[v] + wIm * re[v];
        re[v] = re[u] - tr;
        im[v] = im[u] - ti;
        re[u] += tr;
        im[u] += ti;
        const float nwRe = wRe * wlenRe - wIm * wlenIm;
        wIm = wRe * wlenIm + wIm * wlenRe;
        wRe = nwRe;
      }
    }
  }
}

static float lufsFromMs(float ms) {
  if (ms < 1e-12f) {
    return -120.0f;
  }
  return -0.691f + 10.0f * log10f(ms);
}

static uint8_t lufsToBar(float lufs) {
  float n = (lufs + 60.0f) / 60.0f;
  if (n < 0.0f) {
    n = 0.0f;
  }
  if (n > 1.0f) {
    n = 1.0f;
  }
  return (uint8_t)(n * 255.0f + 0.5f);
}

static int8_t toI8(int sample) {
  int v = sample / 256;
  if (v > 127) {
    v = 127;
  }
  if (v < -127) {
    v = -127;
  }
  return (int8_t)v;
}

static float avgBlocks(int n) {
  if (g_blockFill <= 0) {
    return 0.0f;
  }
  if (n > g_blockFill) {
    n = g_blockFill;
  }
  double sum = 0;
  for (int i = 0; i < n; ++i) {
    int idx = (g_blockPos - 1 - i) % kLoudBlocks;
    if (idx < 0) {
      idx += kLoudBlocks;
    }
    sum += g_blocks[idx];
  }
  return (float)(sum / n);
}

static void pushBlock(float ms) {
  g_blocks[g_blockPos] = ms;
  g_blockPos = (g_blockPos + 1) % kLoudBlocks;
  if (g_blockFill < kLoudBlocks) {
    ++g_blockFill;
  }
  const float lufs = lufsFromMs(ms);
  if (lufs > -70.0f) {
    if (!g_iInit) {
      g_iMs = ms;
      g_iInit = true;
    } else {
      g_iMs += (ms - g_iMs) * 0.01f;
    }
  }
}

static void feedSample(int16_t left, int16_t right) {
  const int32_t mono = ((int32_t)left + (int32_t)right) / 2;
  g_left[g_ringPos] = left;
  g_right[g_ringPos] = right;
  g_mono[g_ringPos] = (int16_t)mono;
  g_ringPos = (g_ringPos + 1) % kRing;
  if (g_ringCount < kRing) {
    ++g_ringCount;
  }

  float x = (float)mono / 32768.0f;
  x = g_hp.process(x);
  x = g_shelf.process(x);
  g_acc += (double)x * (double)x;
  ++g_accN;
  if (g_accN >= kLoudBlock) {
    pushBlock((float)(g_acc / (double)g_accN));
    g_acc = 0;
    g_accN = 0;
  }
}

static void logBins(uint8_t *out, int nOut, bool smooth) {
  const float fMin = 1.0f;
  const float fMax = (float)(kFftN / 2 - 1);
  for (int b = 0; b < nOut; ++b) {
    const float t0 = (float)b / (float)nOut;
    const float t1 = (float)(b + 1) / (float)nOut;
    int i0 = (int)(fMin * powf(fMax / fMin, t0) + 0.5f);
    int i1 = (int)(fMin * powf(fMax / fMin, t1) + 0.5f);
    if (i0 < 1) {
      i0 = 1;
    }
    if (i1 <= i0) {
      i1 = i0 + 1;
    }
    if (i1 > kFftN / 2) {
      i1 = kFftN / 2;
    }
    float m = 0.0f;
    for (int i = i0; i < i1; ++i) {
      if (g_mag[i] > m) {
        m = g_mag[i];
      }
    }
    float n = 0.0f;
    if (g_agcPeak > 1e-8f) {
      n = m / g_agcPeak;
    }
    if (n > 1.0f) {
      n = 1.0f;
    }
    n = powf(n, 0.55f);
    float byte = n * 255.0f;
    if (smooth) {
      g_bandSmooth[b] = g_bandSmooth[b] * 0.35f + byte * 0.65f;
      byte = g_bandSmooth[b];
    }
    if (byte < 0.0f) {
      byte = 0.0f;
    }
    if (byte > 255.0f) {
      byte = 255.0f;
    }
    out[b] = (uint8_t)(byte + 0.5f);
  }
}

static void fillWave(VizFrame *frame, const int16_t *window, int peakAbs) {
  if (peakAbs < kGate) {
    memset(frame->wave, 0, sizeof(frame->wave));
    return;
  }
  int start = 0;
  int span = kFftN;
  if (g_mode == MODE_OSCILLOSCOPE) {
    int trig = 0;
    for (int i = 1; i < kFftN; ++i) {
      if (window[i - 1] < 0 && window[i] >= 0) {
        trig = i;
        break;
      }
    }
    int trig2 = -1;
    for (int i = trig + 4; i < kFftN; ++i) {
      if (window[i - 1] < 0 && window[i] >= 0) {
        trig2 = i;
        break;
      }
    }
    start = trig;
    if (trig2 > trig) {
      span = trig2 - trig;
      if (start + span * 2 <= kFftN) {
        span *= 2;
      }
    } else {
      span = kFftN - trig;
    }
    if (span < 8) {
      span = 8;
    }
  }
  for (int i = 0; i < kWavePoints; ++i) {
    int src = start + (i * span) / kWavePoints;
    if (src >= kFftN) {
      src = kFftN - 1;
    }
    frame->wave[i] = toI8(window[src]);
  }
}

static void analyze(VizFrame *frame) {
  int16_t window[kFftN];
  int16_t left[kFftN];
  int16_t right[kFftN];
  int peakAbs = 0;
  for (int i = 0; i < kFftN; ++i) {
    const int age = kFftN - i;
    window[i] = ringAt(g_mono, age);
    left[i] = ringAt(g_left, age);
    right[i] = ringAt(g_right, age);
    int a = window[i] < 0 ? -(int)window[i] : (int)window[i];
    if (a > peakAbs) {
      peakAbs = a;
    }
  }

  if (!g_hannReady) {
    for (int i = 0; i < kFftN; ++i) {
      g_hann[i] = 0.5f * (1.0f - cosf(2.0f * PI * (float)i / (float)(kFftN - 1)));
    }
    g_hannReady = true;
  }

  memset(frame->spectrum, 0, sizeof(frame->spectrum));
  memset(frame->spectro, 0, sizeof(frame->spectro));

  if (peakAbs < kGate) {
    memset(g_bandSmooth, 0, sizeof(g_bandSmooth));
  }

  if (peakAbs >= kGate) {
    for (int i = 0; i < kFftN; ++i) {
      g_re[i] = ((float)window[i] / 32768.0f) * g_hann[i];
      g_im[i] = 0.0f;
    }
    fft256(g_re, g_im);
    float framePeak = 0.0f;
    for (int i = 0; i < kFftN / 2; ++i) {
      g_mag[i] = sqrtf(g_re[i] * g_re[i] + g_im[i] * g_im[i]);
      if (i > 0 && g_mag[i] > framePeak) {
        framePeak = g_mag[i];
      }
    }
    if (framePeak > g_agcPeak) {
      g_agcPeak = framePeak;
    } else {
      g_agcPeak *= 0.995f;
    }
    if (g_agcPeak < 1e-4f) {
      g_agcPeak = 1e-4f;
    }
    logBins(frame->spectrum, kSpecBands, true);
    logBins(frame->spectro, kSpectroBins, false);
  } else if (g_agcPeak > 1e-4f) {
    g_agcPeak *= 0.995f;
  }

  fillWave(frame, window, peakAbs);

  for (int i = 0; i < kStereoPoints; ++i) {
    const int src = (i * kFftN) / kStereoPoints;
    if (peakAbs < kGate) {
      frame->xyL[i] = 0;
      frame->xyR[i] = 0;
    } else {
      frame->xyL[i] = toI8(left[src]);
      frame->xyR[i] = toI8(right[src]);
    }
  }

  frame->lufsM = lufsToBar(lufsFromMs(avgBlocks(4)));
  frame->lufsS = lufsToBar(lufsFromMs(avgBlocks(kLoudBlocks)));
  frame->lufsI = g_iInit ? lufsToBar(lufsFromMs(g_iMs)) : 0;
}

static void publish(const VizFrame &frame) {
  g_out = frame;
  g_out.seq = ++g_seq;
  g_have = true;
}

static void resetAnalysis() {
  g_ringPos = 0;
  g_ringCount = 0;
  g_acc = 0;
  g_accN = 0;
  g_blockPos = 0;
  g_blockFill = 0;
  g_iMs = 0;
  g_iInit = false;
  g_agcPeak = 1e-4f;
  g_wasLive = false;
  g_silenceSince = 0;
  g_lastUdpMs = 0;
  g_hp.reset();
  g_shelf.reset();
  memset(g_bandSmooth, 0, sizeof(g_bandSmooth));
  memset(g_blocks, 0, sizeof(g_blocks));
  g_have = false;
}

static void drainUdp() {
  uint8_t buf[1200];
  for (;;) {
    int len = g_udp.parsePacket();
    if (len <= 0) {
      return;
    }
    if (len > (int)sizeof(buf)) {
      while (g_udp.available()) {
        g_udp.read(buf, sizeof(buf));
      }
      continue;
    }
    const int n = g_udp.read(buf, len);
    if (n < (int)sizeof(AudioHdr)) {
      continue;
    }
    AudioHdr hdr;
    memcpy(&hdr, buf, sizeof(hdr));
    if (hdr.magic != kAudioMagic) {
      continue;
    }
    if (hdr.rate != kAudioRate) {
      if (!g_rateWarned) {
        Serial.printf("audio drop rate=%u (want %u)\n", hdr.rate, kAudioRate);
        g_rateWarned = true;
      }
      continue;
    }
    if (hdr.channels < 1 || hdr.channels > 2 || hdr.count == 0 || hdr.count > 512) {
      continue;
    }
    const int need = (int)sizeof(AudioHdr) + (int)hdr.count * (int)hdr.channels * 2;
    if (n < need) {
      continue;
    }
    const uint8_t *p = buf + sizeof(AudioHdr);
    for (uint16_t i = 0; i < hdr.count; ++i) {
      int16_t left = 0;
      int16_t right = 0;
      memcpy(&left, p, 2);
      p += 2;
      if (hdr.channels == 2) {
        memcpy(&right, p, 2);
        p += 2;
      } else {
        right = left;
      }
      feedSample(left, right);
    }
    g_lastUdpMs = millis();
    ++g_packets;
  }
}

static void step(uint32_t now) {
  const bool udpRecent = g_lastUdpMs != 0 && (now - g_lastUdpMs) < 500;
  if (udpRecent && g_ringCount >= kFftN) {
    VizFrame frame;
    memset(&frame, 0, sizeof(frame));
    analyze(&frame);
    frame.live = 1;
    publish(frame);
    g_wasLive = true;
    g_silenceSince = 0;
    return;
  }
  if (!g_wasLive) {
    return;
  }
  if (g_silenceSince == 0) {
    g_silenceSince = now;
  }
  for (int i = 0; i < 800; ++i) {
    feedSample(0, 0);
  }
  if (g_ringCount < kFftN) {
    return;
  }
  VizFrame frame;
  memset(&frame, 0, sizeof(frame));
  analyze(&frame);
  frame.live = 0;
  publish(frame);
  if ((frame.lufsM == 0 && frame.lufsS == 0) || (now - g_silenceSince) > 4000) {
    g_wasLive = false;
    g_silenceSince = 0;
  }
}

void audioSetVizMode(uint8_t mode) {
  g_mode = mode;
}

void audioSetConductor(bool conductor) {
  if (conductor == g_conductor && conductor == g_udpOn) {
    return;
  }
  g_conductor = conductor;
  if (conductor) {
    resetAnalysis();
    if (!g_udpOn) {
      if (g_udp.begin(kAudioUdpPort)) {
        g_udpOn = true;
        Serial.printf("audio listening udp/%u\n", kAudioUdpPort);
      } else {
        Serial.println("audio udp bind failed");
      }
    }
  } else if (g_udpOn) {
    g_udp.stop();
    g_udpOn = false;
    g_have = false;
  }
}

void audioBegin(bool conductor) {
  g_hp = makeHighPass(16000.0f, 60.0f, 0.707f);
  g_shelf = makeHighShelf(16000.0f, 1500.0f, 4.0f);
  audioSetConductor(conductor);
}

void audioPoll(uint32_t now) {
  if (!g_conductor) {
    return;
  }
  if (g_udpOn) {
    drainUdp();
  }
  if (now - g_lastStep < 50) {
    return;
  }
  g_lastStep = now;
  step(now);
}

bool audioTake(VizFrame *out) {
  if (!g_have || !out || g_out.seq == g_takenSeq) {
    return false;
  }
  *out = g_out;
  g_takenSeq = g_out.seq;
  return true;
}

uint32_t audioPackets() {
  return g_packets;
}
