#ifdef TARGET_LPC1768

#define DMA_CHANNELS        8
#define DMA_IRQS            1

enum SimpleDMA_Trigger {
    Trigger_ALWAYS = -1,
    Trigger_SSP0_TX,
    Trigger_SSP0_RX,
    Trigger_SSP1_TX,
    Trigger_SSP1_RX,
    Trigger_ADC,
    Trigger_I2S0,
    Trigger_I2S1,
    Trigger_DAC,
    Trigger_UART0_TX,
    Trigger_UART0_RX,
    Trigger_UART1_TX,
    Trigger_UART1_RX,
    Trigger_UART2_TX,
    Trigger_UART2_RX,
    Trigger_UART3_TX,
    Trigger_UART3_RX,
    Trigger_MATCH0_0 = 24,
    Trigger_MATCH0_1,
    Trigger_MATCH1_0,
    Trigger_MATCH1_1,
    Trigger_MATCH2_0,
    Trigger_MATCH2_1,
    Trigger_MATCH3_0,
    Trigger_MATCH3_1
};  

#endif