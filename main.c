// Standard includes
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

// Simplelink includes
#include "simplelink.h"

// Driverlib includes
#include "rom.h"
#include "rom_map.h"
#include "hw_memmap.h"
#include "hw_common_reg.h"
#include "hw_types.h"
#include "hw_ints.h"
#include "uart.h"
#include "spi.h"
#include "interrupt.h"
#include "pin_mux_config.h"
#include "utils.h"
#include "prcm.h"
#include "hw_apps_rcm.h"
#include "gpio.h"
#include "hw_nvic.h"
#include "systick.h"

// Common interface include
#include "uart_if.h"
#include "gpio_if.h"
#include "common.h"
#include "Adafruit_GFX.h"
#include "Adafruit_SSD1351.h"

// Custom includes
#include "utils/network_utils.h"

//*****************************************************************************
//                          MACROS
//*****************************************************************************
//NEED TO UPDATE THIS FOR IT TO WORK!
#define DATE                27    /* Current Date */
#define MONTH               2     /* Month 1-12 */
#define YEAR                2025  /* Current year */
#define HOUR                12    /* Time - hours */
#define MINUTE              18    /* Time - minutes */
#define SECOND              0     /* Time - seconds */

#define APPLICATION_VERSION  "WQ25"
#define APP_NAME             "SSL + IR Decoding"
#define CONSOLE              UARTA0_BASE
#define SPI_IF_BIT_RATE      100000
#define SERVER_NAME          "52.25.173.10"
#define GOOGLE_DST_PORT      8443

#define POSTHEADER "POST /things/LAB4_AWS/shadow HTTP/1.1\r\n"             // CHANGE ME
#define GETHEADER "GET /things/LAB4_AWS/shadow HTTP/1.1\r\n"
#define HOSTHEADER "Host: ahvzuro29rftq-ats.iot.us-west-2.amazonaws.com\r\n"        // CHANGE ME
#define CHEADER "Connection: Keep-Alive\r\n"
#define CTHEADER "Content-Type: application/json; charset=utf-8\r\n"
#define CLHEADER1 "Content-Length: "
#define CLHEADER2 "\r\n\r\n"

//*****************************************************************************
//                 GLOBAL VARIABLES -- Start
//*****************************************************************************
// Binary & HEX from manual decoding
    /* Button 0     =   0110 1110 1111 | 6EF        // SPACE
       Button 1     =   1111 1110 1111 | FEF
       Button 2     =   0111 1110 1111 | 7EF        // ABC
       Button 3     =   1011 1110 1111 | BEF        // DEF
       Button 4     =   0011 1110 1111 | 3EF        // GHI
       Button 5     =   1101 1110 1111 | DEF        // JKL
       Button 6     =   0101 1110 1111 | 5EF        // MNO
       Button 7     =   1001 1110 1111 | 9EF        // PQRS
       Button 8     =   0001 1110 1111 | 1EF        // TUV
       Button 9     =   1110 1110 1111 | EEF        // WXYZ
       Button LAST  =   0010 0010 1111 | 22F
       Button MUTE  =   1101 0110 1111 | D6F
    */
 // ASCII Characters
    /*
        DEC         HEX
        0           SPACE
    */
// some helpful macros for systick

// the cc3200's fixed clock frequency of 80 MHz
// note the use of ULL to indicate an unsigned long long constant
#define SYSCLKFREQ 80000000ULL

// systick reload value set to 40ms period
// (PERIOD_SEC) * (SYSCLKFREQ) = PERIOD_TICKS
#define SYSTICK_RELOAD_VAL 3200000UL

#define TICKS_TO_US(ticks) \
    ((((ticks) / SYSCLKFREQ) * 1000000ULL) + \
    ((((ticks) % SYSCLKFREQ) * 1000000ULL) / SYSCLKFREQ))\


// track systick counter periods elapsed
// if it is not 0, we know the transmission ended
volatile int systick_cnt = 0;

volatile unsigned char pin_out_intflag;     // flag set when full IR code received
volatile int edge_counter = 0;              // count how many edges/pulses processed in current transmission
volatile int data = 0;                      // variable that holds the accumulated IR decoded bits
volatile bool reading_data = false;         // flag to indicate whether we are currently processing transmission
volatile int prev_data = -1; // <-- maybe set back to 0? set to -1 to try and fix indentation

