import { execFile } from "node:child_process"
import { existsSync } from "node:fs"
import { homedir } from "node:os"
import path from "node:path"
import { promisify } from "node:util"
import { SerialPort } from "serialport"
import type { UsbDevice } from "../shared/contracts"

const run = promisify(execFile)

export class DeviceManager {
  private active = false

  constructor(
    private readonly token: string,
    private readonly firmwareDirectory: string,
  ) {}

  async listUsb(): Promise<UsbDevice[]> {
    return (await SerialPort.list())
      .filter((port) =>
        port.path.includes("usbmodem") &&
        port.manufacturer?.toLowerCase().includes("espressif") === true &&
        /^[0-9a-f]{2}(?::[0-9a-f]{2}){5}$/i.test(port.serialNumber || ""),
      )
      .map((port) => ({
        path: port.path,
        name: `AZORIA Touch · ${port.serialNumber}`,
        verified: true,
        vendorId: port.vendorId,
        productId: port.productId,
        serialNumber: port.serialNumber,
        manufacturer: port.manufacturer,
      }))
  }

  private async assertDevice(devicePath: string): Promise<void> {
    const devices = await this.listUsb()
    if (!devices.some((device) => device.path === devicePath)) {
      throw new Error("目标不是当前检测到的 USB 设备")
    }
  }

  async verifyUsb(devicePath: string): Promise<{ chip: string; mac: string }> {
    await this.assertDevice(devicePath)
    const platformio = path.join(homedir(), ".platformio")
    const python = path.join(platformio, "penv/bin/python")
    const esptool = path.join(platformio, "packages/tool-esptoolpy/esptool.py")
    if (!existsSync(python) || !existsSync(esptool)) throw new Error("本地芯片校验工具不可用")
    const { stdout, stderr } = await run(
      python,
      [esptool, "--chip", "esp32s3", "--port", devicePath, "chip-id"],
      { timeout: 20000, maxBuffer: 256 * 1024 },
    )
    const output = `${stdout}\n${stderr}`
    if (!output.includes("Connected to ESP32-S3")) throw new Error("USB 设备不是受支持的 ESP32-S3")
    const mac = output.match(/MAC:\s+([0-9a-f:]{17})/i)?.[1]?.toUpperCase()
    const expected = (await this.listUsb()).find((device) => device.path === devicePath)?.serialNumber?.toUpperCase()
    if (!mac || !expected || mac !== expected) throw new Error("芯片 MAC 与 USB 身份不一致，已阻止刷写")
    return { chip: "ESP32-S3", mac }
  }

  private exchange(devicePath: string, command: string, marker: string, timeoutMs: number): Promise<string> {
    if (this.active) return Promise.reject(new Error("USB 正在执行另一项操作"))
    this.active = true
    return new Promise((resolve, reject) => {
      const port = new SerialPort({ path: devicePath, baudRate: 115200, autoOpen: false })
      let received = ""
      const finish = (error?: Error) => {
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

  async flash(devicePath: string): Promise<void> {
    await this.verifyUsb(devicePath)
    const candidates = [path.join(homedir(), ".local/bin/pio"), "/opt/homebrew/bin/pio", "/usr/local/bin/pio"]
    const executable = candidates.find(existsSync)
    if (!executable || !existsSync(path.join(this.firmwareDirectory, "platformio.ini"))) {
      throw new Error("本地刷写工具或固件工程不可用")
    }
    await run(executable, ["run", "-d", this.firmwareDirectory, "-t", "upload", "--upload-port", devicePath], {
      timeout: 240000,
      maxBuffer: 2 * 1024 * 1024,
    })
  }
}
