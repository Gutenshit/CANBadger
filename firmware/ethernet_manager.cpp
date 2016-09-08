	/*
	  * ethernet_manager.cpp
	 *
	 *  Created on: Apr 5, 2016
	 *      Author: codewhite
	 */

	#include "ethernet_manager.hpp"

	// global stuff
	bool sendBroadcast = false;

	void toggleBroadcast() {
		sendBroadcast = true;
	}

	EthernetManager::EthernetManager(int deviceUid, SettingsHandler *settingsHandler, Mail<EthernetMessage, 16> *commandQueue, SPI *ram, DigitalOut *ramCS1) {
		this->deviceUid = deviceUid;
		this->settingsHandler = settingsHandler;
		char *cbName = settingsHandler->getSettings()->settings["id"];
		this->nameLength = strlen(cbName);
		this->deviceIdentifier = new char[17+this->nameLength];
		snprintf(this->deviceIdentifier, 17+this->nameLength, "CB|%d;%s|%d|", this->deviceUid,
				cbName, CB_VERSION);
		this->idLength = strlen(this->deviceIdentifier);
		this->dataOutDestPort = 0;
		sendBroadcast = false;
		this->broadcastTicker.attach(toggleBroadcast, 2.0);
		this->commandQueue = commandQueue;
		this->ram = ram;
		this->ramCS1 = ramCS1;
		this->ramStart = 0;
		this->ramEnd = 0;
		this->xramSerializationBuffer = new char[21];

		this->debugLog("Initializing ethernet..");

		eth.init(); //Use DHCP
		//eth.init("10.0.0.123", "255.255.255.0", "0.0.0.0");  // use static ip
		eth.connect();

	#ifdef DEBUG
		char* adr = eth.getIPAddress();
		char *debugMsg = new char[43];
		sprintf(debugMsg, "Initialized ethernet with ip: %s", adr);
		this->debugLog(debugMsg);
	#endif
	}

	EthernetManager::~EthernetManager() {
		this->eth.disconnect();
	}

	void EthernetManager::mainStarterThread(const void *args) {
		EthernetManager *klass = (EthernetManager*) args;
		klass->run();
	}

	void EthernetManager::run() {
		Endpoint client;
		char *recvBuf = new char[130];
		int error = this->actionSocket.bind(13371);
		this->actionSocket.set_blocking(false, 50);

		error = this->broadcastSocket.init();
		this->broadcastSocket.set_broadcasting(true);
		this->broadcastEndpoint.set_address("255.255.255.255", 13370);

		while(true) {
			if(sendBroadcast) {
				this->broadcastSocket.sendTo(this->broadcastEndpoint, this->deviceIdentifier, this->idLength);
				sendBroadcast = false;
			}

			// try to receive from action socket
			this->actionSocketMutex.lock();
			error = this->actionSocket.receiveFrom(client, recvBuf, 2048);
			this->actionSocketMutex.unlock();
			if(error > 5) {
				char *remoteAddress = client.get_address();
				EthernetMessage *msg = EthernetMessage::unserialize(recvBuf);
				if(this->settingsHandler->getSettings()->isConnected && (strcmp(this->settingsHandler->getSettings()->connectedTo, remoteAddress) == 0)) {
					switch(msg->type) {
						case ACTION:
							if(msg->actionType == STOP_CURRENT_ACTION) {
								// we need to handle this case in this thread as the main thread is probably busy right now
								this->settingsHandler->getSettings()->currentActionIsRunning = false;
								delete msg;
								break;
							} else {
								// append to action queue
								this->commandQueue->put(msg);
								continue;
							}
					}
				} else {
					// the connect message should contain a port number in DATA
					// the server will open that port for us so we can send data
					// so the message looks like this: 4 12345|
					//                           msgtype port delim
					if(msg->type == CONNECT) {
						this->settingsHandler->handleConnect(msg, remoteAddress);
						this->dataOutDestPort = (msg->data[3] << 24) | (msg->data[2] << 16) | (msg->data[1] << 8) | (msg->data[0]);
						this->outputEndpoint.set_address(remoteAddress, this->dataOutDestPort);
						EthernetMessage* ack = new EthernetMessage();
						ack->type = ACK;
						ack->dataLength = 0;
						char* msgData = EthernetMessage::serialize(ack);
						this->actionSocketMutex.lock();
						error = this->actionSocket.sendTo(this->outputEndpoint, msgData, 6);
						this->actionSocketMutex.unlock();
						Thread::wait(100);
						delete ack;
						delete[] msgData;
					}
				}
			}

			if(this->settingsHandler->getSettings()->isConnected) {
				uint16_t numMsgs = 0;
				osEvent evt = this->outQueue.get(15);
				while(evt.status == osEventMail && (numMsgs < 200)) {
					//if(numMsgs > 200)
					//	cancel = true;
					EthernetMessage *outMsg;
					outMsg = 0;
					outMsg = (EthernetMessage*) evt.value.p;
					if(outMsg != 0) {
						char* msgData = EthernetMessage::serialize(outMsg);
						this->actionSocketMutex.lock();
						error = this->actionSocket.sendTo(this->outputEndpoint, msgData, 6 + outMsg->dataLength);
						this->actionSocketMutex.unlock();
						if(outMsg->data)
							delete[] outMsg->data;
						this->outQueue.free(outMsg); // clean up
						delete[] msgData;
					}
					numMsgs++;
					evt = this->outQueue.get(15);
				}

				// empty the xram
				unsigned int sentCount = 0;
				/* pre-allocate 21 bytes so we dont have to re-allocate things
				   this can be done, because xram transfers are only used by the canlogger, currently */
				char *xramSendBuffer = new char[21];
				while(this->ramStart != this->ramEnd) {
					if(sentCount > 200)
						break;

					if(this->ramStart > 0x1fff4) {
						this->ramStart = 0;
					}

					char headerBuf[6];
					this->ramMutex.lock();
					ram_read(this->ramStart, headerBuf, 6);
					this->ramMutex.unlock();

					uint32_t dataLength = (headerBuf[5] << 24) | (headerBuf[4] << 16) | (headerBuf[3] << 8) | (headerBuf[2]);
					if(dataLength > 255) {
						continue;
					}

					this->ramMutex.lock();
					ram_read(this->ramStart+6, xramSendBuffer+6, dataLength);
					this->ramMutex.unlock();
					memcpy(xramSendBuffer, headerBuf, 6);
					this->actionSocketMutex.lock();
					this->actionSocket.sendTo(this->outputEndpoint, xramSendBuffer, 6+dataLength);
					this->actionSocketMutex.unlock();
					if(this->ramStart + 6 + dataLength > 0x20000) {
						// wrap around
						this->ramStart = 0;
					} else {
						this->ramStart += 6 + dataLength;
					}
					sentCount++;
				}
				delete[] xramSendBuffer;
			}

			Thread::yield();
		}
	}

	int EthernetManager::sendMessage(MessageType type, char *data, uint32_t dataLength) {
		osStatus error;
		EthernetMessage* out_msg = this->outQueue.calloc(15);
		if(out_msg != 0) {
			out_msg->type = type;
			out_msg->data = data;
			out_msg->dataLength = dataLength;
			error = this->outQueue.put(out_msg);
			if(error == osOK)
				return 0;
			else
				return -1;
		} else
			return -1;
	}

	int EthernetManager::sendFormattedDataMessage(char *msg, ...) {
		va_list argptr;
		va_start(argptr, msg);

		int msg_length = snprintf(NULL, 0, msg, argptr);
		char *data = new char[msg_length];
		sprintf(data, msg, argptr);
		va_end(argptr);

		osStatus error;
		EthernetMessage* out_msg = this->outQueue.alloc(15);
		if(out_msg != 0) {
			out_msg->type = DATA;
			out_msg->data = data;
			out_msg->dataLength = msg_length;
			error = this->outQueue.put(out_msg);
			if(error == osOK)
				return 0;
			else
				return -1;
		} else {
			delete[] data;
		}
		return -1;
	}

