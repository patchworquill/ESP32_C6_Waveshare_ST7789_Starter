#pragma once

#include <stdint.h>

#include "mmm_proto.h"

struct MeshPeerInfo {
  char name[16];
  uint8_t role;
  uint8_t mode;
  uint8_t ip[4];
  int8_t rssi;
};

typedef void (*MeshConfigFn)(uint8_t mode, uint8_t r, uint8_t g, uint8_t b, uint8_t bri,
                             uint32_t rev);
typedef void (*MeshClaimFn)();

void meshSetIdentity(const char *name, uint8_t role, uint8_t mode, uint8_t r, uint8_t g,
                     uint8_t b, uint8_t bri, uint32_t rev);
void meshBegin(MeshConfigFn onConfig, MeshClaimFn onClaim);
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
