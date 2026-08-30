import Foundation
import CoreBluetooth
import CryptoKit
import Darwin

private let serviceUUID = CBUUID(string: "7A6F0001-4E6D-4A9B-8F41-3C41588FEE68")
private let requestUUID = CBUUID(string: "7A6F0002-4E6D-4A9B-8F41-3C41588FEE68")
private let responseUUID = CBUUID(string: "7A6F0003-4E6D-4A9B-8F41-3C41588FEE68")

private struct BridgeConfig: Decodable {
    let token: String
    let port: Int
}

private final class BLEBridge: NSObject, CBCentralManagerDelegate, CBPeripheralDelegate, @unchecked Sendable {
    private let config: BridgeConfig
    private let key: SymmetricKey
    private var central: CBCentralManager!
    private var peripheral: CBPeripheral?
    private var responseCharacteristic: CBCharacteristic?
    private var responseCache: [String: String] = [:]

    init(config: BridgeConfig) {
        self.config = config
        key = SymmetricKey(data: Data(config.token.utf8))
        super.init()
        central = CBCentralManager(delegate: self, queue: .main)
    }

    private func signature(_ message: String) -> String {
        HMAC<SHA256>.authenticationCode(for: Data(message.utf8), using: key)
            .prefix(8).map { String(format: "%02x", $0) }.joined()
    }

    private func scan() {
        guard central.state == .poweredOn else { return }
        central.scanForPeripherals(withServices: [serviceUUID], options: nil)
        print("BLE_SCAN")
    }

    func centralManagerDidUpdateState(_ central: CBCentralManager) {
        if central.state == .poweredOn { scan() }
    }

    func centralManager(_ central: CBCentralManager, didDiscover peripheral: CBPeripheral, advertisementData: [String: Any], rssi RSSI: NSNumber) {
        guard self.peripheral == nil else { return }
        self.peripheral = peripheral
        central.stopScan()
        peripheral.delegate = self
        central.connect(peripheral)
    }

    func centralManager(_ central: CBCentralManager, didConnect peripheral: CBPeripheral) {
        responseCache.removeAll(keepingCapacity: true)
        peripheral.discoverServices([serviceUUID])
        print("BLE_CONNECTED=\(peripheral.identifier.uuidString)")
    }

    func centralManager(_ central: CBCentralManager, didFailToConnect peripheral: CBPeripheral, error: Error?) {
        self.peripheral = nil
        responseCharacteristic = nil
        scan()
    }

    func centralManager(_ central: CBCentralManager, didDisconnectPeripheral peripheral: CBPeripheral, timestamp: CFAbsoluteTime, isReconnecting: Bool, error: Error?) {
        self.peripheral = nil
        responseCharacteristic = nil
        scan()
    }

    func peripheral(_ peripheral: CBPeripheral, didDiscoverServices error: Error?) {
        guard error == nil, let services = peripheral.services else { return }
        for service in services where service.uuid == serviceUUID {
            peripheral.discoverCharacteristics([requestUUID, responseUUID], for: service)
        }
    }

    func peripheral(_ peripheral: CBPeripheral, didDiscoverCharacteristicsFor service: CBService, error: Error?) {
        guard error == nil, let characteristics = service.characteristics else { return }
        for characteristic in characteristics {
            if characteristic.uuid == requestUUID {
                peripheral.setNotifyValue(true, for: characteristic)
            } else if characteristic.uuid == responseUUID {
                responseCharacteristic = characteristic
            }
        }
    }

    func peripheral(_ peripheral: CBPeripheral, didUpdateNotificationStateFor characteristic: CBCharacteristic, error: Error?) {
        if characteristic.uuid == requestUUID && characteristic.isNotifying && responseCharacteristic != nil {
            print("BLE_READY")
        }
    }

    func peripheral(_ peripheral: CBPeripheral, didUpdateValueFor characteristic: CBCharacteristic, error: Error?) {
        guard characteristic.uuid == requestUUID, error == nil,
              let data = characteristic.value,
              let message = String(data: data, encoding: .utf8) else { return }
        handle(message)
    }

    private func handle(_ message: String) {
        let parts = message.split(separator: "|", omittingEmptySubsequences: false).map(String.init)
        guard parts.count >= 5, parts[0] == "Q" else { return }
        let unsigned = parts.dropLast().joined(separator: "|")
        guard signature(unsigned) == parts.last else { return }
        let nonce = parts[1]
        let requestID = parts[2]
        let cacheKey = "\(nonce)|\(requestID)"
        if let cached = responseCache[cacheKey] { write(cached); return }
        switch parts[3] {
        case "S" where parts.count == 5:
            fetchStatus(nonce: nonce, requestID: requestID, cacheKey: cacheKey)
        case "C" where parts.count == 8:
            setControl(nonce: nonce, requestID: requestID, control: parts[4], value: parts[5], final: parts[6] == "1", cacheKey: cacheKey)
        default:
            send(nonce: nonce, requestID: requestID, payload: "E|protocol", cacheKey: cacheKey)
        }
    }

