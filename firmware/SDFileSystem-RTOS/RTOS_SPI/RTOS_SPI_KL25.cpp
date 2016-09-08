#ifdef TARGET_KL25Z
#include "RTOS_SPI.h"

RTOS_SPI::RTOS_SPI(PinName mosi, PinName miso, PinName sclk, PinName _unused) : SPI(mosi, miso, sclk) {    
    if (_spi.spi == SPI0) {
        read_dma.trigger(Trigger_SPI0_RX);
        write_dma.trigger(Trigger_SPI0_TX);
    } else {
        read_dma.trigger(Trigger_SPI1_RX);
        write_dma.trigger(Trigger_SPI1_TX);
    }
    
    read_dma.source(&_spi.spi->D, false);
    write_dma.destination(&_spi.spi->D, false);
};

void RTOS_SPI::bulkInternal(uint8_t *read_data, uint8_t *write_data, int length, bool read_inc, bool write_inc) {
    aquire();
    _spi.spi->C2 |= SPI_C2_TXDMAE_MASK | SPI_C2_RXDMAE_MASK;

    read_dma.destination(read_data, read_inc);
    if (write_inc)
        write_dma.source(write_data+1, write_inc);
    else
        write_dma.source(write_data, write_inc);

    //simply start the read_dma
    read_dma.start(length);
    
    //Write the first byte manually, since this is recommended method (and the normal method sends the first byte twice)
    while((_spi.spi->S & SPI_S_SPTEF_MASK) == 0);
    _spi.spi->D = write_data[0];
    
    write_dma.wait(length-1);
    while(read_dma.isBusy());

    _spi.spi->C2 &= ~(SPI_C2_TXDMAE_MASK | SPI_C2_RXDMAE_MASK);
}
#endif