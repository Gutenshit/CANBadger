/*
 * EthernetMessage.hpp
 *
 * Application layer packet data structure
 *
 *  Created on: Apr 6, 2016
 *      Author: codewhite
 */

#ifndef ETHERNET_MESSAGE_HPP_
#define ETHERNET_MESSAGE_HPP_

#include "mbed.h"
#include "string.h"

enum MessageType {
	ACK,
	NACK, // nacks can provide a reason / error message in data fied
	DATA, // for raw frame data, can contain multiple frames in data field
	ACTION,
	CONNECT,
	DEBUG_MSG
};


enum ActionType {
	NO_TYPE,
	SETTINGS, // update settings request
	REPLAY,
	LOG_RAW_CAN_TRAFFIC,
	ENABLE_TESTMODE,
	STOP_CURRENT_ACTION,
	RESET,
	START_UDS,
	START_TP,
	UDS,
	TP,
	HIJACK,
	MITM,
	UPDATE_SD,
	DOWNLOAD_FILE,
	DELETE_FILE,
	CLEAR_RULES,
	ADD_RULE,
	ENABLE_MITM_MODE,
	START_REPLAY
};

enum TestType {
	LOG_RAW_CAN_TEST = 1,
	DISABLE_TESTMODE,
	STOP_TEST,
	RESET_DEVICE
};

/*
 * EthernetMessages look like this on the wire:
 * MesgType (1 byte) | ActionType (1 byte) | dataLength (4 byte) | ... data | 0x00 (optional)
 * the types require casting because you can't define the type of an enum
 *  - to circumvent this, ethernetMessage class has two separate fields for them and unserialize will populate them
 *  NOTE: dataLength is transmitted little-endian
 */
struct EthernetMessageHeader {
	uint8_t type;
	uint8_t actionType;
	uint32_t dataLength;
};

class EthernetMessage {
public:
	EthernetMessageHeader header;
	MessageType type;
	ActionType actionType;
	char *data; // string of arbitrary length
	uint32_t dataLength; // size of paylod. important when sending

	// will parse ethernetMessage to wire format
	static char* serialize(EthernetMessage* msg, char* buf = 0);
	// will parse an ethernet message from a datagram / binary data
	static EthernetMessage* unserialize(char* data);
};

#endif /* ETHERNET_MESSAGE_HPP_ */
