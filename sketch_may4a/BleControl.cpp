#include "BleControl.h"
#include "ControlProtocol.h"
#include "RuntimeResources.h"
#include <Arduino.h>
#include <atomic>
#include <esp_gap_ble_api.h>
#include <esp_gatts_api.h>
#include <esp_gatt_common_api.h>
#include <esp32/rom/ets_sys.h>
#include <string.h>

namespace {
// ESP-IDF UUID byte arrays are little-endian. These are the standard NUS UUIDs.
uint8_t serviceUuid[16] = {
  0x9e,0xca,0xdc,0x24,0x0e,0xe5,0xa9,0xe0,0x93,0xf3,0xa3,0xb5,0x01,0x00,0x40,0x6e
};
uint8_t rxUuid[16] = {
  0x9e,0xca,0xdc,0x24,0x0e,0xe5,0xa9,0xe0,0x93,0xf3,0xa3,0xb5,0x02,0x00,0x40,0x6e
};
uint8_t txUuid[16] = {
  0x9e,0xca,0xdc,0x24,0x0e,0xe5,0xa9,0xe0,0x93,0xf3,0xa3,0xb5,0x03,0x00,0x40,0x6e
};
uint16_t primaryUuid = ESP_GATT_UUID_PRI_SERVICE;
uint16_t declarationUuid = ESP_GATT_UUID_CHAR_DECLARE;
uint16_t cccdUuid = ESP_GATT_UUID_CHAR_CLIENT_CONFIG;
uint8_t txProperties = ESP_GATT_CHAR_PROP_BIT_READ | ESP_GATT_CHAR_PROP_BIT_NOTIFY;
uint8_t rxProperties = ESP_GATT_CHAR_PROP_BIT_WRITE | ESP_GATT_CHAR_PROP_BIT_WRITE_NR;
uint8_t txInitial[20] = {};
uint8_t rxInitial[1] = {};
uint8_t cccdInitial[2] = {};
enum AttributeIndex { Service, TxDeclaration, TxValue, TxCccd, RxDeclaration, RxValue, AttributeCount };
uint16_t handles[AttributeCount] = {};
const esp_gatts_attr_db_t database[AttributeCount] = {
  {{ESP_GATT_AUTO_RSP}, {ESP_UUID_LEN_16, reinterpret_cast<uint8_t *>(&primaryUuid),
    ESP_GATT_PERM_READ, 16, 16, serviceUuid}},
  {{ESP_GATT_AUTO_RSP}, {ESP_UUID_LEN_16, reinterpret_cast<uint8_t *>(&declarationUuid),
    ESP_GATT_PERM_READ, 1, 1, &txProperties}},
  {{ESP_GATT_AUTO_RSP}, {ESP_UUID_LEN_128, txUuid,
    ESP_GATT_PERM_READ, sizeof(txInitial), 0, txInitial}},
  {{ESP_GATT_AUTO_RSP}, {ESP_UUID_LEN_16, reinterpret_cast<uint8_t *>(&cccdUuid),
    ESP_GATT_PERM_READ | ESP_GATT_PERM_WRITE, 2, 2, cccdInitial}},
  {{ESP_GATT_AUTO_RSP}, {ESP_UUID_LEN_16, reinterpret_cast<uint8_t *>(&declarationUuid),
    ESP_GATT_PERM_READ, 1, 1, &rxProperties}},
  {{ESP_GATT_AUTO_RSP}, {ESP_UUID_LEN_128, rxUuid,
    ESP_GATT_PERM_WRITE, COMMAND_CAPACITY, 0, rxInitial}}
};

std::atomic<bool> connected{false};
std::atomic<bool> subscribed{false};
std::atomic<bool> ready{false};
std::atomic<bool> advertiseRequested{false};
std::atomic<uint8_t> advertisementPending{3};
std::atomic<uint16_t> connectionId{0};
esp_gatt_if_t interfaceId = ESP_GATT_IF_NONE;
BleCommandReceiver commandReceiver = nullptr;
CommandLineBuffer commandLines;

esp_ble_adv_params_t advertising = {
  .adv_int_min = 0x100, .adv_int_max = 0x180,
  .adv_type = ADV_TYPE_IND, .own_addr_type = BLE_ADDR_TYPE_PUBLIC,
  .peer_addr = {}, .peer_addr_type = BLE_ADDR_TYPE_PUBLIC,
  .channel_map = ADV_CHNL_ALL,
  .adv_filter_policy = ADV_FILTER_ALLOW_SCAN_ANY_CON_ANY
};

bool check(const char *operation, esp_err_t result) {
  if (result == ESP_OK) return true;
  ets_printf("[bluetooth] %s failed: %s (0x%x)\n",
      operation, esp_err_to_name(result), static_cast<unsigned>(result));
  return false;
}

void gapEvents(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param) {
  if (event == ESP_GAP_BLE_ADV_DATA_SET_COMPLETE_EVT) {
    if (param->adv_data_cmpl.status == ESP_BT_STATUS_SUCCESS) {
      advertisementPending.fetch_and(static_cast<uint8_t>(~1U));
      advertiseRequested = true;
    }
  } else if (event == ESP_GAP_BLE_SCAN_RSP_DATA_SET_COMPLETE_EVT) {
    if (param->scan_rsp_data_cmpl.status == ESP_BT_STATUS_SUCCESS) {
      advertisementPending.fetch_and(static_cast<uint8_t>(~2U));
      advertiseRequested = true;
    }
  } else if (event == ESP_GAP_BLE_ADV_START_COMPLETE_EVT) {
    if (param->adv_start_cmpl.status == ESP_BT_STATUS_SUCCESS)
      ets_printf("[bluetooth] BLE UART advertising ready (one client)\n");
    else
      ets_printf("[bluetooth] advertising failed: status=%u\n",
          static_cast<unsigned>(param->adv_start_cmpl.status));
  }
}

void gattEvents(esp_gatts_cb_event_t event, esp_gatt_if_t interface,
                esp_ble_gatts_cb_param_t *param) {
  if (event == ESP_GATTS_REG_EVT) {
    if (param->reg.status != ESP_GATT_OK) {
      ets_printf("[bluetooth] UART app registration failed\n");
      return;
    }
    interfaceId = interface;
    check("UART attribute table", esp_ble_gatts_create_attr_tab(
        database, interfaceId, AttributeCount, 0));
  } else if (event == ESP_GATTS_CREAT_ATTR_TAB_EVT) {
    if (param->add_attr_tab.status != ESP_GATT_OK ||
        param->add_attr_tab.num_handle != AttributeCount) {
      ets_printf("[bluetooth] UART attribute table failed: status=%u\n",
          static_cast<unsigned>(param->add_attr_tab.status));
      return;
    }
    memcpy(handles, param->add_attr_tab.handles, sizeof(handles));
    check("UART service start", esp_ble_gatts_start_service(handles[Service]));
  } else if (event == ESP_GATTS_START_EVT) {
    if (param->start.status == ESP_GATT_OK) {
      ready = true;
      advertiseRequested = true;
      logRuntimeMemory("BLE UART service ready");
    }
  } else if (event == ESP_GATTS_CONNECT_EVT) {
    connectionId = param->connect.conn_id;
    subscribed = false;
    commandLines.reset();
    connected = true;
    ets_printf("[bluetooth] BLE client connected: id=%u\n",
        static_cast<unsigned>(param->connect.conn_id));
    logRuntimeMemory("BLE connected");
  } else if (event == ESP_GATTS_DISCONNECT_EVT) {
    connected = false;
    subscribed = false;
    commandLines.reset();
    advertiseRequested = true;
    ets_printf("[bluetooth] BLE client disconnected: reason=0x%x\n",
        static_cast<unsigned>(param->disconnect.reason));
    logRuntimeMemory("BLE disconnected");
  } else if (event == ESP_GATTS_WRITE_EVT && !param->write.is_prep) {
    if (param->write.handle == handles[TxCccd] && param->write.len == 2) {
      subscribed = (param->write.value[0] & 1U) != 0;
    } else if (param->write.handle == handles[RxValue]) {
      commandLines.feed(reinterpret_cast<const char *>(param->write.value),
          param->write.len, [](const char *line) {
            if (commandReceiver) commandReceiver(line);
          });
    }
  }
}
}

