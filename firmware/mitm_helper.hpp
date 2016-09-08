/*
 * mitm_helper.hpp
 *
 *  Created on: Jul 20, 2016
 *      Author: user
 */

#ifndef MITM_HELPER_HPP_
#define MITM_HELPER_HPP_

#include "ethernet_manager.hpp"
#include "ethernet_message.hpp"
#include "SDFileSystem.h"

class MitmHelper {
public:
	MitmHelper();
	virtual ~MitmHelper();

	// clears the rule file
	static void clearRules(SDFileSystem *sd, EthernetManager *eth);
	// adds a rule to the rule file
	static void addRule(SDFileSystem *sd, EthernetMessage *msg, EthernetManager *eth);

private:
	uint32_t getUInt32FromData(uint32_t offset, char *data);
};

#endif /* MITM_HELPER_HPP_ */
