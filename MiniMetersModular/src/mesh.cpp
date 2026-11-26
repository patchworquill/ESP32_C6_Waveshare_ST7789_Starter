#include "mmm_log.h"
#include "mesh.h"

#include <WiFi.h>
#include <esp_now.h>
#include <string.h>

static_assert(sizeof(MeshViz) <= 250, "viz packet exceeds ESP-NOW");
static_assert(sizeof(MeshHello) <= 250, "hello packet exceeds ESP-NOW");
static_assert(sizeof(MeshConfig) <= 250, "config packet exceeds ESP-NOW");
static_assert(sizeof(MeshSetMode) <= 250, "mode packet exceeds ESP-NOW");
static_assert(sizeof(MeshSetRole) <= 250, "role packet exceeds ESP-NOW");
static_assert(sizeof(MeshSetInput) <= 250, "input packet exceeds ESP-NOW");

static const uint8_t kBcast[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
static const uint32_t kPeerTimeoutMs = 5000;
static const int kMaxPeers = 8;

struct RxItem {
  uint8_t mac[6];
  int8_t rssi;
  uint8_t len;
  uint8_t data[250];
};

struct PeerSlot {
  bool used;
  uint8_t mac[6];
  MeshPeerInfo info;
  uint32_t lastMs;
};

struct Identity {
  char name[16];
  uint8_t role;
  uint8_t mode;
  uint8_t caps;
  uint8_t input;
  uint8_t r, g, b, bri;
  uint8_t theme;
  uint32_t rev;
};

static Identity g_id;
static PeerSlot g_peers[kMaxPeers];
static QueueHandle_t g_rxQueue = nullptr;
static MeshConfigFn g_onConfig = nullptr;
static MeshClaimFn g_onClaim = nullptr;
static MeshModeFn g_onMode = nullptr;
static MeshRoleFn g_onRole = nullptr;
static MeshInputFn g_onInput = nullptr;
static uint8_t g_directMac[6];
static uint8_t g_directData[8];
static uint8_t g_directLen = 0;
static uint8_t g_directLeft = 0;
static bool g_ok = false;
static volatile bool g_txBusy = false;
static uint32_t g_txStamp = 0;

static uint8_t g_configLeft = 0;
static uint8_t g_claimLeft = 0;
static uint32_t g_nextConfig = 0;
static uint32_t g_nextClaim = 0;
static uint32_t g_lastHello = 0;
static bool g_helloDue = false;

static VizFrame g_rxViz;
static bool g_vizPending = false;

static void onSent(const uint8_t *mac, esp_now_send_status_t status) {
  (void)mac;
  (void)status;
  g_txBusy = false;
}

static void onRecv(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
  if (!g_rxQueue || !info || !info->src_addr || !data || len <= 0) {
    return;
  }
  RxItem item;
  memset(&item, 0, sizeof(item));
  memcpy(item.mac, info->src_addr, 6);
  if (info->rx_ctrl) {
    item.rssi = (int8_t)info->rx_ctrl->rssi;
  }
  if (len > (int)sizeof(item.data)) {
    len = (int)sizeof(item.data);
  }
  memcpy(item.data, data, (size_t)len);
  item.len = (uint8_t)len;
  xQueueSend(g_rxQueue, &item, 0);
}

static bool sendPkt(const uint8_t *mac, const uint8_t *data, size_t len) {
  if (!g_ok || !mac || g_txBusy || len == 0 || len > 250) {
    return false;
  }
  g_txBusy = true;
  g_txStamp = millis();
  if (esp_now_send(mac, data, len) != ESP_OK) {
    g_txBusy = false;
    return false;
  }
  return true;
}

static bool sendRaw(const uint8_t *data, size_t len) {
  return sendPkt(kBcast, data, len);
}

static void ensurePeer(const uint8_t mac[6]) {
  if (!g_ok || esp_now_is_peer_exist(mac)) {
    return;
  }
  esp_now_peer_info_t peer;
  memset(&peer, 0, sizeof(peer));
  memcpy(peer.peer_addr, mac, 6);
  peer.channel = 0;
  peer.ifidx = WIFI_IF_STA;
  peer.encrypt = false;
  const esp_err_t add = esp_now_add_peer(&peer);
  if (add != ESP_OK && add != ESP_ERR_ESPNOW_EXIST) {
    Serial.printf("esp-now peer add failed %d\n", (int)add);
  }
}

static void notePeer(const uint8_t mac[6], int mode, int role, int input) {
  for (int i = 0; i < kMaxPeers; ++i) {
    if (!g_peers[i].used || memcmp(g_peers[i].mac, mac, 6) != 0) {
      continue;
    }
    if (mode >= 0) {
      g_peers[i].info.mode = (uint8_t)mode;
    }
    if (role >= 0) {
      g_peers[i].info.role = (uint8_t)role;
    }
    if (input >= 0) {
      g_peers[i].info.input = (uint8_t)input;
    }
  }
}

static void queueDirect(const uint8_t mac[6], const void *pkt, uint8_t len) {
  if (len > sizeof(g_directData)) {
    len = sizeof(g_directData);
  }
  memcpy(g_directMac, mac, 6);
  memcpy(g_directData, pkt, len);
  g_directLen = len;
  g_directLeft = 3;
}

static void agePeers(uint32_t now) {
  for (int i = 0; i < kMaxPeers; ++i) {
    if (!g_peers[i].used) {
      continue;
    }
    if (now - g_peers[i].lastMs > kPeerTimeoutMs) {
      Serial.printf("mesh peer lost %s\n", g_peers[i].info.name);
      g_peers[i].used = false;
    }
  }
}

static PeerSlot *findPeer(const uint8_t mac[6]) {
  for (int i = 0; i < kMaxPeers; ++i) {
    if (g_peers[i].used && memcmp(g_peers[i].mac, mac, 6) == 0) {
      return &g_peers[i];
    }
  }
  return nullptr;
}

static PeerSlot *allocPeer(const uint8_t mac[6]) {
  PeerSlot *slot = findPeer(mac);
  if (slot) {
    return slot;
  }
  for (int i = 0; i < kMaxPeers; ++i) {
    if (!g_peers[i].used) {
      memset(&g_peers[i], 0, sizeof(g_peers[i]));
      memcpy(g_peers[i].mac, mac, 6);
      g_peers[i].used = true;
      return &g_peers[i];
    }
  }
  int oldest = 0;
  for (int i = 1; i < kMaxPeers; ++i) {
    if (g_peers[i].lastMs < g_peers[oldest].lastMs) {
      oldest = i;
    }
  }
  memset(&g_peers[oldest], 0, sizeof(g_peers[oldest]));
  memcpy(g_peers[oldest].mac, mac, 6);
  g_peers[oldest].used = true;
  return &g_peers[oldest];
}

static void fillHeader(uint8_t *magic, uint8_t *ver, uint8_t *type, uint8_t kind) {
  *magic = kMeshMagic;
  *ver = kMeshVer;
  *type = kind;
}

static bool sendHello() {
  MeshHello pkt;
  memset(&pkt, 0, sizeof(pkt));
  fillHeader(&pkt.magic, &pkt.ver, &pkt.type, MESH_HELLO);
  pkt.role = g_id.role;
  pkt.mode = g_id.mode;
  pkt.caps = g_id.caps;
  pkt.input = g_id.input;
  pkt.led_r = g_id.r;
  pkt.led_g = g_id.g;
  pkt.led_b = g_id.b;
  pkt.led_bri = g_id.bri;
  pkt.theme = g_id.theme;
  pkt.rev = g_id.rev;
  IPAddress ip = WiFi.localIP();
  pkt.ip[0] = ip[0];
  pkt.ip[1] = ip[1];
  pkt.ip[2] = ip[2];
  pkt.ip[3] = ip[3];
  strncpy(pkt.name, g_id.name, sizeof(pkt.name) - 1);
  return sendRaw(reinterpret_cast<uint8_t *>(&pkt), sizeof(pkt));
}

static bool sendConfigNow() {
  MeshConfig pkt;
  memset(&pkt, 0, sizeof(pkt));
  fillHeader(&pkt.magic, &pkt.ver, &pkt.type, MESH_CONFIG);
  pkt.led_r = g_id.r;
  pkt.led_g = g_id.g;
  pkt.led_b = g_id.b;
  pkt.led_bri = g_id.bri;
  pkt.theme = g_id.theme;
  pkt.rev = g_id.rev;
  return sendRaw(reinterpret_cast<uint8_t *>(&pkt), sizeof(pkt));
}

static bool sendClaimNow() {
  MeshClaim pkt;
  memset(&pkt, 0, sizeof(pkt));
  fillHeader(&pkt.magic, &pkt.ver, &pkt.type, MESH_CLAIM);
  pkt.role = ROLE_CONDUCTOR;
  return sendRaw(reinterpret_cast<uint8_t *>(&pkt), sizeof(pkt));
}

static void handlePacket(const RxItem &item) {
  if (item.len < 3 || item.data[0] != kMeshMagic || item.data[1] != kMeshVer) {
    return;
  }
  uint8_t self[6];
  WiFi.macAddress(self);
  if (memcmp(item.mac, self, 6) == 0) {
    return;
  }

  const uint8_t type = item.data[2];
  if (type == MESH_HELLO && item.len >= sizeof(MeshHello)) {
    MeshHello hello;
    memcpy(&hello, item.data, sizeof(hello));
    hello.name[sizeof(hello.name) - 1] = 0;
    PeerSlot *slot = allocPeer(item.mac);
    const bool fresh = slot->info.name[0] == 0;
    memcpy(slot->info.mac, item.mac, 6);
    slot->info.role = hello.role;
    slot->info.mode = hello.mode;
    slot->info.caps = hello.caps;
    slot->info.input = hello.input;
    memcpy(slot->info.ip, hello.ip, 4);
    slot->info.rssi = item.rssi;
    memset(slot->info.name, 0, sizeof(slot->info.name));
    if (hello.name[0]) {
      strncpy(slot->info.name, hello.name, sizeof(slot->info.name) - 1);
    } else {
      snprintf(slot->info.name, sizeof(slot->info.name), "%02x%02x", item.mac[4], item.mac[5]);
    }
    slot->lastMs = millis();
    if (fresh) {
      Serial.printf("mesh peer %s rssi=%d\n", slot->info.name, item.rssi);
    }
    ensurePeer(item.mac);
    if (g_onConfig) {
      g_onConfig(hello.led_r, hello.led_g, hello.led_b, hello.led_bri, hello.theme, hello.rev);
    }
    return;
  }

  if (type == MESH_CONFIG && item.len >= sizeof(MeshConfig)) {
    MeshConfig cfg;
    memcpy(&cfg, item.data, sizeof(cfg));
    if (g_onConfig) {
      g_onConfig(cfg.led_r, cfg.led_g, cfg.led_b, cfg.led_bri, cfg.theme, cfg.rev);
    }
    return;
  }

  if (type == MESH_SET_MODE && item.len >= sizeof(MeshSetMode)) {
    MeshSetMode pkt;
    memcpy(&pkt, item.data, sizeof(pkt));
    if (pkt.mode < MODE_COUNT && g_onMode) {
      g_onMode(pkt.mode);
    }
    return;
  }

  if (type == MESH_SET_ROLE && item.len >= sizeof(MeshSetRole)) {
    MeshSetRole pkt;
    memcpy(&pkt, item.data, sizeof(pkt));
    if (g_onRole) {
      g_onRole(pkt.role);
    }
    return;
  }

  if (type == MESH_SET_INPUT && item.len >= sizeof(MeshSetInput)) {
    MeshSetInput pkt;
    memcpy(&pkt, item.data, sizeof(pkt));
    if (pkt.input <= INPUT_USB && g_onInput) {
      g_onInput(pkt.input);
    }
    return;
  }

  if (type == MESH_CLAIM && item.len >= sizeof(MeshClaim)) {
    MeshClaim claim;
    memcpy(&claim, item.data, sizeof(claim));
    if (claim.role == ROLE_CONDUCTOR && g_onClaim) {
      g_onClaim();
    }
    return;
  }

  if (type == MESH_VIZ && item.len >= sizeof(MeshViz)) {
    MeshViz pkt;
    memcpy(&pkt, item.data, sizeof(pkt));
    g_rxViz.seq = pkt.seq;
    g_rxViz.live = pkt.live;
    memcpy(g_rxViz.spectrum, pkt.spectrum, sizeof(g_rxViz.spectrum));
    memcpy(g_rxViz.spectro, pkt.spectro, sizeof(g_rxViz.spectro));
    memcpy(g_rxViz.wave, pkt.wave, sizeof(g_rxViz.wave));
    memcpy(g_rxViz.xyL, pkt.xyL, sizeof(g_rxViz.xyL));
    memcpy(g_rxViz.xyR, pkt.xyR, sizeof(g_rxViz.xyR));
    g_rxViz.lufsM = pkt.lufsM;
    g_rxViz.lufsS = pkt.lufsS;
    g_rxViz.lufsI = pkt.lufsI;
    g_vizPending = true;
  }
}

void meshSetIdentity(const char *name, uint8_t role, uint8_t mode, uint8_t caps, uint8_t input,
                     uint8_t r, uint8_t g, uint8_t b, uint8_t bri, uint8_t theme, uint32_t rev) {
  memset(g_id.name, 0, sizeof(g_id.name));
  if (name) {
    strncpy(g_id.name, name, sizeof(g_id.name) - 1);
  }
  g_id.role = role;
  g_id.mode = mode;
  g_id.caps = caps;
  g_id.input = input;
  g_id.r = r;
  g_id.g = g;
  g_id.b = b;
  g_id.bri = bri;
  g_id.theme = theme;
  g_id.rev = rev;
}

void meshBegin(MeshConfigFn onConfig, MeshClaimFn onClaim, MeshModeFn onMode, MeshRoleFn onRole,
               MeshInputFn onInput) {
  g_onConfig = onConfig;
  g_onClaim = onClaim;
  g_onMode = onMode;
  g_onRole = onRole;
  g_onInput = onInput;
  g_rxQueue = xQueueCreate(8, sizeof(RxItem));
  if (!g_rxQueue) {
    Serial.println("esp-now queue failed");
    return;
  }

  if (esp_now_init() != ESP_OK) {
    Serial.println("esp-now init failed");
    return;
  }
  esp_now_register_send_cb(onSent);
  esp_now_register_recv_cb(onRecv);

  esp_now_peer_info_t peer;
  memset(&peer, 0, sizeof(peer));
  memcpy(peer.peer_addr, kBcast, 6);
  peer.channel = 0;
  peer.ifidx = WIFI_IF_STA;
  peer.encrypt = false;
  const esp_err_t add = esp_now_add_peer(&peer);
  if (add != ESP_OK && add != ESP_ERR_ESPNOW_EXIST) {
    Serial.printf("esp-now peer add failed %d\n", (int)add);
    return;
  }

  g_ok = true;
  g_helloDue = true;
  Serial.printf("esp-now up  channel=%d\n", WiFi.channel());
}

void meshPollRx() {
  if (!g_rxQueue) {
    return;
  }
  RxItem item;
  while (xQueueReceive(g_rxQueue, &item, 0) == pdTRUE) {
    handlePacket(item);
  }
  agePeers(millis());
}

void meshPollTx(uint32_t now) {
  if (!g_ok) {
    return;
  }
  if (g_txBusy && (now - g_txStamp) > 40) {
    g_txBusy = false;
  }
  if (g_directLeft && g_directLen) {
    ensurePeer(g_directMac);
    if (sendPkt(g_directMac, g_directData, g_directLen)) {
      --g_directLeft;
    }
    return;
  }
  if (g_configLeft && now >= g_nextConfig) {
    if (sendConfigNow()) {
      if (g_configLeft) {
        --g_configLeft;
      }
      g_nextConfig = now + 100;
    }
    return;
  }
  if (g_claimLeft && now >= g_nextClaim) {
    if (sendClaimNow()) {
      if (g_claimLeft) {
        --g_claimLeft;
      }
      g_nextClaim = now + 200;
    }
    return;
  }
  if (g_helloDue || (now - g_lastHello) >= 1000) {
    if (sendHello()) {
      g_helloDue = false;
      g_lastHello = now;
    }
  }
}

void meshSendConfig() {
  g_configLeft = 2;
  g_nextConfig = 0;
}

void meshSendClaim() {
  g_claimLeft = 3;
  g_nextClaim = 0;
}

void meshAnnounce() {
  g_helloDue = true;
}

void meshSendSetMode(const uint8_t mac[6], uint8_t mode) {
  MeshSetMode pkt;
  memset(&pkt, 0, sizeof(pkt));
  fillHeader(&pkt.magic, &pkt.ver, &pkt.type, MESH_SET_MODE);
  pkt.mode = mode;
  queueDirect(mac, &pkt, sizeof(pkt));
  notePeer(mac, mode, -1, -1);
}

void meshSendSetRole(const uint8_t mac[6], uint8_t role) {
  MeshSetRole pkt;
  memset(&pkt, 0, sizeof(pkt));
  fillHeader(&pkt.magic, &pkt.ver, &pkt.type, MESH_SET_ROLE);
  pkt.role = role;
  queueDirect(mac, &pkt, sizeof(pkt));
  notePeer(mac, -1, role, -1);
}

void meshSendSetInput(const uint8_t mac[6], uint8_t input) {
  MeshSetInput pkt;
  memset(&pkt, 0, sizeof(pkt));
  fillHeader(&pkt.magic, &pkt.ver, &pkt.type, MESH_SET_INPUT);
  pkt.input = input;
  queueDirect(mac, &pkt, sizeof(pkt));
  notePeer(mac, -1, -1, input);
}

bool meshFindPeer(const char *name, MeshPeerInfo *out) {
  if (!name || !name[0] || !out) {
    return false;
  }
  agePeers(millis());
  for (int i = 0; i < kMaxPeers; ++i) {
    if (!g_peers[i].used) {
      continue;
    }
    if (strcmp(g_peers[i].info.name, name) == 0) {
      *out = g_peers[i].info;
      return true;
    }
  }
  return false;
}

void meshSendViz(const VizFrame &frame) {
  if (!g_ok) {
    return;
  }
  MeshViz pkt;
  memset(&pkt, 0, sizeof(pkt));
  fillHeader(&pkt.magic, &pkt.ver, &pkt.type, MESH_VIZ);
  pkt.live = frame.live;
  pkt.seq = frame.seq;
  memcpy(pkt.spectrum, frame.spectrum, sizeof(pkt.spectrum));
  memcpy(pkt.spectro, frame.spectro, sizeof(pkt.spectro));
  memcpy(pkt.wave, frame.wave, sizeof(pkt.wave));
  memcpy(pkt.xyL, frame.xyL, sizeof(pkt.xyL));
  memcpy(pkt.xyR, frame.xyR, sizeof(pkt.xyR));
  pkt.lufsM = frame.lufsM;
  pkt.lufsS = frame.lufsS;
  pkt.lufsI = frame.lufsI;
  sendRaw(reinterpret_cast<uint8_t *>(&pkt), sizeof(pkt));
}

bool meshTakeViz(VizFrame *out) {
  if (!g_vizPending || !out) {
    return false;
  }
  *out = g_rxViz;
  g_vizPending = false;
  return true;
}

int meshPeerCount() {
  agePeers(millis());
  int n = 0;
  for (int i = 0; i < kMaxPeers; ++i) {
    if (g_peers[i].used) {
      ++n;
    }
  }
  return n;
}

int meshCopyPeers(MeshPeerInfo *out, int maxOut) {
  agePeers(millis());
  int n = 0;
  for (int i = 0; i < kMaxPeers && n < maxOut; ++i) {
    if (!g_peers[i].used) {
      continue;
    }
    if (out) {
      out[n] = g_peers[i].info;
    }
    ++n;
  }
  if (out) {
    for (int i = 0; i < n; ++i) {
      for (int j = i + 1; j < n; ++j) {
        if (strcmp(out[j].name, out[i].name) < 0) {
          MeshPeerInfo tmp = out[i];
          out[i] = out[j];
          out[j] = tmp;
        }
      }
    }
  }
  return n;
}

bool meshReady() {
  return g_ok;
}

bool meshOtherConductorOutranksUs() {
  uint8_t self[6];
  WiFi.macAddress(self);
  for (int i = 0; i < kMaxPeers; ++i) {
    if (!g_peers[i].used || g_peers[i].info.role != ROLE_CONDUCTOR) {
      continue;
    }
    if (memcmp(self, g_peers[i].mac, 6) < 0) {
      return true;
    }
  }
  return false;
}
