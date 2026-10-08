/*
 *   Copyright 2026 NXP
 *
 *   NXP Proprietary. This software is owned or controlled by NXP and may only be
 *   used strictly in accordance with the applicable license terms.  By expressly
 *   accepting such terms or by downloading, installing, activating and/or otherwise
 *   using the software, you are agreeing that you have read, and that you agree to
 *   comply with and are bound by, such license terms.  If you do not agree to be
 *   bound by the applicable license terms, then you may not retain, install,
 *   activate or otherwise use the software.
 *
 */

/**
*   @file main.c
*
*   @addtogroup main_module main module documentation
*   @{
*/

/*==================================================================================================
*                                        INCLUDE FILES
==================================================================================================*/
#include "Mcal.h"
#include "Clock_Ip.h"
#include "Port_Ci_Port_Ip.h"
#include "OsIf.h"
#include "Flexio_Mcl_Ip.h"
#include "Flexio_I2c_Ip.h"
#include "Lpuart_Uart_Ip.h"
#include "Lpi2c_Ip.h"
#include "Adc_Ip.h"
#include "IntCtrl_Ip.h"
#include "board_gpio.h"
#include <stdbool.h>
#include <string.h>
#include "ssd1306.h"

/*==================================================================================================
*                                     DEFINES
==================================================================================================*/
#define OLED_FLEXIO_INSTANCE    0U
#define OLED_FLEXIO_CHANNEL     0U
#define UART_INSTANCE           1U

/* ADC hardware unit used to sample the Slider 2 Click analog output. */
#define ADCHWUNIT_0_VS_0_INSTANCE 0u

/* Update intervals */
#define DISPLAY_UPDATE_INTERVAL 200U    /* Refresh the OLED value every 200 ms */
#define UART_SEND_INTERVAL      1000U   /* Report slider value over UART every 1 second */

#define UART_MESSAGE_DELAY      100U    /* Delay between UART messages (ms) */

/*==================================================================================================
*                                      GLOBAL VARIABLES
==================================================================================================*/
volatile int exit_code = 0;

volatile boolean notif_triggered = FALSE;
volatile uint16 data = 0U;

/*==================================================================================================
*                                      LOCAL FUNCTIONS
==================================================================================================*/
void LPI2C0_Master_Callback(Lpi2c_Ip_MasterEventType event, uint8 userData){

}

static void Delay(uint32 ms)
{
    uint32 cur = OsIf_GetCounter(OSIF_COUNTER_SYSTEM);
    uint32 elapsed = 0U;
    uint32 timeout = OsIf_MicrosToTicks(ms * 1000U, OSIF_COUNTER_SYSTEM);

    while (elapsed < timeout) {
        elapsed += OsIf_GetElapsed(&cur, OSIF_COUNTER_SYSTEM);
    }
}

static void UART_SendString(const char* str)
{
    uint32 len = 0;
    const char* p = str;

    while (*p != '\0')
    {
        len++;
        p++;
    }

    if (len > 0)
    {
        Lpuart_Uart_Ip_SyncSend(UART_INSTANCE, (const uint8*)str, len, 1000000U);
    }
}

static void UART_SendStringWithDelay(const char* str)
{
    UART_SendString(str);
    Delay(UART_MESSAGE_DELAY);
}

static void IntToString(sint16 value, char* buffer)
{
    uint8 idx = 0;
    sint16 temp = value;
    uint8 digits[6];
    uint8 digitCount = 0;

    if (value < 0)
    {
        buffer[idx++] = '-';
        temp = -value;
    }

    if (temp == 0)
    {
        buffer[idx++] = '0';
    }
    else
    {
        while (temp > 0)
        {
            digits[digitCount++] = (uint8)(temp % 10);
            temp /= 10;
        }

        while (digitCount > 0)
        {
            buffer[idx++] = '0' + digits[--digitCount];
        }
    }

    buffer[idx] = '\0';
}

static void OLED_PeripheralInit(void)
{
    Flexio_Mcl_Ip_InitDevice(&Flexio_Ip_xFlexioInit);
    Flexio_I2c_Ip_MasterInit(OLED_FLEXIO_INSTANCE, OLED_FLEXIO_CHANNEL, &Flexio_I2cMasterChannel0);
    SSD1306_Init();
}

/*!
 * @brief Draw a string horizontally centered in the visible viewport.
 *
 * The built-in font advances 6 px per character at size 1, so each character
 * occupies 6 * size pixels of horizontal space. The visible area is
 * SCREEN_WIDTH pixels wide.
 */
static void OLED_DrawCentered(int y, const char* str, uint8 size)
{
    int len = (int)strlen(str);
    int width = len * 6 * (int)size;
    int x = (SCREEN_WIDTH - width) / 2;

    if (x < 0)
    {
        x = 0;
    }

    SSD1306_DrawString(x, y, str, size, true);
}

static void OLED_ShowStartupScreen(void)
{
    SSD1306_ClearBuffer();
    OLED_DrawCentered(2,  "FRDM-A-S32K118", 1);
    OLED_DrawCentered(13, "Slider 2 Click", 1);
    SSD1306_DrawLine(0, 23, SCREEN_WIDTH - 1, 23, true);
    OLED_DrawCentered(28, "Initializing", 1);
    SSD1306_UpdateScreen();
    Delay(2000U);
}

