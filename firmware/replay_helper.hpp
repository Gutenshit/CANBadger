/*
 * replay_helper.hpp
 *
 *  Created on: Jul 21, 2016
 *      Author: user
 */

#ifndef REPLAY_HELPER_HPP_
#define REPLAY_HELPER_HPP_

#include "ethernet_manager.hpp"
#include "ethernet_message.hpp"
#include "canbadger_settings.hpp"
#include "CAN.h"

class ReplayHelper {
public:
	ReplayHelper(EthernetManager *eth, CanbadgerSettings *settings,
			Mail<EthernetMessage, 16> *queue, CAN *can1, CAN *can2);
	virtual ~ReplayHelper();

	void sendFramesLoop();

private:
	EthernetManager *eth;
	CanbadgerSettings *settings;
	CAN *can1;
	CAN *can2;
	Mail<EthernetMessage, 16> *queue;
};

#endif /* REPLAY_HELPER_HPP_ */
