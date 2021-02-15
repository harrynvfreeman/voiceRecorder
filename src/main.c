/*******************************************************************************
  Main Source File

  Company:
    Microchip Technology Inc.

  File Name:
    main.c

  Summary:
    This file contains the "main" function for a project.

  Description:
    This file contains the "main" function for a project.  The
    "main" function calls the "SYS_Initialize" function to initialize the state
    machines of all modules in the system
 *******************************************************************************/

// *****************************************************************************
// *****************************************************************************
// Section: Included Files
// *****************************************************************************
// *****************************************************************************

#include <stddef.h>                     // Defines NULL
#include <stdbool.h>                    // Defines true
#include <stdlib.h>                     // Defines EXIT_FAILURE
#include "definitions.h"                // SYS function prototypes

#include "sys/kmem.h"

#define page_size 1024
#define row_size 128
#define rows_per_page 8
#define dma_erase_size 512
#define dma_write_size 16
#define max_buffer_size 1024

//why is it like debugger pins have moved when I change linker script?
//pin 6 is acting super weird

//Why does linker have to be multiple of 0?
//E.G. 0xA000 works but 0xA400 doesn't
//Using 0xB000
static const uint32_t start_address = PA_TO_KVA1(0x1D00B800);
static const uint32_t end_address = PA_TO_KVA1(0x1D020000);
static const uint32_t max_samples = 83968;//end_address - start_address;
static const uint32_t adc_mid = 128;

//ADC1BUF0 Address
//PA_TO_KVA1 is an | so should not have affect
static const void *adc_start_addr = PA_TO_KVA1(0xBF809070);
//DMA buffer which is what is DMA destination
static volatile uint32_t dma_buffer[max_buffer_size];
//Current size of DMA buffer writing
static volatile uint32_t dma_buffer_size;
//Flash buffer which is used to write to flash
//NOTE this is uint8_t
static volatile uint8_t flash_buffer[max_buffer_size];
//Current size of flash buffer
static volatile uint32_t flash_buffer_size;
//Used to prevent consecutive page erases
static volatile bool allow_page_erase;
//Determine if DMA should continue
static volatile bool dma_enabled;
//Total number of recorded values written to flash
static volatile uint32_t record_write_count;
//true means currently
//looking for rising edge
//false means currently
//looking for falling edge
static volatile bool record_state;

static volatile uint8_t play_data[row_size];
static volatile uint32_t play_read_count;
//true means currently
//looking for rising edge
//false means currently
//looking for falling edge
static volatile bool play_state;

//true means don't update tmr3
//false means update tmr3
static volatile bool adc_state;
static volatile uint16_t last_adc_val;

void initialize_adc();
void initialize_dma();
void initialize_tmr3();
void initialize_record();
void initialize_play();

void dma_handler();
void adc_handler();
void tmr3_handler();
void record_handler();
void play_handler();

void enable_ext1_int();
void enable_ext2_int();

void erase_page(uint32_t address);
void write_row(volatile uint8_t *data, uint32_t address);
void read_row(volatile uint8_t *data, uint32_t address);

// *****************************************************************************
// *****************************************************************************
// Section: Main Entry Point
// *****************************************************************************
// *****************************************************************************

static volatile uint32_t led_val = 0;
//static uint8_t temp[DMA_WRITE_SIZE];

int main ( void )
{
    //BMXPUPBA = BMXPFMSZ - 86016;
    /* Initialize all modules */
    SYS_Initialize ( NULL );
    
    TMR2_Start();
    OCMP4_Enable();
    OCMP4_CompareSecondaryValueSet(adc_mid);
    
    initialize_adc();
    initialize_dma();
    initialize_tmr3();
    initialize_record();
    initialize_play();
            
    while ( true )
    {
        /* Maintain state machines of all polled MPLAB Harmony modules. */
        SYS_Tasks ();
    }

    /* Execution should not come here during normal operation */

    return ( EXIT_FAILURE );
}

