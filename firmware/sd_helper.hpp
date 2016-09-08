
#ifndef SD_HELPER_HPP_
#define SD_HELPER_HPP_

#include <string>
#include <vector>

#include "SDFileSystem.h"
#include "ethernet_manager.hpp"


class SdHelper {
public:
	SdHelper();
	virtual ~SdHelper();

	static void updateSdContents(SDFileSystem *fs, EthernetManager *ethMan, char *folder);
	static void downloadFile(SDFileSystem *fs, EthernetManager *ethMan, char *fileName);
	static void deleteFile(SDFileSystem *fs, EthernetManager *ethMan, char *fileName);
};

#endif /* SD_HELPER_HPP_ */
