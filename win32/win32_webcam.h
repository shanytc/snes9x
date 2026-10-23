#ifndef WIN32_WEBCAM_H
#define WIN32_WEBCAM_H

#include <vector>
#include <string>

void GBCameraRegister();
void GBCameraEnumerate(std::vector<std::wstring> &names);
bool GBCameraStart(int deviceIndex);
void GBCameraStop();
bool GBCameraIsRunning();

// Copies the latest frame as 8-bit luma, top row first and not mirrored, for
// the Barcode Battler II camera scan. False until a frame has arrived.
bool GBCameraGetFrame(std::vector<unsigned char> &luma, int &width, int &height);

#endif
