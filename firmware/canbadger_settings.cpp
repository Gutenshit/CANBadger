#include "canbadger_settings.hpp"

void CanbadgerSettings::persist() {
	FileHandle *fp = fs->open("/canbadger_settings.txt", O_RDWR | O_CREAT | O_TRUNC);
	if(fp == NULL) {
		// something bad happened! file should be created if it does not exist!
		return;
	}

	fp->write(this->settings["id"], strlen(this->settings["id"]));
	fp->write("\n", 1);

	fp->close();
}

CanbadgerSettings* CanbadgerSettings::restore(SDFileSystem *fs)  {
	FileHandle *fp = fs->open("/canbadger_settings.txt", O_RDONLY);
	if(fp == NULL) {
		// no settings have been saved yet! create new random dummy settings
		return new CanbadgerSettings(fs);
	}

	// read node name
	// node name is stored on the very first line
	// since at this point, we only store the name in this file, we read it like this:
	unsigned int fileLength = fp->flen();
	char *name = new char[fileLength];
	fp->read(name, fileLength-1);
	fp->close();
	name[fileLength-1] = 0;

	CanbadgerSettings *cbSettings = new CanbadgerSettings(fs);
	cbSettings->settings["id"] = name;

	return cbSettings;
}
