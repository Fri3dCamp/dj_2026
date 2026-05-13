/* Copyright (c) 2026 Bert Outtier <outtierbert@gmail.com>
 *
 * DJ Addon firmware for the Fri3d Badge 2024/2026.
 *
 * This firmware runs on a WCH CH32X035 microcontroller and exposes all
 * hardware inputs/outputs over 3 interfaces simultaneously:
 *
 *   1. USB MIDI (class-compliant, no driver needed)
 *      - Potmeters, sliders  → Control Change messages
 *      - Button matrix       → Control Change messages
 *      - Rotary encoders     → Control Change messages
 *      - Incoming CC         → WS2812 LED color control
 *
 *   2. I2C slave (address 0x3A, bus speed 400kHz)
 *      - Master can read a register map containing all sensor values.
 *      - Master can write LED color data into the writable region.
 *
 *   3. UART
 *      - Sends and receives the same MIDI packets as on USB
 *
 * The USB interface can be connected to a PC using a USB-C cable.
 * The I2C and UART interfaces are connected to the badge through the expansion connector.
 *
 * Hardware connected:
 *   - 6 potentiometers (3 left: PC0, PC3, PA2 / 3 right: PA6, PB1, PB0)
 *   - 3 faders / sliders (left: PA3, right: PA5, crossfader: PA4)
 *   - 3×3 button matrix, with 8 buttons connected (cols: PB7/PB8/PB11  rows: PC14/PC15/PB6)
 *   - 8 WS2812C RGB LEDs driven over SPI1 MOSI (PA7) using DMA
 *   - 2 rotary encoders (quadrature) on TIM2 (left: PA0/PA1) and TIM1 (right: PB9/PB10)
 *   - I2C1 on PC18 (SDA) / PC19 (SCL)
 *   - USART3 on PB3/PB4 for MIDI over UART (in debug mode: for debug output)
 */

#include <ch32x035.h> /* both X033 and X035 */
#include <stdlib.h>   /* atoi() */
#include <string.h>   /* memset() */

#include <wch_usbmidi_internal.h>

/* we use our own custom debug lib */
#include "debug.h"

/*
 * Analog input pin definitions.
 * Each entry defines the GPIO port/pin, the ADC channel number, and the
 * "rank" (conversion order) used when setting up the ADC scan sequence.
 * Rank 1 is converted first; all 9 channels are read continuously via DMA.
 */
#define PM_LEFT_TOP_PORT       GPIOC /* PC0: Potmeter left top */
#define PM_LEFT_TOP_PIN        GPIO_Pin_0
#define PM_LEFT_TOP_CHANNEL    ADC_Channel_10
#define PM_LEFT_TOP_RANK       (3)
#define PM_LEFT_MID_PORT       GPIOC /* PC3: Potmeter left mid */
#define PM_LEFT_MID_PIN        GPIO_Pin_3
#define PM_LEFT_MID_CHANNEL    ADC_Channel_13
#define PM_LEFT_MID_RANK       (2)
#define PM_LEFT_BOTTOM_PORT    GPIOA /* PA2: Potmeter left bottom */
#define PM_LEFT_BOTTOM_PIN     GPIO_Pin_2
#define PM_LEFT_BOTTOM_CHANNEL ADC_Channel_2
#define PM_LEFT_BOTTOM_RANK    (1)

#define PM_RIGHT_TOP_PORT       GPIOA /* PA6: Potmeter right top */
#define PM_RIGHT_TOP_PIN        GPIO_Pin_6
#define PM_RIGHT_TOP_CHANNEL    ADC_Channel_6
#define PM_RIGHT_TOP_RANK       (7)
#define PM_RIGHT_MID_PORT       GPIOB /* PB1: Potmeter right mid (TODO: PB5?) */
#define PM_RIGHT_MID_PIN        GPIO_Pin_1
#define PM_RIGHT_MID_CHANNEL    ADC_Channel_9
#define PM_RIGHT_MID_RANK       (6)
#define PM_RIGHT_BOTTOM_PORT    GPIOB /* PB0: Potmeter right bottom */
#define PM_RIGHT_BOTTOM_PIN     GPIO_Pin_0
#define PM_RIGHT_BOTTOM_CHANNEL ADC_Channel_8
#define PM_RIGHT_BOTTOM_RANK    (5)

#define SLIDER_LEFT_PORT     GPIOA /* PA3: Slider left */
#define SLIDER_LEFT_PIN      GPIO_Pin_3
#define SLIDER_LEFT_CHANNEL  ADC_Channel_3
#define SLIDER_LEFT_RANK     (4)
#define SLIDER_RIGHT_PORT    GPIOA /* PA5: Slider right */
#define SLIDER_RIGHT_PIN     GPIO_Pin_5
#define SLIDER_RIGHT_CHANNEL ADC_Channel_5
#define SLIDER_RIGHT_RANK    (8)
#define SLIDER_MID_PORT      GPIOA /* PA4: Slider mid */
#define SLIDER_MID_PIN       GPIO_Pin_4
#define SLIDER_MID_CHANNEL   ADC_Channel_4
#define SLIDER_MID_RANK      (9)
#define ADC_CHANNELS         (9) /* total number of ADC channels being scanned */

/*
 * Rotary encoder pins.
 * Each encoder has two quadrature signals (A and B) fed into a hardware timer
 * configured in encoder mode. The timer counts pulses automatically
 * without interrupts, wrapping around within [0, MIDI_MAX].
 * See: Scratch_Right_Encoder_Init() and Scratch_Left_Encoder_Init()
 *   Left encoder  → TIM2 (PA0=B, PA1=A)
 *   Right encoder → TIM1 (PB9=B, PB10=A)
 */
#define SCRATCH_LEFT_A_PORT  GPIOA /* PA1: Scratchpad right A */
#define SCRATCH_LEFT_A_PIN   GPIO_Pin_1
#define SCRATCH_LEFT_B_PORT  GPIOA /* PA0: Scratchpad right B */
#define SCRATCH_LEFT_B_PIN   GPIO_Pin_0
#define SCRATCH_LEFT_TIM     TIM2  /* timer with encoder mode */
#define SCRATCH_RIGHT_A_PORT GPIOB /* PB10: Scratchpad left A */
#define SCRATCH_RIGHT_A_PIN  GPIO_Pin_10
#define SCRATCH_RIGHT_B_PORT GPIOB /* PB9: Scratchpad left B */
#define SCRATCH_RIGHT_B_PIN  GPIO_Pin_9
#define SCRATCH_RIGHT_TIM    TIM1 /* timer with encoder mode */

/*
 * Button matrix (3×3, 8 buttons tracked).
 * Columns are driven LOW one at a time (output).
 * Rows are read with pull-ups (input).
 * A LOW row while a column is active means that button is pressed.
 * Note: col2/row2 is not connected to a button and is ignored.
 *   Bit layout of matrix_state (uint8_t): [col2r1][col2r0][col1r2][col1r1][col1r0][col0r2][col0r1][col0r0]
 */
#define BUTTON_ROW0_PORT GPIOC /* PC14: Button matrix Row 0 */
#define BUTTON_ROW0_PIN  GPIO_Pin_14
#define BUTTON_ROW1_PORT GPIOC /* PB5: Button matrix Row 1 */
#define BUTTON_ROW1_PIN  GPIO_Pin_15
#define BUTTON_ROW2_PORT GPIOB /* PB6: Button matrix Row 2 */
#define BUTTON_ROW2_PIN  GPIO_Pin_6

#define BUTTON_COL0_PORT GPIOB /* PB7: Button matrix Col 0 */
#define BUTTON_COL0_PIN  GPIO_Pin_7
#define BUTTON_COL1_PORT GPIOB /* PB8: Button matrix Col 1 */
#define BUTTON_COL1_PIN  GPIO_Pin_8
#define BUTTON_COL2_PORT GPIOB /* PB11: Button matrix Col 2 */
#define BUTTON_COL2_PIN  GPIO_Pin_11
#define N_COLS           (3)

#define TIMER_FREQ ((SystemCoreClock / 10000) - 1) /* the output frequency of TIM3: 100Hz */

/*
 * WS2812C LED definitions.
 * LEDs are driven via SPI1 MOSI (PA7). Because the WS2812 protocol requires
 * precise pulse widths that can't be done with GPIO toggling at high speed,
 * we use SPI to encode each WS2812 bit as 4 SPI bits:
 *   WS2812 '0' → 0x8 (1000): short high (~333ns), long low  (~1µs)
 *   WS2812 '1' → 0xE (1110): long high  (~1µs),  short low (~333ns)
 * Two WS2812 bits are packed per SPI byte (high nibble + low nibble).
 * One color channel (8 bits) → 4 SPI bytes. One LED (G+R+B) → 12 SPI bytes.
 * The full COLOR_BUFFER is sent via DMA so the CPU is not blocked.
 */
