#pragma once

#include <stdint.h>

#include "mmm_proto.h"

void audioSetVizMode(uint8_t mode);
void audioSetInput(uint8_t input);
uint8_t audioInput();
uint8_t audioHearing();
void audioBegin(bool conductor);
void usbAudioStart();
void audioSetConductor(bool conductor);
void audioPoll(uint32_t now);
bool audioTake(VizFrame *out);
uint32_t audioPackets();
uint32_t audioUsbPackets();
