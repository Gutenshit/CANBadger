/*
 * TestManager.hpp
 *
 *  Created on: Jun 22, 2016
 *      Author: user
 */

#ifndef TEST_MANAGER_HPP_
#define TEST_MANAGER_HPP_

#include "mbed.h"
#include <can_logger.hpp>

extern "C" void mbed_reset();

class TestManager {
public:
	TestManager(EthernetManager*, SettingsHandler*, CAN*, CAN*);
	virtual ~TestManager();

	void performCanLoggerTest();

	void stopTesting();

	static void resetDevice();

private:
	EthernetManager *ethMan;
	SettingsHandler *settingsHandler;

	bool isTesting;
	CAN *can1;
	CAN *can2;
};

#endif /* TEST_MANAGER_HPP_ */