#define LED_PORT         GPIOA /* PA7: WS2812 leds (SPI1 MOSI) */
#define LED_PIN          GPIO_Pin_7
#define LEDS_NUM         (8)
#define Pixel_PRE_LEN    (12u) /* SPI bytes per LED: 3 channels × 4 bytes/channel */
#define Pixel_RESET_LEN  (25u) /* SPI bytes for WS2812 reset pulse (>50µs of 0x00) */
#define COLOR_BUFFER_LEN (((LEDS_NUM * 3) * Pixel_PRE_LEN) + Pixel_RESET_LEN)
#define SPI1_DMA_TX_CH   DMA1_Channel3

/* I2C on the expansion connector towards the badge PC18/PC19 */
#define SDA_PORT         GPIOC
#define SDA_PIN          GPIO_Pin_18
#define SCL_PORT         GPIOC
#define SCL_PIN          GPIO_Pin_19
#define I2C_ADDRESS      (0x3A) /* 7-bit slave address; write addr=0x74, read addr=0x75 */
#define I2C_TIMEOUT      (-2)
#define I2C_TIMEOUT_TICK (1000)
#define I2C_SPEED        (400000) /* 400 kHz fast-mode I2C */
#define UART_BAUDRATE    (115200)

/* Minimum change in scaled ADC value (after >>5) before sending a USB MIDI CC message.
 * Prevents flooding the host with MIDI messages when the ADC fluctuates by ±1 LSB.
 */
#define HYSTERESIS (1)

/* midi-usb */
#define MIDI_CHANNEL (0)    /* MIDI channel 1 (0-indexed) */
#define MIDI_MAX     (0x7f) /* maximum value for a 7-bit MIDI data byte */

/*
 * Bytes at offset < RESULT_RW_OFFSET are read-only.
 * Writing starts at RESULT_RW_OFFSET (the LED region).
 * Total size = RESULT_BUFFER_SIZE = 50 bytes.
 */
#define RESULT_BUFFER_SIZE (3 + 1 + (ADC_CHANNELS * 2) + 2 + 2 + (LEDS_NUM * 3))
#define RESULT_RW_OFFSET   (3 + 1 + (ADC_CHANNELS * 2) + 2 + 2) /* first writable byte offset */

static const uint8_t base_note_adc[ADC_CHANNELS] = {
    0x40, /* PM_LEFT_TOP */
    0x41, /* PM_LEFT_MID */
    0x42, /* PM_LEFT_BOTTOM */
    0x43, /* SLIDER_LEFT */
    0x50, /* PM_RIGHT_TOP */
    0x51, /* PM_RIGHT_MID */
    0x52, /* PM_RIGHT_BOTTOM */
    0x53, /* SLIDER_RIGHT */
    0x59, /* SLIDER_MID */
};

/* array to map a button index to a note
 * idx:col:row
 *
 * | 0:0:0 | 1:0:1 | 2:0:2 | 3:1:0 |
 * | 4:1:1 | 5:1:2 | 6:2:0 | 7:2:1 |
 *
 */
static const uint8_t button_note[] = {
    0x64, /* col 0 row 0 */
    0x66, /* col 0 row 1 */
    0x65, /* col 0 row 2 */
    0x60, /* col 1 row 0 */
    0x62, /* col 1 row 1 */
    0x61, /* col 1 row 2 */
    0x67, /* col 2 row 0 */
    0x63, /* col 2 row 1 */
};

/* MIDI Control Change note numbers for encoders.
 * NOTE_ENCODER_x   → absolute position value (0–127)
 * NOTE_ENCODER_x+1 → activity flag: MIDI_MAX when turning, 0 when stopped */
#define NOTE_ENCODER_LEFT  (0x44)
#define NOTE_ENCODER_RIGHT (0x54)
/* Incoming CC notes >= BASE_NOTE_LEDS control LED colors (index = note - BASE_NOTE_LEDS) */
#define BASE_NOTE_LEDS (0x20)

typedef struct
{
    uint8_t g; /* Green */
    uint8_t r; /* Red */
    uint8_t b; /* Blue */
} ws2812b_color_t;

/* Predefined color palette used when the host sends a CC value 1–9 to set an LED.
 * Stored in GRB order (WS2812 wire format). Index 0 = value 1, index 8 = value 9.
 * Value 0 turns the LED off (handled separately).
 */
static const ws2812b_color_t mixxx_palette[] = {
    {.g = 0x0a, .r = 0xc5, .b = 0x08}, // 1: orange-red
    {.g = 0xbe, .r = 0x32, .b = 0x44}, // 2: teal
    {.g = 0xd4, .r = 0x42, .b = 0xf4}, // 3: yellow-green
    {.g = 0xd2, .r = 0xf8, .b = 0x00}, // 4: warm white
    {.g = 0x44, .r = 0x00, .b = 0xff}, // 5: blue
    {.g = 0x00, .r = 0xaf, .b = 0xcc}, // 6: cyan
    {.g = 0xa6, .r = 0xfc, .b = 0xd7}, // 7: white
    {.g = 0xf2, .r = 0xf2, .b = 0xff}, // 8: bright white
    {.g = 0x80, .r = 0xff, .b = 0x00}, // 9: green
};

/*
 * I2C register map layout (also the layout of addon_data_t / raw_data[]):
 *
 *   Offset  Size  Description
 *   ------  ----  ---------------------------------------------------
 *   0x00     3    Firmware version [major, minor, patch]  (READ-ONLY)
 *   0x03     1    Button matrix state byte                (READ-ONLY)
 *   0x04    18    ADC channels[0..8] as uint16_t, little-endian (READ-ONLY)
 *   0x16     2    Left encoder counter as uint16_t        (READ-ONLY)
 *   0x18     2    Right encoder counter as uint16_t       (READ-ONLY)
 *   0x1A    24    LED data: 8 × {G, R, B}                 (READ-WRITE)
 *
 * Packed struct that maps directly onto the I2C register map.
 * __attribute__((packed)) ensures no padding bytes are inserted,
 * so sizeof(addon_data_t) == RESULT_BUFFER_SIZE exactly.
 */
typedef struct __attribute__((packed))
{
    uint8_t version[3];                  /* firmware version [major, minor, patch] */
    uint8_t matrix_state;                /* bitmask of 8 button states (1 = pressed) */
    uint16_t adc_channels[ADC_CHANNELS]; /* raw 12-bit ADC values from DMA (left-justified in uint16) */
    uint16_t left_encoder;               /* quadrature counter for left encoder (wraps 0–MIDI_MAX) */
    uint16_t right_encoder;              /* quadrature counter for right encoder (wraps 0–MIDI_MAX) */
    ws2812b_color_t leds[LEDS_NUM];      /* current LED colors in GRB order (writable via I2C) */
} addon_data_t;

/* Compile-time check: the struct layout must match the register map exactly */
_Static_assert(sizeof(addon_data_t) == RESULT_BUFFER_SIZE, "raw data and struct size are not aligned!");

/*
 * Global firmware state.
 * Flags are set by interrupt handlers and consumed by the main loop.
 * The union lets I2C code treat the structured data as a flat byte array
 * for simple indexed access using raw_data_ptr.
 */
typedef struct
{
    uint8_t flag_update_leds : 1;       /* set when LED data has changed and w2812_sync() must be called */
    uint8_t flag_matrix_scan_done : 1;  /* set when the button matrix state has been updated */
    uint8_t flag_slave_first_write : 1; /* set on every ADDR phase; the next RXNE byte is the register offset. */
    uint8_t reserved : 5;               /* reserved for future use */
    uint8_t slave_offset;               /* register offset captured after the most recent ADDR+W. */
    uint8_t slave_position;             /* current read/write cursor, reset to offset on every ADDR (including repeated-START), so write-then-read works without special-casing. */
    uint8_t unused;                     /* unused byte to make the data 4 byte aligned */
    union
    {
        addon_data_t data;                    /* structured access to the register map */
        uint8_t raw_data[RESULT_BUFFER_SIZE]; /* flat byte access for I2C transfers */
    };
} addon_state_t;

/* Global firmware state. */
static addon_state_t state;

/*
 * SPI transmit buffer for WS2812 LEDs.
 * Each LED takes Pixel_PRE_LEN (12) bytes of encoded SPI data.
 * The last Pixel_RESET_LEN (25) bytes must be 0x00 to generate the reset pulse.
 * This buffer is sent once per LED update via DMA (no CPU involvement during TX).
 */
static uint8_t color_buf[COLOR_BUFFER_LEN] = {0};

