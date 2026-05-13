#include "wch_usbmidi_internal.h"
#include <string.h>

volatile uint8_t  USB_SetupReq, USB_SetupTyp, USB_Config, USB_Addr, USB_ENUM_OK;
volatile uint16_t USB_SetupLen;
const uint8_t*    USB_pDescr;

// RX FIFO
#define RX_FIFO_SIZE 256
volatile uint8_t rx_fifo[RX_FIFO_SIZE];
volatile uint16_t rx_head = 0;
volatile uint16_t rx_tail = 0;

void rx_fifo_push(uint8_t b) {
    uint16_t next = (rx_head + 1) % RX_FIFO_SIZE;
    if (next != rx_tail) {
        rx_fifo[rx_head] = b;
        rx_head = next;
    }
}

// TX FIFO (Non-blocking output)
#define TX_FIFO_SIZE 256
volatile uint8_t tx_fifo[TX_FIFO_SIZE];
volatile uint16_t tx_head = 0;
volatile uint16_t tx_tail = 0;

static int tx_fifo_push(const uint8_t* data, uint16_t len) {
    uint16_t next;
    for(uint16_t i=0; i<len; i++) {
        next = (tx_head + 1) % TX_FIFO_SIZE;
        if(next == tx_tail) return 0; // Buffer full
        tx_fifo[tx_head] = data[i];
        tx_head = next;
    }
    return 1;
}

// Helper: Attempt to send pending data from FIFO to USB hardware
static void USB_send_from_fifo(void) {
    // CRITICAL SECTION START: Prevent ISR from interrupting FIFO/Register access
    NVIC_DisableIRQ(USBFS_IRQn);

    // Only send if endpoint is ready (NAK indicates idle/ready for new TX)
    if((USBFSD->UEP2_CTRL_H & USBFS_UEP_T_RES_MASK) == USBFS_UEP_T_RES_NAK) {
        uint16_t count = 0;
        // Fill USB packet buffer (up to 64 bytes) from FIFO
        while(tx_head != tx_tail && count < 64) {
            wch_usbmidi_EP2_buffer[64 + count] = tx_fifo[tx_tail];
            tx_tail = (tx_tail + 1) % TX_FIFO_SIZE;
            count++;
        }

        if(count > 0) {
            USBFSD->UEP2_TX_LEN = count;
            // Set to ACK to transmit
            USBFSD->UEP2_CTRL_H = (USBFSD->UEP2_CTRL_H & ~USBFS_UEP_T_RES_MASK) | USBFS_UEP_T_RES_ACK;
        }
    }
    
    // CRITICAL SECTION END
    NVIC_EnableIRQ(USBFS_IRQn);
}

// Internal Init
static inline void USB_EP_init(void) {
  USBFSD->UEP0_DMA    = (uint32_t)wch_usbmidi_EP0_buffer;
  USBFSD->UEP0_CTRL_H = USBFS_UEP_R_RES_ACK | USBFS_UEP_T_RES_NAK;
  USBFSD->UEP0_TX_LEN = 0;

  // EP2 used for both IN and OUT in MIDI
  USBFSD->UEP2_DMA    = (uint32_t)wch_usbmidi_EP2_buffer;
  USBFSD->UEP2_3_MOD  = USBFS_UEP2_RX_EN | USBFS_UEP2_TX_EN;
  USBFSD->UEP2_CTRL_H = USBFS_UEP_AUTO_TOG | USBFS_UEP_R_RES_ACK | USBFS_UEP_T_RES_NAK;
  USBFSD->UEP2_TX_LEN = 0;

  USB_ENUM_OK = 0;
  USB_Config  = 0;
  USB_Addr    = 0;
}