volatile uint32_t global_time = 0;          //
volatile uint32_t prev_detection = 0;       //
volatile uint32_t last_time_button_pressed = 0;

volatile int curr_cycle = -1;
volatile char curr_letter = 0;
volatile int x = 0;
volatile char msg[50];
int l;
volatile bool message_sent = false;
volatile bool caps_lock = false;

#if defined(ccs)
extern void (* const g_pfnVectors[])(void);
#endif
#if defined(ewarm)
extern uVectorEntry __vector_table;
#endif
//*****************************************************************************
//                 GLOBAL VARIABLES -- End
//*****************************************************************************



//*****************************************************************************
//                      LOCAL DEFINITION
//*****************************************************************************
static void GPIOA0IntHandler(void) {
    unsigned long ulStatus;

    ulStatus = MAP_GPIOIntStatus (GPIOA0_BASE, true);
    MAP_GPIOIntClear(GPIOA0_BASE, ulStatus);        // clear interrupts on GPIOA0

    uint32_t pin_state = MAP_GPIOPinRead(GPIOA0_BASE, 0x8);
    if ((pin_state & 0x8) == 0) // Read falling edge
    {
        HWREG(NVIC_ST_CURRENT) = 1;
        systick_cnt = 0;

        // Initialize decoding variables on starting edge
        if (!reading_data)
        {
            reading_data = true;
            edge_counter = 0;
            data = 0;
        }
    }
    else // Read rising edge
    {
        if (reading_data)
        {
            // Measure time between falling and rising edges
            uint64_t delta = TICKS_TO_US(SYSTICK_RELOAD_VAL - SysTickValueGet());
            if (edge_counter == 0)
            {
                // Reset decoding variables on invalid start bit and exit
                if (delta <= 2000)
                {
                    reading_data = false;
                    edge_counter = 0;
                    return;
                }
            }
            else
            {
                // Write to data
                data <<= 1;
                if (delta <= 1000)
                {
                    data |= 1;
                }
            }
            edge_counter++;

            // Return the decoded signal if complete
            if (edge_counter == 13)
            {
                pin_out_intflag = 1;
                reading_data = false;
                edge_counter = 0;
            }
        }
    }
}

/**
 * Reset SysTick Counter
 */
static inline void SysTickReset(void) {
    // any write to the ST_CURRENT register clears it
    // after clearing it automatically gets reset without
    // triggering exception logic
    // see reference manual section 3.2.1
    HWREG(NVIC_ST_CURRENT) = 1;

    // clear the global count variable
    systick_cnt = 0;
}

/**
 * SysTick Interrupt Handler
 *
 * Keep track of whether the systick counter wrapped
 */
static void SysTickHandler(void) {
    // increment every time the systick handler fires
    systick_cnt++;
    global_time += 40;
}


//*****************************************************************************
//
//! This function updates the date and time of CC3200.
//!
//! \param None
//!
//! \return
//!     0 for success, negative otherwise
//!
//*****************************************************************************

static int set_time() {
    long retVal;

    g_time.tm_day = DATE;
    g_time.tm_mon = MONTH;
    g_time.tm_year = YEAR;
    g_time.tm_sec = HOUR;
    g_time.tm_hour = MINUTE;
    g_time.tm_min = SECOND;

    retVal = sl_DevSet(SL_DEVICE_GENERAL_CONFIGURATION,
                          SL_DEVICE_GENERAL_CONFIGURATION_DATE_TIME,
                          sizeof(SlDateTime),(unsigned char *)(&g_time));

    ASSERT_ON_ERROR(retVal);
    return SUCCESS;
}