/*
 * Initialize USART3 on PB3 (TX) / PB4 (RX) at the given baud rate.
 * In debug builds, PRINT() macros send text here.
 * In release builds, the USART is used to send and receive raw 4-byte MIDI packets
 * (same format as USB MIDI) so the badge can exchange MIDI commands with the addon.
 */
static void USART3_Output_Init(uint32_t baudrate)
{
    GPIO_InitTypeDef GPIO_InitStructure;
    USART_InitTypeDef USART_InitStructure;

    RCC_APB1PeriphClockCmd(RCC_APB1Periph_USART3, ENABLE);
    RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOB, ENABLE);

    /* PB3=TX, PB4=RX — alternate function push-pull */
    GPIO_InitStructure.GPIO_Pin = GPIO_Pin_3 | GPIO_Pin_4;
    GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_AF_PP;
    GPIO_Init(GPIOB, &GPIO_InitStructure);

    USART_InitStructure.USART_BaudRate = baudrate;
    USART_InitStructure.USART_WordLength = USART_WordLength_8b;
    USART_InitStructure.USART_StopBits = USART_StopBits_1;
    USART_InitStructure.USART_Parity = USART_Parity_No;
    USART_InitStructure.USART_HardwareFlowControl = USART_HardwareFlowControl_None;
    USART_InitStructure.USART_Mode = USART_Mode_Rx | USART_Mode_Tx;

    USART_Init(USART3, &USART_InitStructure);
    USART_Cmd(USART3, ENABLE);
}

/*
 * Encode one byte of WS2812 data into 4 SPI bytes.
 * Each WS2812 bit becomes a 4-bit SPI pattern:
 *   bit = 1 → 0xE (1110): long high, short low  → WS2812 "1" pulse
 *   bit = 0 → 0x8 (1000): short high, long low  → WS2812 "0" pulse
 * Two WS2812 bits are packed into one SPI byte (high nibble + low nibble),
 * so 8 WS2812 bits → 4 SPI bytes. Bits are processed MSB first.
 *
 * @param res    output: 4 SPI bytes
 * @param input  input: 1 byte of WS2812 color data
 */
static void convToBit(uint8_t *res, uint8_t input)
{
    uint8_t mask = 0x80;
    for (int i = 0; i < 4; i++)
    {
        /* encode the high bit of the current pair into the high nibble */
        uint8_t result = (input & mask) ? 0xE : 0x8;
        result <<= 4;
        mask >>= 1;
        /* encode the low bit of the current pair into the low nibble */
        result |= (input & mask) ? 0xE : 0x8;
        mask >>= 1;
        res[i] = result;
    }
}

/*
 * Encode one LED's RGB color into 12 SPI bytes in color_buf.
 * WS2812 wire order is G → R → B (not R → G → B!).
 * Each channel is 4 SPI bytes via convToBit(), laid out consecutively:
 *   buf[0..3]  = green channel
 *   buf[4..7]  = red channel
 *   buf[8..11] = blue channel
 */
static void colorToBit(uint8_t *buf, uint8_t r, uint8_t g, uint8_t b)
{
    uint8_t *res = buf;
    convToBit(res, g);
    convToBit(&(res[4]), r);
    convToBit(&(res[8]), b);
}

/*********************************************************************
 * @fn      setPixelColor
 *
 * @brief   Set the pixel color of an LED
 *
 * @param   index - index of LED
 *          r  - red channel
 *          g  - green channel
 *          b  - blue channel
 *
 *
 * @return  none
 */
static void setPixelColor(uint16_t index, uint8_t r, uint8_t g, uint8_t b)
{
    uint8_t *buf = &(color_buf[index * Pixel_PRE_LEN]);
    colorToBit(buf, r, g, b);
}

/*********************************************************************
 * @fn      SPI_1Lines_HalfDuplex_Init
 *
 * @brief   Configuring the SPI for half-duplex communication.
 *
 * @return  none
 */
void SPI_1Lines_HalfDuplex_Init(void)
{
    GPIO_InitTypeDef GPIO_InitStructure = {0};
    SPI_InitTypeDef SPI_InitStructure = {0};

    RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOA | RCC_APB2Periph_SPI1, ENABLE);

    GPIO_InitStructure.GPIO_Pin = LED_PIN;
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_AF_PP;
    GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_Init(LED_PORT, &GPIO_InitStructure);

    SPI_InitStructure.SPI_Direction = SPI_Direction_1Line_Tx;
    SPI_InitStructure.SPI_Mode = SPI_Mode_Master;
    SPI_InitStructure.SPI_DataSize = SPI_DataSize_8b;
    SPI_InitStructure.SPI_CPOL = SPI_CPOL_High;
    SPI_InitStructure.SPI_CPHA = SPI_CPHA_1Edge;
    SPI_InitStructure.SPI_NSS = SPI_NSS_Soft;
    SPI_InitStructure.SPI_BaudRatePrescaler = SPI_BaudRatePrescaler_16;
    SPI_InitStructure.SPI_FirstBit = SPI_FirstBit_MSB;
    SPI_InitStructure.SPI_CRCPolynomial = 7;
    SPI_Init(SPI1, &SPI_InitStructure);

    SPI_Cmd(SPI1, ENABLE);
}

/*********************************************************************
 * @fn      SPI1_DMA_Init
 *
 * @brief   Initialize DMA for SPI1 TX
 *
 * @return  none
 */
static void SPI1_DMA_Init(void)
{
    DMA_InitTypeDef DMA_InitStructure = {0};

    RCC_AHBPeriphClockCmd(RCC_AHBPeriph_DMA1, ENABLE);

    DMA_DeInit(SPI1_DMA_TX_CH);

    DMA_InitStructure.DMA_PeripheralBaseAddr = (uint32_t)&SPI1->DATAR;
    DMA_InitStructure.DMA_MemoryBaseAddr = (uint32_t)color_buf;
    DMA_InitStructure.DMA_DIR = DMA_DIR_PeripheralDST;
    DMA_InitStructure.DMA_BufferSize = COLOR_BUFFER_LEN;
    DMA_InitStructure.DMA_PeripheralInc = DMA_PeripheralInc_Disable;
    DMA_InitStructure.DMA_MemoryInc = DMA_MemoryInc_Enable;
    DMA_InitStructure.DMA_PeripheralDataSize = DMA_PeripheralDataSize_Byte;
    DMA_InitStructure.DMA_MemoryDataSize = DMA_MemoryDataSize_Byte;
    DMA_InitStructure.DMA_Mode = DMA_Mode_Normal;
    DMA_InitStructure.DMA_Priority = DMA_Priority_High;
    DMA_InitStructure.DMA_M2M = DMA_M2M_Disable;

    DMA_Init(SPI1_DMA_TX_CH, &DMA_InitStructure);

    SPI_I2S_DMACmd(SPI1, SPI_I2S_DMAReq_Tx, ENABLE);
}

/*
 * Push the current LED color state out to the physical WS2812 LEDs via SPI DMA.
 *
 * Steps:
 *   1. Re-encode all LED colors from state.data.leds[] into the SPI bit buffer.
 *   2. Wait for any in-progress DMA transfer to finish (so we don't corrupt it).
 *   3. Reset the DMA counter and re-enable to start a fresh transfer.
 *
 * After this function returns, the DMA is running in the background; the CPU
 * is free to do other work while the SPI hardware clocks out the bit stream.
 */
static void w2812_sync(void)
{
    /* step 1: encode RGB values into the SPI bit-stream buffer */
    for (int i = 0; i < LEDS_NUM; i++)
    {
        setPixelColor(i, state.data.leds[i].r, state.data.leds[i].g, state.data.leds[i].b);
    }
    /* step 2: wait until the previous DMA transfer has completed */
    while (DMA_GetCurrDataCounter(SPI1_DMA_TX_CH) != 0)
        ;
    /* step 3: clear the transfer-complete flag, reload the counter, and restart DMA */
    DMA_ClearFlag(DMA1_FLAG_TC3);
    DMA_Cmd(SPI1_DMA_TX_CH, DISABLE);
    DMA_SetCurrDataCounter(SPI1_DMA_TX_CH, COLOR_BUFFER_LEN);
    DMA_Cmd(SPI1_DMA_TX_CH, ENABLE);
}

/*
 * Configure a timer in quadrature encoder mode.
 *
 * In this mode the timer counts up or down automatically as the encoder
 * rotates: channel 1 (A) and channel 2 (B) signals determine direction.
 * The counter wraps at TIM_Period (MIDI_MAX = 127), keeping the value
 * always in the 0–127 range that maps directly to MIDI CC values.
 *
 * A hardware input filter (TIM_ICFilter = 10) rejects glitches shorter
 * than 5 samples at Fdts/16, helping with encoder debouncing.
 *
 * The counter is initialized to the midpoint (63) so that both
 * clockwise and counter-clockwise movement are immediately readable.
 */