void initialize_adc() {
    //auto sampling and auto convert mode
    
    //WARNING WARNING WARNING How to handle interrupts before here?
    //Maybe do not enable in adc initialize
    ADC_Disable();
    adc_state = false;
    AD1CHS = 0x40000;
    //Do I need this?
    ADC_CallbackRegister(*adc_handler, NULL);
    ADC_Enable();
    ADC_SamplingStart();
}

void initialize_dma() {
    DMAC_ChannelCallbackRegister(DMAC_CHANNEL_0, *dma_handler, NULL);
}

void initialize_tmr3() {
    TMR3_CallbackRegister(*tmr3_handler, NULL);
}

void initialize_record() {
    record_write_count = 0;
    record_state = true;
    EVIC_ExternalInterruptCallbackRegister(EXTERNAL_INT_2, *record_handler, NULL);
    INTCONSET = 0x04;
    enable_ext2_int();
}

void initialize_play() {
    play_state = true;
    EVIC_ExternalInterruptCallbackRegister(EXTERNAL_INT_1, *play_handler, NULL);
    INTCONSET = 0x02;
    enable_ext1_int();
}

void dma_handler() {
    if ((!dma_enabled) || record_write_count >= max_samples) {
        ADC_Disable();
        adc_state = true;
        AD1CHS = 0x40000;
        ADC_Enable();
        ADC_SamplingStart();
        return;
    }
    
    //DMA finished, transfer to flash buffer
    //if anything to transfer
    if (dma_buffer_size > 0) {
        int i;
        for (i = 0; i < dma_buffer_size; i++) {
            flash_buffer[flash_buffer_size + i] = (uint8_t)(dma_buffer[i] >> 2);
        }
        flash_buffer_size += dma_buffer_size;
        dma_buffer_size = 0;
    }
    
    //Check if need to do page erase
    if ((record_write_count % page_size == 0) && (allow_page_erase)) {
        //set allow_page_erase to false to prevent consecutive erases
        allow_page_erase = false;
        //start DMA
        dma_buffer_size = dma_erase_size;
        DMAC_ChannelTransfer(DMAC_CHANNEL_0, adc_start_addr, sizeof(uint32_t), dma_buffer, dma_buffer_size*sizeof(uint32_t), sizeof(uint32_t)); 
        //erase page
        erase_page(start_address + record_write_count);
        return;
    }
    
    //Check if need to row write
    if (flash_buffer_size >= row_size) {
        //start DMA
        dma_buffer_size = dma_write_size;
        DMAC_ChannelTransfer(DMAC_CHANNEL_0, adc_start_addr, sizeof(uint32_t), dma_buffer, dma_buffer_size*sizeof(uint32_t), sizeof(uint32_t));
        //write row
        write_row(flash_buffer, start_address + record_write_count);
        //update records written to flash
        record_write_count += row_size;
        //update flash_buffer
        int i;
        for (i = 0; i < flash_buffer_size - row_size; i++) {
            flash_buffer[i] = flash_buffer[i + row_size];
        }
        flash_buffer_size -= row_size;
        //allow page erase
        allow_page_erase = true;
        return;
    }
    
    //Otherwise DMA for duration
    dma_buffer_size = row_size - flash_buffer_size;
    DMAC_ChannelTransfer(DMAC_CHANNEL_0, adc_start_addr, sizeof(uint32_t), dma_buffer, dma_buffer_size*sizeof(uint32_t), sizeof(uint32_t));
}

void adc_handler() {
    last_adc_val = ADC1BUF0;
    IFS0CLR = _IFS0_AD1IF_MASK;
}

void tmr3_handler() {
    if ((record_write_count < row_size) || (play_read_count >= (record_write_count - row_size))) {
        OCMP4_CompareSecondaryValueSet(adc_mid);
        return;
    }
    
    //right now read row by row
    //can probably increase
    if (play_read_count % row_size == 0) {
        read_row(play_data, start_address + play_read_count);
    }
    
    OCMP4_CompareSecondaryValueSet(play_data[play_read_count % row_size]);
    play_read_count++;
}

