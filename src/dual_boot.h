#pragma once

// X3 dual-boot with CrossInk (this fork only, not upstream): the main menu gets one extra row that boots the app in
// the other OTA slot. OtaBootSwitch.{h,cpp} are verbatim copies from CrossInk (src/network/); keep them identical.
namespace dualboot {

void begin();             // registers this app's name in NVS ota_names (CrossInk reads it) and finds the other app
const char* otherName();  // the other app's name, or nullptr when its slot holds no valid app
void switchToOther();     // points otadata at the other app and restarts; returns only on failure

}  // namespace dualboot