static void Encoder_Timer_Init(TIM_TypeDef *TIMx)
{
    TIM_TimeBaseInitTypeDef TIM_TimeBaseStructure;
    TIM_ICInitTypeDef TIM_ICInitStructure;

    TIM_TimeBaseStructInit(&TIM_TimeBaseStructure);
    TIM_TimeBaseStructure.TIM_Prescaler = 0x0;
    TIM_TimeBaseStructure.TIM_Period = MIDI_MAX; /* counter wraps at 127 (0–127 = 128 steps) */
    TIM_TimeBaseStructure.TIM_ClockDivision = TIM_CKD_DIV1;
    TIM_TimeBaseStructure.TIM_CounterMode = TIM_CounterMode_Up;
    TIM_TimeBaseInit(TIMx, &TIM_TimeBaseStructure);

    /* TI12 mode: both TI1 and TI2 are used for counting (full quadrature) */
    TIM_EncoderInterfaceConfig(TIMx, TIM_EncoderMode_TI12, TIM_ICPolarity_Rising, TIM_ICPolarity_Rising);

    TIM_ICStructInit(&TIM_ICInitStructure);
    TIM_ICInitStructure.TIM_ICFilter = 10; /* 1010: Fsampling = Fdts/16, N=5 — filters short glitches */
    TIM_ICInit(TIMx, &TIM_ICInitStructure);

    TIM_SetCounter(TIMx, MIDI_MAX >> 1); /* start at midpoint so both directions are usable immediately */
}

static void Scratch_Right_Encoder_Init(void)
{
    GPIO_InitTypeDef GPIO_InitStructure;

    RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOB | RCC_APB2Periph_TIM1, ENABLE);

    GPIO_InitStructure.GPIO_Pin = SCRATCH_RIGHT_B_PIN | SCRATCH_RIGHT_A_PIN; /* PB9 and PB10 */
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_IN_FLOATING;
    GPIO_Init(SCRATCH_RIGHT_A_PORT, &GPIO_InitStructure);

    Encoder_Timer_Init(SCRATCH_RIGHT_TIM);
}

static void Scratch_Left_Encoder_Init(void)
{
    GPIO_InitTypeDef GPIO_InitStructure;

    RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOA, ENABLE);
    RCC_APB1PeriphClockCmd(RCC_APB1Periph_TIM2, ENABLE);

    GPIO_InitStructure.GPIO_Pin = SCRATCH_LEFT_B_PIN | SCRATCH_LEFT_A_PIN; /* PA0 and PA1 */
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_IN_FLOATING;
    GPIO_Init(SCRATCH_LEFT_A_PORT, &GPIO_InitStructure);

    Encoder_Timer_Init(SCRATCH_LEFT_TIM);
}

/*
 * Process one I2C event from the I2C1_EV_IRQHandler interrupt.
 *
 * Called on every I2C bus event: address match, byte received, transmit buffer
 * empty, or STOP detected. All event flags are snapshotted from STAR1 once at
 * entry so that a single call can handle multiple simultaneous flags atomically.
 *
 * Register-pointer protocol (same as most I2C sensor/EEPROM devices):
 *   WRITE transaction (master → slave):
 *     1st byte after address+W  = register offset → latched in slave_offset.
 *     Subsequent payload bytes  = written into raw_data[] at slave_position
 *                                 only if slave_position >= RESULT_RW_OFFSET
 *                                 (the writable LED region). Read-only bytes
 *                                 are silently discarded.
 *     STOP: flag cleared so the peripheral is ready for the next transaction.
 *
 *   READ transaction (master ← slave), typically after a write to set the offset:
 *     On each TXE interrupt the master is clocking out one byte. We send
 *     raw_data[slave_position] and advance slave_position. The master signals
 *     the end of the read with a NACK.
 *
 *   Repeated-START (write offset, then read without a STOP in between):
 *     The ADDR event fires again; slave_position is reset to slave_offset so
 *     the read starts at the register the master just pointed to.
 */
static void i2c_slave_process(void)
{
    uint32_t flag1 = 0, flag2 = 0;

    /* Snapshot all pending event flags in one read to avoid races. */
    flag1 = I2C1->STAR1;

    /* ADDR: our slave address was matched on the bus (start of any transaction).
     * Reset slave_position to slave_offset so that a repeated-START read begins
     * at the register the master last wrote, without needing a new WRITE phase.
     * Set flag_slave_first_write so the next RXNE byte is treated as the
     * register pointer rather than payload data.
     */
    if (flag1 & I2C_STAR1_ADDR)
    {
        state.slave_position = state.slave_offset;
        state.flag_slave_first_write = 1;
    }

    /* RXNE: receive data register not empty — master sent a byte.
     * The first byte after address+W is the register pointer; every byte
     * after that is payload to be written into the register map.
     */
    if (flag1 & I2C_STAR1_RXNE)
    {
        uint8_t byte = I2C_ReceiveData(I2C1);
        if (state.flag_slave_first_write)
        {
            /* Register pointer: latch it as both the persistent offset (used to
             * reset slave_position on repeated-START) and the current cursor.
             */
            state.slave_offset = byte;
            state.slave_position = byte;
            state.flag_slave_first_write = 0;
            PRINT("I2C reg: 0x%02x\r\n", byte);
        }
        else
        {
            if (state.slave_position < RESULT_BUFFER_SIZE)
            {
                if (state.slave_position >= RESULT_RW_OFFSET)
                {
                    /* Writable region (LED data): store the byte and notify the main loop. */
                    state.raw_data[state.slave_position] = byte;
                    state.flag_update_leds = 1;
                }
                else
                {
                    /* Read-only region: discard the byte silently.
                     * slave_position is still incremented below so the cursor advances
                     * even though we did not write, keeping alignment for any further bytes.
                     */
                    PRINT("ERROR: trying to write 0x%x to readonly data: 0x%x\r\n", byte, state.slave_position);
                }
            }
            state.slave_position++;
        }
    }

    /* Process transmitting data (master is reading from us).
     * Send one byte from raw_data[] at the current pointer position and advance
     * the pointer so consecutive TXE interrupts walk through the register file.
     * If slave_position is out of range, send 0x00 as a safe dummy byte.
     */
    if (flag1 & I2C_STAR1_TXE)
    {
        if (state.slave_position < RESULT_BUFFER_SIZE)
        {
            I2C_SendData(I2C1, state.raw_data[state.slave_position++]);
        }
        else
        {
            /* send dummy data */
            I2C_SendData(I2C1, 0x00);
        }
    }

    /* STOPF: master issued a STOP condition, ending the current transaction.
     * Hardware clears STOPF by: read STAR1 (done above) then write CTLR1.
     */
    if (flag1 & I2C_STAR1_STOPF)
    {
        PRINT("I2C STOP\r\n");
        /* writing CTLR1 after reading STAR1 clears STOPF */
        I2C1->CTLR1 &= ~(I2C_CTLR1_STOP);
    }

    /* Reading STAR2 releases clock stretching so the master can continue.
     * The dummy cast suppresses the unused-variable warning.
     */
    flag2 = I2C1->STAR2;
    (void)flag2;
}

