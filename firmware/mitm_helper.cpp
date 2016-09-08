/*
 * mitm_helper.cpp
 *
 *  Created on: Jul 20, 2016
 *      Author: user
 */

#include <mitm_helper.hpp>

MitmHelper::MitmHelper() {}

MitmHelper::~MitmHelper() {}

uint32_t MitmHelper::getUInt32FromData(uint32_t offset, char *data) {
	return (data[offset+3] << 24) | (data[offset+2] << 16) | (data[offset+1] << 8) | (data[offset+0]);
}

void MitmHelper::clearRules(SDFileSystem *sd, EthernetManager *eth) {
	sd->remove("/MITM/rules.txt");
	Thread::wait(10);
	eth->sendMessage(ACK, new char[1], 0);
	return;
}

void MitmHelper::addRule(SDFileSystem *sd, EthernetMessage *msg, EthernetManager *eth) {
	// rules come already formatted so we just have to append it to the rules.txt
	// truncate the file first
	FileHandle *fp2 = sd->open("/MITM/rules.txt", O_CREAT | O_RDWR);

	// TODO: create folder if it does not exist
	if(fp2 == NULL) {
		eth->debugLog("Rule file could not be opened! Aborting.");
		eth->sendMessage(NACK, new char[1], 0);
		return;
	}

	if(msg->dataLength < 42 || msg->dataLength > 80) {
		eth->debugLog("Rule has invalid length! Aborting.");
		eth->sendMessage(NACK, new char[1], 0);
		fp2->close();
		return;
	}

	fp2->lseek(0, SEEK_END);
	fp2->write(msg->data, msg->dataLength);
	fp2->write("\n", 1);
	fp2->fsync();
	fp2->close();

	eth->sendMessage(ACK, new char[1], 0);
	delete msg->data;
	return;
}