bool setupBleUart(const char *name, BleCommandReceiver receiver) {
  commandReceiver = receiver;
  logRuntimeMemory("before compact BLE UART");
  // Only register a GATT SERVER. The old BLE wrapper also registered an unused
  // GATT client and allocated per-object semaphores/maps.
  if (!check("GAP callback", esp_ble_gap_register_callback(gapEvents)) ||
      !check("GATT server callback", esp_ble_gatts_register_callback(gattEvents)) ||
      !check("BLE name", esp_ble_gap_set_device_name(name)) ||
      !check("BLE MTU", esp_ble_gatt_set_local_mtu(23))) return false;

  esp_ble_adv_data_t advertisement = {};
  advertisement.service_uuid_len = sizeof(serviceUuid);
  advertisement.p_service_uuid = serviceUuid;
  advertisement.flag = ESP_BLE_ADV_FLAG_GEN_DISC | ESP_BLE_ADV_FLAG_BREDR_NOT_SPT;
  esp_ble_adv_data_t scanResponse = {};
  scanResponse.set_scan_rsp = true;
  scanResponse.include_name = true;
  if (!check("advertisement data", esp_ble_gap_config_adv_data(&advertisement)) ||
      !check("scan response", esp_ble_gap_config_adv_data(&scanResponse))) return false;
  return check("UART application", esp_ble_gatts_app_register(0x5043));
}

void serviceBleUart() {
  if (!connected.load() && ready.load() && advertisementPending.load() == 0 &&
      advertiseRequested.exchange(false)) {
    check("advertising start", esp_ble_gap_start_advertising(&advertising));
  }
}

bool bleUartConnected() { return connected.load(); }
bool bleUartSubscribed() { return connected.load() && subscribed.load(); }
bool sendBleUart(const uint8_t *bytes, size_t length) {
  if (!bleUartSubscribed() || !length || length > 20) return false;
  if (esp_ble_gatts_set_attr_value(handles[TxValue], length, bytes) != ESP_OK) return false;
  return esp_ble_gatts_send_indicate(interfaceId, connectionId.load(), handles[TxValue],
      length, const_cast<uint8_t *>(bytes), false) == ESP_OK;
}
