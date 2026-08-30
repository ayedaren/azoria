#include "features/display_control/service.h"

#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include "services/ble_transport.h"

namespace DisplayControl {
namespace {

enum class ControlKind : uint8_t {
  Brightness,
  Volume,
  Mute,
  Input,
  Count,
};

struct Command {
  ControlKind kind = ControlKind::Brightness;
  int value = 0;
  char text[8]{};
  uint32_t sequence = 0;
  uint32_t queued_at_ms = 0;
  bool final_value = true;
};

DeviceConfig remote_config;
RemoteState remote_state;
SemaphoreHandle_t state_mutex = nullptr;
TaskHandle_t remote_task_handle = nullptr;
String device_id;
String device_hostname;
constexpr uint16_t kDiscoveryPort = 8733;
constexpr uint16_t kCoordinationPort = 8734;
constexpr uint32_t kDefaultRequestTimeoutMs = 2800;
constexpr uint32_t kFinalRequestTimeoutMs = 9000;
constexpr uint32_t kPreviewMaxAgeMs = 900;
constexpr uint32_t kFinalMaxAgeMs = 1800;
constexpr uint32_t kStatusRequestTimeoutMs = 8000;
constexpr uint32_t kConfirmedSettleMs = 2000;
constexpr uint32_t kRegistrationIntervalMs = 30000;
constexpr uint32_t kIdleStatusIntervalMs = 30000;
constexpr char kFirmwareVersion[] = "0.4.17";
Command latest_commands[static_cast<size_t>(ControlKind::Count)]{};
bool command_available[static_cast<size_t>(ControlKind::Count)]{};
uint32_t next_sequence = 1;
uint32_t desired_sequences[static_cast<size_t>(ControlKind::Count)]{};
uint32_t settle_until[static_cast<size_t>(ControlKind::Count)]{};
bool desktop_address_was_discovered = false;
WiFiUDP discovery_udp;
bool discovery_started = false;
String pending_discovery_nonce;
IPAddress pending_discovery_address;
uint32_t pending_discovery_at = 0;
String pending_result_command_id;
String pending_result_control;
String pending_result_response;
bool pending_result_ready = false;
String command_boot_nonce;

bool isPrivateIpv4(const IPAddress &address) {
  const uint8_t first = address[0];
  const uint8_t second = address[1];
  return first == 10 || first == 127 ||
         (first == 169 && second == 254) ||
         (first == 172 && second >= 16 && second <= 31) ||
         (first == 192 && second == 168);
}

bool isLocalDesktopHost(const String &host) {
  IPAddress address;
  return !host.isEmpty() && address.fromString(host) &&
         isPrivateIpv4(address);
}

IPAddress coordinationBroadcastAddress() {
  IPAddress local = WiFi.localIP();
  IPAddress mask = WiFi.subnetMask();
  return IPAddress(local[0] | static_cast<uint8_t>(~mask[0]),
                   local[1] | static_cast<uint8_t>(~mask[1]),
                   local[2] | static_cast<uint8_t>(~mask[2]),
                   local[3] | static_cast<uint8_t>(~mask[3]));
}

String wireField(const String &wire, int wanted) {
  int start = 0;
  for (int current = 0; current < wanted; ++current) {
    start = wire.indexOf('|', start);
    if (start < 0) return "";
    ++start;
  }
  int end = wire.indexOf('|', start);
  return end < 0 ? wire.substring(start) : wire.substring(start, end);
}

int wireFieldCount(const String &wire) {
  if (wire.isEmpty()) return 0;
  int count = 1;
  for (size_t index = 0; index < wire.length(); ++index) {
    if (wire[index] == '|') ++count;
  }
  return count;
}

int jsonInt(const String &json, const char *key, int fallback);
bool jsonBool(const String &json, const char *key, bool fallback);
String jsonString(const String &json, const char *key, const char *fallback);
int jsonValueStart(const String &json, const char *key);

size_t kindIndex(ControlKind kind) {
  return static_cast<size_t>(kind);
}

const char *controlName(ControlKind kind) {
  switch (kind) {
    case ControlKind::Brightness: return "brightness";
    case ControlKind::Volume: return "volume";
    case ControlKind::Mute: return "mute";
    case ControlKind::Input: return "input";
    default: return "";
  }
}

bool &pendingFor(ControlKind kind) {
  switch (kind) {
    case ControlKind::Brightness: return remote_state.brightness_pending;
    case ControlKind::Volume: return remote_state.volume_pending;
    case ControlKind::Mute: return remote_state.mute_pending;
    case ControlKind::Input: return remote_state.input_pending;
    default: return remote_state.brightness_pending;
  }
}

bool anyPendingLocked() {
  return remote_state.brightness_pending || remote_state.volume_pending ||
         remote_state.mute_pending || remote_state.input_pending;
}

bool timeReached(uint32_t now, uint32_t target) {
  return target == 0 || static_cast<int32_t>(now - target) >= 0;
}

bool statusCanOverwriteLocked(ControlKind kind, uint32_t now) {
  return !pendingFor(kind) &&
         timeReached(now, settle_until[kindIndex(kind)]);
}

String baseUrl() {
  return "http://" + remote_config.host + ":" + String(remote_config.port);
}

void wakeRemoteTask() {
  if (remote_task_handle) xTaskNotifyGive(remote_task_handle);
}

void updateMessage(bool online, const char *message) {
  if (!state_mutex) return;
  xSemaphoreTake(state_mutex, portMAX_DELAY);
  if (remote_state.online != online || strcmp(remote_state.message, message)) {
    remote_state.online = online;
    strlcpy(remote_state.message, message, sizeof(remote_state.message));
    ++remote_state.revision;
  }
  xSemaphoreGive(state_mutex);
}

void updateReady(bool ready, bool online, const char *message) {
  if (!state_mutex) return;
  xSemaphoreTake(state_mutex, portMAX_DELAY);
  if (remote_state.ready != ready || remote_state.online != online ||
      strcmp(remote_state.message, message)) {
    remote_state.ready = ready;
    remote_state.online = online;
    strlcpy(remote_state.message, message, sizeof(remote_state.message));
    ++remote_state.revision;
  }
  xSemaphoreGive(state_mutex);
}

void setDesiredLocked(const Command &command) {
  desired_sequences[kindIndex(command.kind)] = command.sequence;
  pendingFor(command.kind) = true;
  // RemoteState keeps the last acknowledged monitor values. The widgets retain
  // the user's pending values locally, so a failed command can safely reconcile
  // back to the last ACK instead of mistaking an optimistic value for success.
  strlcpy(remote_state.message, "Saving changes", sizeof(remote_state.message));
  ++remote_state.revision;
}

bool isLatestCommand(const Command &command) {
  if (!state_mutex) return false;
  xSemaphoreTake(state_mutex, portMAX_DELAY);
  bool latest =
      desired_sequences[kindIndex(command.kind)] == command.sequence;
  xSemaphoreGive(state_mutex);
  return latest;
}

bool commandExpired(const Command &command) {
  const uint32_t max_age =
      command.final_value ? kFinalMaxAgeMs : kPreviewMaxAgeMs;
  return millis() - command.queued_at_ms > max_age;
}

bool applyConfirmedResponse(const Command &command, const String &response) {
  bool confirmed = jsonBool(response, "confirmed", false);
  if (!state_mutex || !command.final_value || !confirmed) {
    return false;
  }
  int value_start = jsonValueStart(response, "value");
  if (value_start < 0 ||
      response.startsWith("null", value_start)) {
    return false;
  }
  if (command.kind == ControlKind::Mute &&
      !response.startsWith("true", value_start) &&
      !response.startsWith("false", value_start)) {
    return false;
  }
  if (command.kind == ControlKind::Input &&
      (value_start >= static_cast<int>(response.length()) ||
       response[value_start] != '"')) {
    return false;
  }

  xSemaphoreTake(state_mutex, portMAX_DELAY);
  if (desired_sequences[kindIndex(command.kind)] != command.sequence) {
    xSemaphoreGive(state_mutex);
    return false;
  }

  switch (command.kind) {
    case ControlKind::Brightness:
      remote_state.brightness =
          constrain(jsonInt(response, "value", command.value), 0, 100);
      break;
    case ControlKind::Volume:
      remote_state.volume =
          constrain(jsonInt(response, "value", command.value), 0, 100);
      break;
    case ControlKind::Mute:
      remote_state.muted =
          jsonBool(response, "value", command.value != 0);
      break;
    case ControlKind::Input: {
      String value = jsonString(response, "value", command.text);
      strlcpy(remote_state.input, value.c_str(), sizeof(remote_state.input));
      break;
    }
    default:
      break;
  }
  pendingFor(command.kind) = false;
  settle_until[kindIndex(command.kind)] = millis() + kConfirmedSettleMs;
  remote_state.online = true;
  strlcpy(remote_state.message,
          anyPendingLocked() ? "Saving changes" : "Saved to monitor",
          sizeof(remote_state.message));
  ++remote_state.revision;
  xSemaphoreGive(state_mutex);
  Serial.printf("DDC_CONFIRMED,CONTROL=%s,SEQ=%lu\n",
                controlName(command.kind),
                static_cast<unsigned long>(command.sequence));
  return true;
}

void applyCommandFailure(const Command &command) {
  if (!state_mutex) return;
  xSemaphoreTake(state_mutex, portMAX_DELAY);
  if (desired_sequences[kindIndex(command.kind)] == command.sequence) {
    pendingFor(command.kind) = false;
    char message[48];
    snprintf(message, sizeof(message), "%s verification failed",
             controlName(command.kind));
    strlcpy(remote_state.message, message, sizeof(remote_state.message));
    ++remote_state.revision;
    Serial.printf("DDC_VERIFY_FAILED,CONTROL=%s,SEQ=%lu\n",
                  controlName(command.kind),
                  static_cast<unsigned long>(command.sequence));
  }
  xSemaphoreGive(state_mutex);
}

int jsonValueStart(const String &json, const char *key) {
  String marker = "\"" + String(key) + "\":";
  int start = json.indexOf(marker);
  if (start < 0) return -1;
  start += marker.length();
  while (start < static_cast<int>(json.length())) {
    char value = json[start];
    if (value != ' ' && value != '\t' && value != '\r' && value != '\n') {
      break;
    }
    ++start;
  }
  return start;
}

int jsonInt(const String &json, const char *key, int fallback) {
  int start = jsonValueStart(json, key);
  if (start < 0) return fallback;
  if (json.startsWith("null", start)) return fallback;
  return json.substring(start).toInt();
}

bool jsonBool(const String &json, const char *key, bool fallback) {
  int start = jsonValueStart(json, key);
  if (start < 0) return fallback;
  return json.startsWith("true", start) ? true :
         json.startsWith("false", start) ? false : fallback;
}

String jsonString(const String &json, const char *key, const char *fallback) {
  int start = jsonValueStart(json, key);
  if (start < 0 || start >= static_cast<int>(json.length()) ||
      json[start] != '"') {
    return String(fallback);
  }
  ++start;
  int end = json.indexOf('"', start);
  return end < 0 ? String(fallback) : json.substring(start, end);
}

bool wifiRequest(const char *method, const String &path, const char *body,
                 String &response,
                 uint32_t timeout_ms = kDefaultRequestTimeoutMs) {
  if (!isLocalDesktopHost(remote_config.host)) {
    Serial.println("HTTP_FAIL,status=non-local-desktop-blocked");
    return false;
  }
  WiFiClient client;
  HTTPClient http;
  http.setConnectTimeout(1800);
  http.setTimeout(timeout_ms);
  if (!http.begin(client, baseUrl() + path)) {
    Serial.printf("HTTP_FAIL,path=%s,status=begin\n", path.c_str());
    return false;
  }
  http.addHeader("Authorization", "Bearer " + remote_config.token);
  int code;
  if (!strcmp(method, "POST")) {
    http.addHeader("Content-Type", "application/json");
    code = http.POST(String(body));
  } else {
    code = http.GET();
  }
  if (code > 0) response = http.getString();
  http.end();
  if (code < 200 || code >= 300) {
    Serial.printf("HTTP_FAIL,path=%s,status=%d\n", path.c_str(), code);
  }
  return code >= 200 && code < 300;
}

bool request(const char *method, const String &path, const char *body,
             String &response,
             uint32_t timeout_ms = kDefaultRequestTimeoutMs) {
  if (WiFi.status() == WL_CONNECTED && !remote_config.host.isEmpty()) {
    bool accepted = wifiRequest(method, path, body, response, timeout_ms);
    if (accepted) {
      Serial.printf("TRANSPORT=WIFI,path=%s\n", path.c_str());
      return true;
    }
  }
  if (bleTransportReady() &&
      bleTransportRequest(method, path, body, response, timeout_ms)) {
    Serial.printf("TRANSPORT=BLE,path=%s\n", path.c_str());
    return true;
  }
  return false;
}

void sendDiscoveryReply(const IPAddress &address, uint16_t port,
                        const String &message) {
  if (!discovery_udp.beginPacket(address, port)) return;
  discovery_udp.write(
      reinterpret_cast<const uint8_t *>(message.c_str()), message.length());
  discovery_udp.endPacket();
}

void sendCoordinationBroadcast(const String &message) {
  if (WiFi.status() != WL_CONNECTED) return;
  if (!discovery_udp.beginPacket(coordinationBroadcastAddress(),
                                 kCoordinationPort)) return;
  discovery_udp.write(
      reinterpret_cast<const uint8_t *>(message.c_str()), message.length());
  discovery_udp.endPacket();
}

void handlePassiveDiscovery() {
  if (WiFi.status() != WL_CONNECTED) {
    if (discovery_started) discovery_udp.stop();
    discovery_started = false;
    pending_discovery_nonce = "";
    return;
  }
  if (!discovery_started) {
    discovery_started = discovery_udp.begin(kDiscoveryPort);
    if (discovery_started) {
      Serial.printf("Waiting for AZORIA Desktop discovery on UDP %u\n",
                    kDiscoveryPort);
    }
    return;
  }

  int packet_length = discovery_udp.parsePacket();
  if (packet_length <= 0 || packet_length > 255) return;
  IPAddress sender = discovery_udp.remoteIP();
  uint16_t sender_port = discovery_udp.remotePort();
  if (!isPrivateIpv4(sender)) return;
  char buffer[256]{};
  int read = discovery_udp.read(buffer, sizeof(buffer) - 1);
  if (read <= 0) return;
  String wire(buffer);

  if (wireField(wire, 0) == "AZORIA_DESKTOP_HEARTBEAT_V1" &&
      wireFieldCount(wire) == 6) {
    String advertised_address = wireField(wire, 2);
    bool reachable = wireField(wire, 3) == "1";
    if (advertised_address != sender.toString()) return;
    if (reachable) {
      remote_config.host = advertised_address;
      remote_config.port = 8732;
      desktop_address_was_discovered = true;
    }
    return;
  }

  if (wireField(wire, 0) == "AZORIA_DESKTOP_RESULT_V1" &&
      wireFieldCount(wire) == 9) {
    String touch_id = wireField(wire, 1);
    String boot_nonce = wireField(wire, 2);
    String command_id = wireField(wire, 3);
    String control = wireField(wire, 7);
    String value = wireField(wire, 8);
    if (touch_id != device_id || boot_nonce != command_boot_nonce ||
        command_id != pending_result_command_id ||
        control != pending_result_control) return;
    bool accepted = wireField(wire, 5) == "1";
    bool confirmed = wireField(wire, 6) == "1";
    if (!accepted) {
      pending_result_response =
          "{\"accepted\":false,\"confirmed\":false}";
    } else {
      String encoded = value;
      if (control == "input") encoded = "\"" + value + "\"";
      if (control == "mute") encoded = value == "1" ? "true" : "false";
      pending_result_response =
          "{\"accepted\":true,\"confirmed\":" +
          String(confirmed ? "true" : "false") +
          ",\"value\":" + encoded + "}";
    }
    pending_result_ready = true;
    return;
  }

  if (wireField(wire, 0) == "AZORIA_DESKTOP_DISCOVER_V1" &&
      wireFieldCount(wire) == 2) {
    String nonce = wireField(wire, 1);
    if (nonce.length() != 24) return;
    pending_discovery_nonce = nonce;
    pending_discovery_address = sender;
    pending_discovery_at = millis();
    String reply = "AZORIA_TOUCH_V1|" + nonce + "|" + device_id + "|" +
                   device_hostname + "|" + kFirmwareVersion;
    sendDiscoveryReply(sender, sender_port, reply);
    return;
  }

  if (wireField(wire, 0) != "AZORIA_DESKTOP_CONFIG_V1" ||
      wireFieldCount(wire) != 4) {
    return;
  }
  String nonce = wireField(wire, 1);
  String host = wireField(wire, 2);
  int port = wireField(wire, 3).toInt();
  if (nonce != pending_discovery_nonce || sender != pending_discovery_address ||
      millis() - pending_discovery_at > 5000 ||
      host != sender.toString() || port <= 0 || port > 65535) {
    return;
  }
  remote_config.host = host;
  remote_config.port = static_cast<uint16_t>(port);
  desktop_address_was_discovered = true;
  pending_discovery_nonce = "";
  updateMessage(false, "Desktop discovered");
  sendDiscoveryReply(sender, sender_port,
                     "AZORIA_TOUCH_CONFIGURED_V1|" + nonce + "|" +
                         device_id);
  wakeRemoteTask();
  Serial.printf("AZORIA Desktop configured: %s:%u\n", host.c_str(), port);
}

void invalidateDiscoveredDesktop() {
  if (!desktop_address_was_discovered) return;
  remote_config.host = "";
  desktop_address_was_discovered = false;
}

void registerDevice() {
  char body[320];
  String address = WiFi.localIP().toString();
  snprintf(body, sizeof(body),
           "{\"device_id\":\"%s\",\"hostname\":\"%s\",\"board\":"
           "\"VIEWE UEDX48480040E-WB-A V1.3\",\"firmware\":\"%s\","
           "\"address\":\"%s\"}",
           device_id.c_str(), device_hostname.c_str(), kFirmwareVersion,
           address.c_str());
  String response;
  if (request("POST", "/v1/device/register", body, response)) {
    Serial.printf("Registered with Desktop as %s\n", device_id.c_str());
  }
}

bool readStatus() {
  String response;
  if (!request("GET", "/v1/status", nullptr, response,
               kStatusRequestTimeoutMs)) {
    updateReady(false, false, "Desktop offline");
    return false;
  }
  xSemaphoreTake(state_mutex, portMAX_DELAY);
  int brightness = constrain(jsonInt(response, "brightness", remote_state.brightness), 0, 100);
  int volume = constrain(jsonInt(response, "volume", remote_state.volume), 0, 100);
  bool muted = jsonBool(response, "mute", remote_state.muted);
  String input = jsonString(response, "input", remote_state.input);
  uint32_t now = millis();
  bool brightness_writable =
      statusCanOverwriteLocked(ControlKind::Brightness, now);
  bool volume_writable = statusCanOverwriteLocked(ControlKind::Volume, now);
  bool mute_writable = statusCanOverwriteLocked(ControlKind::Mute, now);
  bool input_writable = statusCanOverwriteLocked(ControlKind::Input, now);
  bool changed = !remote_state.ready || !remote_state.online ||
                 (brightness_writable &&
                  remote_state.brightness != brightness) ||
                 (volume_writable && remote_state.volume != volume) ||
                 (mute_writable && remote_state.muted != muted) ||
                 (input_writable && strcmp(remote_state.input, input.c_str())) ||
                 strcmp(remote_state.message,
                        anyPendingLocked() ? "Saving changes" : "DDC connected");
  if (changed) {
    remote_state.ready = true;
    remote_state.online = true;
    if (brightness_writable) remote_state.brightness = brightness;
    if (volume_writable) remote_state.volume = volume;
    if (mute_writable) remote_state.muted = muted;
    if (input_writable) {
      strlcpy(remote_state.input, input.c_str(), sizeof(remote_state.input));
    }
    strlcpy(remote_state.message,
            anyPendingLocked() ? "Saving changes" : "DDC connected",
            sizeof(remote_state.message));
    ++remote_state.revision;
  }
  xSemaphoreGive(state_mutex);
  return true;
}

bool takeNextCommand(Command &command) {
  if (!state_mutex) return false;
  xSemaphoreTake(state_mutex, portMAX_DELAY);
  size_t selected = static_cast<size_t>(ControlKind::Count);
  for (size_t index = 0;
       index < static_cast<size_t>(ControlKind::Count); ++index) {
    if (command_available[index] &&
        (selected == static_cast<size_t>(ControlKind::Count) ||
         latest_commands[index].sequence <
             latest_commands[selected].sequence)) {
      selected = index;
    }
  }
  if (selected == static_cast<size_t>(ControlKind::Count)) {
    xSemaphoreGive(state_mutex);
    return false;
  }
  command = latest_commands[selected];
  command_available[selected] = false;
  xSemaphoreGive(state_mutex);
  return true;
}

void buildCommandBody(const Command &command, char *body, size_t body_size) {
  if (command.kind == ControlKind::Mute) {
    snprintf(body, body_size,
             "{\"control\":\"%s\",\"value\":%s,\"final\":%s}",
             controlName(command.kind),
             command.value ? "true" : "false",
             command.final_value ? "true" : "false");
  } else if (command.kind == ControlKind::Input) {
    snprintf(body, body_size,
             "{\"control\":\"input\",\"value\":\"%s\",\"final\":%s}",
             command.text, command.final_value ? "true" : "false");
  } else {
    snprintf(body, body_size,
             "{\"control\":\"%s\",\"value\":%d,\"final\":%s}",
             controlName(command.kind), command.value,
             command.final_value ? "true" : "false");
  }
}

bool broadcastCommand(const Command &command, String &response,
                      uint32_t timeout_ms) {
  if (WiFi.status() != WL_CONNECTED || !discovery_started) return false;
  String value;
  if (command.kind == ControlKind::Input) {
    value = command.text;
  } else {
    value = String(command.value);
  }
  String command_id = String(command.sequence);
  String unsigned_message =
      "AZORIA_TOUCH_COMMAND_V1|" + device_id + "|" +
      command_boot_nonce + "|" + command_id + "|" +
      controlName(command.kind) + "|" + value + "|" +
      String(command.final_value ? "1" : "0");
  pending_result_command_id = command_id;
  pending_result_control = controlName(command.kind);
  pending_result_response = "";
  pending_result_ready = false;

  sendCoordinationBroadcast(unsigned_message);
  if (!command.final_value) {
    response = "{\"accepted\":true,\"confirmed\":false}";
    return true;
  }

  uint32_t started = millis();
  uint32_t last_send = started;
  while (millis() - started < timeout_ms) {
    handlePassiveDiscovery();
    if (pending_result_ready) {
      response = pending_result_response;
      pending_result_command_id = "";
      pending_result_control = "";
      return true;
    }
    // Repeating the same command ID is safe: the elected Desktop returns its
    // cached result instead of writing DDC/CI a second time.
    if (millis() - last_send >= 350) {
      sendCoordinationBroadcast(unsigned_message);
      last_send = millis();
    }
    delay(10);
  }
  pending_result_command_id = "";
  pending_result_control = "";
  return false;
}

enum class CommandResult : uint8_t {
  PreviewComplete,
  FinalConfirmed,
  FinalAccepted,
  FinalFailed,
  Superseded,
};

bool applyAcceptedResponse(const Command &command, const String &response) {
  if (!state_mutex || command.kind != ControlKind::Input ||
      !jsonBool(response, "accepted", false) ||
      jsonBool(response, "confirmed", false)) {
    return false;
  }
  xSemaphoreTake(state_mutex, portMAX_DELAY);
  if (desired_sequences[kindIndex(command.kind)] != command.sequence) {
    xSemaphoreGive(state_mutex);
    return false;
  }
  strlcpy(remote_state.input, command.text, sizeof(remote_state.input));
  pendingFor(command.kind) = false;
  settle_until[kindIndex(command.kind)] = millis() + kConfirmedSettleMs;
  remote_state.online = true;
  strlcpy(remote_state.message, "Input command sent",
          sizeof(remote_state.message));
  ++remote_state.revision;
  xSemaphoreGive(state_mutex);
  Serial.printf("DDC_ACCEPTED,CONTROL=%s,SEQ=%lu,CONFIRMED=0\n",
                controlName(command.kind),
                static_cast<unsigned long>(command.sequence));
  return true;
}

CommandResult runCommand(const Command &command) {
  if (commandExpired(command)) {
    Serial.printf("DDC_EXPIRED,CONTROL=%s,SEQ=%lu\n",
                  controlName(command.kind),
                  static_cast<unsigned long>(command.sequence));
    if (isLatestCommand(command)) applyCommandFailure(command);
    return CommandResult::Superseded;
  }
  char body[112];
  buildCommandBody(command, body, sizeof(body));
  // Every UI action is delivered at most once. A delayed retry is more harmful
  // than a missed preview because it can overwrite a newer user choice.
  constexpr uint8_t max_attempts = 1;
  for (uint8_t attempt = 0; attempt < max_attempts; ++attempt) {
    // Never send or acknowledge a stale final. Slider previews are also safe
    // to drop once a newer desired value exists.
    if (!isLatestCommand(command)) {
      Serial.printf("DDC_SUPERSEDED,CONTROL=%s,SEQ=%lu,FINAL=%d\n",
                    controlName(command.kind),
                    static_cast<unsigned long>(command.sequence),
                    command.final_value ? 1 : 0);
      return CommandResult::Superseded;
    }

    String response;
    bool accepted = false;
    if (WiFi.status() == WL_CONNECTED) {
      accepted = broadcastCommand(
          command, response,
          command.final_value ? kFinalRequestTimeoutMs
                              : kDefaultRequestTimeoutMs);
    } else if (bleTransportReady()) {
      accepted = bleTransportRequest(
          "POST", "/v1/control", body, response,
          command.final_value ? kFinalRequestTimeoutMs
                              : kDefaultRequestTimeoutMs);
    }
    if (!command.final_value) {
      if (!accepted) {
        Serial.printf("DDC_PREVIEW_FAILED,CONTROL=%s,SEQ=%lu\n",
                      controlName(command.kind),
                      static_cast<unsigned long>(command.sequence));
      }
      // Preview traffic is deliberately best effort. Its response never
      // commits monitor state, clears pending, schedules status, or declares
      // the Desktop unavailable.
      return CommandResult::PreviewComplete;
    }

    if (accepted && applyConfirmedResponse(command, response)) {
      return CommandResult::FinalConfirmed;
    }
    if (accepted && applyAcceptedResponse(command, response)) {
      return CommandResult::FinalAccepted;
    }
    if (accepted && !jsonBool(response, "accepted", false)) {
      Serial.printf("DDC_REJECTED,CONTROL=%s,SEQ=%lu\n",
                    controlName(command.kind),
                    static_cast<unsigned long>(command.sequence));
      break;
    }
    if (accepted) {
      Serial.printf("DDC_UNCONFIRMED,CONTROL=%s,SEQ=%lu,ATTEMPT=%u\n",
                    controlName(command.kind),
                    static_cast<unsigned long>(command.sequence),
                    static_cast<unsigned>(attempt + 1));
    }

  }

  if (!isLatestCommand(command)) return CommandResult::Superseded;
  applyCommandFailure(command);
  return CommandResult::FinalFailed;
}

void remoteTask(void *) {
  uint32_t last_status = 0;
  uint32_t last_registration = 0;
  bool desktop_verified = false;
  for (;;) {
    bool ble_ready = bleTransportReady();
    bool wifi_ready = WiFi.status() == WL_CONNECTED;
    handlePassiveDiscovery();
    if ((!wifi_ready || remote_config.host.isEmpty()) && !ble_ready) {
      desktop_verified = false;
      if (!wifi_ready) {
        invalidateDiscoveredDesktop();
        updateReady(false, false, "Wi-Fi offline");
      } else {
        updateReady(false, false, "Waiting for AZORIA Desktop");
      }
      ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(100));
      continue;
    }
    if (!desktop_verified) {
      if (wifi_ready && !remote_config.host.isEmpty()) {
        registerDevice();
        last_registration = millis();
      }
      if (!readStatus()) {
        if (!bleTransportReady()) invalidateDiscoveredDesktop();
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(900));
        continue;
      }
      desktop_verified = true;
      last_status = millis();
    } else if (wifi_ready && !remote_config.host.isEmpty() &&
               millis() - last_registration >= kRegistrationIntervalMs) {
      registerDevice();
      last_registration = millis();
    }

