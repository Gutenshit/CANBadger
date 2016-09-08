#ifndef SIMPLEDMA_H
#define SIMPLEDMA_H

#ifdef RTOS_H
#include "rtos.h"
#endif

#include "mbed.h"
#include "SimpleDMA_KL25.h"
#include "SimpleDMA_LPC1768.h"


/**
* SimpleDMA, DMA made simple! (Okay that was bad)
*
* A class to easily make basic DMA operations happen. Not all features
* of the DMA peripherals are used, but the main ones are: From and to memory
* and peripherals, either continiously or triggered
*/
class SimpleDMA {
public:
/**
* Constructor
*
* @param channel - optional parameter which channel should be used, default is automatic channel selection
*/
SimpleDMA(int channel = -1);

/**
* Set the source of the DMA transfer
*
* Autoincrement increments the pointer after each transfer. If the source
* is an array this should be true, if it is a peripheral or a single memory
* location it should be false.
*
* The source can be any pointer to any memory location. Automatically
* the wordsize is calculated depending on the type, if required you can
* also override this.
*
* @param pointer - pointer to the memory location
* @param autoinc - should the pointer be incremented by the DMA module
* @param size - wordsize in bits (optional, generally can be omitted)
* @return - 0 on success
*/
template<typename Type>
void source(Type* pointer, bool autoinc, int size = sizeof(Type) * 8) {
    _source = (uint32_t)pointer;
    source_inc = autoinc;
    source_size = size;
}

/**
* Set the destination of the DMA transfer
*
* Autoincrement increments the pointer after each transfer. If the source
* is an array this should be true, if it is a peripheral or a single memory
* location it should be false.
*
* The destination can be any pointer to any memory location. Automatically
* the wordsize is calculated depending on the type, if required you can
* also override this.
*
* @param pointer - pointer to the memory location
* @param autoinc - should the pointer be incremented by the DMA module
* @param size - wordsize in bits (optional, generally can be omitted)
* @return - 0 on success
*/
template<typename Type>
void destination(Type* pointer, bool autoinc, int size = sizeof(Type) * 8) {
    _destination = (uint32_t)pointer;
    destination_inc = autoinc;
    destination_size = size;
}

/**
* Set the trigger for the DMA operation
*
* In SimpleDMA_[yourdevice].h you can find the names of the different triggers.
* Trigger_ALWAYS is defined for all devices, it will simply move the data
* as fast as possible. Used for memory-memory transfers. If nothing else is set
* that will be used by default.
*
* @param trig - trigger to use
* @param return - 0 on success
*/
void trigger(SimpleDMA_Trigger trig) {
    _trigger = trig;
}

/**
* Set the DMA channel
*
* Generally you will not need to call this function, the constructor does so for you
*
* @param chan - DMA channel to use, -1 = variable channel (highest priority channel which is available)
*/
void channel(int chan);

/**
* Start the transfer
*
* @param length - number of BYTES to be moved by the DMA
*/
int start(int length);

/**
* Is the DMA channel busy
*
* @param channel - channel to check, -1 = current channel
* @return - true if it is busy
*/
bool isBusy( int channel = -1 );

/**
* Attach an interrupt upon completion of DMA transfer or error
*
* @param function - function to call upon completion (may be a member function)
*/
void attach(void (*function)(void)) {
    _callback.attach(function);
    }
    
template<typename T>
    void attach(T *object, void (T::*member)(void)) {
        _callback.attach(object, member);
    }

#ifdef RTOS_H
/**
* Start a DMA transfer similar to start, however block current Thread
* until the transfer is finished
*
* When using this function only the current Thread is halted.
* The Thread is moved to Waiting state: other Threads will continue
* to run normally. 
*
* This function is only available if you included rtos.h before 
* including SimpleDMA.h.
*
* @param length - number of BYTES to be moved by the DMA
*/
void wait(int length) {
    id = Thread::gettid();
    this->attach(this, &SimpleDMA::waitCallback);
    this->start(length);
    Thread::signal_wait(0x1);
}
#endif

protected:
int _channel;
SimpleDMA_Trigger _trigger;
uint32_t _source;
uint32_t _destination;
bool source_inc;
bool destination_inc;
uint8_t source_size;
uint8_t destination_size;

bool auto_channel;

//IRQ handlers
FunctionPointer _callback;
void irq_handler(void);

static SimpleDMA *irq_owner[DMA_CHANNELS];

static void irq_handler0( void ); 

#if DMA_IRQS > 1
static void irq_handler1( void );
static void irq_handler2( void );
static void irq_handler3( void );
#endif

//Keep searching until we find a non-busy channel, start with lowest channel number
int getFreeChannel(void);

#ifdef RTOS_H
osThreadId id;
void waitCallback(void) {
    osSignalSet(id, 0x1);    
}
#endif
};
#endif