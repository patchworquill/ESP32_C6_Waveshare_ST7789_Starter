#pragma once

#include <stdint.h>

#include "mmm_proto.h"

struct MeshPeerInfo {
  uint8_t mac[6];
  char name[16];
  uint8_t role;
  uint8_t mode;
  uint8_t caps;
  uint8_t input;
  uint8_t ip[4];
  int8_t rssi;
};

typedef void (*MeshConfigFn)(uint8_t r, uint8_t g, uint8_t b, uint8_t bri, uint8_t theme,
                             uint32_t rev);
typedef void (*MeshClaimFn)();
typedef void (*MeshModeFn)(uint8_t mode);
typedef void (*MeshRoleFn)(uint8_t role);
typedef void (*MeshInputFn)(uint8_t input);

void meshSetIdentity(const char *name, uint8_t role, uint8_t mode, uint8_t caps, uint8_t input,
                     uint8_t r, uint8_t g, uint8_t b, uint8_t bri, uint8_t theme, uint32_t rev);
void meshBegin(MeshConfigFn onConfig, MeshClaimFn onClaim, MeshModeFn onMode, MeshRoleFn onRole,
               MeshInputFn onInput);
void meshSendSetMode(const uint8_t mac[6], uint8_t mode);
void meshSendSetRole(const uint8_t mac[6], uint8_t role);
void meshSendSetInput(const uint8_t mac[6], uint8_t input);
bool meshFindPeer(const char *name, MeshPeerInfo *out);
void meshPollRx();
void meshPollTx(uint32_t now);
void meshSendConfig();
void meshSendClaim();
void meshAnnounce();
void meshSendViz(const VizFrame &frame);
bool meshTakeViz(VizFrame *out);
int meshPeerCount();
int meshCopyPeers(MeshPeerInfo *out, int maxOut);
bool meshReady();
bool meshOtherConductorOutranksUs();