static int http_post(int iTLSSockID, char* msg){
    char acSendBuff[512];
    char acRecvbuff[1460];
    char cCLLength[200];
    char* pcBufHeaders;
    int lRetVal = 0;

    pcBufHeaders = acSendBuff;
    strcpy(pcBufHeaders, POSTHEADER);
    pcBufHeaders += strlen(POSTHEADER);
    strcpy(pcBufHeaders, HOSTHEADER);
    pcBufHeaders += strlen(HOSTHEADER);
    strcpy(pcBufHeaders, CHEADER);
    pcBufHeaders += strlen(CHEADER);
    strcpy(pcBufHeaders, "\r\n\r\n");

    int dataLength = strlen(msg);

    strcpy(pcBufHeaders, CTHEADER);
    pcBufHeaders += strlen(CTHEADER);
    strcpy(pcBufHeaders, CLHEADER1);

    pcBufHeaders += strlen(CLHEADER1);
    sprintf(cCLLength, "%d", dataLength);

    strcpy(pcBufHeaders, cCLLength);
    pcBufHeaders += strlen(cCLLength);
    strcpy(pcBufHeaders, CLHEADER2);
    pcBufHeaders += strlen(CLHEADER2);

    strcpy(pcBufHeaders, msg);
    pcBufHeaders += strlen(msg);

    int testDataLength = strlen(pcBufHeaders);

    UART_PRINT(acSendBuff);


    //
    // Send the packet to the server */
    //
    lRetVal = sl_Send(iTLSSockID, acSendBuff, strlen(acSendBuff), 0);
    if(lRetVal < 0) {
        UART_PRINT("POST failed. Error Number: %i\n\r",lRetVal);
        sl_Close(iTLSSockID);
        GPIO_IF_LedOn(MCU_RED_LED_GPIO);
        return lRetVal;
    }
    lRetVal = sl_Recv(iTLSSockID, &acRecvbuff[0], sizeof(acRecvbuff), 0);
    if(lRetVal < 0) {
        UART_PRINT("Received failed. Error Number: %i\n\r",lRetVal);
        //sl_Close(iSSLSockID);
        GPIO_IF_LedOn(MCU_RED_LED_GPIO);
           return lRetVal;
    }
    else {
        acRecvbuff[lRetVal+1] = '\0';
        UART_PRINT(acRecvbuff);
        UART_PRINT("\n\r\n\r");
    }

    return 0;
}
static int http_get(int iTLSSockID, char* msg){
    char acSendBuff[512];
    char acRecvbuff[1460];
    char cCLLength[200];
    char* pcBufHeaders;
    int lRetVal = 0;

    pcBufHeaders = acSendBuff;
    strcpy(pcBufHeaders, GETHEADER);
    pcBufHeaders += strlen(GETHEADER);
    strcpy(pcBufHeaders, HOSTHEADER);
    pcBufHeaders += strlen(HOSTHEADER);
    strcpy(pcBufHeaders, CHEADER);
    pcBufHeaders += strlen(CHEADER);
    strcpy(pcBufHeaders, "\r\n\r\n");

    int dataLength = strlen(msg);

    strcpy(pcBufHeaders, CTHEADER);
    pcBufHeaders += strlen(CTHEADER);
    strcpy(pcBufHeaders, CLHEADER1);

    pcBufHeaders += strlen(CLHEADER1);
    sprintf(cCLLength, "%d", dataLength);

    strcpy(pcBufHeaders, cCLLength);
    pcBufHeaders += strlen(cCLLength);
    strcpy(pcBufHeaders, CLHEADER2);
    pcBufHeaders += strlen(CLHEADER2);

    strcpy(pcBufHeaders, msg);
    pcBufHeaders += strlen(msg);

    int testDataLength = strlen(pcBufHeaders);

    UART_PRINT(acSendBuff);


    //
    // Send the packet to the server */
    //
    lRetVal = sl_Send(iTLSSockID, acSendBuff, strlen(acSendBuff), 0);
    if(lRetVal < 0) {
        UART_PRINT("GET failed. Error Number: %i\n\r",lRetVal);
        sl_Close(iTLSSockID);
        GPIO_IF_LedOn(MCU_RED_LED_GPIO);
        return lRetVal;
    }
    lRetVal = sl_Recv(iTLSSockID, &acRecvbuff[0], sizeof(acRecvbuff), 0);
    if(lRetVal < 0) {
        UART_PRINT("Received failed. Error Number: %i\n\r",lRetVal);
        //sl_Close(iSSLSockID);
        GPIO_IF_LedOn(MCU_RED_LED_GPIO);
           return lRetVal;
    }
    else {
        acRecvbuff[lRetVal+1] = '\0';
        UART_PRINT(acRecvbuff);
        UART_PRINT("\n\r\n\r");
    }

    return 0;
}


