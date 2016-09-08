/*
 * TestManager.cpp
 *
 *  Created on: Jun 22, 2016
 *      Author: user
 */

#include <test_manager.hpp>

TestManager::TestManager(EthernetManager* ethMan, SettingsHandler* settingsHandler, CAN *can1, CAN *can2) {
	this->ethMan = ethMan;
	this->settingsHandler = settingsHandler;
	this->isTesting = false;
	this->can1 = can1;
	this->can2 = can2;
}

TestManager::~TestManager() {}

void TestManager::performCanLoggerTest() {
	this->isTesting = true;
	//Thread thr(cl->startLogging, NULL, osPriorityNormal, (DEFAULT_STACK_SIZE * 2));
	uint8_t BSBuffer[2048]={0}; //Buffer to store bullshit
	uint32_t BSCounter1=0; //Counter for random bullshit
	uint32_t BSCounter2=0; //Counter for random bullshit
	CanLogger *cl = new CanLogger(this->ethMan, this->settingsHandler, BSBuffer, &BSCounter1, &BSCounter2, this->can1, this->can2);
	cl->startLogging();
	while(this->isTesting) { Thread::yield(); }
	cl->stopLogging();
}

void TestManager::stopTesting() {
	this->isTesting = false;
}

void TestManager::resetDevice() {
	NVIC_SystemReset();
}
