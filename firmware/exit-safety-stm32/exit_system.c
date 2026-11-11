/**
 ******************************************************************************
 * Project : Smart Parking Infrastructure using Cyber-Physical Systems
 * Module  : Exit Safety Monitoring & Hazard Detection
 * Target  : STM32F4 (STM32 HAL, STM32CubeIDE)
 *
 * Sensors : PIR motion sensor  (PB0)  - pedestrian near the exit lane
 *           Moisture sensor    (PB2)  - wet floor detection
 *           LDR (ADC1 CH0)     (PA0)  - ambient light / night detection
 * Outputs : Exit light LED     (PC13) - switched on at night
 *           MAX7219 8x8 matrix (SPI1: PA5 SCK, PA7 MOSI, PA4 CS)
 *           SSD1306 0.96" OLED (I2C1: PB6 SCL, PB7 SDA) - driver message
 *
 * Priority logic (highest first):
 *   WET FLOOR  >  PEDESTRIAN  >  NIGHT  >  NORMAL
 ******************************************************************************
 */
#include "stm32f4xx_hal.h"
#include <string.h>

/* ================= CONFIGURATION ================= */
#define NIGHT_THRESHOLD   2000U            /* 12-bit ADC: below = dark      */
#define SSD1306_ADDR      (0x3C << 1)      /* 7-bit 0x3C, HAL wants 8-bit   */

typedef enum {
    STATUS_NORMAL = 0,
    STATUS_NIGHT,
    STATUS_PEDESTRIAN,
    STATUS_WET
} SystemStatus;

/* ================= GLOBAL HANDLES ================= */
ADC_HandleTypeDef hadc1;
SPI_HandleTypeDef hspi1;
I2C_HandleTypeDef hi2c1;

/* ================= FUNCTION PROTOTYPES ================= */
void SystemClock_Config(void);
static void GPIO_Init_All(void);
static void ADC1_Init(void);
static void SPI1_Init(void);
static void I2C1_Init(void);

static void MAX7219_Send(uint8_t addr, uint8_t data);
static void MAX7219_Init(void);
static void Display_Status(SystemStatus status);

static void OLED_Command(uint8_t cmd);
static void OLED_Init(void);
static void OLED_Clear(void);
static void OLED_PrintLine(uint8_t page, const char *text);
static void OLED_ShowStatus(SystemStatus status);

/* ================= MAIN ================= */
int main(void)
{
    HAL_Init();
    SystemClock_Config();

    GPIO_Init_All();
    ADC1_Init();
    SPI1_Init();
    I2C1_Init();

    HAL_ADC_Start(&hadc1);
    MAX7219_Init();
    OLED_Init();
    OLED_Clear();

    SystemStatus last = (SystemStatus)0xFF;

    while (1)
    {
        GPIO_PinState pir      = HAL_GPIO_ReadPin(GPIOB, GPIO_PIN_0);
        GPIO_PinState moisture = HAL_GPIO_ReadPin(GPIOB, GPIO_PIN_2);

        HAL_ADC_PollForConversion(&hadc1, HAL_MAX_DELAY);
        uint32_t ldr = HAL_ADC_GetValue(&hadc1);
        uint8_t  is_night = (ldr < NIGHT_THRESHOLD);

        /* Exit-lane light follows ambient light */
        HAL_GPIO_WritePin(GPIOC, GPIO_PIN_13, is_night ? GPIO_PIN_SET : GPIO_PIN_RESET);

        /* Priority-based hazard classification */
        SystemStatus status;
        if (moisture == GPIO_PIN_SET)      status = STATUS_WET;
        else if (pir == GPIO_PIN_SET)      status = STATUS_PEDESTRIAN;
        else if (is_night)                 status = STATUS_NIGHT;
        else                               status = STATUS_NORMAL;

        Display_Status(status);
        if (status != last) {              /* redraw OLED only on change */
            OLED_ShowStatus(status);
            last = status;
        }

        HAL_Delay(300);
    }
}