    private func request(path: String, method: String, body: Data? = nil) -> URLRequest {
        var result = URLRequest(url: URL(string: "http://127.0.0.1:\(config.port)\(path)")!)
        result.httpMethod = method
        result.setValue("Bearer \(config.token)", forHTTPHeaderField: "Authorization")
        if let body {
            result.httpBody = body
            result.setValue("application/json", forHTTPHeaderField: "Content-Type")
        }
        result.timeoutInterval = 10
        return result
    }

    private func fetchStatus(nonce: String, requestID: String, cacheKey: String) {
        URLSession.shared.dataTask(with: request(path: "/v1/status", method: "GET")) { [weak self] data, response, error in
            guard let self else { return }
            guard error == nil, let http = response as? HTTPURLResponse,
                  (200..<300).contains(http.statusCode), let data,
                  let json = try? JSONSerialization.jsonObject(with: data) as? [String: Any] else {
                sendOnMain(nonce: nonce, requestID: requestID, payload: "E|backend", cacheKey: cacheKey)
                return
            }
            let brightness = json["brightness"] as? Int ?? 0
            let volume = json["volume"] as? Int ?? 0
            let contrast = json["contrast"] as? Int ?? 0
            let mute = (json["mute"] as? Bool ?? false) ? 1 : 0
            let input = json["input"] as? String ?? "dp1"
            let power = (json["power"] as? Bool ?? true) ? 1 : 0
            sendOnMain(nonce: nonce, requestID: requestID, payload: "S|\(brightness)|\(volume)|\(contrast)|\(mute)|\(input)|\(power)", cacheKey: cacheKey)
        }.resume()
    }

    private func setControl(nonce: String, requestID: String, control: String, value: String, final: Bool, cacheKey: String) {
        let typedValue: Any
        if control == "mute" || control == "power" {
            typedValue = value == "1"
        } else if control == "input" {
            typedValue = value
        } else {
            typedValue = Int(value) ?? 0
        }
        let body: [String: Any] = ["control": control, "value": typedValue, "final": final]
        let data = try! JSONSerialization.data(withJSONObject: body)
        URLSession.shared.dataTask(with: request(path: "/v1/control", method: "POST", body: data)) { [weak self] data, response, error in
            guard let self else { return }
            guard error == nil, let http = response as? HTTPURLResponse,
                  (200..<300).contains(http.statusCode), let data,
                  let json = try? JSONSerialization.jsonObject(with: data) as? [String: Any] else {
                sendOnMain(nonce: nonce, requestID: requestID, payload: "E|backend", cacheKey: cacheKey)
                return
            }
            let accepted = (json["accepted"] as? Bool ?? false) ? 1 : 0
            let confirmed = (json["confirmed"] as? Bool ?? false) ? 1 : 0
            let returned: String
            if let bool = json["value"] as? Bool { returned = bool ? "1" : "0" }
            else if let number = json["value"] as? NSNumber { returned = number.stringValue }
            else { returned = json["value"] as? String ?? value }
            sendOnMain(nonce: nonce, requestID: requestID, payload: "C|\(accepted)|\(confirmed)|\(returned)", cacheKey: cacheKey)
        }.resume()
    }

    private func sendOnMain(nonce: String, requestID: String, payload: String, cacheKey: String) {
        DispatchQueue.main.async { [weak self] in self?.send(nonce: nonce, requestID: requestID, payload: payload, cacheKey: cacheKey) }
    }

    private func send(nonce: String, requestID: String, payload: String, cacheKey: String) {
        let unsigned = "R|\(nonce)|\(requestID)|\(payload)"
        let message = "\(unsigned)|\(signature(unsigned))"
        responseCache[cacheKey] = message
        if responseCache.count > 128 { responseCache.removeAll(keepingCapacity: true); responseCache[cacheKey] = message }
        write(message)
    }

    private func write(_ message: String) {
        guard let peripheral, let responseCharacteristic else { return }
        peripheral.writeValue(Data(message.utf8), for: responseCharacteristic, type: .withResponse)
    }
}

private func loadConfig(path: String) throws -> BridgeConfig {
    try JSONDecoder().decode(BridgeConfig.self, from: Data(contentsOf: URL(fileURLWithPath: path)))
}

setvbuf(stdout, nil, _IOLBF, 0)
setvbuf(stderr, nil, _IOLBF, 0)

guard let configIndex = CommandLine.arguments.firstIndex(of: "--config"),
      CommandLine.arguments.indices.contains(configIndex + 1) else {
    fputs("usage: AzoriaBLEBridge --config PATH\n", stderr)
    exit(2)
}

do {
    let bridge = BLEBridge(config: try loadConfig(path: CommandLine.arguments[configIndex + 1]))
    withExtendedLifetime(bridge) { RunLoop.main.run() }
} catch {
    fputs("BLE bridge failed: \(error)\n", stderr)
    exit(1)
}
