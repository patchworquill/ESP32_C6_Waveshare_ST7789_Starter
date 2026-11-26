#pragma once

#include <Arduino.h>

#ifndef MMM_USB_AUDIO
#define MMM_USB_AUDIO 0
#endif

// The S3 is a USB speaker and microphone on the same cable as the serial
// port. CDC-on-boot has to stay off so the audio class can register first,
// which makes Arduino's Serial the UART. Point Serial at our CDC instead.
#if MMM_USB_AUDIO
#include <USB.h>
#ifndef USB_INTERFACE_AUDIO
#define USB_INTERFACE_AUDIO USB_INTERFACE_CUSTOM
#endif
extern USBCDC USBSerial;
#undef Serial
#define Serial USBSerial
#endif
