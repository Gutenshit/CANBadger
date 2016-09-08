/*
 * ethernet_manager.hpp
 *
 * use this to do everything ethernet related
 *
 *  Created on: Apr 5, 2016
 *      Author: codewhite
 */

#ifndef ETHERNET_MANAGER_HPP_
#define ETHERNET_MANAGER_HPP_

#define DEBUG
#define START_THREAD 1
#define CB_VERSION 1

/****SPI RAM stuff***/
#define CMD_READ    0x03
#define CMD_WRITE   0x02
#define CMD_RDMR    0x05
#define CMD_WRMR    0x01

#include "mbed.h"
#include "EthernetInterface.h"
#include "rtos.h"
#include "ethernet_message.hpp"
#include "settings_handler.hpp"
#include "stdlib.h"


class EthernetManager {
public:
	/* initializes ethernet manager
	 * receives uid of device, which will be used for discovery
	 * will setup the ethernet interface and request an ip using dhcp
	 */
	EthernetManager(int deviceUid, SettingsHandler *settingsHandler,
			Mail<EthernetMessage, 16> *commandQueue, SPI *ram, DigitalOut *ramCS1);
	// destructor
	~EthernetManager();

	/* used for putting messages into outQueue and sending them from the application
	 * returns an error code if anything went wrong, otherwise ??
	 */
	int sendMessage(MessageType type, char *data, uint32_t dataLength);

	/*
	 * used for easily sending a string message, as in device.printf
	 *
	 */
	int sendFormattedDataMessage(char *msg, ...);
	int sendFormattedDebugMessage(char *msg, ...);

	/*
	 * for when you really have to decide at which point a packet is sent
	 * contrary to sendMessage, memory management is at the hand of the caller
	 */
	int sendMessageBlocking(MessageType type, char *data, uint32_t dataLength);
	int sendMessagesBlocking(EthernetMessage **messages, size_t numMessages);
	// store frame in xram and send it - used for canlogger
	int sendRamFrame(MessageType type, char *data, uint32_t dataLength);

	static void mainStarterThread(const void *args);
	void run();

	// this will block - you have been warned
	int debugLog(char *debugMsg);

	// will clear the ramStart and ramEnd counters
	void resetRam();
private:
	int deviceUid;
	char *deviceIdentifier;
	SettingsHandler *settingsHandler;
	EthernetInterface eth;
	Ticker broadcastTicker;
	unsigned int dataOutDestPort;
	size_t nameLength;
	size_t idLength;

	UDPSocket broadcastSocket;
	UDPSocket actionSocket;

	Endpoint broadcastEndpoint;
	Endpoint outputEndpoint;

	Mail<EthernetMessage, 64> outQueue;
	Mail<EthernetMessage, 16> *commandQueue;
	Mutex actionSocketMutex;

	SPI *ram;
	DigitalOut *ramCS1;
	Mutex ramMutex;
	volatile uint32_t ramStart;
	volatile uint32_t ramEnd;
	uint32_t ram_write(uint32_t addr, char *buf, uint32_t len);
	uint32_t ram_read(uint32_t addr, char *buf, uint32_t len);
	char *xramSerializationBuffer;

};



#endif /* ETHERNET_MANAGER_HPP_ */