void record_handler() {
    //disable record interrupt
    EVIC_ExternalInterruptDisable(EXTERNAL_INT_2);
    if (record_state) {
        //disable play backinterrupt
        EVIC_ExternalInterruptDisable(EXTERNAL_INT_1);        
        //set interrupt for falling edge
        INTCONCLR = 0x04;
        //reset everything for record
        dma_buffer_size = 0;
        flash_buffer_size;
        allow_page_erase = true;
        record_write_count = 0;
        //enable and start adc
        ADC_Disable();
        adc_state = true;
        AD1CHS = 0x10000;
        ADC_Enable();
        ADC_SamplingStart();
        dma_enabled = true;
        LATA = 1;
        dma_handler();
    } else {
        //WARNING WARNING WARNING
        //should I be disabling adc interrupt here?
        //Need to handle the case if adc interrupt triggers after?
        //And operations are in progress?
        //could disable adc interrupt here actually
        
        //wait for dma to finish
        //and disable adc
        //if busy then will disable adc in next interrupt
        dma_enabled = false;
        if (!DMAC_ChannelIsBusy(DMAC_CHANNEL_0)) {
           ADC_Disable(); 
           adc_state = true;
           AD1CHS = 0x40000;
           ADC_Enable();
           ADC_SamplingStart();
        }
        
        //WARNING WARNING WARNING
        //Last row_size - 1 samples will not
        //be recorded, do that here?
        
        //set interrupt for rising edge
        INTCONSET = 0x04;
        //reenable playback interrupt
        enable_ext1_int();
        LATA = 0;
    }
    //update record_state
    record_state = !record_state;
    //reenable record inerrupt
    enable_ext2_int();
}

void play_handler() {
    //disable playback interrupt
    EVIC_ExternalInterruptDisable(EXTERNAL_INT_1);
    if (play_state) {
        //disable record interrupt
        EVIC_ExternalInterruptDisable(EXTERNAL_INT_2);
        //set interrupt for falling edge
        INTCONCLR = 0x02;
        //reset play_count
        play_read_count = 0;
        //disable adc
        ADC_Disable();
        //reset and start timer3
        PR3 = (uint32_t)(1000 + 3.9*last_adc_val);
        TMR3 = 0;
        TMR3_Start();
        LATA = 1;
    } else {
        //disable tmr3
        TMR3_Stop();
        //set interrupt for rising edge
        INTCONSET = 0x02;
        //reset output compare
        OCMP4_CompareSecondaryValueSet(adc_mid);
        //enable adc
        ADC_Enable();
        ADC_SamplingStart();
        //reenable record inerrupt
        enable_ext2_int();
        LATA = 0;
    }
    
    //update play_state
    play_state = !play_state;
    //reenable playback interrupt
    enable_ext1_int();
}

void enable_ext1_int() {
    IFS0CLR = _IFS0_INT1IF_MASK;
    EVIC_ExternalInterruptEnable(EXTERNAL_INT_1);
}

void enable_ext2_int() {
    IFS0CLR = _IFS0_INT2IF_MASK;
    EVIC_ExternalInterruptEnable(EXTERNAL_INT_2);
}

void erase_page(uint32_t address) {
    bool erase_succ = NVM_PageErase(address);
    if (!erase_succ) {
        LATA = 1;
        while (true);
    }
}

void write_row(volatile uint8_t *data, uint32_t address) {
    bool write_succ = NVM_RowWrite((volatile uint32_t *)data,  address);
    if (!write_succ) {
        LATA = 1;
        while (true);
    }
}

void read_row(volatile uint8_t *data, uint32_t address) {
    bool read_succ = NVM_Read((volatile uint32_t *)data, row_size*sizeof(uint8_t), address);
    if (!read_succ) {
        LATA = 1;
        while (true);
    }
}

/*******************************************************************************
 End of File
*/

