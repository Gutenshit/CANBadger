/*
 * settings_handler.cpp
 *
 *  Created on: Apr 6, 2016
 *      Author: codewhite
 */

#include "settings_handler.hpp"

SettingsHandler::SettingsHandler(CanbadgerSettings *settings) {
	this->settings = settings;
}

bool SettingsHandler::handleSettingsUpdate(EthernetMessage *msg) {
	// setting updates are key;value
	char *semicolon_pos = strrchr(msg->data, ';');

	if(semicolon_pos > 0) {
		size_t key_length = semicolon_pos - msg->data;
		size_t value_length = msg->dataLength - key_length;
		char *key = new char[key_length+1];
		char *value = new char[value_length];
		strncpy(key, msg->data, semicolon_pos - msg->data);
		strncpy(value, semicolon_pos + 1, value_length);

		if(!key || !value)
			return false;

		// update settings
		this->settings->set(key, value);

		// persist
		this->settings->persist();
		return true;
	}

	delete[] msg->data;
	delete msg;

	return false;
}

void SettingsHandler::handleConnect(EthernetMessage *msg, char *remoteAddress) {
	// copy the string to a safe location
	int addr_len = strlen(remoteAddress);
	char *connectedTo = new char[addr_len+1];
	strncpy(connectedTo, remoteAddress, addr_len);
	connectedTo[addr_len] = 0x00;
	this->settings->connectedTo = connectedTo;
	this->settings->isConnected = true;
}

CanbadgerSettings* SettingsHandler::getSettings() {
	return this->settings;
}
