#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/hid/IOHIDManager.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr int kVendorId = 0x043e;
constexpr int kProductId = 0x9a39;
constexpr int kUsagePage = 0xff00;
constexpr int kUsage = 0x0001;
constexpr size_t kReportSize = 0x40;
constexpr uint8_t kDdcDestination = 0x6e;

struct ReportWaiter {
  std::array<uint8_t, kReportSize> callback_buffer{};
  std::array<uint8_t, kReportSize> response{};
  CFIndex response_length = 0;
  bool received = false;
  uint8_t expected_opcode = 0;
  CFRunLoopRef run_loop = nullptr;
};

void input_report_callback(void *context, IOReturn result, void *,
                           IOHIDReportType, uint32_t, uint8_t *report,
                           CFIndex report_length) {
  auto *waiter = static_cast<ReportWaiter *>(context);
  if (result != kIOReturnSuccess || report_length <= 0) {
    return;
  }
  // The NXP bridge shares this HID interface with asynchronous monitor
  // telemetry. Only consume the DDC reply for the command currently in flight.
  if (report_length < 14 || report[8] != waiter->expected_opcode) {
    return;
  }
  waiter->response_length =
      report_length > static_cast<CFIndex>(kReportSize)
          ? static_cast<CFIndex>(kReportSize)
          : report_length;
  std::memcpy(waiter->response.data(), report,
              static_cast<size_t>(waiter->response_length));
  waiter->received = true;
  if (waiter->run_loop) {
    CFRunLoopStop(waiter->run_loop);
  }
}

CFMutableDictionaryRef make_matching_dictionary() {
  CFMutableDictionaryRef dictionary = CFDictionaryCreateMutable(
      kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks,
      &kCFTypeDictionaryValueCallBacks);
  if (!dictionary) {
    return nullptr;
  }

  struct MatchValue {
    CFStringRef key;
    int value;
  };
  const MatchValue values[] = {
      {CFSTR(kIOHIDVendorIDKey), kVendorId},
      {CFSTR(kIOHIDProductIDKey), kProductId},
      {CFSTR(kIOHIDPrimaryUsagePageKey), kUsagePage},
      {CFSTR(kIOHIDPrimaryUsageKey), kUsage},
  };
  for (const auto &entry : values) {
    CFNumberRef number =
        CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &entry.value);
    if (!number) {
      CFRelease(dictionary);
      return nullptr;
    }
    CFDictionarySetValue(dictionary, entry.key, number);
    CFRelease(number);
  }
  return dictionary;
}

class LgHidDevice {
 public:
  ~LgHidDevice() { close(); }

  bool open(std::string &error) {
    manager_ =
        IOHIDManagerCreate(kCFAllocatorDefault, kIOHIDOptionsTypeNone);
    if (!manager_) {
      error = "could not create IOHIDManager";
      return false;
    }

    CFMutableDictionaryRef matching = make_matching_dictionary();
    if (!matching) {
      error = "could not build HID matching dictionary";
      return false;
    }
    IOHIDManagerSetDeviceMatching(manager_, matching);
    CFRelease(matching);

    IOReturn result =
        IOHIDManagerOpen(manager_, kIOHIDOptionsTypeNone);
    if (result != kIOReturnSuccess) {
      char message[96];
      std::snprintf(message, sizeof(message),
                    "could not open IOHIDManager (0x%08x)", result);
      error = message;
      return false;
    }

    CFSetRef devices = IOHIDManagerCopyDevices(manager_);
    if (!devices || CFSetGetCount(devices) == 0) {
      if (devices) {
        CFRelease(devices);
      }
      error = "LG Monitor Controls HID 043e:9a39 is not connected";
      return false;
    }

    std::vector<const void *> values(
        static_cast<size_t>(CFSetGetCount(devices)));
    CFSetGetValues(devices, values.data());
    device_ = static_cast<IOHIDDeviceRef>(const_cast<void *>(values.front()));
    CFRetain(device_);
    CFRelease(devices);

    result = IOHIDDeviceOpen(device_, kIOHIDOptionsTypeNone);
    if (result != kIOReturnSuccess) {
      char message[96];
      std::snprintf(message, sizeof(message),
                    "could not open LG Monitor Controls HID (0x%08x)", result);
      error = message;
      return false;
    }

    waiter_.run_loop = CFRunLoopGetCurrent();
    IOHIDDeviceRegisterInputReportCallback(
        device_, waiter_.callback_buffer.data(),
        static_cast<CFIndex>(waiter_.callback_buffer.size()),
        input_report_callback, &waiter_);
    IOHIDDeviceScheduleWithRunLoop(device_, waiter_.run_loop,
                                   kCFRunLoopDefaultMode);
    scheduled_ = true;
    return true;
  }

