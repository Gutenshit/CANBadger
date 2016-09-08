/*
 * ethernet_message.cpp
 *
 *  Created on: Apr 11, 2016
 *      Author: codewhite
 */

#include "ethernet_message.hpp"

char* EthernetMessage::serialize(EthernetMessage* msg, char *buf) {
	if(msg->type != ACK && msg->type != NACK) {
		if(msg->dataLength == 0)
			return 0;
	}
	char *ethMsg;
	if(buf == NULL)
		ethMsg = new char[6+msg->dataLength];
	else
		ethMsg = buf;

	ethMsg[0] = (uint8_t) msg->type;
	ethMsg[1] = (uint8_t) msg->actionType;
	ethMsg[5] = (msg->dataLength >> 24) & 0xFF;
	ethMsg[4] = (msg->dataLength >> 16) & 0xFF;
	ethMsg[3] = (msg->dataLength >> 8) & 0xFF;
	ethMsg[2] = msg->dataLength & 0xFF;
	memcpy(ethMsg+6, msg->data, msg->dataLength);

	return ethMsg;
}

EthernetMessage* EthernetMessage::unserialize(char *data) {
	EthernetMessage *msg = new EthernetMessage;
	EthernetMessageHeader header;
	header.type = (uint8_t) data[0];
	header.actionType = (uint8_t) data[1];
	header.dataLength = (data[5] << 24) | (data[4] << 16) | (data[3] << 8) | (data[2]);

	// enforce data length boundaries
	if(header.dataLength < 0 || header.dataLength > 2048)
		header.dataLength = 0;

	// set types
	msg->type = (MessageType) header.type;
	msg->actionType = (ActionType) header.actionType;
	msg->dataLength = header.dataLength;

	// copy data
	msg->data = new char[header.dataLength+1];
	memcpy(msg->data, data+6, header.dataLength);
	msg->data[header.dataLength] = 0;

	return msg;
}