/* initialize the I2C interface */
static void IIC_Init(uint32_t bound, uint16_t address)
{
    GPIO_InitTypeDef GPIO_InitStructure = {0};
    I2C_InitTypeDef I2C_InitStructure = {0};
    NVIC_InitTypeDef NVIC_InitStruct = {0};

    /* enable I2C1 and GPIOC clocks */
    RCC_APB2PeriphClockCmd(RCC_APB2Periph_AFIO | RCC_APB2Periph_GPIOC, ENABLE);
    RCC_APB1PeriphClockCmd(RCC_APB1Periph_I2C1, ENABLE);

    /* remap PC18/PC19 to I2C1 SDA/SCL */
    GPIO_PinRemapConfig(GPIO_PartialRemap3_I2C1, ENABLE); /* 011: Mapping (SCL/PC19, SDA/PC18) */

    /* Disable DIO (SWD) interface on these pins */
    GPIO_PinRemapConfig(GPIO_Remap_SWJ_Disable, ENABLE);

    /* configure the GPIO as SDA/SCL pins */
    GPIO_InitStructure.GPIO_Pin = SDA_PIN | SCL_PIN;
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_AF_PP; /* automatic open-drain */
    GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_Init(GPIOC, &GPIO_InitStructure);

    /* configure I2C1 */
    I2C_InitStructure.I2C_ClockSpeed = bound;                                 /* bus speed */
    I2C_InitStructure.I2C_Mode = I2C_Mode_I2C;                                /* there is only 1 mode */
    I2C_InitStructure.I2C_DutyCycle = I2C_DutyCycle_16_9;                     /* I2C fast mode Tlow/Thigh = 16/9 */
    I2C_InitStructure.I2C_OwnAddress1 = address << 1;                         /* 7 or 10 bit address */
    I2C_InitStructure.I2C_Ack = I2C_Ack_Enable;                               /* automatic acknowledge */
    I2C_InitStructure.I2C_AcknowledgedAddress = I2C_AcknowledgedAddress_7bit; /* use 7 bit address */
    I2C_Init(I2C1, &I2C_InitStructure);

    /* configure I2C interrupts */
    NVIC_InitStruct.NVIC_IRQChannel = I2C1_EV_IRQn;
    NVIC_InitStruct.NVIC_IRQChannelPreemptionPriority = 0;
    NVIC_InitStruct.NVIC_IRQChannelSubPriority = 1;
    NVIC_InitStruct.NVIC_IRQChannelCmd = ENABLE;
    NVIC_Init(&NVIC_InitStruct);

    NVIC_InitStruct.NVIC_IRQChannel = I2C1_ER_IRQn;
    NVIC_InitStruct.NVIC_IRQChannelPreemptionPriority = 0;
    NVIC_InitStruct.NVIC_IRQChannelSubPriority = 1;
    NVIC_InitStruct.NVIC_IRQChannelCmd = ENABLE;
    NVIC_Init(&NVIC_InitStruct);

    /* Enable I2C event, error, and buffer interrupts.
     * EVT fires on: address match, byte received, byte transmitted, stop detected.
     * ERR fires on: bus error, arbitration lost, acknowledge failure, etc.
     * BUF fires on: TXE/RXNE (needed so we get an interrupt for each data byte).
     */
    I2C_ITConfig(I2C1, I2C_IT_EVT | I2C_IT_ERR | I2C_IT_BUF, ENABLE); /* TODO: also I2C_IT_BUF? */

    /* Clock stretching: allow the slave to hold SCL low if it is not ready.
     * This prevents data loss when the interrupt handler is slightly slow.
     */
    I2C_StretchClockCmd(I2C1, ENABLE);

    /* enable I2C1 */
    I2C_Cmd(I2C1, ENABLE);
}

/*
 * Drive one column of the button matrix LOW (active) and all others HIGH.
 * Columns are open-drain outputs; a LOW output "selects" that column.
 * Rows (inputs with pull-ups) will read LOW for any button pressed in that column.
 * The default case (invalid col) sets all columns HIGH, de-selecting the matrix.
 */
static void Matrix_Set_Col(uint8_t col)
{
    switch (col)
    {
        case 0:
            GPIO_WriteBit(GPIOB, BUTTON_COL1_PIN | BUTTON_COL2_PIN, Bit_SET);
            GPIO_WriteBit(GPIOB, BUTTON_COL0_PIN, Bit_RESET);
            break;
        case 1:
            GPIO_WriteBit(GPIOB, BUTTON_COL0_PIN | BUTTON_COL2_PIN, Bit_SET);
            GPIO_WriteBit(GPIOB, BUTTON_COL1_PIN, Bit_RESET);
            break;
        case 2:
            GPIO_WriteBit(GPIOB, BUTTON_COL0_PIN | BUTTON_COL1_PIN, Bit_SET);
            GPIO_WriteBit(GPIOB, BUTTON_COL2_PIN, Bit_RESET);
            break;
        default:
            GPIO_WriteBit(GPIOB, BUTTON_COL0_PIN | BUTTON_COL1_PIN | BUTTON_COL2_PIN, Bit_SET);
    }
}

/*
 * Read the three row GPIO inputs and pack the pressed buttons for the given
 * column into the correct bit positions for matrix_state.
 *
 * Rows are active LOW (pulled HIGH when no button pressed; LOW when pressed).
 * The three row bits for this column are shifted left by (col * 3) so that:
 *   col 0 → bits [2:0]
 *   col 1 → bits [5:3]
 *   col 2 → bits [8:6]  ← bit 8 overflows uint8_t, so col2/row2 is dropped
 *
 * @param col  currently active column (0–2)
 * @param b    snapshot of GPIOB input register (contains ROW2)
 * @param c    snapshot of GPIOC input register (contains ROW0 and ROW1)
 * @return     bitmask of pressed buttons for this column, in their final position
 */
static uint8_t io_to_scan_result(uint8_t col, uint32_t b, uint32_t c)
{
    uint8_t out = 0;

    /* ROW2 is on GPIOB; a LOW reading means the button is pressed */
    if (!(b & (BUTTON_ROW2_PIN)))
    {
        out |= 1;
    }
    out <<= 1;

    /* ROW1 is on GPIOC */
    if (!(c & (BUTTON_ROW1_PIN)))
    {
        out |= 1;
    }
    out <<= 1;

    /* ROW0 is on GPIOC */
    if (!(c & (BUTTON_ROW0_PIN)))
    {
        out |= 1;
    }
    out <<= (col * 3); /* shift into the correct bit range for this column */

    /* Note: for col=2, the ROW2 bit ends up at bit position 8, which is
     * silently dropped when stored in the uint8_t matrix_state. This means
     * the 9th button (col2/row2) is not tracked.
     */

    return out;
}

/*********************************************************************
 * @fn      Matrix_Scan
 *
 * @brief   Scan one step of the button matrix, called from TIM3 ISR at 100 Hz.
 *
 * The scan uses a two-sample debounce scheme per column:
 *   - every 50ms (every 5th call): take a first sample and save it.
 *   - every 100ms (every 10th call): take a second sample. If both match,
 *     the reading is considered stable and written into scan_result.
 *   - After all 3 columns are scanned, update matrix_state if changed.
 *
 * Each full matrix scan takes 100ms to complete.
 *
 * @return  none
 */
static void Matrix_Scan(void)
{
    static uint8_t scan_cnt = 0;          /* tick counter, resets every 10 ticks */
    static uint8_t scan_col = 0;          /* column currently being scanned (0–2) */
    static uint8_t scan_result = 0;       /* accumulated button state for all scanned columns */
    static uint8_t previous_col_scan = 0; /* first sample, taken at scan_cnt % 5 */

    scan_cnt++;
    if ((scan_cnt % 10) == 0)
    {
        /* Second sample: compare with the first sample taken at scan_cnt % 5 */
        scan_cnt = 0;

        uint8_t col_scan = io_to_scan_result(scan_col, GPIO_ReadInputData(GPIOB), GPIO_ReadInputData(GPIOC));
        if (col_scan == previous_col_scan)
        {
            /* Both samples agree: the button state for this column is stable.
             * OR the stable bits into the accumulated result (other columns
             * contribute their own bits via their own scan iterations).
             */
            scan_result |= col_scan;
        }
        else
        {
            /* Samples disagree (button bouncing). Discard this column result.
             * TODO: ideally we should keep the previously-confirmed state here
             */
        }

        /* Advance to the next column and activate it.
         * The column change happens here, so the GPIO has the full inter-scan
         * period to settle before we read it again.
         */
        scan_col = (scan_col + 1) % N_COLS;
        Matrix_Set_Col(scan_col);

        /* When scan_col wraps back to 0, all 3 columns have been scanned */
        if (scan_col == 0)
        {
            /* Only update and notify if the button state actually changed */
            if (state.data.matrix_state != scan_result)
            {
                state.data.matrix_state = scan_result;
                state.flag_matrix_scan_done = 1; /* signal the main loop */
            }
            /* Reset accumulated result for the next full scan cycle */
            scan_result = 0;
        }
    }
    else if ((scan_cnt % 5) == 0)
    {
        /* First sample: snapshot the current rows state for this column */
        previous_col_scan = io_to_scan_result(scan_col, GPIO_ReadInputData(GPIOB), GPIO_ReadInputData(GPIOC));
    }
}

/*********************************************************************
 * @fn      Matrix_Init
 *
 * @brief   Initialize matrix gpio and timer3 for button matrix scan
 *
 * @param   arr - The specific period value
 *          psc - The specifies prescaler value
 *
 * @return  none
 */