void jsonify(const char *msg, char *output) {
    const char *format = "{"
                         "\"state\": {\r\n"
                         "\"desired\" : {\r\n"
                         "\"default\" : \"%s\"\r\n"
                         "}"
                         "}"
                         "}\r\n\r\n";
    snprintf(output, 256, format, msg);
}

//*****************************************************************************
//
//! Application startup display on UART
//!
//! \param  none
//!
//! \return none
//!
//*****************************************************************************
static void
DisplayBanner(char * AppName)
{

    Report("\n\n\n\r");
    Report("\t\t *************************************************\n\r");
    Report("\t\t        CC3200 %s Application       \n\r", AppName);
    Report("\t\t *************************************************\n\r");
    Report("\n\n\n\r");
}

//*****************************************************************************
//
//! Board Initialization & Configuration
//!
//! \param  None
//!
//! \return None
//
//*****************************************************************************
static void
BoardInit(void)
{
/* In case of TI-RTOS vector table is initialize by OS itself */
#ifndef USE_TIRTOS
  //
  // Set vector table base
  //
#if defined(ccs)
    MAP_IntVTableBaseSet((unsigned long)&g_pfnVectors[0]);
#endif
#if defined(ewarm)
    MAP_IntVTableBaseSet((unsigned long)&__vector_table);
#endif
#endif
    //
    // Enable Processor
    //
    MAP_IntMasterEnable();
    MAP_IntEnable(FAULT_SYSTICK);

    PRCMCC3200MCUInit();
}

/**
 * Initializes SysTick Module
 */