/* ================= CLOCK: 84 MHz from HSI ================= */
void SystemClock_Config(void)
{
    RCC_OscInitTypeDef osc = {0};
    RCC_ClkInitTypeDef clk = {0};

    __HAL_RCC_PWR_CLK_ENABLE();
    __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE2);

    osc.OscillatorType      = RCC_OSCILLATORTYPE_HSI;
    osc.HSIState            = RCC_HSI_ON;
    osc.HSICalibrationValue = RCC_HSICALIBRATION_DEFAULT;
    osc.PLL.PLLState        = RCC_PLL_ON;
    osc.PLL.PLLSource       = RCC_PLLSOURCE_HSI;
    osc.PLL.PLLM            = 16;
    osc.PLL.PLLN            = 336;
    osc.PLL.PLLP            = RCC_PLLP_DIV4;
    osc.PLL.PLLQ            = 7;
    HAL_RCC_OscConfig(&osc);

    clk.ClockType      = RCC_CLOCKTYPE_HCLK | RCC_CLOCKTYPE_SYSCLK |
                         RCC_CLOCKTYPE_PCLK1 | RCC_CLOCKTYPE_PCLK2;
    clk.SYSCLKSource   = RCC_SYSCLKSOURCE_PLLCLK;
    clk.AHBCLKDivider  = RCC_SYSCLK_DIV1;
    clk.APB1CLKDivider = RCC_HCLK_DIV2;
    clk.APB2CLKDivider = RCC_HCLK_DIV1;
    HAL_RCC_ClockConfig(&clk, FLASH_LATENCY_2);
}

/* ================= GPIO ================= */
static void GPIO_Init_All(void)
{
    GPIO_InitTypeDef gpio = {0};

    __HAL_RCC_GPIOA_CLK_ENABLE();
    __HAL_RCC_GPIOB_CLK_ENABLE();
    __HAL_RCC_GPIOC_CLK_ENABLE();

    /* PIR (PB0) + moisture (PB2) inputs */
    gpio.Pin  = GPIO_PIN_0 | GPIO_PIN_2;
    gpio.Mode = GPIO_MODE_INPUT;
    gpio.Pull = GPIO_NOPULL;
    HAL_GPIO_Init(GPIOB, &gpio);

    /* Exit light LED (PC13) */
    gpio.Pin   = GPIO_PIN_13;
    gpio.Mode  = GPIO_MODE_OUTPUT_PP;
    gpio.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(GPIOC, &gpio);

    /* MAX7219 chip select (PA4), idle high */
    gpio.Pin = GPIO_PIN_4;
    HAL_GPIO_Init(GPIOA, &gpio);
    HAL_GPIO_WritePin(GPIOA, GPIO_PIN_4, GPIO_PIN_SET);
}

/* ================= ADC: LDR on PA0 ================= */
static void ADC1_Init(void)
{
    ADC_ChannelConfTypeDef ch = {0};

    hadc1.Instance                   = ADC1;
    hadc1.Init.ClockPrescaler        = ADC_CLOCK_SYNC_PCLK_DIV4;
    hadc1.Init.Resolution            = ADC_RESOLUTION_12B;
    hadc1.Init.ScanConvMode          = DISABLE;
    hadc1.Init.ContinuousConvMode    = ENABLE;
    hadc1.Init.ExternalTrigConvEdge  = ADC_EXTERNALTRIGCONVEDGE_NONE;
    hadc1.Init.DataAlign             = ADC_DATAALIGN_RIGHT;
    hadc1.Init.NbrOfConversion       = 1;
    hadc1.Init.EOCSelection          = ADC_EOC_SINGLE_CONV;
    HAL_ADC_Init(&hadc1);

    ch.Channel      = ADC_CHANNEL_0;
    ch.Rank         = 1;
    ch.SamplingTime = ADC_SAMPLETIME_144CYCLES;
    HAL_ADC_ConfigChannel(&hadc1, &ch);
}

