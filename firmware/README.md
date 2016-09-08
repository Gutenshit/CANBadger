# You can haz CANBadger too!
Greetings visitor! We had a great time presenting the CANBadger at Blackhat US and Defcon 2016. But now, it's time to do some work, isn't it?
Please read along for information on how to get started with your CANBadger.
# Getting started
## Hardware
If you want to build your own CANBadger, please use the [gerber files](https://github.com/Gutenshit/CANBadger). With this, you can order the boards right away.
For assembly, refer to the schematics to avoid soldering stuff in the wrong place.
Note that the sd card reader should be face down - it's supposed to be on the back of the board with its pins going up.
The Pin 1 position for the two can tranceivers is indicated by a small, white dot. For the xram it is bottom right, next to the pin header. (look at [this](https://github.com/Gutenshit/CANBadger/blob/master/CANBadger%20V0.2%20Board%20Layout.pdf) for clarification).
## Firmware
We released [sources](https://github.com/Gutenshit/CANBadger/tree/master/firmware) and [binaries]() of the firmware and we try to keep them as synchronized as possible - if you have an mbed, you just need to drop the binary and press the reset button.
If you have the lpcxpresso lpc1769, you need to flash the binary in a different way, using CooCox IDE or LPCXpresso.
## Software
You can find sources and binaries for the CANBadger Server [here](https://github.com/Gutenshit/CANBadger-Server).
## License
This project is licensed under GPLv3.