static void SysTickInit(void) {

    // configure the reset value for the systick countdown register
    MAP_SysTickPeriodSet(SYSTICK_RELOAD_VAL);

    // register interrupts on the systick module
    MAP_SysTickIntRegister(SysTickHandler);

    // enable interrupts on systick
    // (trigger SysTickHandler when countdown reaches 0)
    MAP_SysTickIntEnable();

    // enable the systick module itself
    MAP_SysTickEnable();
}
//*****************************************************************************
//
//! Main function handling the uart echo. It takes the input string from the
//! terminal while displaying each character of string. whenever enter command
//! is received it will echo the string(display). if the input the maximum input
//! can be of 80 characters, after that the characters will be treated as a part
//! of next string.
//!
//! \param  None
//!
//! \return None
//! 
//*****************************************************************************
void main()
 {
    unsigned long ulStatus;
    long lRetVal = -1;
    //
    // Initailizing the board
    //
    BoardInit();

    //
    // Muxing for Enabling UART_TX and UART_RX.
    //
    PinMuxConfig();

    // Enable SysTick
    SysTickInit();

    //
    // Initialising the Terminal.
    //
    InitTerm();
    //
    // Clearing the Terminal.
    //
    ClearTerm();
    DisplayBanner(APP_NAME);
    UART_PRINT("My terminal works!\n\r");

    //
    // Reset the peripheral
    //
    MAP_PRCMPeripheralReset(PRCM_GSPI);

    MAP_SPIReset(GSPI_BASE);

    //
    // Configure SPI interface
    //
    MAP_SPIConfigSetExpClk(GSPI_BASE,MAP_PRCMPeripheralClockGet(PRCM_GSPI),
                     SPI_IF_BIT_RATE,SPI_MODE_MASTER,SPI_SUB_MODE_0,
                     (SPI_SW_CTRL_CS |
                     SPI_4PIN_MODE |
                     SPI_TURBO_OFF |
                     SPI_CS_ACTIVEHIGH |
                     SPI_WL_8));

    //
    // Enable SPI for communication
    //
    MAP_SPIEnable(GSPI_BASE);

    Adafruit_Init();
    fillScreen(0x0000);

    //
    // Register the interrupt handler
    //
    MAP_GPIOIntRegister(GPIOA0_BASE, GPIOA0IntHandler);

    //
    // Configure both edges interrupt on pin out
    //
    MAP_GPIOIntTypeSet(GPIOA0_BASE, 0x8, GPIO_BOTH_EDGES);

    ulStatus = MAP_GPIOIntStatus(GPIOA0_BASE, false);
    MAP_GPIOIntClear(GPIOA0_BASE, ulStatus);            // clear interrupts on GPIOA0

    // clear global variables
    pin_out_intflag = 0;

    // Enable pin out interrupt
    MAP_GPIOIntEnable(GPIOA0_BASE, 0x8);

    // initialize global default app configuration
    g_app_config.host = SERVER_NAME;
    g_app_config.port = GOOGLE_DST_PORT;

    //Connect the CC3200 to the local access point
    lRetVal = connectToAccessPoint();
    //Set time so that encryption can be used
    lRetVal = set_time();
    if(lRetVal < 0) {
        UART_PRINT("Unable to set time in the device");
        LOOP_FOREVER();
    }
    //Connect to the website with TLS encryption
    lRetVal = tls_connect();
    if(lRetVal < 0) {
        ERR_PRINT(lRetVal);
    }

    while (1) {
        if (pin_out_intflag) {
            pin_out_intflag = 0;    // clear flag
            if ((global_time - prev_detection) >= 200) { // Ignore repeated data after first signal
                prev_detection = global_time;

                // Erase previous message from screen
                if (message_sent) {
                    message_sent = false;
                    fillRect(0, 64, 128, 9, 0x0000);
                    x = 6;
                    msg[0] = '\0';
                }

                if (((data != prev_data) || (global_time - last_time_button_pressed >= 1500)) && (prev_data != 0xFEF) && (prev_data != 0xD6F) && (prev_data != 0x22F))
                {
                    int l = strlen(msg);
                    msg[l] = curr_letter;
                    msg[l + 1] = '\0';
                    x += 6;
                    curr_cycle = 0;
                }
                else
                {   // we press same button
                    curr_cycle++;
                }
                prev_data = data;
                last_time_button_pressed = global_time;

                switch(data) {
                    case 0x6EF: // space
                        curr_letter = 32;
                        break;
                    case 0xFEF:
                        caps_lock = !caps_lock;
                        curr_cycle--;
                        break;
                    case 0x7EF: // ABC
                        curr_cycle %= 3;
                        curr_letter = 'a' + curr_cycle - (caps_lock * 32);
                        break;
                    case 0xBEF: // DEF
                        curr_cycle %= 3;
                        curr_letter = 'd' + curr_cycle -(caps_lock * 32);
                        break;
                    case 0x3EF: // GHI
                        curr_cycle %= 3;
                        curr_letter = 'g' + curr_cycle - (caps_lock * 32);
                        break;
                    case 0xDEF: // JKL
                        curr_cycle %= 3;
                        curr_letter = 'j' + curr_cycle - (caps_lock * 32);
                        break;
                    case 0x5EF: // MNO
                        curr_cycle %= 3;
                        curr_letter = 'm' + curr_cycle - (caps_lock * 32);
                        break;
                    case 0x9EF: // PQRS
                        curr_cycle %= 4;
                        curr_letter = 'p' + curr_cycle - (caps_lock * 32);
                        break;
                    case 0x1EF: // TUV
                        curr_cycle %= 3;
                        curr_letter = 't' + curr_cycle - (caps_lock * 32);
                        break;
                    case 0xEEF: // WXYZ
                        curr_cycle %= 4;
                        curr_letter = 'w' + curr_cycle - (caps_lock * 32);
                        break;
                    case 0x22F: // delete
                        curr_letter = 32;
                        l = strlen(msg);
                        if (l > 0) {
                            msg[l - 1] = '\0';
                            x -= 6;
                        }
                        prev_data = data;
                        break;
                    case 0xD6F: // enter
                        l = strlen(msg);
                        msg[l] = '\0';
                        
                        char json_msg[256];
                        jsonify(msg, json_msg);
                        Report("JSON to be sent: %s\n\r", json_msg);
                        http_post(lRetVal, json_msg);

                        MAP_UtilsDelay(80000);
                        message_sent = true;
                        curr_cycle = -1;
                        break;
                }

                if (data != 0xD6F && data != 0xFEF) {
                    drawChar(x, 64, curr_letter, 0xFFFF, 0x0000, 1);
                }
            }
        }
    }
}

//*****************************************************************************
//
// Close the Doxygen group.
//! @}
//
//*****************************************************************************
