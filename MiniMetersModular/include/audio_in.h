#pragma once

#include <stdint.h>

#include "mmm_proto.h"

void audioSetVizMode(uint8_t mode);
void audioBegin(bool conductor);
void audioSetConductor(bool conductor);
void audioPoll(uint32_t now);
bool audioTake(VizFrame *out);
uint32_t audioPackets();
