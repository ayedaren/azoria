import Foundation
import CoreBluetooth
import CryptoKit
import Darwin

private let serviceUUID = CBUUID(string: "7A6F0001-4E6D-4A9B-8F41-3C41588FEE68")
private let controlUUID = CBUUID(string: "7A6F0004-4E6D-4A9B-8F41-3C41588FEE68")
private let dataUUID = CBUUID(string: "7A6F0005-4E6D-4A9B-8F41-3C41588FEE68")
private let statusUUID = CBUUID(string: "7A6F0006-4E6D-4A9B-8F41-3C41588FEE68")

private struct DeviceConfig: Decodable {
    let token: String
}

private struct Arguments {
    let file: String
    let token: String
}

private func usage() -> Never {
    fputs("usage: ble_ota.swift --file firmware.bin (--token TOKEN | --config backend/config.local.json)\n", stderr)
    exit(2)
}

private func argument(named name: String) -> String? {
    guard let index = CommandLine.arguments.firstIndex(of: name),
          CommandLine.arguments.indices.contains(index + 1) else { return nil }
    return CommandLine.arguments[index + 1]
}

private func loadArguments() throws -> Arguments {
    guard let file = argument(named: "--file") else { usage() }
    let token: String
    if let supplied = argument(named: "--token") {
        token = supplied
    } else if let configPath = argument(named: "--config") {
        let data = try Data(contentsOf: URL(fileURLWithPath: configPath))
        token = try JSONDecoder().decode(DeviceConfig.self, from: data).token
    } else {
        usage()
    }
    guard token.count >= 20 else {
        throw NSError(domain: "AzoriaBLEOTA", code: 1,
                      userInfo: [NSLocalizedDescriptionKey: "token is too short"])
    }
    return Arguments(file: file, token: token)
}

