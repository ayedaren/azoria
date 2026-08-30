#include "services/ble_transport.h"

#include <BLEDevice.h>
#include <BLEServer.h>
#include <Update.h>
#include <mbedtls/md.h>
#include <mbedtls/sha256.h>

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

namespace {

constexpr char kServiceUuid[] = "7A6F0001-4E6D-4A9B-8F41-3C41588FEE68";
constexpr char kRequestUuid[] = "7A6F0002-4E6D-4A9B-8F41-3C41588FEE68";
constexpr char kResponseUuid[] = "7A6F0003-4E6D-4A9B-8F41-3C41588FEE68";
constexpr char kOtaControlUuid[] = "7A6F0004-4E6D-4A9B-8F41-3C41588FEE68";
constexpr char kOtaDataUuid[] = "7A6F0005-4E6D-4A9B-8F41-3C41588FEE68";
constexpr char kOtaStatusUuid[] = "7A6F0006-4E6D-4A9B-8F41-3C41588FEE68";

String auth_token;
String boot_nonce;
BLECharacteristic *request_characteristic = nullptr;
BLECharacteristic *ota_control_characteristic = nullptr;
BLECharacteristic *ota_data_characteristic = nullptr;
BLECharacteristic *ota_status_characteristic = nullptr;
SemaphoreHandle_t response_mutex = nullptr;
SemaphoreHandle_t response_ready = nullptr;
SemaphoreHandle_t exchange_mutex = nullptr;
volatile bool central_connected = false;
volatile bool central_subscribed = false;
uint32_t next_request_id = 1;
String received_response;
bool ota_active = false;
bool ota_sha_initialized = false;
uint32_t ota_expected_size = 0;
uint32_t ota_received_size = 0;
uint32_t ota_next_sequence = 0;
String ota_expected_sha;
mbedtls_sha256_context ota_sha;
bool ota_reboot_pending = false;
uint32_t ota_reboot_at = 0;

String signature(const String &message) {
  uint8_t digest[32]{};
  mbedtls_md_context_t context;
  mbedtls_md_init(&context);
  const mbedtls_md_info_t *info =
      mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  if (!info ||
      mbedtls_md_setup(&context, info, 1) != 0 ||
      mbedtls_md_hmac_starts(
          &context,
          reinterpret_cast<const unsigned char *>(auth_token.c_str()),
          auth_token.length()) != 0 ||
      mbedtls_md_hmac_update(
          &context,
          reinterpret_cast<const unsigned char *>(message.c_str()),
          message.length()) != 0 ||
      mbedtls_md_hmac_finish(&context, digest) != 0) {
    mbedtls_md_free(&context);
    return "";
  }
  mbedtls_md_free(&context);
  char output[17]{};
  for (size_t index = 0; index < 8; ++index) {
    snprintf(output + index * 2, 3, "%02x", digest[index]);
  }
  return String(output);
}

String field(const String &message, int wanted) {
  int start = 0;
  int current = 0;
  while (start <= static_cast<int>(message.length())) {
    int end = message.indexOf('|', start);
    if (end < 0) end = message.length();
    if (current == wanted) return message.substring(start, end);
    ++current;
    start = end + 1;
  }
  return "";
}

int fieldCount(const String &message) {
  if (message.isEmpty()) return 0;
  int count = 1;
  for (size_t index = 0; index < message.length(); ++index) {
    if (message[index] == '|') ++count;
  }
  return count;
}

String jsonString(const char *json, const char *key) {
  String source(json ? json : "");
  String marker = "\"" + String(key) + "\":\"";
  int start = source.indexOf(marker);
  if (start < 0) return "";
  start += marker.length();
  int end = source.indexOf('"', start);
  return end < 0 ? String() : source.substring(start, end);
}

String jsonScalar(const char *json, const char *key) {
  String source(json ? json : "");
  String marker = "\"" + String(key) + "\":";
  int start = source.indexOf(marker);
  if (start < 0) return "";
  start += marker.length();
  while (start < static_cast<int>(source.length()) &&
         (source[start] == ' ' || source[start] == '\t')) {
    ++start;
  }
  int end = start;
  while (end < static_cast<int>(source.length()) &&
         source[end] != ',' && source[end] != '}') {
    ++end;
  }
  String value = source.substring(start, end);
  value.trim();
  return value;
}

bool constantTimeEqual(const String &left, const String &right) {
  if (left.length() != right.length()) return false;
  uint8_t difference = 0;
  for (size_t index = 0; index < left.length(); ++index) {
    difference |= static_cast<uint8_t>(left[index] ^ right[index]);
  }
  return difference == 0;
}

bool isHexSha256(const String &value) {
  if (value.length() != 64) return false;
  for (size_t index = 0; index < value.length(); ++index) {
    const char character = value[index];
    if (!((character >= '0' && character <= '9') ||
          (character >= 'a' && character <= 'f') ||
          (character >= 'A' && character <= 'F'))) {
      return false;
    }
  }
  return true;
}

String sha256Hex(const uint8_t *digest) {
  char output[65]{};
  for (size_t index = 0; index < 32; ++index) {
    snprintf(output + index * 2, 3, "%02x", digest[index]);
  }
  return String(output);
}

void sendOtaStatus(const String &status) {
  if (!ota_status_characteristic || !central_connected) return;
  ota_status_characteristic->setValue(status);
  ota_status_characteristic->notify();
}

void clearOtaSha() {
  if (ota_sha_initialized) {
    mbedtls_sha256_free(&ota_sha);
    ota_sha_initialized = false;
  }
}

void abortOta(const String &reason) {
  if (Update.isRunning()) Update.abort();
  clearOtaSha();
  ota_active = false;
  ota_expected_size = 0;
  ota_received_size = 0;
  ota_next_sequence = 0;
  ota_expected_sha = "";
  sendOtaStatus("ERROR|" + reason);
}

void finishOtaSha(String &actual_sha) {
  uint8_t digest[32]{};
  if (!ota_sha_initialized || mbedtls_sha256_finish(&ota_sha, digest) != 0) {
    actual_sha = "";
    clearOtaSha();
    return;
  }
  actual_sha = sha256Hex(digest);
  clearOtaSha();
}

void handleOtaControl(BLECharacteristic *characteristic) {
  String message = characteristic->getValue();
  int separator = message.lastIndexOf('|');
  if (separator < 0) {
    sendOtaStatus("ERROR|protocol");
    return;
  }
  String unsigned_message = message.substring(0, separator);
  String supplied_signature = message.substring(separator + 1);
  if (!constantTimeEqual(signature(unsigned_message), supplied_signature)) {
    sendOtaStatus("ERROR|auth");
    return;
  }

  if (field(message, 0) == "BEGIN" && fieldCount(message) == 4) {
    if (ota_active || Update.isRunning()) {
      sendOtaStatus("ERROR|busy");
      return;
    }
    String size_text = field(message, 1);
    String expected_sha = field(message, 2);
    long parsed_size = size_text.toInt();
    if (parsed_size <= 0 || !isHexSha256(expected_sha) ||
        String(parsed_size) != size_text) {
      sendOtaStatus("ERROR|invalid_begin");
      return;
    }
    ota_expected_size = static_cast<uint32_t>(parsed_size);
    ota_expected_sha = expected_sha;
    ota_expected_sha.toLowerCase();
    if (!Update.begin(ota_expected_size, U_FLASH)) {
      Serial.print("BLE OTA begin failed: ");
      Update.printError(Serial);
      ota_expected_size = 0;
      ota_expected_sha = "";
      sendOtaStatus("ERROR|space");
      return;
    }
    mbedtls_sha256_init(&ota_sha);
    if (mbedtls_sha256_starts(&ota_sha, 0) != 0) {
      abortOta("sha");
      return;
    }
    ota_sha_initialized = true;
    ota_received_size = 0;
    ota_next_sequence = 0;
    ota_active = true;
    sendOtaStatus("READY|" + String(ota_expected_size));
    return;
  }

  if (field(message, 0) == "END" && fieldCount(message) == 4) {
    if (!ota_active || field(message, 1) != String(ota_expected_size) ||
        field(message, 2) != ota_expected_sha || unsigned_message !=
        ("END|" + String(ota_expected_size) + "|" + ota_expected_sha)) {
      sendOtaStatus("ERROR|not_ready");
      return;
    }
    String actual_sha;
    finishOtaSha(actual_sha);
    if (ota_received_size != ota_expected_size ||
        !constantTimeEqual(actual_sha, ota_expected_sha)) {
      abortOta("checksum");
      return;
    }
    ota_active = false;
    if (!Update.end(false)) {
      Serial.print("BLE OTA finish failed: ");
      Update.printError(Serial);
      abortOta("finish");
      return;
    }
    sendOtaStatus("DONE|" + String(ota_received_size));
    ota_reboot_pending = true;
    ota_reboot_at = millis() + 1200;
    return;
  }
  sendOtaStatus("ERROR|protocol");
}

void handleOtaData(BLECharacteristic *characteristic) {
  const size_t length = characteristic->getLength();
  uint8_t *data = characteristic->getData();
  if (!ota_active || !data || length <= 4) {
    if (!ota_active) sendOtaStatus("ERROR|not_ready");
    return;
  }
  uint32_t sequence = static_cast<uint32_t>(data[0]) |
                      (static_cast<uint32_t>(data[1]) << 8) |
                      (static_cast<uint32_t>(data[2]) << 16) |
                      (static_cast<uint32_t>(data[3]) << 24);
  const size_t chunk_size = length - 4;
  if (sequence != ota_next_sequence ||
      chunk_size > ota_expected_size - ota_received_size ||
      Update.write(data + 4, chunk_size) != chunk_size ||
      mbedtls_sha256_update(&ota_sha, data + 4, chunk_size) != 0) {
    abortOta("chunk");
    return;
  }
  ota_received_size += static_cast<uint32_t>(chunk_size);
  ++ota_next_sequence;
  if ((ota_next_sequence & 0x0f) == 0 ||
      ota_received_size == ota_expected_size) {
    sendOtaStatus("PROGRESS|" + String(ota_received_size) + "|" +
                  String(ota_expected_size));
  }
}

void acceptResponse(BLECharacteristic *characteristic) {
  if (!response_mutex || !response_ready) return;
  String value = characteristic->getValue();
  if (value.isEmpty()) return;
  xSemaphoreTake(response_mutex, portMAX_DELAY);
  received_response = value;
  xSemaphoreGive(response_mutex);
  xSemaphoreGive(response_ready);
}

class ServerCallbacks : public BLEServerCallbacks {
 public:
  void onConnect(BLEServer *) override {
    central_connected = true;
    Serial.println("BLE central connected");
  }

