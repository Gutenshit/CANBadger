#ifndef CANBADGER_SETTINGS_HPP
#define CANBADGER_SETTINGS_HPP

#include <map>
#include <string>
#include "SDFileSystem.h"

using namespace std;

class CanbadgerSettings {
public:
	CanbadgerSettings(SDFileSystem *fs) {
		this->isConnected = false;
		this->connectedTo = 0;
		this->fs = fs;
		char *id = new char[18];
		strcpy(id, "unnamed CanBadger");
		this->settings["id"] = id;
	};

	void set(char* key, char* value) {
		this->settings[key] = value;
	}

	char* get(char* key) {
		return this->settings[key];
	}

	// used to write settings to the sd
	// to persist settings across reboots
	// settings will be saved in /canbadger_settings.txt
	void persist();

	static CanbadgerSettings* restore(SDFileSystem *fs);

	bool isConnected;
	char *connectedTo; // receiving server ip
	std::map<std::string, char*> settings;
	bool currentActionIsRunning = false;
	SDFileSystem *fs;
};

#endif