static void Matrix_Init(uint16_t arr, uint16_t psc)
{
    GPIO_InitTypeDef GPIO_InitStructure = {0};
    TIM_TimeBaseInitTypeDef TIM_TimeBaseStructure = {0};
    NVIC_InitTypeDef NVIC_InitStructure = {0};

    /* Enable GPIOB clock */
    RCC_APB2PeriphClockCmd(RCC_APB2Periph_AFIO | RCC_APB2Periph_GPIOB, ENABLE);

    /* Enable Timer3 Clock */
    RCC_APB1PeriphClockCmd(RCC_APB1Periph_TIM3, ENABLE);

    /* the columns are the outputs */
    GPIO_InitStructure.GPIO_Pin = BUTTON_COL0_PIN | BUTTON_COL1_PIN | BUTTON_COL2_PIN;
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_Out_PP;
    GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_Init(GPIOB, &GPIO_InitStructure);

    /* the rows are the inputs */
    GPIO_InitStructure.GPIO_Pin = BUTTON_ROW0_PIN | BUTTON_ROW1_PIN;
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_IPU;
    GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_Init(GPIOC, &GPIO_InitStructure);

    GPIO_InitStructure.GPIO_Pin = BUTTON_ROW2_PIN;
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_IPU;
    GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_Init(BUTTON_ROW2_PORT, &GPIO_InitStructure);

    /* Initialize Timer3 */
    TIM_TimeBaseStructure.TIM_Period = arr;
    TIM_TimeBaseStructure.TIM_Prescaler = psc;
    TIM_TimeBaseStructure.TIM_ClockDivision = TIM_CKD_DIV1;
    TIM_TimeBaseStructure.TIM_CounterMode = TIM_CounterMode_Up;
    TIM_TimeBaseInit(TIM3, &TIM_TimeBaseStructure);

    /* enable timer interrupts */
    TIM_ITConfig(TIM3, TIM_IT_Update, ENABLE);

    /* configure timer interrupt */
    NVIC_InitStructure.NVIC_IRQChannel = TIM3_IRQn;
    NVIC_InitStructure.NVIC_IRQChannelPreemptionPriority = 2;
    NVIC_InitStructure.NVIC_IRQChannelSubPriority = 2;
    NVIC_InitStructure.NVIC_IRQChannelCmd = ENABLE;
    NVIC_Init(&NVIC_InitStructure);

    /* activate the first column */
    Matrix_Set_Col(0);

    /* Enable Timer3 */
    TIM_Cmd(TIM3, ENABLE);
}

/*
 * Initialize ADC1 in continuous scan mode with DMA.
 *
 * All 9 analog inputs (potmeters + faders) are measured continuously in a
 * round-robin fashion without CPU involvement. The ADC hardware cycles through
 * the configured channels in rank order and writes each 12-bit result directly
 * into state.data.adc_channels[] via DMA1_Channel1 in circular mode.
 *
 * The ADC clock is divided by 16 to slow it down for stable readings.
 * Results are right-aligned 12-bit values (0–4095).
 */
static void ADC_MultiChannel_Init(void)
{
    ADC_InitTypeDef ADC_InitStructure = {0};
    GPIO_InitTypeDef GPIO_InitStructure = {0};

    RCC_APB2PeriphClockCmd(RCC_APB2Periph_ADC1 | RCC_APB2Periph_GPIOA | RCC_APB2Periph_GPIOB | RCC_APB2Periph_GPIOC, ENABLE);

    GPIO_InitStructure.GPIO_Pin = GPIO_Pin_2 | GPIO_Pin_3 | GPIO_Pin_4 | GPIO_Pin_5 | GPIO_Pin_6;
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_AIN;
    GPIO_Init(GPIOA, &GPIO_InitStructure);

    GPIO_InitStructure.GPIO_Pin = GPIO_Pin_0 | GPIO_Pin_1;
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_AIN;
    GPIO_Init(GPIOB, &GPIO_InitStructure);

    GPIO_InitStructure.GPIO_Pin = GPIO_Pin_0 | GPIO_Pin_3;
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_AIN;
    GPIO_Init(GPIOC, &GPIO_InitStructure);

    /* reset the ADC before we configure it */
    ADC_DeInit(ADC1);

    /* dial down the clock so that we have stable readings */
    ADC_CLKConfig(ADC1, ADC_CLK_Div16);

    /* configure the ADC */
    ADC_InitStructure.ADC_Mode = ADC_Mode_Independent;                  /* operate in independent mode */
    ADC_InitStructure.ADC_ScanConvMode = ENABLE;                        /* scan multiple channels */
    ADC_InitStructure.ADC_ContinuousConvMode = ENABLE;                  /* continuous conversion */
    ADC_InitStructure.ADC_ExternalTrigConv = ADC_ExternalTrigConv_None; /* no external trigger to start the conversion of regular channels */
    ADC_InitStructure.ADC_DataAlign = ADC_DataAlign_Right;              /* right align the ADC data */
    ADC_InitStructure.ADC_NbrOfChannel = ADC_CHANNELS;                  /* number of regular ADC channels to convert */
    ADC_InitStructure.ADC_OutputBuffer = 0;                             /* Not used on CH32X035? set to 0 */
    ADC_InitStructure.ADC_Pga = 0;                                      /* Not used on CH32X035? set to 0 */
    ADC_Init(ADC1, &ADC_InitStructure);

    /* configure the ADC channels */
    ADC_RegularChannelConfig(ADC1, PM_LEFT_TOP_CHANNEL, PM_LEFT_TOP_RANK, ADC_SampleTime_11Cycles);
    ADC_RegularChannelConfig(ADC1, PM_LEFT_MID_CHANNEL, PM_LEFT_MID_RANK, ADC_SampleTime_11Cycles);
    ADC_RegularChannelConfig(ADC1, PM_LEFT_BOTTOM_CHANNEL, PM_LEFT_BOTTOM_RANK, ADC_SampleTime_11Cycles);
    ADC_RegularChannelConfig(ADC1, PM_RIGHT_TOP_CHANNEL, PM_RIGHT_TOP_RANK, ADC_SampleTime_11Cycles);
    ADC_RegularChannelConfig(ADC1, PM_RIGHT_MID_CHANNEL, PM_RIGHT_MID_RANK, ADC_SampleTime_11Cycles);
    ADC_RegularChannelConfig(ADC1, PM_RIGHT_BOTTOM_CHANNEL, PM_RIGHT_BOTTOM_RANK, ADC_SampleTime_11Cycles);
    ADC_RegularChannelConfig(ADC1, SLIDER_LEFT_CHANNEL, SLIDER_LEFT_RANK, ADC_SampleTime_11Cycles);
    ADC_RegularChannelConfig(ADC1, SLIDER_RIGHT_CHANNEL, SLIDER_RIGHT_RANK, ADC_SampleTime_11Cycles);
    ADC_RegularChannelConfig(ADC1, SLIDER_MID_CHANNEL, SLIDER_MID_RANK, ADC_SampleTime_11Cycles);

    /* enable the ADC DMA request */
    ADC_DMACmd(ADC1, ENABLE);

    /* enable the ADC */
    ADC_Cmd(ADC1, ENABLE);
}

/* configure the DMA for ADC reading to memory */
static void DMA_Tx_Init(DMA_Channel_TypeDef *DMA_CHx, uint32_t peripheralAddress, uint32_t memoryAddress, uint16_t bufferSize)
{
    DMA_InitTypeDef DMA_InitStructure = {0};

    RCC_AHBPeriphClockCmd(RCC_AHBPeriph_DMA1, ENABLE);

    DMA_DeInit(DMA_CHx);
    DMA_InitStructure.DMA_PeripheralBaseAddr = peripheralAddress;
    DMA_InitStructure.DMA_MemoryBaseAddr = memoryAddress;
    DMA_InitStructure.DMA_DIR = DMA_DIR_PeripheralSRC; /* peripheral to memory (ADC to buffer) */
    DMA_InitStructure.DMA_BufferSize = bufferSize;
    DMA_InitStructure.DMA_PeripheralInc = DMA_PeripheralInc_Disable;            /* Peripheral address is NOT incremented */
    DMA_InitStructure.DMA_MemoryInc = DMA_MemoryInc_Enable;                     /* Memory address IS incremented (next item in the buffer...) */
    DMA_InitStructure.DMA_PeripheralDataSize = DMA_PeripheralDataSize_HalfWord; /* 16 bits */
    DMA_InitStructure.DMA_MemoryDataSize = DMA_MemoryDataSize_HalfWord;         /* 16 bits */
    DMA_InitStructure.DMA_Mode = DMA_Mode_Circular;
    DMA_InitStructure.DMA_Priority = DMA_Priority_High; /* ws2812 is DMA_Priority_VeryHigh */
    DMA_InitStructure.DMA_M2M = DMA_M2M_Disable;
    DMA_Init(DMA_CHx, &DMA_InitStructure);
}

/* set all leds to the same color */
static void setColor(uint8_t r, uint8_t g, uint8_t b)
{
    for (int i = 0; i < LEDS_NUM; i++)
    {
        state.data.leds[i].r = r;
        state.data.leds[i].g = g;
        state.data.leds[i].b = b;
    }
}

/*
 * Play a short RGB boot animation to confirm the LEDs are working.
 * Cycles red → green → blue, 500 ms each.
 */
