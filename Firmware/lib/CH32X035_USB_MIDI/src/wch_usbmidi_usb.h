#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#ifndef WCH_USBMIDI_USB_DEFS
#define WCH_USBMIDI_USB_DEFS

#define USB_DESCR_TYP_DEVICE    0x01
#define USB_DESCR_TYP_CONFIG    0x02
#define USB_DESCR_TYP_STRING    0x03
#define USB_DESCR_TYP_INTERF    0x04
#define USB_DESCR_TYP_ENDP      0x05
#define USB_DESCR_TYP_CS_INTF   0x24
#define USB_DESCR_TYP_CS_ENDP   0x25

#define USB_DEV_CLASS_AUDIO     0x01
#define USB_SUBCLASS_AUDIOCONTROL 0x01
#define USB_SUBCLASS_MIDISTREAMING 0x03

#define USB_ENDP_TYPE_BULK      0x02
#define USB_ENDP_TYPE_INTER     0x03

#define USB_ENDP_ADDR_EP2_OUT   0x02
#define USB_ENDP_ADDR_EP2_IN    0x82

typedef struct __attribute__((packed)) {
    uint8_t  bLength;
    uint8_t  bDescriptorType;
    uint16_t bcdUSB;
    uint8_t  bDeviceClass;
    uint8_t  bDeviceSubClass;
    uint8_t  bDeviceProtocol;
    uint8_t  bMaxPacketSize0;
    uint16_t idVendor;
    uint16_t idProduct;
    uint16_t bcdDevice;
    uint8_t  iManufacturer;
    uint8_t  iProduct;
    uint8_t  iSerialNumber;
    uint8_t  bNumConfigurations;
} USB_DEV_DESCR, *PUSB_DEV_DESCR;

typedef struct __attribute__((packed)) {
    uint8_t  bLength;
    uint8_t  bDescriptorType;
    uint16_t bString[64];
} USB_STR_DESCR, *PUSB_STR_DESCR;

#endif // WCH_USBMIDI_USB_DEFS

#ifdef __cplusplus
}
#endif

