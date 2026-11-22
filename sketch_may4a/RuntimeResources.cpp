#include "RuntimeResources.h"
#include <Arduino.h>
#include <esp_bt.h>
#include <esp_err.h>
#include <esp_heap_caps.h>
#include <esp32/rom/ets_sys.h>

namespace {
void onAllocationFailure(size_t size, uint32_t capabilities, const char *function) {
  // ROM output does not allocate a String/buffer when the heap is exhausted.
  ets_printf("[memory] ALLOCATION FAILED: bytes=%u caps=0x%08x function=%s\n",
      static_cast<unsigned>(size), static_cast<unsigned>(capabilities),
      function ? function : "unknown");
}
}

void installRuntimeDiagnostics() {
  const esp_err_t result = heap_caps_register_failed_alloc_callback(onAllocationFailure);
  if (result != ESP_OK) {
    ets_printf("[memory] allocation hook failed: %s (0x%x)\n",
        esp_err_to_name(result), static_cast<unsigned>(result));
  }
}

void logRuntimeMemory(const char *stage) {
  constexpr uint32_t caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
  ets_printf("[memory] %s: free=%u largest=%u minimum=%u dma-largest=%u stack-free=%u\n",
      stage,
      static_cast<unsigned>(heap_caps_get_free_size(caps)),
      static_cast<unsigned>(heap_caps_get_largest_free_block(caps)),
      static_cast<unsigned>(heap_caps_get_minimum_free_size(caps)),
      static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_DMA)),
      static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));
}

bool startTunerBluetoothController() {
  esp_bt_controller_status_t state = esp_bt_controller_get_status();
  if (state == ESP_BT_CONTROLLER_STATUS_ENABLED) return true;
  if (state == ESP_BT_CONTROLLER_STATUS_IDLE) {
    esp_bt_controller_config_t config = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    config.mode = ESP_BT_MODE_BTDM;
    config.ble_max_conn = runtime_resources::BLE_CONNECTIONS;
    config.bt_max_acl_conn = runtime_resources::CLASSIC_CONNECTIONS;
    // Do not change bt_max_sync_conn: IDF 4 says it is compile-time only.
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
