#pragma once

#include <stdint.h>

// ESP-NOW payload shared by every MiniMeters node. Both boards run this
// firmware and join the same Wi-Fi AP, so they are already on one channel.

static const uint8_t kMeshMagic = 0xA5;
static const uint8_t kMeshVer = 4;

enum MeshType : uint8_t {
  MESH_HELLO = 1,
  MESH_CONFIG = 2,     // LED + theme, every node
  MESH_CLAIM = 3,      // sender is the conductor
  MESH_VIZ = 4,
  MESH_SET_MODE = 5,   // unicast: set this node's visualizer
  MESH_SET_ROLE = 6,   // unicast: set this node's role
  MESH_SET_INPUT = 7,  // unicast: set this node's audio input
};

enum NodeRole : uint8_t {
  ROLE_NODE = 0,
  ROLE_CONDUCTOR = 1,
};

enum VizMode : uint8_t {
  MODE_SPECTRUM = 0,
  MODE_STEREOGRAM = 1,
  MODE_SPECTROGRAM = 2,
  MODE_WAVEFORM = 3,
  MODE_OSCILLOSCOPE = 4,
  MODE_OFF = 5,
  MODE_COUNT = 6,
};

enum NodeCaps : uint8_t {
  CAP_DISPLAY = 1 << 0,
  CAP_USB_AUDIO = 1 << 1,
};

enum AudioInput : uint8_t {
  INPUT_AUTO = 0,  // USB while the host is playing, otherwise UDP
  INPUT_UDP = 1,
  INPUT_USB = 2,
};

static const int kSpecBands = 32;
static const int kSpectroBins = 64;
static const int kWavePoints = 48;
static const int kStereoPoints = 24;

static const uint16_t kAudioUdpPort = 42100;
static const uint32_t kAudioMagic = 0x31414D4D;  // 'MMA1' little-endian
static const uint32_t kAudioRate = 16000;

struct VizFrame {
  uint32_t seq;
  uint8_t live;
  uint8_t spectrum[kSpecBands];
  uint8_t spectro[kSpectroBins];
  int8_t wave[kWavePoints];
  int8_t xyL[kStereoPoints];
  int8_t xyR[kStereoPoints];
  uint8_t lufsM;
  uint8_t lufsS;
  uint8_t lufsI;
};

#pragma pack(push, 1)

struct MeshHello {
  uint8_t magic;
  uint8_t ver;
  uint8_t type;
  uint8_t role;
  uint8_t mode;
  uint8_t led_r;
  uint8_t led_g;
  uint8_t led_b;
  uint8_t led_bri;
  uint8_t theme;
  uint32_t rev;
  uint8_t ip[4];
  char name[16];
  uint8_t caps;
  uint8_t input;
};

struct MeshConfig {
  uint8_t magic;
  uint8_t ver;
  uint8_t type;
  uint8_t led_r;
  uint8_t led_g;
  uint8_t led_b;
  uint8_t led_bri;
  uint8_t theme;
  uint32_t rev;
};

struct MeshSetMode {
  uint8_t magic;
  uint8_t ver;
  uint8_t type;
  uint8_t mode;
};

struct MeshSetRole {
  uint8_t magic;
  uint8_t ver;
  uint8_t type;
  uint8_t role;
};

struct MeshSetInput {
  uint8_t magic;
  uint8_t ver;
  uint8_t type;
  uint8_t input;
};

struct MeshClaim {
  uint8_t magic;
  uint8_t ver;
  uint8_t type;
  uint8_t role;
};

struct MeshViz {
  uint8_t magic;
  uint8_t ver;
  uint8_t type;
  uint8_t live;
  uint32_t seq;
  uint8_t spectrum[kSpecBands];
  uint8_t spectro[kSpectroBins];
  int8_t wave[kWavePoints];
  int8_t xyL[kStereoPoints];
  int8_t xyR[kStereoPoints];
  uint8_t lufsM;
  uint8_t lufsS;
  uint8_t lufsI;
};

#pragma pack(pop)