static void led_boot_sequence()
{
    setColor(255, 0, 0);
    w2812_sync();
    Delay_Ms(500);
    setColor(0, 255, 0);
    w2812_sync();
    Delay_Ms(500);
    setColor(0, 0, 255);
    w2812_sync();
    Delay_Ms(500);
}

/*
 * Send a 4-byte USB MIDI packet to the host.
 * USB MIDI packets are always 4 bytes:
 *   [0] = Cable number (high nibble) + CIN code (low nibble)
 *   [1] = MIDI status byte (e.g. 0xB0 = CC on channel 0)
 *   [2] = MIDI data byte 1
 *   [3] = MIDI data byte 2
 *
 * In release builds (no DEBUG), the same packet is also echoed over USART3
 * so the badge can receive it via serial.
 */
static void USBSendPacket(uint8_t cin, uint8_t b1, uint8_t b2, uint8_t b3)
{
    uint8_t packet[4];
    packet[0] = (cin & 0x0F); /* Cable 0, CIN in low nibble */
    packet[1] = b1;
    packet[2] = b2;
    packet[3] = b3;
    USB_write(packet, 4);
#ifndef DEBUG
    /* Echo to USART3 for the badge to receive (blocking, byte by byte) */
    for (int i = 0; i < 4; i++)
    {
        while (USART_GetFlagStatus(USART3, USART_FLAG_TC) == RESET)
            ;
        USART_SendData(USART3, packet[i]);
    }
#endif
}

/* The value of the CC is equal to 64 (0x40) plus/minus the number of steps, for
 * example for 10 steps back the value would be 54 (0x36).
 */
static void USBSendControlChange(uint8_t channel, uint8_t control, uint8_t value)
{
    USBSendPacket(0x0B, 0xB0 | (channel & 0x0F), control, value);
}

/*
 * Process one incoming USB MIDI packet received from the host.
 * Only Control Change (0x0B) is acted upon; it maps CC note numbers
 * >= BASE_NOTE_LEDS (0x20) to LED colors using the mixxx_palette.
 * All other message types are logged for debugging and otherwise ignored.
 *
 * @param cin  Code Index Number (low nibble of packet byte 0)
 * @param b1   MIDI status byte
 * @param b2   MIDI data byte 1 (note / control number)
 * @param b3   MIDI data byte 2 (velocity / value)
 */
static void handle_midi(uint8_t cin, uint8_t b1, uint8_t b2, uint8_t b3)
{
    uint8_t channel = b1 & 0x0F;

    if (channel != MIDI_CHANNEL)
    {
        PRINT("we ignore channel %d\r\n", channel);
        return;
    }

    switch (cin)
    {
        case 0x08: /* Note Off */
            PRINT("note off: channel %d, note %d, velocity %d\r\n", channel, b2, b3);
            break;

        case 0x09: /* Note On */
            if (b3 > 0)
            {
                PRINT("note on: channel %d, note %d, velocity %d\r\n", channel, b2, b3);
            }
            else if (b3 == 0)
            {
                PRINT("note off: channel %d, note %d, velocity %d\r\n", channel, b2, b3);
            }
            break;

        case 0x0A: /* Poly Key Pressure */
            PRINT("Poly key pressure: channel %d, note %d, velocity %d\r\n", channel,
                  b2, b3);
            break;

        case 0x0B: /* Control Change */
            /* TODO: use this to change the leds? */
            if (b2 < BASE_NOTE_LEDS)
            {
                /* invalid led index */
                break;
            }

            if ((b2 - BASE_NOTE_LEDS) >= LEDS_NUM)
            {
                /* invalid led index */
                break;
            }

            if (b3 == 0)
            {
                state.data.leds[b2 - BASE_NOTE_LEDS].r = 0;
                state.data.leds[b2 - BASE_NOTE_LEDS].g = 0;
                state.data.leds[b2 - BASE_NOTE_LEDS].b = 0;
            }
            else if (b3 < 10)
            {
                state.data.leds[b2 - BASE_NOTE_LEDS] = mixxx_palette[b3 - 1];
            }
            state.flag_update_leds = 1;
            break;

        case 0x0C: /* Program Change */
            PRINT("Program change: channel %d, b2 0x%x\r\n", channel, b2);
            break;

        case 0x0D: /* Channel Pressure (Aftertouch) */
            PRINT("Channel Pressure (Aftertouch): channel %d, b2 0x%x\r\n", channel,
                  b2);
            break;

        case 0x0E: /* Pitch Bend */
            /* Reconstruct 14-bit value from LSB (b2) and MSB (b3) */
            int val = (b2 & 0x7F) | ((b3 & 0x7F) << 7);
            val -= 8192; /* Center at 0 */
            PRINT("Pitch bend: channel %d, val %d\r\n", channel, val);
            break;

        case 0x0F: /* Single Byte (Real Time) */
            PRINT("single byte: 0x%02x\r\n", b1);
            break;

        /* 0x05 could be SysEx end OR standard 1-byte System Common (Tune Request */
        /* 0xF6) */
        case 0x05:
            if (b1 >= 0xF8)
            { /* If it's real time embedded here (rare but legal) */
                PRINT("SysEx?: 0x%02x\r\n", b1);
            }
            break;

        default:
            /* SysEx (0x04, 0x06, 0x07) and others ignored */
            break;
    }
}

/*
 * Main entry point.
 *
 * Initializes all peripherals, then runs the main event loop forever.
 * The loop is purely polling-based: flags set by interrupt handlers are
 * checked and acted upon each iteration.
 *
 * Initialization order matters: I2C is initialized before SWD is disabled
 * (PC18/PC19 overlap with SWD pins). A 1-second delay before I2C init
 * gives the SWD debugger time to attach and program the chip.
 */