int EthernetManager::sendFormattedDebugMessage(char *msg, ...) {
	va_list argptr;
	va_start(argptr, msg);

	int msg_length = snprintf(NULL, 0, msg, argptr);
	char *data = new char[msg_length];
	sprintf(data, msg, argptr);
	va_end(argptr);

	osStatus error;
	EthernetMessage* out_msg = this->outQueue.alloc(15);
	if(out_msg != 0) {
		out_msg->type = DEBUG_MSG;
		out_msg->data = data;
		out_msg->dataLength = msg_length;
		error = this->outQueue.put(out_msg);
		if(error == osOK)
			return 0;
		else
			return -1;
	} else {
		delete[] data;
	}
	return -1;
}

	int EthernetManager::sendMessageBlocking(MessageType type, char *data, uint32_t dataLength) {
		if(this->settingsHandler->getSettings()->isConnected) {
			EthernetMessage *msg = new EthernetMessage();
			msg->type = type;
			msg->dataLength = dataLength;
			msg->data = data;
			char* msgData = EthernetMessage::serialize(msg);

			this->actionSocketMutex.lock();
			int error = this->actionSocket.sendTo(this->outputEndpoint, msgData, 6 + msg->dataLength);
			this->actionSocketMutex.unlock();
			if(msg->data)
				delete[] msg->data;
			delete msg;
			return error;
		} else {
			return -1;
		}
	}

	int EthernetManager::sendMessagesBlocking(EthernetMessage **messages, size_t numMessages) {
		if(this->settingsHandler->getSettings()->isConnected) {
			char *packet = new char[130];
			int error = 0;
			for(unsigned int i = 0; i < numMessages; i++) {
				EthernetMessage *msg = messages[i];
				snprintf(packet, 129, "M|%d|%s", msg->type, msg->data);
				error = this->actionSocket.sendTo(this->outputEndpoint, packet, strlen(packet));
				if(error != 0)
					return -1;
			}
			return 0;
		}
		return -1;
	}

