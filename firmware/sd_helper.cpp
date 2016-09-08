#include <sd_helper.hpp>

SdHelper::SdHelper() {}

SdHelper::~SdHelper() {}

void SdHelper::updateSdContents(SDFileSystem *fs, EthernetManager *ethMan, char *folder) {
	string fileNames;
	DIR *dp;
	struct dirent *dirp;
	dp = opendir(folder);

	if(!dp) {
		ethMan->sendMessage(NACK, new char[1], 0);
		return;
	}

	//read all directory and file names in current directory into filename vector
	while((dirp = readdir(dp)) != NULL) {
		fileNames.append(string(dirp->d_name));
		fileNames.append("|");
	}
	closedir(dp);

	// send back results
	char *data = new char[fileNames.length()];
	strncpy(data, fileNames.c_str(), fileNames.length());
	ethMan->sendMessage(DATA, data, (uint32_t) fileNames.length()+1);
	delete[] folder;
}

void SdHelper::downloadFile(SDFileSystem *fs, EthernetManager *ethMan, char *fileName) {
	FileHandle *fp = fs->open(fileName, O_RDONLY);

	if(fp == NULL) {
		ethMan->sendMessage(NACK, new char[1], 0);
		return;
	}

	uint32_t fsize = fp->flen();

	int i;
	int chars_read = 0;
	for(i = fsize; i > 256; i -= 256) {
		// copy the buffer so we can delete it ourselves
		// ethMan will delete it otherwise which could mess up stuff
		// also send blocking so that packets come in order and we dont run out of ram
		char *sliceBuf = new char[257];
		chars_read = fp->read(sliceBuf, 256);
		ethMan->sendMessage(DATA, sliceBuf, 256);
		Thread::wait(10);
	}
	char *lastSlice = new char[i];
	uint32_t last_size = fp->read(lastSlice, 256);
	ethMan->sendMessageBlocking(DATA, lastSlice, last_size);

	fp->close();

	ethMan->sendMessage(ACK, new char[1], 0);
	delete[] fileName;
}

void SdHelper::deleteFile(SDFileSystem *fs, EthernetManager *ethMan, char *fileName) {
	FileHandle *fp = fs->open(fileName, O_RDONLY);

	if(fp == NULL) {
		ethMan->sendMessage(NACK, new char[1], 0);
		return;
	} else {
		fp->close();
		fs->remove(fileName);
	}

	ethMan->sendMessage(ACK, new char[1], 0);
	delete[] fileName;
}
