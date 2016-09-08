/*
 * replay_helper.cpp
 *
 *  Created on: Jul 21, 2016
 *      Author: user
 */

#include <replay_helper.hpp>

ReplayHelper::ReplayHelper(EthernetManager *eth, CanbadgerSettings *settings,
		Mail<EthernetMessage, 16> *queue, CAN *can1, CAN *can2) {
	this->eth = eth;
	this->settings = settings;
	this->can1 = can1;
	this->can2 = can2;
	this->queue = queue;
}

ReplayHelper::~ReplayHelper() {}

void ReplayHelper::sendFramesLoop() {
	// take over recv queue and send incoming frames
	this->settings->currentActionIsRunning = true;
	while(this->settings->currentActionIsRunning) {
		osEvent evt = this->queue->get(25);
		if(evt.status == osEventMail) {
			EthernetMessage *msg;
			msg = 0;
			msg = (EthernetMessage*) evt.value.p;
			uint8_t sendTimeout = 0;
			if(msg != 0) {
				if(msg->type == ACTION && msg->actionType == REPLAY) {
					sendTimeout = 0;
					bool sent = false;
					uint16_t canId = msg->data[0] | msg->data[1] << 8;
					CANMessage canMsg(canId, msg->data+2);
					while(!sent && sendTimeout < 20) {
						if(can1->write(canMsg)) {
							sent = true;
						}
						sendTimeout++;
					} // write the message
					if(sent)
						this->eth->sendMessage(ACK, new char[1], 0);
					else
						this->eth->sendMessage(NACK, new char[1], 0);
				}
			}
		}
	}
}
