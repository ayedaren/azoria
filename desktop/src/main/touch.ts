import { execFile } from "node:child_process"
import { createHash } from "node:crypto"
import { existsSync } from "node:fs"
import { readFile, stat } from "node:fs/promises"
import { homedir } from "node:os"
import path from "node:path"
import { promisify } from "node:util"
import { SerialPort } from "serialport"
import type { FirmwareImage, UsbDevice } from "../shared/contracts"

const run = promisify(execFile)

export class TouchManager {
  private active = false

  constructor(
    private readonly token: string,
  ) {}

  private tools(): { python: string; esptool: string } {
    const platformio = path.join(homedir(), ".platformio")
    const python = path.join(platformio, "penv/bin/python")
    const esptool = path.join(platformio, "packages/tool-esptoolpy/esptool.py")
    if (!existsSync(python) || !existsSync(esptool)) throw new Error("本地固件工具不可用")
    return { python, esptool }
  }

  async listUsb(): Promise<UsbDevice[]> {
    const candidates = (await SerialPort.list())
      .filter((port) =>
        port.path.includes("usbmodem") &&
        port.manufacturer?.toLowerCase().includes("espressif") === true &&
        /^[0-9a-f]{2}(?::[0-9a-f]{2}){5}$/i.test(port.serialNumber || ""),
      )
    const devices: UsbDevice[] = []
    for (const port of candidates) {
      let verified = false
      try {
        const response = await this.exchange(port.path, "AZORIA_IDENTIFY\n", "AZORIA_TOUCH_V1", 1500)
        verified = response.includes("AZORIA_TOUCH_V1")
      } catch { /* Unverified ESP32-S3 devices remain available for recovery flashing. */ }
      devices.push({
        path: port.path,
        name: `${verified ? "AZORIA Touch" : "ESP32-S3"} · ${port.serialNumber}`,
        verified,
        vendorId: port.vendorId,
        productId: port.productId,
        serialNumber: port.serialNumber,
        manufacturer: port.manufacturer,
      })
    }
    return devices
  }

  private async assertDevice(devicePath: string): Promise<void> {
    const devices = await this.listUsb()
    if (!devices.some((device) => device.path === devicePath)) {
      throw new Error("目标不是当前检测到的 USB 设备")
    }
  }

  async verifyUsb(devicePath: string): Promise<{ chip: string; mac: string }> {
    await this.assertDevice(devicePath)
    const { python, esptool } = this.tools()
    let stdout = ""
    let stderr = ""
    try {
      const result = await run(python, [esptool, "--chip", "esp32s3", "--port", devicePath, "chip-id"], {
        timeout: 20000, maxBuffer: 256 * 1024,
      })
      stdout = result.stdout
      stderr = result.stderr
    } catch (error) {
      const output = `${(error as { stdout?: string }).stdout || ""}\n${(error as { stderr?: string }).stderr || ""}`
      if (/no serial data received|failed to connect/i.test(output)) throw new Error("无法连接 ESP32-S3 刷写模式")
      throw new Error("ESP32-S3 身份校验失败")
    }
    const output = `${stdout}\n${stderr}`
    if (!output.includes("Connected to ESP32-S3")) throw new Error("USB 设备不是受支持的 ESP32-S3")
    const mac = output.match(/MAC:\s+([0-9a-f:]{17})/i)?.[1]?.toUpperCase()
    const expected = (await this.listUsb()).find((device) => device.path === devicePath)?.serialNumber?.toUpperCase()
    if (!mac || !expected || mac !== expected) throw new Error("芯片 MAC 与 USB 身份不一致，已阻止刷写")
    return { chip: "ESP32-S3", mac }
  }

  async inspectFirmware(firmwarePath: string): Promise<FirmwareImage> {
    if (path.extname(firmwarePath).toLowerCase() !== ".bin") throw new Error("请选择 .bin 固件")
    const file = await readFile(firmwarePath)
    const details = await stat(firmwarePath)
    if (!details.isFile() || details.size < 4096 || details.size > 0x640000) throw new Error("固件大小无效")
    if (!file.includes(Buffer.from("AZORIA_TOUCH_V1")) || !file.includes(Buffer.from("AZORIA Touch"))) {
      throw new Error("不是 AZORIA Touch 固件")
    }
    const { python, esptool } = this.tools()
    let output = ""
    try {
      const result = await run(python, [esptool, "image-info", firmwarePath], {
        timeout: 15000, maxBuffer: 512 * 1024,
      })
      output = `${result.stdout}\n${result.stderr}`
    } catch { throw new Error("固件镜像校验失败") }
    if (!/Detected image type:\s*ESP32-S3/i.test(output) || !/Checksum:.*\(valid\)/i.test(output)) {
      throw new Error("固件不适用于 ESP32-S3")
    }
    const version = output.match(/App version:\s*([^\s]+)/i)?.[1]
    if (!version || !/^\d+\.\d+\.\d+(?:[-+][0-9A-Za-z.-]+)?$/.test(version)) throw new Error("固件版本无效")
    return {
      path: firmwarePath,
      name: path.basename(firmwarePath),
      version,
      chip: "ESP32-S3",
      size: details.size,
      sha256: createHash("sha256").update(file).digest("hex"),
    }
  }