    Command command{};
    if (takeNextCommand(command)) {
      runCommand(command);
      // A final control response already performs a targeted DDC readback and
      // is authoritative. Avoid an immediate five-control status sweep after
      // every drag; the normal idle probe still catches external OSD changes.
      last_status = millis();
      continue;
    }
    if (millis() - last_status >= kIdleStatusIntervalMs) {
      if (!readStatus()) {
        desktop_verified = false;
        if (!bleTransportReady()) invalidateDiscoveredDesktop();
      }
      last_status = millis();
    }
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(60));
  }
}

bool enqueueLatest(Command command) {
  if (!state_mutex || !remote_task_handle) return false;
  xSemaphoreTake(state_mutex, portMAX_DELAY);
  command.sequence = next_sequence++;
  command.queued_at_ms = millis();
  const size_t index = kindIndex(command.kind);
  latest_commands[index] = command;
  command_available[index] = true;
  setDesiredLocked(command);
  xSemaphoreGive(state_mutex);
  wakeRemoteTask();
  return true;
}

}  // namespace

void startRemote(const DeviceConfig &config) {
  remote_config = config;
  uint64_t chip = ESP.getEfuseMac();
  char id[18];
  snprintf(id, sizeof(id), "%04X%08X",
           static_cast<uint16_t>(chip >> 32), static_cast<uint32_t>(chip));
  device_id = id;
  char boot_nonce[9]{};
  snprintf(boot_nonce, sizeof(boot_nonce), "%08lx",
           static_cast<unsigned long>(esp_random()));
  command_boot_nonce = boot_nonce;
  device_hostname = "azoria-touch-" + device_id.substring(device_id.length() - 6);
  device_hostname.toLowerCase();
  state_mutex = xSemaphoreCreateMutex();
  if (!state_mutex) {
    Serial.println("DDC command scheduler allocation failed");
    return;
  }
  if (!startBleTransport(device_hostname, remote_config.token)) {
    Serial.println("BLE transport initialization failed");
  }
  if (xTaskCreatePinnedToCore(remoteTask, "ddc-network", 8192, nullptr, 1,
                              &remote_task_handle, 0) != pdPASS) {
    Serial.println("DDC network task creation failed");
    remote_task_handle = nullptr;
  }
  Serial.println("AZORIA Touch ready: waiting for Desktop discovery, BLE fallback enabled");
}