void USB_init(void) {
    // Use CH32X035-specific RCC functions instead of direct register access
    RCC_APB2PeriphClockCmd(RCC_APB2Periph_AFIO, ENABLE);
    RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOC, ENABLE);
    RCC_AHBPeriphClockCmd(RCC_AHBPeriph_USBFS, ENABLE);
    
    // Wait for clocks to stabilize
    for(volatile int i = 0; i < 5000; i++) __NOP();
    
    // Use proper CH32X035 GPIO initialization
    GPIO_InitTypeDef GPIO_InitStructure = {0};
    
    // PC16 (USB D-) as floating input
    GPIO_InitStructure.GPIO_Pin = GPIO_Pin_16;
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_IN_FLOATING;
    GPIO_Init(GPIOC, &GPIO_InitStructure);

    // PC17 (USB D+) as input with pull-up
    GPIO_InitStructure.GPIO_Pin = GPIO_Pin_17;
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_IPU;
    GPIO_Init(GPIOC, &GPIO_InitStructure);
    
    // Critical: Use CH32X035-specific AFIO macros (try both approaches)
    // Approach 1: Try CH32X035 macro names
    #ifdef UDP_PUE_MASK
        // For 3.3V operation (standard)
        AFIO->CTLR = (AFIO->CTLR & ~(UDP_PUE_MASK | UDM_PUE_MASK)) 
                   | USB_PHY_V33 | UDP_PUE_1K5 | USB_IOEN;
    #else
        // Fallback to CH32X033 macro names
        AFIO->CTLR = (AFIO->CTLR & ~(AFIO_CTLR_UDP_PUE | AFIO_CTLR_UDM_PUE))
                   | AFIO_CTLR_USB_PHY_V33
                   | AFIO_CTLR_UDP_PUE_1K5
                   | AFIO_CTLR_USB_IOEN;
    #endif
    
    // Long delay after PHY configuration
    for(volatile int i = 0; i < 20000; i++) __NOP();

    // Generate unique serial number from 96-bit UID before USB enumeration
    generate_all_string_descriptors();

    // Reset USB core completely
    USBFSD->BASE_CTRL = 0x00;
    for(volatile int i = 0; i < 5000; i++) __NOP();

    // Initialize endpoints using your existing function
    USB_EP_init();
    
    // Set device address and clear interrupts
    USBFSD->DEV_ADDR = 0x00;
    USBFSD->INT_FG = 0xff;
    
    // Configure USB device controller
    USBFSD->UDEV_CTRL = USBFS_UD_PD_DIS | USBFS_UD_PORT_EN;
    
    // Enable USB with pull-up - this makes device visible to host
    USBFSD->BASE_CTRL = USBFS_UC_DEV_PU_EN | USBFS_UC_INT_BUSY | USBFS_UC_DMA_EN;
    
    // Very long delay for enumeration
    for(volatile int i = 0; i < 100000; i++) __NOP();

    // Enable interrupts
    USBFSD->INT_EN = USBFS_UIE_SUSPEND | USBFS_UIE_BUS_RST | USBFS_UIE_TRANSFER;
    NVIC_EnableIRQ(USBFS_IRQn);
}

void USB_EP0_copyDescr(uint8_t len) {
  uint8_t* tgt = wch_usbmidi_EP0_buffer;
  while(len--) *tgt++ = *USB_pDescr++;
}

static inline void USB_EP0_SETUP(void) {
  uint8_t len = 0;
  USB_SetupLen = ((uint16_t)USB_SetupBuf->wLengthH<<8) | (USB_SetupBuf->wLengthL);
  USB_SetupReq = USB_SetupBuf->bRequest;
  USB_SetupTyp = USB_SetupBuf->bRequestType;

  if((USB_SetupTyp & 0x60) == 0x00) {
    switch(USB_SetupReq) {
      case 0x06: /* GET_DESCRIPTOR */
        switch(USB_SetupBuf->wValueH) {
          case USB_DESCR_TYP_DEVICE:
            USB_pDescr = (const uint8_t*)&wch_usbmidi_DevDescr; len = sizeof(wch_usbmidi_DevDescr); break;
          case USB_DESCR_TYP_CONFIG:
            USB_pDescr = wch_usbmidi_CfgDescr; len = wch_usbmidi_CfgDescrLen; break;
          case USB_DESCR_TYP_STRING: {
            switch(USB_SetupBuf->wValueL) {
              case 0:   USB_pDescr = (const uint8_t*)&wch_usbmidi_LangDescr; break;
              case 1:   USB_pDescr = (const uint8_t*)&wch_usbmidi_ManufDescr; break;
              case 2:   USB_pDescr = (const uint8_t*)&wch_usbmidi_ProdDescr; break;
              case 3:   USB_pDescr = (const uint8_t*)&wch_usbmidi_SerDescr; break;
              case 4:   USB_pDescr = (const uint8_t*)&wch_usbmidi_InterfDescr; break;
              default:  USB_pDescr = (const uint8_t*)&wch_usbmidi_SerDescr; break;
            }
            len = ((const uint8_t*)USB_pDescr)[0];
            break;
          }
          default: len = 0xff; break;
        }
        if(len != 0xff) {
          if(USB_SetupLen > len) USB_SetupLen = len;
          len = USB_SetupLen >= EP0_SIZE ? EP0_SIZE : USB_SetupLen;
          USB_EP0_copyDescr(len);
        }
        break;
      case 0x05: /* SET_ADDRESS */
        USB_Addr = USB_SetupBuf->wValueL; break;
      case 0x08: /* GET_CONFIGURATION */
        wch_usbmidi_EP0_buffer[0] = USB_Config; if(USB_SetupLen > 1) USB_SetupLen = 1; len = USB_SetupLen; break;
      case 0x09: /* SET_CONFIGURATION */
        USB_Config  = USB_SetupBuf->wValueL; USB_ENUM_OK = 1; break;
      case 0x00: /* GET_STATUS */
        wch_usbmidi_EP0_buffer[0] = 0x00; wch_usbmidi_EP0_buffer[1] = 0x00; if(USB_SetupLen > 2) USB_SetupLen = 2; len = USB_SetupLen; break;
      default:
        len = 0xff; break;
    }
  } else {
    // No Class requests for MIDI usually (unlike CDC)
    len = 0xff;
  }

  if(len == 0xff) {
    USB_SetupReq = 0xff;
    USBFSD->UEP0_CTRL_H = USBFS_UEP_T_TOG | USBFS_UEP_T_RES_STALL | USBFS_UEP_R_TOG | USBFS_UEP_R_RES_STALL;
  } else {
    USB_SetupLen -= len;
    USBFSD->UEP0_TX_LEN = len;
    USBFSD->UEP0_CTRL_H = USBFS_UEP_T_TOG | USBFS_UEP_T_RES_ACK | USBFS_UEP_R_TOG | USBFS_UEP_R_RES_ACK;
  }
}

