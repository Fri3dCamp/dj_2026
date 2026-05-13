#include "wch_usbmidi_usb.h"
#include "wch_usbmidi_config.h"
#include "wch_usbmidi_internal.h"

#define EP0_SIZE 64
#define EP2_SIZE 64

__attribute__((aligned(4))) unsigned char wch_usbmidi_EP0_buffer[(EP0_SIZE+2<64?EP0_SIZE+2:64)];
// EP2 is used for both IN and OUT, so we need space.
// The CDC handler allocated (EP2_SIZE+2 < 64 ? ... : 64) + 64.
// Let's stick to that to be safe for double buffering or whatever magic it does.
__attribute__((aligned(4))) unsigned char wch_usbmidi_EP2_buffer[64 + 64];

const USB_DEV_DESCR wch_usbmidi_DevDescr = {
  .bLength            = sizeof(USB_DEV_DESCR),
  .bDescriptorType    = USB_DESCR_TYP_DEVICE,
  .bcdUSB             = 0x0200,
  .bDeviceClass       = 0x00, // MIDI defined at Interface level
  .bDeviceSubClass    = 0x00,
  .bDeviceProtocol    = 0x00,
  .bMaxPacketSize0    = EP0_SIZE,
  .idVendor           = WCH_USBMIDI_VENDOR_ID,
  .idProduct          = WCH_USBMIDI_PRODUCT_ID,
  .bcdDevice          = WCH_USBMIDI_DEVICE_VERSION,
  .iManufacturer      = 1,
  .iProduct           = 2,
  .iSerialNumber      = 3,
  .bNumConfigurations = 1
};

// Configuration Descriptor (Pure MIDI)
// We define it as a raw byte array because the MIDI descriptors have variable length CS descriptors.
// This is different from the struct approach in CDC, but easier for MIDI's complex structure.
// The handler just needs a pointer to bytes.

__attribute__((aligned(4))) const uint8_t wch_usbmidi_CfgDescr[] = {
    // Configuration Descriptor
    0x09, 0x02,                     // bLength, bDescriptorType (Config)
    0x61, 0x00,                     // wTotalLength (97 bytes)
    0x02,                           // bNumInterfaces (2: Audio Control + MIDI Streaming)
    0x01,                           // bConfigurationValue
    0x00,                           // iConfiguration
    0x80,                           // bmAttributes (Bus Powered)
    WCH_USBMIDI_MAX_POWER_mA / 2,   // MaxPower

    // -----------------------------------------------------------------------
    // Interface 0: Audio Control
    // -----------------------------------------------------------------------
    0x09, 0x04,                     // bLength, bDescriptorType (Interface)
    0x00,                           // bInterfaceNumber
    0x00,                           // bAlternateSetting
    0x00,                           // bNumEndpoints
    0x01,                           // bInterfaceClass (Audio)
    0x01,                           // bInterfaceSubClass (Audio Control)
    0x00,                           // bInterfaceProtocol
    0x00,                           // iInterface

    // Audio Control Interface Header Descriptor
    0x09, 0x24,                     // bLength, CS_INTERFACE
    0x01,                           // bDescriptorSubtype (HEADER)
    0x00, 0x01,                     // bcdADC (1.00)
    0x09, 0x00,                     // wTotalLength (9 bytes)
    0x01,                           // bInCollection
    0x01,                           // baInterfaceNr[1] -> Interface 1 (MIDI Streaming)

    // -----------------------------------------------------------------------
    // Interface 1: MIDI Streaming
    // -----------------------------------------------------------------------
    0x09, 0x04,                     // bLength, bDescriptorType (Interface)
    0x01,                           // bInterfaceNumber
    0x00,                           // bAlternateSetting
    0x02,                           // bNumEndpoints
    0x01,                           // bInterfaceClass (Audio)
    0x03,                           // bInterfaceSubClass (MIDI Streaming)
    0x00,                           // bInterfaceProtocol
    0x00,                           // iInterface

    // MIDI Adapter Class specific MS Interface Descriptor
    0x07, 0x24,                     // bLength, CS_INTERFACE
    0x01,                           // bDescriptorSubtype (MS_HEADER)
    0x00, 0x01,                     // bcdADC (1.00)
    0x25, 0x00,                     // wTotalLength (37 bytes)

    // MIDI IN Jack Descriptor (Embedded) - ID 1
    0x06, 0x24, 0x02, 0x01, 0x01, 0x00,
    
    // MIDI IN Jack Descriptor (External) - ID 2
    0x06, 0x24, 0x02, 0x02, 0x02, 0x00,
    
    // MIDI OUT Jack Descriptor (Embedded) - ID 3
    0x09, 0x24, 0x03, 0x01, 0x03, 0x01, 0x02, 0x01, 0x00,
    
    // MIDI OUT Jack Descriptor (External) - ID 4
    0x09, 0x24, 0x03, 0x02, 0x04, 0x01, 0x01, 0x01, 0x00,

    // -----------------------------------------------------------------------
    // Endpoint 2 OUT (Bulk) - MIDI OUT (Host -> Device)
    // -----------------------------------------------------------------------
    0x07, 0x05,                     // bLength, ENDPOINT
    USB_ENDP_ADDR_EP2_OUT,          // bEndpointAddress (0x02)
    USB_ENDP_TYPE_BULK,             // bmAttributes (Bulk)
    0x40, 0x00,                     // wMaxPacketSize (64)
    0x00,                           // bInterval

    // MS Bulk Data Endpoint Descriptor (Class Specific)
    0x05, 0x25,                     // bLength, CS_ENDPOINT
    0x01,                           // bDescriptorSubtype (MS_GENERAL)
    0x01,                           // bNumEmbMIDIJack
    0x01,                           // baAssocJackID (1 - Emb MIDI IN)

    // -----------------------------------------------------------------------
    // Endpoint 2 IN (Bulk) - MIDI IN (Device -> Host)
    // -----------------------------------------------------------------------
    0x07, 0x05,                     // bLength, ENDPOINT
    USB_ENDP_ADDR_EP2_IN,           // bEndpointAddress (0x82)
    USB_ENDP_TYPE_BULK,             // bmAttributes (Bulk)
    0x40, 0x00,                     // wMaxPacketSize (64)
    0x00,                           // bInterval

    // MS Bulk Data Endpoint Descriptor (Class Specific)
    0x05, 0x25,                     // bLength, CS_ENDPOINT
    0x01,                           // bDescriptorSubtype (MS_GENERAL)
    0x01,                           // bNumEmbMIDIJack
    0x03                            // baAssocJackID (3 - Emb MIDI OUT)
};