/* ================= SPI1: MAX7219 ================= */
static void SPI1_Init(void)
{
    hspi1.Instance               = SPI1;
    hspi1.Init.Mode              = SPI_MODE_MASTER;
    hspi1.Init.Direction         = SPI_DIRECTION_2LINES;
    hspi1.Init.DataSize          = SPI_DATASIZE_8BIT;
    hspi1.Init.CLKPolarity       = SPI_POLARITY_LOW;
    hspi1.Init.CLKPhase          = SPI_PHASE_1EDGE;
    hspi1.Init.NSS               = SPI_NSS_SOFT;
    hspi1.Init.BaudRatePrescaler = SPI_BAUDRATEPRESCALER_16;
    hspi1.Init.FirstBit          = SPI_FIRSTBIT_MSB;
    HAL_SPI_Init(&hspi1);
}

/* ================= I2C1: SSD1306 OLED ================= */
static void I2C1_Init(void)
{
    hi2c1.Instance             = I2C1;
    hi2c1.Init.ClockSpeed      = 400000;
    hi2c1.Init.DutyCycle       = I2C_DUTYCYCLE_2;
    hi2c1.Init.OwnAddress1     = 0;
    hi2c1.Init.AddressingMode  = I2C_ADDRESSINGMODE_7BIT;
    hi2c1.Init.DualAddressMode = I2C_DUALADDRESS_DISABLE;
    hi2c1.Init.GeneralCallMode = I2C_GENERALCALL_DISABLE;
    hi2c1.Init.NoStretchMode   = I2C_NOSTRETCH_DISABLE;
    HAL_I2C_Init(&hi2c1);
}

/* ================= HAL MSP: peripheral pin muxing ================= */
void HAL_ADC_MspInit(ADC_HandleTypeDef *hadc)
{
    if (hadc->Instance != ADC1) return;
    GPIO_InitTypeDef gpio = {0};
    __HAL_RCC_ADC1_CLK_ENABLE();
    __HAL_RCC_GPIOA_CLK_ENABLE();
    gpio.Pin  = GPIO_PIN_0;                     /* PA0 = ADC1_IN0 (LDR) */
    gpio.Mode = GPIO_MODE_ANALOG;
    gpio.Pull = GPIO_NOPULL;
    HAL_GPIO_Init(GPIOA, &gpio);
}

void HAL_SPI_MspInit(SPI_HandleTypeDef *hspi)
{
    if (hspi->Instance != SPI1) return;
    GPIO_InitTypeDef gpio = {0};
    __HAL_RCC_SPI1_CLK_ENABLE();
    __HAL_RCC_GPIOA_CLK_ENABLE();
    gpio.Pin       = GPIO_PIN_5 | GPIO_PIN_7;   /* PA5 SCK, PA7 MOSI */
    gpio.Mode      = GPIO_MODE_AF_PP;
    gpio.Pull      = GPIO_NOPULL;
    gpio.Speed     = GPIO_SPEED_FREQ_HIGH;
    gpio.Alternate = GPIO_AF5_SPI1;
    HAL_GPIO_Init(GPIOA, &gpio);
}

void HAL_I2C_MspInit(I2C_HandleTypeDef *hi2c)
{
    if (hi2c->Instance != I2C1) return;
    GPIO_InitTypeDef gpio = {0};
    __HAL_RCC_GPIOB_CLK_ENABLE();
    gpio.Pin       = GPIO_PIN_6 | GPIO_PIN_7;   /* PB6 SCL, PB7 SDA */
    gpio.Mode      = GPIO_MODE_AF_OD;
    gpio.Pull      = GPIO_PULLUP;
    gpio.Speed     = GPIO_SPEED_FREQ_HIGH;
    gpio.Alternate = GPIO_AF4_I2C1;
    HAL_GPIO_Init(GPIOB, &gpio);
    __HAL_RCC_I2C1_CLK_ENABLE();
}

/* SysTick drives HAL_Delay / HAL_GetTick */
void SysTick_Handler(void)
{
    HAL_IncTick();
}

