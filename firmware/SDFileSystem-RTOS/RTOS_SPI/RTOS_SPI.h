#ifndef RTOS_SPI_H
#define RTOS_SPI_H

#include "mbed.h"
#include "rtos.h"
#include "SimpleDMA.h"

/**
* RTOS_SPI uses SimpleDMA to control SPI using DMA. 
*
* As the name says it is intended to be used with RTOS.
* The current Thread is paused until it is done, meanwhile
* other Threads will continue to run. Once finished the 
* Thread will continue.
*
* This is a child class of SPI, so all regular SPI functions
* are available + two for DMA control.
*/
class RTOS_SPI: public SPI {
    public:
    /** Create a SPI master connected to the specified pins
     *
     *  mosi or miso can be specfied as NC if not used
     *
     *  @param mosi - SPI Master Out, Slave In pin
     *  @param miso - SPI Master In, Slave Out pin
     *  @param sclk - SPI Clock pin
     */
    RTOS_SPI(PinName mosi, PinName miso, PinName sclk, PinName _unused=NC);
    
    /** 
    * Write a block of data via SPI
    *
    * This throws away all read data
    *
    * @param *write_data - uint8_t pointer to data to write
    * @param length - number of bytes to write
    * @param array - true if write_data is an array, false if it is a single constant value
    */
    void bulkWrite(uint8_t *write_data, int length, bool array = true) {
        uint8_t dummy;
        bulkInternal(&dummy, write_data, length, false, array);
    }
    
    /** 
    * Write a block of data via SPI, read returning value
    *
    * @param *read_data - uint8_t pointer to array where received data should be stored
    * @param *write_data - uint8_t pointer to data to write
    * @param length - number of bytes to write
    * @param array - true if write_data is an array, false if it is a single constant value
    */
    void bulkReadWrite(uint8_t *read_data, uint8_t *write_data, int length, bool array = true) {
        bulkInternal(read_data, write_data, length, true, array);
    }
    
    private:
    void bulkInternal(uint8_t *read_data, uint8_t *write_data, int length, bool read_inc, bool write_inc);
    
    SimpleDMA read_dma;
    SimpleDMA write_dma;
    
};


#endif