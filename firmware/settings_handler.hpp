/*
 * settings_handler.hpp
 *
 * this class should handle all the settings update
 *
 *  Created on: Apr 6, 2016
 *      Author: codewhite
 */

#ifndef SETTINGS_HANDLER_HPP_
#define SETTINGS_HANDLER_HPP_

#include "canbadger_settings.hpp"
#include "ethernet_message.hpp"
#include "string.h"

class SettingsHandler {
public:
	SettingsHandler(CanbadgerSettings *settings);

	// handle updateSettings packet
	// returns false on failure
	bool handleSettingsUpdate(EthernetMessage *msg);
	// handle connect
	void handleConnect(EthernetMessage *msg, char *remoteAddress);

	CanbadgerSettings *getSettings();
private:
	CanbadgerSettings *settings;
};

#endif /* SETTINGS_HANDLER_HPP_ */
