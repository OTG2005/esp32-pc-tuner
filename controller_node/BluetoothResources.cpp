#include "BluetoothResources.h"
#include "RuntimeResources.h"
#include <esp_bt.h>
#include <esp_err.h>
#include <esp32/rom/ets_sys.h>

bool startTunerBluetoothController() {
  esp_bt_controller_status_t state = esp_bt_controller_get_status();
  if (state == ESP_BT_CONTROLLER_STATUS_ENABLED) return true;
  if (state == ESP_BT_CONTROLLER_STATUS_IDLE) {
    esp_bt_controller_config_t config = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    config.mode = ESP_BT_MODE_BTDM;
    config.ble_max_conn = runtime_resources::BLE_CONNECTIONS;
    config.bt_max_acl_conn = runtime_resources::CLASSIC_CONNECTIONS;
    const esp_err_t result = esp_bt_controller_init(&config);
    if (result != ESP_OK) {
      ets_printf("[bluetooth] controller init failed: %s (0x%x)\n",
          esp_err_to_name(result), static_cast<unsigned>(result));
      return false;
    }
    state = esp_bt_controller_get_status();
  }
  if (state == ESP_BT_CONTROLLER_STATUS_INITED) {
    const esp_err_t result = esp_bt_controller_enable(ESP_BT_MODE_BTDM);
    if (result != ESP_OK) {
      ets_printf("[bluetooth] controller enable failed: %s (0x%x)\n",
          esp_err_to_name(result), static_cast<unsigned>(result));
      return false;
    }
  }
  return esp_bt_controller_get_status() == ESP_BT_CONTROLLER_STATUS_ENABLED;
}