  bool exchange(uint8_t source, uint8_t opcode,
                const std::vector<uint8_t> &payload,
                uint8_t expected_length, bool response_required,
                std::array<uint8_t, kReportSize> &response,
                CFIndex &response_length, std::string &error) {
    if (!device_ || payload.size() > 52) {
      error = "invalid HID DDC request";
      return false;
    }

    std::array<uint8_t, kReportSize> report{};
    const uint8_t header[] = {0x08, 0x01, 0x55, 0x03,
                              0x00, 0x00, 0x03, 0x37};
    std::memcpy(report.data(), header, sizeof(header));

    std::vector<uint8_t> ddc;
    ddc.reserve(payload.size() + 3);
    ddc.push_back(source);
    ddc.push_back(static_cast<uint8_t>(0x80 | payload.size()));
    ddc.insert(ddc.end(), payload.begin(), payload.end());
    uint8_t checksum = kDdcDestination;
    for (uint8_t byte : ddc) {
      checksum ^= byte;
    }
    ddc.push_back(checksum);
    report[4] = static_cast<uint8_t>(ddc.size());
    std::copy(ddc.begin(), ddc.end(), report.begin() + 8);

    waiter_.received = false;
    waiter_.response_length = 0;
    waiter_.expected_opcode = opcode;

    IOReturn result = IOHIDDeviceSetReport(
        device_, kIOHIDReportTypeOutput, 0, report.data(),
        static_cast<CFIndex>(report.size()));
    if (result != kIOReturnSuccess) {
      char message[96];
      std::snprintf(message, sizeof(message),
                    "LG HID write failed (0x%08x)", result);
      error = message;
      return false;
    }

    report[1] = 0x02;
    report[3] = 0x04;
    report[4] = expected_length;
    report[6] = 0x0b;
    result = IOHIDDeviceSetReport(
        device_, kIOHIDReportTypeOutput, 0, report.data(),
        static_cast<CFIndex>(report.size()));
    if (result != kIOReturnSuccess) {
      char message[96];
      std::snprintf(message, sizeof(message),
                    "LG HID read request failed (0x%08x)", result);
      error = message;
      return false;
    }

    // Set VCP and LG's private input command are intentionally write-once.
    // This monitor does not consistently return a DDC reply for writes, and
    // waiting/retrying can turn one UI action into delayed duplicate changes.
    if (!response_required) {
      response_length = 0;
      return true;
    }

    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(600);
    while (!waiter_.received && std::chrono::steady_clock::now() < deadline) {
      CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.05, true);
    }
    if (!waiter_.received) {
      if (response_required) {
        error = "LG HID response timed out";
        return false;
      }
      response_length = 0;
      return true;
    }
    response = waiter_.response;
    response_length = waiter_.response_length;
    return validate_response(response, response_length, error);
  }

 private:
  static bool validate_response(
      const std::array<uint8_t, kReportSize> &response,
      CFIndex response_length, std::string &error) {
    if (response_length < 14) {
      error = "LG HID response is too short";
      return false;
    }
    size_t ddc_length = static_cast<size_t>(response[5] & 0x7f);
    const size_t available = static_cast<size_t>(response_length) - 7;
    if (ddc_length + 2 > available) {
      ddc_length = available >= 2 ? available - 2 : 0;
    }
    uint8_t checksum = kDdcDestination ^ 0x50;
    const size_t end = 5 + ddc_length + 2;
    for (size_t index = 5; index < end; ++index) {
      checksum ^= response[index];
    }
    if (checksum != 0) {
      char message[256];
      int written = std::snprintf(
          message, sizeof(message),
          "LG HID response checksum failed (0x%02x), report=", checksum);
      const size_t dump_length =
          std::min(static_cast<size_t>(response_length), size_t{24});
      for (size_t index = 0;
           index < dump_length && written > 0 &&
           static_cast<size_t>(written) + 3 < sizeof(message);
           ++index) {
        written += std::snprintf(message + written, sizeof(message) - written,
                                 "%02x", response[index]);
      }
      error = message;
      return false;
    }
    return true;
  }

  void close() {
    if (device_) {
      if (scheduled_) {
        IOHIDDeviceUnscheduleFromRunLoop(device_, CFRunLoopGetCurrent(),
                                         kCFRunLoopDefaultMode);
      }
      IOHIDDeviceClose(device_, kIOHIDOptionsTypeNone);
      CFRelease(device_);
      device_ = nullptr;
    }
    if (manager_) {
      IOHIDManagerClose(manager_, kIOHIDOptionsTypeNone);
      CFRelease(manager_);
      manager_ = nullptr;
    }
  }

  IOHIDManagerRef manager_ = nullptr;
  IOHIDDeviceRef device_ = nullptr;
  ReportWaiter waiter_{};
  bool scheduled_ = false;
};