private final class BLEOTAClient: NSObject, CBCentralManagerDelegate,
    CBPeripheralDelegate {
    private let token: String
    private let key: SymmetricKey
    private let image: Data
    private let imageSHA: String
    private var central: CBCentralManager!
    private var peripheral: CBPeripheral?
    private var control: CBCharacteristic?
    private var dataCharacteristic: CBCharacteristic?
    private var status: CBCharacteristic?
    private var offset = 0
    private var sequence: UInt32 = 0
    private var endSent = false
    private var completed = false

    init(arguments: Arguments) throws {
        token = arguments.token
        key = SymmetricKey(data: Data(arguments.token.utf8))
        image = try Data(contentsOf: URL(fileURLWithPath: arguments.file))
        guard !image.isEmpty, image.count <= Int(UInt32.max) else {
            throw NSError(domain: "AzoriaBLEOTA", code: 2,
                          userInfo: [NSLocalizedDescriptionKey: "invalid firmware size"])
        }
        let digest = SHA256.hash(data: image)
        imageSHA = digest.map { String(format: "%02x", $0) }.joined()
        super.init()
        central = CBCentralManager(delegate: self, queue: .main)
    }

    private func signature(_ message: String) -> String {
        let code = HMAC<SHA256>.authenticationCode(
            for: Data(message.utf8), using: key
        )
        return code.prefix(8).map { String(format: "%02x", $0) }.joined()
    }

    private func signed(_ unsigned: String) -> Data {
        Data((unsigned + "|" + signature(unsigned)).utf8)
    }

    private func fail(_ message: String) -> Never {
        fputs("AzoriaBLEOTA: \(message)\n", stderr)
        exit(1)
    }

    private func scan() {
        guard central.state == .poweredOn else { return }
        print("BLE_SCAN")
        central.scanForPeripherals(withServices: [serviceUUID], options: nil)
    }

    func centralManagerDidUpdateState(_ central: CBCentralManager) {
        guard central.state == .poweredOn else {
            if central.state != .unknown { fail("Bluetooth unavailable (state \(central.state.rawValue))") }
            return
        }
        scan()
    }

    func centralManager(_ central: CBCentralManager,
                        didDiscover peripheral: CBPeripheral,
                        advertisementData: [String: Any],
                        rssi RSSI: NSNumber) {
        guard self.peripheral == nil else { return }
        self.peripheral = peripheral
        central.stopScan()
        peripheral.delegate = self
        central.connect(peripheral)
        print("BLE_DISCOVERED=\(peripheral.name ?? peripheral.identifier.uuidString)")
    }

    func centralManager(_ central: CBCentralManager,
                        didConnect peripheral: CBPeripheral) {
        print("BLE_CONNECTED=\(peripheral.identifier.uuidString)")
        peripheral.discoverServices([serviceUUID])
    }

    func centralManager(_ central: CBCentralManager,
                        didFailToConnect peripheral: CBPeripheral,
                        error: Error?) {
        self.peripheral = nil
        print("BLE_CONNECT_FAILED=\(error?.localizedDescription ?? "unknown")")
        scan()
    }

    func centralManager(_ central: CBCentralManager,
                        didDisconnectPeripheral peripheral: CBPeripheral,
                        timestamp: CFAbsoluteTime,
                        isReconnecting: Bool,
                        error: Error?) {
        if completed {
            exit(0)
        }
        fail("BLE disconnected before OTA completed")
    }

    func peripheral(_ peripheral: CBPeripheral,
                    didDiscoverServices error: Error?) {
        guard error == nil, let services = peripheral.services else {
            fail("service discovery failed")
        }
        guard let service = services.first(where: { $0.uuid == serviceUUID }) else {
            fail("Azoria OTA service not found")
        }
        peripheral.discoverCharacteristics(
            [controlUUID, dataUUID, statusUUID], for: service
        )
    }

    func peripheral(_ peripheral: CBPeripheral,
                    didDiscoverCharacteristicsFor service: CBService,
                    error: Error?) {
        guard error == nil, let characteristics = service.characteristics else {
            fail("characteristic discovery failed")
        }
        for characteristic in characteristics {
            switch characteristic.uuid {
            case controlUUID: control = characteristic
            case dataUUID: dataCharacteristic = characteristic
            case statusUUID: status = characteristic
            default: break
            }
        }
        guard let status else { fail("OTA status characteristic not found") }
        guard control != nil, dataCharacteristic != nil else {
            fail("OTA write characteristics not found")
        }
        peripheral.setNotifyValue(true, for: status)
    }

    func peripheral(_ peripheral: CBPeripheral,
                    didUpdateNotificationStateFor characteristic: CBCharacteristic,
                    error: Error?) {
        guard characteristic.uuid == statusUUID else { return }
        guard error == nil, characteristic.isNotifying else {
            fail("OTA status subscription failed")
        }
        print("BLE_OTA_READY")
        let unsigned = "BEGIN|\(image.count)|\(imageSHA)"
        peripheral.writeValue(signed(unsigned), for: control!, type: .withResponse)
    }

    func peripheral(_ peripheral: CBPeripheral,
                    didWriteValueFor characteristic: CBCharacteristic,
                    error: Error?) {
        if let error { fail("BLE write failed: \(error.localizedDescription)") }
    }

    func peripheral(_ peripheral: CBPeripheral,
                    didUpdateValueFor characteristic: CBCharacteristic,
                    error: Error?) {
        guard error == nil, characteristic.uuid == statusUUID,
              let data = characteristic.value,
              let message = String(data: data, encoding: .utf8) else {
            if let error { fail("OTA status read failed: \(error.localizedDescription)") }
            return
        }
        print("BLE_OTA_STATUS=\(message)")
        let parts = message.split(separator: "|", omittingEmptySubsequences: false).map(String.init)
        guard let kind = parts.first else { return }
        switch kind {
        case "READY":
            guard parts.count == 2, parts[1] == String(image.count) else {
                fail("device rejected firmware size")
            }
            sendMore()
        case "PROGRESS":
            guard parts.count == 3, parts[1] == String(image.count) else { return }
            sendEnd()
        case "DONE":
            completed = true
            print("BLE_OTA_DONE")
            DispatchQueue.main.asyncAfter(deadline: .now() + 1.0) {
                exit(0)
            }
        case "ERROR":
            fail(parts.dropFirst().joined(separator: "|"))
        default:
            break
        }
    }

    private func sendMore() {
        guard !endSent, let peripheral, let dataCharacteristic else { return }
        guard peripheral.canSendWriteWithoutResponse else {
            DispatchQueue.main.asyncAfter(deadline: .now() + 0.02) { [weak self] in
                self?.sendMore()
            }
            return
        }
        let maxPayload = max(20, min(240, peripheral.maximumWriteValueLength(for: .withoutResponse) - 4))
        var sent = 0
        while offset < image.count && sent < 4 && peripheral.canSendWriteWithoutResponse {
            let end = min(offset + maxPayload, image.count)
            var packet = Data()
            packet.append(UInt8(sequence & 0xff))
            packet.append(UInt8((sequence >> 8) & 0xff))
            packet.append(UInt8((sequence >> 16) & 0xff))
            packet.append(UInt8((sequence >> 24) & 0xff))
            packet.append(image[offset..<end])
            peripheral.writeValue(packet, for: dataCharacteristic, type: .withoutResponse)
            offset = end
            sequence += 1
            sent += 1
        }
        if offset == image.count {
            DispatchQueue.main.asyncAfter(deadline: .now() + 0.2) { [weak self] in
                self?.sendEnd()
            }
        } else {
            DispatchQueue.main.asyncAfter(deadline: .now() + 0.01) { [weak self] in
                self?.sendMore()
            }
        }
    }

    private func sendEnd() {
        guard !endSent, offset == image.count, let peripheral, let control else { return }
        endSent = true
        let unsigned = "END|\(image.count)|\(imageSHA)"
        peripheral.writeValue(signed(unsigned), for: control, type: .withResponse)
        print("BLE_OTA_FINALIZING")
    }
}

do {
    let arguments = try loadArguments()
    print("BLE_OTA_IMAGE_BYTES=\(try Data(contentsOf: URL(fileURLWithPath: arguments.file)).count)")
    let client = try BLEOTAClient(arguments: arguments)
    withExtendedLifetime(client) {
        RunLoop.main.run()
    }
} catch {
    fputs("AzoriaBLEOTA: \(error.localizedDescription)\n", stderr)
    exit(1)
}