const uint16_t wch_usbmidi_CfgDescrLen = sizeof(wch_usbmidi_CfgDescr);

// Language descriptor
const USB_STR_DESCR wch_usbmidi_LangDescr = {
  .bLength         = 4,
  .bDescriptorType = USB_DESCR_TYP_STRING,
  .bString         = { WCH_USBMIDI_LANGUAGE }
};

// Dynamic string descriptors
USB_STR_DESCR wch_usbmidi_ManufDescr = {
  .bLength = (uint8_t)(2 + 2 * WCH_USBMIDI_MANUF_LEN),
  .bDescriptorType = USB_DESCR_TYP_STRING,
  .bString = {0} 
};

USB_STR_DESCR wch_usbmidi_ProdDescr = {
  .bLength = (uint8_t)(2 + 2 * WCH_USBMIDI_PROD_LEN),
  .bDescriptorType = USB_DESCR_TYP_STRING,
  .bString = {0} 
};

USB_STR_DESCR wch_usbmidi_SerDescr = {
  .bLength = (uint8_t)(2 + 2 * WCH_USBMIDI_SERIAL_LEN),
  .bDescriptorType = USB_DESCR_TYP_STRING,
  .bString = {0} 
};

USB_STR_DESCR wch_usbmidi_InterfDescr = {
  .bLength = (uint8_t)(2 + 2 * WCH_USBMIDI_INTERF_LEN),
  .bDescriptorType = USB_DESCR_TYP_STRING,
  .bString = {0} 
};

// Helper functions (copied from USBSerial)
void uint32_to_hex_string(uint32_t value, char* output) {
    const char hex_chars[] = "0123456789ABCDEF";
    for (int i = 7; i >= 0; i--) {
        output[7-i] = hex_chars[(value >> (i * 4)) & 0xF];
    }
}

void string_to_utf16le_descriptor(const char* source, uint16_t* dest, int max_len) {
    int i = 0;
    while (source[i] != '\0' && i < max_len) {
        dest[i] = (uint16_t)source[i];
        i++;
    }
}

void generate_manufacturer_descriptor(void) {
    string_to_utf16le_descriptor(WCH_USBMIDI_MANUF_STR, wch_usbmidi_ManufDescr.bString, WCH_USBMIDI_MANUF_LEN);
}

void generate_product_descriptor(void) {
    string_to_utf16le_descriptor(WCH_USBMIDI_PROD_STR, wch_usbmidi_ProdDescr.bString, WCH_USBMIDI_PROD_LEN);
}

void generate_interface_descriptor(void) {
    string_to_utf16le_descriptor(WCH_USBMIDI_INTERF_STR, wch_usbmidi_InterfDescr.bString, WCH_USBMIDI_INTERF_LEN);
}

void generate_unique_serial_descriptor(void) {
    uint32_t uid1 = *(volatile uint32_t*)CH32X035_ESIG_UNIID1;
    uint32_t uid2 = *(volatile uint32_t*)CH32X035_ESIG_UNIID2; 
    uint32_t uid3 = *(volatile uint32_t*)CH32X035_ESIG_UNIID3;
    
    char hex_string[24];
    uint32_to_hex_string(uid3, &hex_string[0]);
    uint32_to_hex_string(uid2, &hex_string[8]);
    uint32_to_hex_string(uid1, &hex_string[16]);
    
    for (int i = 0; i < (int)WCH_USBMIDI_SERIAL_PREFIX_LEN; i++) {
        wch_usbmidi_SerDescr.bString[i] = (uint16_t)WCH_USBMIDI_SERIAL_PREFIX[i];
    }
    
    for (int i = 0; i < WCH_USBMIDI_UID_HEX_CHARS; i++) {
        wch_usbmidi_SerDescr.bString[WCH_USBMIDI_SERIAL_PREFIX_LEN + i] = (uint16_t)hex_string[i];
    }
}

void generate_all_string_descriptors(void) {
    generate_manufacturer_descriptor();
    generate_product_descriptor();
    generate_interface_descriptor();
    generate_unique_serial_descriptor();
}