std::optional<uint16_t> parse_u16(const char *text) {
  if (!text || !*text) {
    return std::nullopt;
  }
  char *end = nullptr;
  unsigned long value = std::strtoul(text, &end, 0);
  if (*end != '\0' || value > 0xffff) {
    return std::nullopt;
  }
  return static_cast<uint16_t>(value);
}

void print_error(const std::string &error) {
  std::fprintf(stderr, "{\"ok\":false,\"error\":\"%s\"}\n", error.c_str());
}

void print_debug_report(const std::array<uint8_t, kReportSize> &report,
                        CFIndex report_length) {
  if (!std::getenv("AZORIA_DDC_DEBUG")) {
    return;
  }
  std::fprintf(stderr, "LG HID report (%ld bytes): ", report_length);
  const size_t dump_length =
      std::min(static_cast<size_t>(report_length), report.size());
  for (size_t index = 0; index < dump_length; ++index) {
    std::fprintf(stderr, "%02x", report[index]);
  }
  std::fputc('\n', stderr);
}

int usage() {
  std::fprintf(
      stderr,
      "usage: lg-hid-control probe | get <vcp> | set <vcp> <value> | "
      "input <dp1|dp2|hdmi1|hdmi2|usbc>\n");
  return 64;
}

}  // namespace

int main(int argc, char **argv) {
  if (argc < 2) {
    return usage();
  }

  LgHidDevice device;
  std::string error;
  if (!device.open(error)) {
    print_error(error);
    return 2;
  }

  const std::string command = argv[1];
  if (command == "probe") {
    std::puts("{\"ok\":true,\"transport\":\"lg-hid-ddc\","
              "\"vendor_id\":1086,\"product_id\":39481}");
    return 0;
  }

  uint8_t source = 0x51;
  uint8_t expected_length = 0x0b;
  std::vector<uint8_t> payload;
  uint16_t opcode = 0;
  if (command == "get" && argc == 3) {
    auto parsed_opcode = parse_u16(argv[2]);
    if (!parsed_opcode || *parsed_opcode > 0xff) {
      return usage();
    }
    opcode = *parsed_opcode;
    payload = {0x01, static_cast<uint8_t>(opcode)};
  } else if (command == "set" && argc == 4) {
    auto parsed_opcode = parse_u16(argv[2]);
    auto value = parse_u16(argv[3]);
    if (!parsed_opcode || *parsed_opcode > 0xff || !value) {
      return usage();
    }
    opcode = *parsed_opcode;
    payload = {0x03, static_cast<uint8_t>(opcode),
               static_cast<uint8_t>(*value >> 8),
               static_cast<uint8_t>(*value & 0xff)};
  } else if (command == "input" && argc == 3) {
    const std::string name = argv[2];
    std::optional<uint16_t> value;
    if (name == "dp1") value = 0xd0;
    if (name == "dp2") value = 0xd1;
    if (name == "hdmi1") value = 0x90;
    if (name == "hdmi2") value = 0x91;
    if (name == "usbc" || name == "usb-c") value = 0xd2;
    if (!value) {
      return usage();
    }
    source = 0x50;
    expected_length = 0x26;
    opcode = 0xf4;
    payload = {0x03, static_cast<uint8_t>(opcode),
               static_cast<uint8_t>(*value >> 8),
               static_cast<uint8_t>(*value & 0xff)};
  } else {
    return usage();
  }

  std::array<uint8_t, kReportSize> response{};
  CFIndex response_length = 0;
  if (!device.exchange(source, static_cast<uint8_t>(opcode), payload,
                       expected_length, command == "get", response,
                       response_length, error)) {
    print_error(error);
    return 3;
  }
  if (command == "get") {
    print_debug_report(response, response_length);
    if (response_length < 15 || response[6] != 0x02) {
      print_error("LG HID response is not a DDC/CI Get VCP reply");
      return 4;
    }
    if (response[7] != 0x00) {
      char message[96];
      std::snprintf(message, sizeof(message),
                    "LG HID DDC/CI Get VCP failed (result 0x%02x)",
                    response[7]);
      print_error(message);
      return 4;
    }
    if (response[8] != opcode) {
      print_error("LG HID response opcode does not match request");
      return 4;
    }
    const uint16_t maximum =
        static_cast<uint16_t>((response[10] << 8) | response[11]);
    const uint16_t current =
        static_cast<uint16_t>((response[12] << 8) | response[13]);
    std::printf("{\"ok\":true,\"opcode\":%u,\"maximum\":%u,\"current\":%u}\n",
                opcode, maximum, current);
  } else {
    std::printf(
        "{\"ok\":true,\"opcode\":%u,\"value\":%u,\"acknowledged\":%s}\n",
        opcode, static_cast<unsigned>((payload[2] << 8) | payload[3]),
        response_length > 0 ? "true" : "false");
  }
  return 0;
}