static inline void USB_EP0_IN(void) {
  uint8_t len;
  switch(USB_SetupReq) {
    case 0x06: /* GET_DESCRIPTOR */
      len = USB_SetupLen >= EP0_SIZE ? EP0_SIZE : USB_SetupLen;
      USB_EP0_copyDescr(len);
      USB_SetupLen -= len;
      USBFSD->UEP0_TX_LEN = len;
      USBFSD->UEP0_CTRL_H ^= USBFS_UEP_T_TOG;
      break;
    case 0x05: /* SET_ADDRESS */
      USBFSD->DEV_ADDR    = (USBFSD->DEV_ADDR & USBFS_UDA_GP_BIT) | USB_Addr;
      USBFSD->UEP0_CTRL_H = USBFS_UEP_T_RES_NAK | USBFS_UEP_R_TOG | USBFS_UEP_R_RES_ACK;
      break;
    default:
      USBFSD->UEP0_CTRL_H = USBFS_UEP_T_RES_NAK | USBFS_UEP_R_TOG | USBFS_UEP_R_RES_ACK;
      break;
  }
}

static inline void USB_EP0_OUT(void) {
  USBFSD->UEP0_CTRL_H = USBFS_UEP_T_TOG | USBFS_UEP_T_RES_ACK | USBFS_UEP_R_RES_ACK;
}

static inline void MIDI_EP2_OUT(void) {
    if(USBFSD->INT_ST & USBFS_UIS_TOG_OK) {
        uint8_t len = USBFSD->RX_LEN;
        // Simple ring buffer push
        for(int i=0; i<len; i++) {
            rx_fifo_push(wch_usbmidi_EP2_buffer[i]);
        }
        USBFSD->UEP2_CTRL_H = USBFS_UEP_R_RES_ACK | USBFS_UEP_AUTO_TOG; // Re-arm for next
    }
}

static inline void MIDI_EP2_IN(void) {
    // TX Completed, hardware automatically NAKs subsequent IN tokens until we re-arm
    USBFSD->UEP2_CTRL_H = (USBFSD->UEP2_CTRL_H & ~USBFS_UEP_T_RES_MASK) | USBFS_UEP_T_RES_NAK;
    
    // Check if more data is waiting in FIFO and send it
    USB_send_from_fifo();
}

// Helper to write (Non-blocking using FIFO)
uint32_t USB_write(const uint8_t* buf, uint32_t len) {
    if(len == 0) return 0;
    
    // Try to push to buffer
    if(!tx_fifo_push(buf, len)) {
        return 0; // Buffer full, packet dropped (non-blocking)
    }

    // Trigger transmission if hardware is idle
    USB_send_from_fifo();
    
    return len;
}

uint32_t USB_available(void) {
    uint16_t head = rx_head;
    uint16_t tail = rx_tail;
    if (head >= tail) return head - tail;
    return RX_FIFO_SIZE + head - tail;
}

uint32_t USB_read(uint8_t* buf, uint32_t len) {
    uint32_t count = 0;
    while(count < len && rx_head != rx_tail) {
        buf[count++] = rx_fifo[rx_tail];
        rx_tail = (rx_tail + 1) % RX_FIFO_SIZE;
    }
    return count;
}

void USBFS_IRQHandler(void) __attribute__((interrupt));
void USBFS_IRQHandler(void) {
  uint8_t intflag = USBFSD->INT_FG;
  uint8_t intst   = USBFSD->INT_ST;
  if(intflag & USBFS_UIF_TRANSFER) {
    uint8_t callIndex = intst & USBFS_UIS_ENDP_MASK;
    switch(intst & USBFS_UIS_TOKEN_MASK) {
      case USBFS_UIS_TOKEN_SETUP: USB_EP0_SETUP(); break;
      case USBFS_UIS_TOKEN_IN:
        switch(callIndex) { case 0: USB_EP0_IN(); break; case 2: MIDI_EP2_IN(); break; default: break; }
        break;
      case USBFS_UIS_TOKEN_OUT:
        switch(callIndex) { case 0: USB_EP0_OUT(); break; case 2: MIDI_EP2_OUT(); break; default: break; }
        break;
    }
    USBFSD->INT_FG = USBFS_UIF_TRANSFER;
  }
  if(intflag & USBFS_UIF_SUSPEND) { USBFSD->INT_FG = USBFS_UIF_SUSPEND; }
  if(intflag & USBFS_UIF_BUS_RST) {
    USB_EP_init();
    USBFSD->DEV_ADDR = 0; USBFSD->INT_FG = 0xff;
  }
}