DeviceConfig getDesktopConfig() {
  return remote_config;
}

RemoteState getRemoteState() {
  RemoteState copy;
  if (!state_mutex) return remote_state;
  xSemaphoreTake(state_mutex, portMAX_DELAY);
  copy = remote_state;
  xSemaphoreGive(state_mutex);
  return copy;
}

bool queueNumericControl(const char *control, int value, bool final_value) {
  Command command{};
  command.value = constrain(value, 0, 100);
  command.final_value = final_value;
  if (!strcmp(control, "brightness")) {
    command.kind = ControlKind::Brightness;
  } else if (!strcmp(control, "volume")) {
    command.kind = ControlKind::Volume;
  } else {
    return false;
  }
  return enqueueLatest(command);
}

bool queueBooleanControl(const char *control, bool value) {
  Command command{};
  if (!strcmp(control, "mute")) {
    command.kind = ControlKind::Mute;
  } else {
    return false;
  }
  command.value = value ? 1 : 0;
  command.final_value = true;
  return enqueueLatest(command);
}

bool queueStringControl(const char *control, const char *value) {
  if (strcmp(control, "input") || !value) return false;
  Command command{};
  command.kind = ControlKind::Input;
  strlcpy(command.text, value, sizeof(command.text));
  command.final_value = true;
  return enqueueLatest(command);
}

}  // namespace DisplayControl
