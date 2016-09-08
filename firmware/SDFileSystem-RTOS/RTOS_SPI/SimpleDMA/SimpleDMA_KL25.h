#ifdef TARGET_KL25Z

#define DMA_CHANNELS        4
#define DMA_IRQS            4

enum SimpleDMA_Trigger {
    Trigger_ALWAYS = 60,
    Trigger_UART0_RX = 2,
    Trigger_UART0_TX,
    Trigger_UART1_RX,
    Trigger_UART1_TX,
    Trigger_UART2_RX,
    Trigger_UART2_TX,
    Trigger_SPI0_RX = 16,
    Trigger_SPI0_TX,
    Trigger_SPI1_RX,
    Trigger_SPI1_TX,
    Trigger_I2C0 = 22,
    Trigger_I2C1,
    Trigger_TPM0_C0,
    Trigger_TPM0_C1,
    Trigger_TPM0_C2,
    Trigger_TPM0_C3,
    Trigger_TPM0_C4,
    Trigger_TPM0_C5,
    Trigger_TPM1_C0 = 32,
    Trigger_TPM1_C1,
    Trigger_TPM2_C0,
    Trigger_TPM2_C1,
    Trigger_ADC0 = 40,
    Trigger_CMP0 = 42,
    Trigger_DAC0 = 45,
    Trigger_PORTA = 49,
    Trigger_PORTD = 52,
    Trigger_TPM0 = 54,
    Trigger_TPM1,
    Trigger_TPM2,
    Trigger_TSI,
    Trigger_ALWAYS0 = 60,
    Trigger_ALWAYS1,
    Trigger_ALWAYS2,
    Trigger_ALWAYS3,
};    

#endif