/* ================= MAX7219 LED MATRIX ================= */
static void MAX7219_Send(uint8_t addr, uint8_t data)
{
    uint8_t tx[2] = {addr, data};
    HAL_GPIO_WritePin(GPIOA, GPIO_PIN_4, GPIO_PIN_RESET);
    HAL_SPI_Transmit(&hspi1, tx, 2, HAL_MAX_DELAY);
    HAL_GPIO_WritePin(GPIOA, GPIO_PIN_4, GPIO_PIN_SET);
}

static void MAX7219_Init(void)
{
    MAX7219_Send(0x09, 0x00);   /* decode mode: none (raw rows) */
    MAX7219_Send(0x0A, 0x08);   /* intensity: medium            */
    MAX7219_Send(0x0B, 0x07);   /* scan limit: all 8 rows       */
    MAX7219_Send(0x0C, 0x01);   /* normal operation             */
    MAX7219_Send(0x0F, 0x00);   /* display test off             */
}

static void Display_Status(SystemStatus status)
{
    for (uint8_t row = 1; row <= 8; row++)
        MAX7219_Send(row, 0x00);

    switch (status) {
    case STATUS_NIGHT:      MAX7219_Send(1, 0x18); break;
    case STATUS_PEDESTRIAN: MAX7219_Send(2, 0x3C); break;
    case STATUS_WET:        MAX7219_Send(3, 0x7E); break;
    default:                MAX7219_Send(4, 0xFF); break;
    }
}

/* ================= SSD1306 OLED ================= */
/* 5x7 font, columns LSB = top row. Space, '!', '-', 0-9, A-Z */
static const uint8_t FONT_5X7[][5] = {
    {0x00,0x00,0x00,0x00,0x00}, /* ' ' */
    {0x00,0x00,0x5F,0x00,0x00}, /* '!' */
    {0x08,0x08,0x08,0x08,0x08}, /* '-' */
    {0x3E,0x51,0x49,0x45,0x3E}, {0x00,0x42,0x7F,0x40,0x00}, /* 0 1 */
    {0x42,0x61,0x51,0x49,0x46}, {0x21,0x41,0x45,0x4B,0x31}, /* 2 3 */
    {0x18,0x14,0x12,0x7F,0x10}, {0x27,0x45,0x45,0x45,0x39}, /* 4 5 */
    {0x3C,0x4A,0x49,0x49,0x30}, {0x01,0x71,0x09,0x05,0x03}, /* 6 7 */
    {0x36,0x49,0x49,0x49,0x36}, {0x06,0x49,0x49,0x29,0x1E}, /* 8 9 */
    {0x7E,0x11,0x11,0x11,0x7E}, {0x7F,0x49,0x49,0x49,0x36}, /* A B */
    {0x3E,0x41,0x41,0x41,0x22}, {0x7F,0x41,0x41,0x22,0x1C}, /* C D */
    {0x7F,0x49,0x49,0x49,0x41}, {0x7F,0x09,0x09,0x09,0x01}, /* E F */
    {0x3E,0x41,0x49,0x49,0x7A}, {0x7F,0x08,0x08,0x08,0x7F}, /* G H */
    {0x00,0x41,0x7F,0x41,0x00}, {0x20,0x40,0x41,0x3F,0x01}, /* I J */
    {0x7F,0x08,0x14,0x22,0x41}, {0x7F,0x40,0x40,0x40,0x40}, /* K L */
    {0x7F,0x02,0x0C,0x02,0x7F}, {0x7F,0x04,0x08,0x10,0x7F}, /* M N */
    {0x3E,0x41,0x41,0x41,0x3E}, {0x7F,0x09,0x09,0x09,0x06}, /* O P */
    {0x3E,0x41,0x51,0x21,0x5E}, {0x7F,0x09,0x19,0x29,0x46}, /* Q R */
    {0x46,0x49,0x49,0x49,0x31}, {0x01,0x01,0x7F,0x01,0x01}, /* S T */
    {0x3F,0x40,0x40,0x40,0x3F}, {0x1F,0x20,0x40,0x20,0x1F}, /* U V */
    {0x3F,0x40,0x38,0x40,0x3F}, {0x63,0x14,0x08,0x14,0x63}, /* W X */
    {0x07,0x08,0x70,0x08,0x07}, {0x61,0x51,0x49,0x45,0x43}, /* Y Z */
};

