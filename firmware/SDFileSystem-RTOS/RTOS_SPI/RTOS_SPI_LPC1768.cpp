#ifdef TARGET_LPC1768
#include "RTOS_SPI.h"

RTOS_SPI::RTOS_SPI(PinName mosi, PinName miso, PinName sclk, PinName _unused) : SPI(mosi, miso, sclk) {    
    if (_spi.spi == LPC_SSP0) {
        read_dma.trigger(Trigger_SSP0_RX);
        write_dma.trigger(Trigger_SSP0_TX);
    } else {
        read_dma.trigger(Trigger_SSP1_RX);
        write_dma.trigger(Trigger_SSP1_TX);
    }
    
    read_dma.source(&_spi.spi->DR, false, 8);
    write_dma.destination(&_spi.spi->DR, false, 8);
};

void RTOS_SPI::bulkInternal(uint8_t *read_data, uint8_t *write_data, int length, bool read_inc, bool write_inc) {
    aquire();
    _spi.spi->DMACR = 3;

    read_dma.destination(read_data, read_inc);
    write_dma.source(write_data, write_inc);
    read_dma.start(length);
    write_dma.wait(length);
    while(read_dma.isBusy());

    _spi.spi->DMACR = 0;
}
#endif