int EthernetManager::sendRamFrame(MessageType type, char *data, uint32_t dataLength) {
	// serialize here, send later
	// use already allocated memory, as this is only used by canlogger right now
	EthernetMessage ethMsg;
	ethMsg.type = type;
	ethMsg.dataLength = dataLength;
	ethMsg.data = data;
	char *msgData = EthernetMessage::serialize(&ethMsg, this->xramSerializationBuffer);
	this->ramMutex.lock();
	if(this->ramEnd + 6 + dataLength > 0x20000) {
		if(this->ramStart > 6 + dataLength) {
			// we have to wrap around
			ram_write(0, msgData, 6 + dataLength);
			this->ramEnd = 6 + dataLength;
			this->ramMutex.unlock();
			return 6 + dataLength;
		} else {
			// no space anymore, drop
			this->ramMutex.unlock();
			return -1;
		}
	} else {
		ram_write(this->ramEnd, msgData, 6 + dataLength);
		this->ramEnd += 6 + dataLength;
		this->ramMutex.unlock();
		return 6 + dataLength;
	}
	this->ramMutex.unlock();
	return -1;
}

	int EthernetManager::debugLog(char *debugMsg) {
		EthernetMessage * msg = this->outQueue.alloc();
		msg->type = DEBUG_MSG;
		int msg_len = strlen(debugMsg);
		msg->data = new char[msg_len];
		msg->dataLength = msg_len;
		strncpy(msg->data, debugMsg, msg_len-1);
		msg->data[msg_len-1] = 0;
		osStatus status = this->outQueue.put(msg);
		if(status == osOK)
			return 0;
		else
			return -1;
	}

	uint32_t EthernetManager::ram_write (uint32_t addr, char *buf, uint32_t len) {
		uint32_t i;
	    *ramCS1 = 0;
	    this->ram->write(CMD_WRITE);
	    this->ram->write((addr >> 16) & 0xff);
	    this->ram->write((addr >> 8) & 0xff);
	    this->ram->write(addr & 0xff);

	    for (i = 0; i < len; i ++) {
	    	this->ram->write(buf[i]);
	    }
	    *ramCS1 = 1;
	    return i;
	}

	uint32_t EthernetManager::ram_read (uint32_t addr, char *buf, uint32_t len) {
		uint32_t i;

	    *ramCS1 = 0;
	    this->ram->write(CMD_READ);
	    this->ram->write((addr >> 16) & 0xff);
	    this->ram->write((addr >> 8) & 0xff);
	    this->ram->write(addr & 0xff);

	    for (i = 0; i < len; i ++) {
	        buf[i] = this->ram->write(0);
	    }
	    *ramCS1 = 1;
	    return i;
	}

void EthernetManager::resetRam() {
	this->ramMutex.lock();
	this->ramStart = 0;
	this->ramEnd = 0;
	this->ramMutex.unlock();
}