static const uint8_t *Font_Glyph(char c)
{
    if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
    if (c >= 'A' && c <= 'Z') return FONT_5X7[13 + (c - 'A')];
    if (c >= '0' && c <= '9') return FONT_5X7[3 + (c - '0')];
    if (c == '!') return FONT_5X7[1];
    if (c == '-') return FONT_5X7[2];
    return FONT_5X7[0];
}

static void OLED_Command(uint8_t cmd)
{
    uint8_t buf[2] = {0x00, cmd};
    HAL_I2C_Master_Transmit(&hi2c1, SSD1306_ADDR, buf, 2, HAL_MAX_DELAY);
}

static void OLED_SetCursor(uint8_t page, uint8_t col)
{
    OLED_Command(0xB0 + page);
    OLED_Command(0x00 | (col & 0x0F));
    OLED_Command(0x10 | (col >> 4));
}

static void OLED_Init(void)
{
    static const uint8_t init_seq[] = {
        0xAE,             /* display off                 */
        0x20, 0x02,       /* page addressing mode        */
        0xC8, 0xA1,       /* flip vertically/horizontally */
        0x40,             /* start line 0                */
        0x81, 0x7F,       /* contrast                    */
        0xA6,             /* normal (not inverted)       */
        0xA8, 0x3F,       /* multiplex 1/64              */
        0xD3, 0x00,       /* display offset 0            */
        0xD5, 0x80,       /* clock divide                */
        0xD9, 0xF1,       /* pre-charge                  */
        0xDA, 0x12,       /* COM pins                    */
        0xDB, 0x40,       /* VCOMH                       */
        0x8D, 0x14,       /* charge pump on              */
        0xA4,             /* follow RAM                  */
        0xAF              /* display on                  */
    };
    HAL_Delay(100);
    for (uint32_t i = 0; i < sizeof(init_seq); i++)
        OLED_Command(init_seq[i]);
}

static void OLED_Clear(void)
{
    uint8_t line[129];
    line[0] = 0x40;                       /* data control byte */
    memset(&line[1], 0x00, 128);
    for (uint8_t page = 0; page < 8; page++) {
        OLED_SetCursor(page, 0);
        HAL_I2C_Master_Transmit(&hi2c1, SSD1306_ADDR, line, sizeof(line), HAL_MAX_DELAY);
    }
}

/* Print up to 21 characters, centred, on one 8-pixel page */
static void OLED_PrintLine(uint8_t page, const char *text)
{
    uint8_t buf[129];
    size_t len = strlen(text);
    if (len > 21) len = 21;

    buf[0] = 0x40;
    memset(&buf[1], 0x00, 128);
    uint8_t x = (uint8_t)((128 - len * 6) / 2);
    for (size_t i = 0; i < len; i++) {
        memcpy(&buf[1 + x], Font_Glyph(text[i]), 5);
        x += 6;
    }
    OLED_SetCursor(page, 0);
    HAL_I2C_Master_Transmit(&hi2c1, SSD1306_ADDR, buf, sizeof(buf), HAL_MAX_DELAY);
}

static void OLED_ShowStatus(SystemStatus status)
{
    OLED_Clear();
    OLED_PrintLine(1, "EXIT MONITOR");
    switch (status) {
    case STATUS_WET:
        OLED_PrintLine(3, "CAUTION!");
        OLED_PrintLine(5, "WET FLOOR");
        break;
    case STATUS_PEDESTRIAN:
        OLED_PrintLine(3, "PEDESTRIAN AHEAD");
        OLED_PrintLine(5, "SLOW DOWN");
        break;
    case STATUS_NIGHT:
        OLED_PrintLine(3, "LOW LIGHT");
        OLED_PrintLine(5, "LIGHTS ON");
        break;
    default:
        OLED_PrintLine(3, "ALL CLEAR");
        OLED_PrintLine(5, "DRIVE SAFE");
        break;
    }
}