int main(void)
{
    uint8_t usb_midi_packet[4];
    uint8_t previous_kb_result = 0;             /* button state from the previous main loop iteration */
    uint8_t current_kb_result = 0;              /* button state from the latest completed matrix scan */
    uint8_t previous_left_encoder = 0;          /* left encoder position last time we sent a MIDI message */
    uint8_t previous_right_encoder = 0;         /* right encoder position last time we sent a MIDI message */
    uint8_t left_encoder_changed = 0;           /* 1 while the left encoder is actively turning */
    uint8_t right_encoder_changed = 0;          /* 1 while the right encoder is actively turning */
    uint8_t adc_previous_results[ADC_CHANNELS]; /* last ADC values that were sent as MIDI CC */
    uint8_t uart_rx_buf[4];
    uint8_t uart_rx_count = 0;

    /* zero out all state flags and register map data */
    memset(&state, 0, sizeof(addon_state_t));
    memset(adc_previous_results, 0, ADC_CHANNELS);
    memset(&uart_rx_buf, 0, 4);

    /* Write firmware version (built from git tags via VERSION_MAJOR/MINOR/PATCH macros)
     * into the register map so the badge can read it over I2C
     */
    char version_major[] = VERSION_MAJOR;
    char version_minor[] = VERSION_MINOR;
    char version_patch[] = VERSION_PATCH;
    state.data.version[0] = atoi(version_major) & 0xff;
    state.data.version[1] = atoi(version_minor) & 0xff;
    state.data.version[2] = atoi(version_patch) & 0xff;

    SystemInit();
#ifdef NVIC_PriorityGroup_2
    NVIC_PriorityGroupConfig(NVIC_PriorityGroup_2); /* 4 preemption levels, 4 sub-priority levels */
#else
    NVIC_PriorityGroupConfig(NVIC_PriorityGroup_1); /* 2 preemption levels, 8 sub-priority levels */
#endif
    SystemCoreClockUpdate();
    Delay_Init();
    USART3_Output_Init(UART_BAUDRATE);

    /* 1-second window for the SWD debugger to attach before IIC_Init() disables those pins */
    Delay_Ms(1000);

    /* Initialize I2C1 as slave (this call also remaps PC18/PC19 away from SWD) */
    IIC_Init(I2C_SPEED, I2C_ADDRESS);

    PRINT("SystemClk: %u\r\n", (unsigned)SystemCoreClock);
    PRINT("ChipID: %08x\r\n", (unsigned)DBGMCU_GetCHIPID());

    /* TIM3 fires at 100 Hz to drive the button matrix scan state machine */
    Matrix_Init(1, TIMER_FREQ);

    /* SPI1 + DMA for WS2812 LED data */
    SPI_1Lines_HalfDuplex_Init();
    SPI1_DMA_Init();
    DMA_Cmd(SPI1_DMA_TX_CH, ENABLE);

    /* initialize the USB-MIDI interface */
    USB_init();

    /* ADC + DMA: continuously converts all 9 channels into state.data.adc_channels[] */
    ADC_MultiChannel_Init();
    DMA_Tx_Init(DMA1_Channel1, (u32)&ADC1->RDATAR, (u32)state.data.adc_channels, ADC_CHANNELS);
    DMA_Cmd(DMA1_Channel1, ENABLE);
    ADC_SoftwareStartConvCmd(ADC1, ENABLE); /* start the first conversion; continuous mode takes over */

    /* TIM2/TIM1 in encoder mode for left and right scratch encoders */
    Scratch_Left_Encoder_Init();
    TIM_Cmd(SCRATCH_LEFT_TIM, ENABLE);
    Scratch_Right_Encoder_Init();
    TIM_Cmd(SCRATCH_RIGHT_TIM, ENABLE);

    PRINT("DJ Addon Init done\r\n");

    /* Indicate successful boot with a short RGB flash */
    led_boot_sequence();

    /* Turn all LEDs off, ready for host control */
    setColor(0, 0, 0);
    w2812_sync();

    while (1)
    {
        /* Snapshot the latest button matrix result set by the TIM3 ISR */
        if (state.flag_matrix_scan_done)
        {
            state.flag_matrix_scan_done = 0;
            current_kb_result = state.data.matrix_state;
        }

        /* Process any MIDI packets received from the USB host (e.g. from Mixxx) */
        if (USB_available())
        {
            if (USB_read(usb_midi_packet, 4) == 4)
            {
                uint8_t cin = usb_midi_packet[0] & 0x0F;
                uint8_t b1 = usb_midi_packet[1];
                uint8_t b2 = usb_midi_packet[2];
                uint8_t b3 = usb_midi_packet[3];
                handle_midi(cin, b1, b2, b3);
            }
        }

        /* Process any MIDI packets received over USART3 from the Fri3d badge.
         * The badge sends the same 4-byte USB MIDI packet format over serial.
         * Non-blocking: accumulate bytes across loop iterations so a partial
         * packet never stalls the main loop.
         */
        while (USART_GetFlagStatus(USART3, USART_FLAG_RXNE) != RESET)
        {
            uart_rx_buf[uart_rx_count++] = USART_ReceiveData(USART3);
            if (uart_rx_count == 4)
            {
                uart_rx_count = 0;
                uint8_t cin = uart_rx_buf[0] & 0x0F;
                uint8_t b1 = uart_rx_buf[1];
                uint8_t b2 = uart_rx_buf[2];
                uint8_t b3 = uart_rx_buf[3];
                handle_midi(cin, b1, b2, b3);
            }
        }

        /* Encoder reporting: send two CC messages per encoder —
         *   NOTE_ENCODER_x   (position): current counter value 0–127
         *   NOTE_ENCODER_x+1 (activity): MIDI_MAX while turning, 0 when stopped
         * The activity flag lets the host distinguish "still turning" from "stopped".
         */
        if (previous_left_encoder != SCRATCH_LEFT_TIM->CNT)
        {
            state.data.left_encoder = SCRATCH_LEFT_TIM->CNT;
            if (!left_encoder_changed)
            {
                /* Rising edge of rotation: signal that the encoder just started moving */
                left_encoder_changed = 1;
                USBSendControlChange(MIDI_CHANNEL, NOTE_ENCODER_LEFT + 1, MIDI_MAX);
                Delay_Ms(10); /* give it time to send */
            }
            previous_left_encoder = state.data.left_encoder;
            USBSendControlChange(MIDI_CHANNEL, NOTE_ENCODER_LEFT, state.data.left_encoder);
        }
        else
        {
            if (left_encoder_changed)
            {
                /* Falling edge: encoder stopped, clear the activity flag */
                left_encoder_changed = 0;
                USBSendControlChange(MIDI_CHANNEL, NOTE_ENCODER_LEFT + 1, 0);
            }
        }

        if (previous_right_encoder != SCRATCH_RIGHT_TIM->CNT)
        {
            state.data.right_encoder = SCRATCH_RIGHT_TIM->CNT;
            if (!right_encoder_changed)
            {
                /* Rising edge of rotation: signal that the encoder just started moving */
                right_encoder_changed = 1;
                USBSendControlChange(MIDI_CHANNEL, NOTE_ENCODER_RIGHT + 1, MIDI_MAX);
                Delay_Ms(10); /* give it time to send before the position update below */
            }
            previous_right_encoder = state.data.right_encoder;
            USBSendControlChange(MIDI_CHANNEL, NOTE_ENCODER_RIGHT, state.data.right_encoder);
        }
        else
        {
            if (right_encoder_changed)
            {
                right_encoder_changed = 0;
                USBSendControlChange(MIDI_CHANNEL, NOTE_ENCODER_RIGHT + 1, 0);
            }
        }

        /*
         * Process ADC channels and button state changes.
         * We loop over ADC_CHANNELS (9) but only check 8 buttons (i < LEDS_NUM)
         * because the 9th button is not tracked (see matrix design note).
         */
        for (int i = 0; i < ADC_CHANNELS; i++)
        {
            /* Scale the 12-bit ADC value to 7-bit MIDI range.
             * >>5 divides by 32: 4096/32 = 128, which fits in a 7-bit MIDI value.
             * The value is inverted (0xff - new_adc) so that fully clockwise = max.
             */
            uint8_t new_adc = ((state.data.adc_channels[i] >> 5) & 0xff);
            uint8_t diff = (adc_previous_results[i] > new_adc) ? (adc_previous_results[i] - new_adc) : (new_adc - adc_previous_results[i]);
            if (diff > HYSTERESIS)
            {
                /* ADC value changed enough to report; send a CC message */
                USBSendControlChange(MIDI_CHANNEL, base_note_adc[i], 0xff - new_adc);
                adc_previous_results[i] = new_adc;
            }

            /* also check button state changes for the first 8 buttons.
             * button_note[] maps bit index to MIDI CC note
             * value: MIDI_MAX=pressed, 0=released.
             */
            if (i < LEDS_NUM)
            {
                uint8_t current_button_state = (current_kb_result & (1 << i)) & 0xff;
                uint8_t previous_button_state = (previous_kb_result & (1 << i)) & 0xff;

                if (current_button_state != previous_button_state)
                {
                    USBSendControlChange(MIDI_CHANNEL, button_note[i], current_button_state ? MIDI_MAX : 0);
                }
            }
        }

        /* If new LED data arrived via I2C or USB-MIDI, push it to the physical LEDs */
        if (state.flag_update_leds)
        {
            state.flag_update_leds = 0;
            w2812_sync();
        }

        /* Latch the current button state as the new baseline for change detection */
        if (previous_kb_result != current_kb_result)
        {
            previous_kb_result = current_kb_result;
        }
    }
}

/* -----------------------------------------------------------------------
 * Interrupt handlers
 * -----------------------------------------------------------------------
 */

/* TIM3 fires at 100 Hz and drives the button matrix scan state machine */
void TIM3_IRQHandler(void) __attribute__((interrupt("WCH-Interrupt-fast")));
void TIM3_IRQHandler(void)
{
    if (TIM_GetITStatus(TIM3, TIM_IT_Update) != RESET)
    {
        Matrix_Scan();
    }
    TIM_ClearITPendingBit(TIM3, TIM_IT_Update);
}

/* Non-Maskable Interrupt — should never fire in normal operation */
void NMI_Handler(void) __attribute__((interrupt("WCH-Interrupt-fast")));
void NMI_Handler(void)
{
    PRINT("NMI_Handler\r\n");
}

/* Hard Fault — unrecoverable CPU error; log and hang (watchdog will reset) */
void HardFault_Handler(void) __attribute__((interrupt("WCH-Interrupt-fast")));
void HardFault_Handler(void)
{
    PRINT("HARDFAULT\r\n");
    while (1)
    {
    }
}

/* Generic I2C1 IRQ (not used; event/error are handled by the dedicated handlers below) */
void I2C1_IRQHandler(void) __attribute__((interrupt("WCH-Interrupt-fast")));
void I2C1_IRQHandler(void)
{
    PRINT("I2C1_IRQHandler\r\n");
}

/* I2C1 event interrupt: address match, data received/transmitted, stop detected */
void I2C1_EV_IRQHandler(void) __attribute__((interrupt));
void I2C1_EV_IRQHandler(void)
{
    i2c_slave_process();
}

/* I2C1 error interrupt: bus error, arbitration loss, acknowledge failure, etc. */
void I2C1_ER_IRQHandler(void) __attribute__((interrupt));
void I2C1_ER_IRQHandler(void)
{
    uint16_t STAR1 = I2C1->STAR1;
    if (STAR1 & I2C_STAR1_BERR) I2C1->STAR1 &= ~I2C_STAR1_BERR;
    if (STAR1 & I2C_STAR1_ARLO) I2C1->STAR1 &= ~I2C_STAR1_ARLO;
    if (STAR1 & I2C_STAR1_AF) I2C1->STAR1 &= ~I2C_STAR1_AF;
}
