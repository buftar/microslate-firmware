#include "dual_boot.h"

#include <Arduino.h>
#include <Logging.h>
#include <Preferences.h>
#include <esp_ota_ops.h>

#include "OtaBootSwitch.h"

namespace dualboot {
namespace {
const esp_partition_t* g_other = nullptr;
char g_name[24] = "";
}  // namespace

void begin() {
  const esp_partition_t* self = esp_ota_get_running_partition();
  const esp_partition_t* other = esp_ota_get_next_update_partition(nullptr);  // two OTA slots: the other one
  if (!self || !other || other == self) return;

  char key[8];
  Preferences prefs;
  if (!prefs.begin("ota_names", false)) return;
  snprintf(key, sizeof(key), "ota_%d", self->subtype - ESP_PARTITION_SUBTYPE_APP_OTA_0);
  if (prefs.getString(key, "") != "MicroSlate") prefs.putString(key, "MicroSlate");  // skip the flash write when set

  esp_app_desc_t desc;
  if (esp_ota_get_partition_description(other, &desc) == ESP_OK) {
    const int slot = other->subtype - ESP_PARTITION_SUBTYPE_APP_OTA_0;
    snprintf(key, sizeof(key), "ota_%d", slot);
    const String name = prefs.getString(key, "");
    if (name.length()) snprintf(g_name, sizeof(g_name), "%s", name.c_str());
    else snprintf(g_name, sizeof(g_name), "OTA Slot %d", slot);
    g_other = other;
    LOG_INF("BOOT", "Other app in slot %d: \"%s\"", slot, g_name);
  }
  prefs.end();
}

const char* otherName() { return g_other ? g_name : nullptr; }

void switchToOther() {
  if (!g_other || !ota_boot::switchTo(g_other)) return;
  esp_restart();
}

}  // namespace dualboot