  private exchange(devicePath: string, command: string, marker: string, timeoutMs: number): Promise<string> {
    if (this.active) return Promise.reject(new Error("USB 正在执行另一项操作"))
    this.active = true
    return new Promise((resolve, reject) => {
      const port = new SerialPort({ path: devicePath, baudRate: 115200, autoOpen: false })
      let received = ""
      let settled = false
      const finish = (error?: Error) => {
        if (settled) return
        settled = true
        clearTimeout(timer)
        const complete = () => { this.active = false; error ? reject(error) : resolve(received) }
        port.isOpen ? port.close(() => complete()) : complete()
      }
      const timer = setTimeout(() => finish(new Error("小屏幕 USB 响应超时")), timeoutMs)
      port.on("data", (data: Buffer) => {
        received += data.toString("utf8")
        if (received.includes("AZORIA_ERROR")) finish(new Error(received.split("AZORIA_ERROR").at(-1)?.trim() || "小屏幕拒绝操作"))
        else if (received.includes(marker)) finish()
      })
      port.on("error", (error) => finish(error))
      port.open((error) => {
        if (error) return finish(error)
        port.write(command, (writeError) => { if (writeError) finish(writeError) })
      })
    })
  }

  async scanWifi(devicePath: string) {
    await this.assertDevice(devicePath)
    const response = await this.exchange(devicePath, "AZORIA_SCAN\n", "AZORIA_NETWORKS_DONE", 20000)
    return response.split(/\r?\n/).filter((line) => line.startsWith("AZORIA_NETWORK ")).flatMap((line) => {
      const values = new URLSearchParams(line.slice("AZORIA_NETWORK ".length))
      const ssid = values.get("ssid")
      if (!ssid) return []
      return [{ ssid, rssi: Number(values.get("rssi") || -100), secure: values.get("secure") === "1" }]
    })
  }

  async configureWifi(devicePath: string, ssid: string, password: string): Promise<void> {
    await this.assertDevice(devicePath)
    if (!ssid || new TextEncoder().encode(ssid).length > 32) throw new Error("Wi‑Fi 名称无效")
    if (new TextEncoder().encode(password).length > 63) throw new Error("Wi‑Fi 密码过长")
    const values = new URLSearchParams({ ssid, pass: password, host: "", port: "8732", token: this.token })
    await this.exchange(devicePath, `AZORIA_CONFIG ${values}\n`, "AZORIA_OK", 30000)
  }

  async prepareBle(devicePath: string): Promise<void> {
    await this.assertDevice(devicePath)
    const values = new URLSearchParams({ token: this.token })
    await this.exchange(devicePath, `AZORIA_PAIR ${values}\n`, "AZORIA_OK", 5000)
  }

  async flash(devicePath: string, firmwarePath: string, expectedSha256: string): Promise<void> {
    await this.verifyUsb(devicePath)
    const firmware = await this.inspectFirmware(firmwarePath)
    if (!/^[0-9a-f]{64}$/.test(expectedSha256) || firmware.sha256 !== expectedSha256) {
      throw new Error("所选固件已发生变化")
    }
    const { python, esptool } = this.tools()
    try {
      await run(python, [
        esptool, "--chip", "esp32s3", "--port", devicePath, "--baud", "460800",
        "--before", "default-reset", "--after", "hard-reset", "write-flash",
        "--flash-mode", "keep", "--flash-freq", "keep", "--flash-size", "keep",
        "0x10000", firmwarePath,
      ], { timeout: 240000, maxBuffer: 2 * 1024 * 1024 })
    } catch (error) {
      const output = `${(error as { stdout?: string }).stdout || ""}\n${(error as { stderr?: string }).stderr || ""}`
      if (/no serial data received|failed to connect/i.test(output)) throw new Error("无法连接 ESP32-S3 刷写模式")
      throw new Error("固件写入失败")
    }
  }
}
