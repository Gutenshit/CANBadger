#ifdef TARGET_LPC1768

#include "SimpleDMA.h"

SimpleDMA *SimpleDMA::irq_owner[8] = {NULL};
LPC_GPDMACH_TypeDef *LPC_GPDMACH[8] = {LPC_GPDMACH0, LPC_GPDMACH1, LPC_GPDMACH2, LPC_GPDMACH3, LPC_GPDMACH4, LPC_GPDMACH5, LPC_GPDMACH6, LPC_GPDMACH7};
uint32_t getTransferType(SimpleDMA_Trigger trig, uint32_t source, uint32_t destination);

SimpleDMA::SimpleDMA(int channel) {
    this->channel(channel);
    
    //Power up
    LPC_SC->PCONP |= 1<<29;
    LPC_GPDMA->DMACConfig = 1;
    trigger(Trigger_ALWAYS);
    
    NVIC_SetVector(DMA_IRQn, (uint32_t)&irq_handler0);
    NVIC_EnableIRQ(DMA_IRQn);          
}

int SimpleDMA::start(int length) {
    if (auto_channel)
        _channel = getFreeChannel();
    else
        while(isBusy());
    
    uint32_t control = (source_inc << 26) | (destination_inc << 27) | (1UL << 31);
    switch (source_size) {
        case 16:
            control |= (1<<18) | (length >> 1);
            break;
        case 32:
            control |= (2<<18) | (length >> 2);  
            break;
        default:
            control |= length;
    }
    switch (destination_size) {
        case 16:
            control |= (1<<21);
            break;
        case 32:
            control |= (2<<21);  
            break;
    } 
    
    LPC_GPDMACH[_channel]->DMACCSrcAddr = _source;
    LPC_GPDMACH[_channel]->DMACCDestAddr = _destination;
    LPC_GPDMACH[_channel]->DMACCLLI = 0;
    LPC_GPDMACH[_channel]->DMACCControl = control;     //Enable interrupt also
    
    irq_owner[_channel] = this;
    
    if (_trigger != Trigger_ALWAYS) {      
        if (_trigger & 16) 
            LPC_SC->DMAREQSEL |= 1 << (_trigger - 24);
        else
            LPC_SC->DMAREQSEL &= ~(1 << (_trigger - 24));
        
        LPC_GPDMACH[_channel]->DMACCConfig = ((_trigger & 15) << 1) + ((_trigger & 15) << 6) + (getTransferType(_trigger, _source, _destination) << 11) +  (1<<15) +1;        //Both parts of the transfer get triggered at same time
    } else 
        LPC_GPDMACH[_channel]->DMACCConfig = (getTransferType(_trigger, _source, _destination) << 11) + 1 + (1<<15);               //Enable channel
    
    return 0;
}

bool SimpleDMA::isBusy( int channel ) {
    if (channel == -1)
        channel = _channel;
    return (LPC_GPDMA->DMACEnbldChns & (1<<channel));
}

void SimpleDMA::irq_handler0(void) {
    while(LPC_GPDMA->DMACIntTCStat != 0) {
        
    uint32_t intloc = 31 - __CLZ(LPC_GPDMA->DMACIntTCStat & 0xFF);
    if (irq_owner[intloc]!=NULL)
        irq_owner[intloc]->irq_handler();
    }
}

void SimpleDMA::irq_handler(void) {
    LPC_GPDMA->DMACIntTCClear = 1<<_channel;
    _callback.call();
}

uint32_t getTransferType(SimpleDMA_Trigger trig, uint32_t source, uint32_t destination) {
    //If it is always, simply put it on memory-to-memory
    if (trig == Trigger_ALWAYS)
        return 0;
    else if ((source >> 28) == 0 || (source >> 28) == 1) {       //if source is RAM/Flash
        if ((destination >> 28) == 0 || (destination >> 28) == 1)        //if destination is RAM/flash
            return 3;                                                               //Return p2p for m2m with a trigger (since I have no idea wtf you are trying to do)
        else
            return 1;                                                               //Source is memory, destination is peripheral, so m2p
        }
    else {
        if ((destination >> 28) == 0 || (destination >> 28))
            return 2;                                                               //Source is peripheral, destination is memory
        else
            return 3;                                                               //Both source and destination are peripherals
        }
        
}
#endif