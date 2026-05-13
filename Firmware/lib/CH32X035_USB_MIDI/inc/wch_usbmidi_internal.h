#pragma once

#include <stdint.h>
#include "wch_usbmidi_usb.h"
#include "wch_usbmidi_config.h"
#include "wch_usbfs_compat.h"
#include <ch32x035.h>

#define EP0_SIZE 64
#define EP2_SIZE 64

// Buffer externs
extern __attribute__((aligned(4))) unsigned char wch_usbmidi_EP0_buffer[];
extern __attribute__((aligned(4))) unsigned char wch_usbmidi_EP2_buffer[];

// Descriptor externs
extern const USB_DEV_DESCR wch_usbmidi_DevDescr;
extern const uint8_t wch_usbmidi_CfgDescr[];
extern const uint16_t wch_usbmidi_CfgDescrLen;

extern const USB_STR_DESCR wch_usbmidi_LangDescr;
extern USB_STR_DESCR wch_usbmidi_ManufDescr;
extern USB_STR_DESCR wch_usbmidi_ProdDescr;
extern USB_STR_DESCR wch_usbmidi_SerDescr;
extern USB_STR_DESCR wch_usbmidi_InterfDescr;

// Setup buffer access
typedef struct __attribute__((packed)) {
    uint8_t  bRequestType;
    uint8_t  bRequest;
    uint8_t  wValueL;
    uint8_t  wValueH;
    uint8_t  wIndexL;
    uint8_t  wIndexH;
    uint8_t  wLengthL;
    uint8_t  wLengthH;
} USB_SETUP_REQ, *PUSB_SETUP_REQ;

#define USB_SetupBuf ((PUSB_SETUP_REQ)wch_usbmidi_EP0_buffer)

#ifdef __cplusplus
extern "C" {
#endif

void generate_all_string_descriptors(void);
void USB_init(void);

// Handler specific (renamed from CDC)
// We need MIDI specific EP handling exposed if C++ needs it, 
// but usually it's handled via shared buffers or helper functions.
// Let's expose helpers.
uint32_t USB_available(void);
uint32_t USB_read(uint8_t* buf, uint32_t len);
uint32_t USB_write(const uint8_t* buf, uint32_t len);

#ifdef __cplusplus
}
#endif