  void onDisconnect(BLEServer *) override {
    central_connected = false;
    central_subscribed = false;
    Serial.println("BLE central disconnected");
    BLEDevice::startAdvertising();
  }
};

class RequestCallbacks : public BLECharacteristicCallbacks {
 public:
#if defined(CONFIG_NIMBLE_ENABLED)
  void onSubscribe(BLECharacteristic *, ble_gap_conn_desc *,
                   uint16_t sub_value) override {
    central_subscribed = sub_value != 0;
    Serial.printf("BLE notifications %s\n",
                  central_subscribed ? "ready" : "disabled");
  }
#endif
};

class ResponseCallbacks : public BLECharacteristicCallbacks {
 public:
  void onWrite(BLECharacteristic *characteristic) override {
    acceptResponse(characteristic);
  }

#if defined(CONFIG_NIMBLE_ENABLED)
  void onWrite(BLECharacteristic *characteristic,
               ble_gap_conn_desc *) override {
    acceptResponse(characteristic);
  }
#endif
};

class OtaControlCallbacks : public BLECharacteristicCallbacks {
 public:
  void onWrite(BLECharacteristic *characteristic) override {
    handleOtaControl(characteristic);
  }

#if defined(CONFIG_NIMBLE_ENABLED)
  void onWrite(BLECharacteristic *characteristic,
               ble_gap_conn_desc *) override {
    handleOtaControl(characteristic);
  }
#endif
};

class OtaDataCallbacks : public BLECharacteristicCallbacks {
 public:
  void onWrite(BLECharacteristic *characteristic) override {
    handleOtaData(characteristic);
  }

#if defined(CONFIG_NIMBLE_ENABLED)
  void onWrite(BLECharacteristic *characteristic,
               ble_gap_conn_desc *) override {
    handleOtaData(characteristic);
  }
#endif
};

bool exchange(const String &unsigned_request, uint32_t request_id,
              String &response, uint32_t timeout_ms) {
  if (!bleTransportReady() || !request_characteristic ||
      !response_mutex || !response_ready || !exchange_mutex) {
    return false;
  }
  if (xSemaphoreTake(exchange_mutex, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
    return false;
  }
  bool success = false;
  do {
    while (xSemaphoreTake(response_ready, 0) == pdTRUE) {
    }
    String message = unsigned_request + "|" + signature(unsigned_request);
    request_characteristic->setValue(message);
    request_characteristic->notify();
    if (xSemaphoreTake(response_ready, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
      Serial.printf("BLE_TIMEOUT,ID=%lu\n",
                    static_cast<unsigned long>(request_id));
      break;
    }
    xSemaphoreTake(response_mutex, portMAX_DELAY);
    response = received_response;
    xSemaphoreGive(response_mutex);

    int last_separator = response.lastIndexOf('|');
    if (last_separator < 0) break;
    String unsigned_response = response.substring(0, last_separator);
    String supplied_signature = response.substring(last_separator + 1);
    if (!constantTimeEqual(signature(unsigned_response), supplied_signature) ||
        field(response, 0) != "R" ||
        field(response, 1) != boot_nonce ||
        field(response, 2).toInt() != static_cast<long>(request_id)) {
      Serial.printf("BLE_AUTH_REJECTED,ID=%lu\n",
                    static_cast<unsigned long>(request_id));
      break;
    }
    success = true;
  } while (false);
  xSemaphoreGive(exchange_mutex);
  return success;
}

bool decodeStatus(const String &wire, String &response) {
  if (fieldCount(wire) != 9 || field(wire, 3) != "S") return false;
  response =
      "{\"brightness\":" + field(wire, 4) +
      ",\"volume\":" + field(wire, 5) +
      ",\"mute\":" + (field(wire, 6) == "1" ? "true" : "false") +
      ",\"input\":\"" + field(wire, 7) +
      "}";
  return true;
}

bool decodeControl(const String &wire, const String &control,
                   String &response) {
  if (fieldCount(wire) == 6 && field(wire, 3) == "E") {
    response =
        "{\"accepted\":false,\"confirmed\":false,\"error\":\"" +
        field(wire, 4) + "\"}";
    return true;
  }
  if (fieldCount(wire) != 8 || field(wire, 3) != "C") return false;
  String returned = field(wire, 6);
  if (control == "input") {
    returned = "\"" + returned + "\"";
  } else if (control == "mute") {
    returned = returned == "1" ? "true" : "false";
  }
  response =
      "{\"accepted\":" + String(field(wire, 4) == "1" ? "true" : "false") +
      ",\"confirmed\":" + String(field(wire, 5) == "1" ? "true" : "false") +
      ",\"value\":" + returned + "}";
  return true;
}

bool decodeGet(const String &wire, String &response) {
  if (fieldCount(wire) != 6 || field(wire, 3) != "G") return false;
  response = field(wire, 4);
  return !response.isEmpty();
}

}  // namespace

bool startBleTransport(const String &device_name, const String &token) {
  if (token.length() < 20) return false;
  auth_token = token;
  char nonce[9]{};
  snprintf(nonce, sizeof(nonce), "%08lx",
           static_cast<unsigned long>(esp_random()));
  boot_nonce = nonce;
  response_mutex = xSemaphoreCreateMutex();
  response_ready = xSemaphoreCreateBinary();
  exchange_mutex = xSemaphoreCreateMutex();
  if (!response_mutex || !response_ready || !exchange_mutex) return false;

  if (!BLEDevice::init(device_name.c_str())) return false;
  BLEDevice::setMTU(256);
  BLEServer *server = BLEDevice::createServer();
  server->setCallbacks(new ServerCallbacks());
  server->advertiseOnDisconnect(true);
  BLEService *service = server->createService(kServiceUuid);
  request_characteristic = service->createCharacteristic(
      kRequestUuid,
      BLECharacteristic::PROPERTY_READ |
          BLECharacteristic::PROPERTY_NOTIFY);
  request_characteristic->setCallbacks(new RequestCallbacks());
  request_characteristic->setValue("Azoria BLE ready");
  BLECharacteristic *response_characteristic =
      service->createCharacteristic(
          kResponseUuid,
          BLECharacteristic::PROPERTY_WRITE |
              BLECharacteristic::PROPERTY_WRITE_NR);
  response_characteristic->setCallbacks(new ResponseCallbacks());
  ota_control_characteristic = service->createCharacteristic(
      kOtaControlUuid,
      BLECharacteristic::PROPERTY_WRITE |
          BLECharacteristic::PROPERTY_WRITE_NR);
  ota_control_characteristic->setCallbacks(new OtaControlCallbacks());
  ota_data_characteristic = service->createCharacteristic(
      kOtaDataUuid,
      BLECharacteristic::PROPERTY_WRITE_NR |
          BLECharacteristic::PROPERTY_WRITE);
  ota_data_characteristic->setCallbacks(new OtaDataCallbacks());
  ota_status_characteristic = service->createCharacteristic(
      kOtaStatusUuid,
      BLECharacteristic::PROPERTY_READ |
          BLECharacteristic::PROPERTY_NOTIFY);
  ota_status_characteristic->setValue("IDLE");
  service->start();

  BLEAdvertising *advertising = BLEDevice::getAdvertising();
  advertising->addServiceUUID(kServiceUuid);
  advertising->setScanResponse(true);
  advertising->setMinPreferred(0x06);
  advertising->setMaxPreferred(0x12);
  BLEDevice::startAdvertising();
  Serial.printf("BLE advertising: %s, nonce=%s\n",
                device_name.c_str(), boot_nonce.c_str());
  return true;
}

bool bleTransportReady() {
  return central_connected && central_subscribed;
}

void bleTransportLoop() {
  if (ota_reboot_pending && static_cast<int32_t>(millis() - ota_reboot_at) >= 0) {
    Serial.println("BLE OTA rebooting");
    Serial.flush();
    ESP.restart();
  }
}

bool bleTransportRequest(const char *method, const String &path,
                         const char *body, String &response,
                         uint32_t timeout_ms) {
  uint32_t request_id = next_request_id++;
  String unsigned_request =
      "Q|" + boot_nonce + "|" + String(request_id);
  String wire;
  if (!strcmp(method, "GET") && path == "/v1/status") {
    unsigned_request += "|S";
    if (!exchange(unsigned_request, request_id, wire, timeout_ms)) {
      return false;
    }
    return decodeStatus(wire, response);
  }
  if (!strcmp(method, "POST") && path == "/v1/control" && body) {
    String control = jsonString(body, "control");
    if (control != "brightness" && control != "volume" &&
        control != "mute" && control != "input") {
      return false;
    }
    String value =
        control == "input" ? jsonString(body, "value")
                           : jsonScalar(body, "value");
    String final_value = jsonScalar(body, "final");
    if (control.isEmpty() || value.isEmpty()) return false;
    if (value == "true") value = "1";
    if (value == "false") value = "0";
    unsigned_request +=
        "|C|" + control + "|" + value + "|" +
        (final_value == "false" ? "0" : "1");
    if (!exchange(unsigned_request, request_id, wire, timeout_ms)) {
      return false;
    }
    return decodeControl(wire, control, response);
  }
  return false;
}
