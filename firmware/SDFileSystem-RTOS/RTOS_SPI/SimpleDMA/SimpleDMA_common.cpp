#include "SimpleDMA.h"

void SimpleDMA::channel(int chan) {
    if (chan == -1) {
        auto_channel = true;
        _channel = 0;
    } else {
        auto_channel = false;
        if (chan >= 0 && chan < DMA_CHANNELS)
            _channel = chan;
        else
            _channel = DMA_CHANNELS-1;
    }
}

int SimpleDMA::getFreeChannel(void) {
    int retval = 0;
    while(1) {
        if (!isBusy(retval))
            return retval;
        retval++;
        if (retval >= DMA_CHANNELS)
            retval = 0;
    }  
}