static void OLED_UpdateDisplay(uint16 sliderValue)
{
    char valueStr[8];

    SSD1306_ClearBuffer();

    /* Header label, centered near the top. */
    OLED_DrawCentered(4, "ADC Value:", 1);

    /* Show the raw slider value centered, size 2 so it fits the 39 px height. */
    IntToString((sint16)sliderValue, valueStr);
    OLED_DrawCentered(20, valueStr, 2);

    SSD1306_UpdateScreen();
}

void AdcConversionCompleteNotif(const uint8 ControlChanIdx)
{
    notif_triggered = TRUE;
    data = Adc_Ip_GetConvData(ADCHWUNIT_0_VS_0_INSTANCE, ControlChanIdx);
}

/*!
 * @brief Trigger one ADC conversion on the slider channel and return the result.
 *
 * @return the 12-bit ADC sample read from the Slider 2 Click analog output.
 */
static uint16 Slider_ReadValue(void)
{
    Adc_Ip_StartConversion(ADCHWUNIT_0_VS_0_INSTANCE, ADC_IP_INPUTCHAN_EXT12, TRUE);

    /* Wait for the conversion complete notification. */
    while (notif_triggered != TRUE);
    notif_triggered = FALSE;

    return data;
}

/*==================================================================================================
*                                          MAIN
==================================================================================================*/

int main(void)
{
    uint16 sliderValue = 0U;

    uint32 lastDisplayUpdateTime = 0;
    uint32 lastUartSendTime = 0;
    uint32 currentTime = 0;

    /* Initialize system clocks */
    Clock_Ip_Init(&Clock_Ip_aClockConfig[0]);

    /* Initialize OS interface for timing */
    OsIf_Init(NULL_PTR);

    /* Initialize GPIO pins */
    Port_Ci_Port_Ip_Init(NUM_OF_CONFIGURED_PINS_PortContainer_0_BOARD_InitPeripherals,
                         g_pin_mux_InitConfigArr_PortContainer_0_BOARD_InitPeripherals);

    /* Initialize UART */
    Lpuart_Uart_Ip_Init(UART_INSTANCE, &Lpuart_Uart_Ip_xHwConfigPB_1);

    /* Initialize and calibrate ADC */
    Adc_Ip_StatusType adcStatus;
    Adc_Ip_Init(ADCHWUNIT_0_VS_0_INSTANCE, &AdcHwUnit_0);

    //enable slider clik
    Board_Gpio_WritePin(D9_PORT, D9_PIN, STD_HIGH);
    //set VREF of slider to 3.3
    Board_Gpio_WritePin(RSL_PORT, RSL_PIN, STD_LOW);

    IntCtrl_Ip_InstallHandler(ADC0_IRQn, Adc_0_Isr, NULL_PTR);
    IntCtrl_Ip_EnableIrq(ADC0_IRQn);

    adcStatus = Adc_Ip_DoCalibration(ADCHWUNIT_0_VS_0_INSTANCE);
    while (adcStatus != ADC_IP_STATUS_SUCCESS)
    {
        adcStatus = Adc_Ip_DoCalibration(ADCHWUNIT_0_VS_0_INSTANCE);
    }

    /* Send startup message */
    Delay(500U);
    UART_SendStringWithDelay("\r\n\r\n");
    UART_SendStringWithDelay("========================================\r\n");
    UART_SendStringWithDelay("  FRDM-A-S32K118 SLIDER 2 CLICK DEMO\r\n");
    UART_SendStringWithDelay("========================================\r\n");
    UART_SendStringWithDelay("  Platform: NXP S32K118\r\n");
    UART_SendStringWithDelay("  Input: Slider 2 Click (ADC)\r\n");
    UART_SendStringWithDelay("  Display: SSD1306 OLED\r\n");
    UART_SendStringWithDelay("========================================\r\n");
    UART_SendStringWithDelay("\r\n");
    UART_SendStringWithDelay("Initializing peripherals...\r\n");

    /* Initialize OLED display */
    UART_SendStringWithDelay("  - Initializing SSD1306 OLED display...\r\n");
    OLED_PeripheralInit();
    UART_SendStringWithDelay("  [OK] OLED display initialized\r\n");
    OLED_ShowStartupScreen();

    /* Initial slider reading */
    UART_SendStringWithDelay("Reading initial slider value...\r\n");
    sliderValue = Slider_ReadValue();
    OLED_UpdateDisplay(sliderValue);

    UART_SendStringWithDelay("System ready. Entering main loop...\r\n");
    UART_SendStringWithDelay("Slider value will be reported every second.\r\n");
    UART_SendStringWithDelay("\r\n");

    /* Main loop */
    for(;;)
    {
        currentTime = OsIf_GetCounter(OSIF_COUNTER_SYSTEM);

        /* Continuously sample the slider. */
        sliderValue = Slider_ReadValue();

        /* Periodic display update */
        if ((currentTime - lastDisplayUpdateTime) >= OsIf_MicrosToTicks(DISPLAY_UPDATE_INTERVAL * 1000U, OSIF_COUNTER_SYSTEM))
        {
            lastDisplayUpdateTime = currentTime;
            OLED_UpdateDisplay(sliderValue);
        }

        /* Periodic UART report */
        if ((currentTime - lastUartSendTime) >= OsIf_MicrosToTicks(UART_SEND_INTERVAL * 1000U, OSIF_COUNTER_SYSTEM))
        {
            char valueStr[8];
            lastUartSendTime = currentTime;

            UART_SendString("  Slider ADC value: ");
            IntToString((sint16)sliderValue, valueStr);
            UART_SendString(valueStr);
            UART_SendString("\r\n");
        }

        if(exit_code != 0)
        {
            break;
        }
    }
    return exit_code;
}

/** @} */
