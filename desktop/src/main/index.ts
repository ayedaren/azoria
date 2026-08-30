import { createHmac } from "node:crypto"
import path from "node:path"
import { app, BrowserWindow, dialog, ipcMain, session, type OpenDialogOptions } from "electron"
import type { ControlRequest } from "../shared/contracts"
import { loadConfig } from "./config"
import { DeviceManager } from "./device"
import { LanController } from "./lan"
import { MonitorController } from "./monitor"

const isDevelopment = !app.isPackaged

app.setName("AZORIA Desktop")
for (const option of [
  "disable-background-networking",
  "disable-component-update",
  "disable-client-side-phishing-detection",
  "disable-sync",
  "metrics-recording-only",
  "no-first-run",
]) app.commandLine.appendSwitch(option)

function createWindow(): BrowserWindow {
  let bluetoothCallback: ((deviceId: string) => void) | undefined
  let bluetoothTimeout: ReturnType<typeof setTimeout> | undefined

  const finishBluetoothSelection = (deviceId: string) => {
    if (bluetoothTimeout) clearTimeout(bluetoothTimeout)
    bluetoothTimeout = undefined
    const callback = bluetoothCallback
    bluetoothCallback = undefined
    callback?.(deviceId)
  }

  const window = new BrowserWindow({
    width: 1180,
    height: 780,
    minWidth: 880,
    minHeight: 640,
    backgroundColor: "#000000",
    titleBarStyle: "hiddenInset",
    trafficLightPosition: { x: 18, y: 18 },
    webPreferences: {
      preload: path.join(__dirname, "../preload/index.js"),
      contextIsolation: true,
      nodeIntegration: false,
      sandbox: true,
    },
  })
  window.webContents.setWindowOpenHandler(() => ({ action: "deny" }))
  window.webContents.on("will-navigate", (event, url) => {
    if (!url.startsWith("file:") && !url.startsWith("http://localhost:")) event.preventDefault()
  })
  window.webContents.on("select-bluetooth-device", (event, devices, callback) => {
    event.preventDefault()
    bluetoothCallback = callback
    const device = devices.find((candidate) => candidate.deviceName.toLowerCase().includes("azoria"))
    if (device) {
      finishBluetoothSelection(device.deviceId)
      return
    }
    bluetoothTimeout ??= setTimeout(() => finishBluetoothSelection(""), 12_000)
  })
  window.on("closed", () => {
    if (bluetoothTimeout) clearTimeout(bluetoothTimeout)
    bluetoothTimeout = undefined
    bluetoothCallback = undefined
  })
  if (isDevelopment && process.env.ELECTRON_RENDERER_URL) {
    void window.loadURL(process.env.ELECTRON_RENDERER_URL)
  } else {
    void window.loadFile(path.join(__dirname, "../renderer/index.html"))
  }
  return window
}

void app.whenReady().then(async () => {
  const config = await loadConfig(app.getPath("userData"))
  const monitor = new MonitorController(
    config.display,
    path.resolve(app.getAppPath(), "desktop/profiles"),
    path.join(app.getPath("userData"), "monitor-profiles"),
  )
  await monitor.initialize()
  const lan = new LanController(config.token, config.desktopId, monitor)
  await lan.start()
  const firmwareDirectory = path.resolve(app.getAppPath(), "firmware")
  const devices = new DeviceManager(config.token, firmwareDirectory)

  session.defaultSession.webRequest.onBeforeRequest(
    { urls: ["http://*/*", "https://*/*"] },
    (details, callback) => {
      const target = new URL(details.url)
      const localDevelopment = isDevelopment && target.hostname === "localhost" && target.port === "5173"
      callback({ cancel: !localDevelopment })
    },
  )

  ipcMain.handle("monitor:status", () => monitor.status())
  ipcMain.handle("monitor:relay-status", () => lan.relayStatus())
  ipcMain.handle("monitor:control", (_event, request: ControlRequest) => monitor.control(request))
  ipcMain.handle("monitor:relay-control", (_event, request: ControlRequest, sourceNonce: string, sourceCommandId: string) =>
    lan.relayControl(request, sourceNonce, sourceCommandId))
  ipcMain.handle("monitor:connection", () => monitor.connection())
  ipcMain.handle("monitor:import-profile", async (event) => {
    const options: OpenDialogOptions = {
      title: "加载显示器配置表",
      properties: ["openFile"],
      filters: [{ name: "显示器配置表", extensions: ["json"] }],
    }
    const parent = BrowserWindow.fromWebContents(event.sender)
    const result = parent ? await dialog.showOpenDialog(parent, options) : await dialog.showOpenDialog(options)
    if (result.canceled || !result.filePaths[0]) return null
    return monitor.importProfile(result.filePaths[0])
  })
  ipcMain.handle("device:list-usb", () => devices.listUsb())
  ipcMain.handle("device:discover-lan", () => lan.discover())
  ipcMain.handle("device:verify-usb", (_event, devicePath: string) => devices.verifyUsb(devicePath))
  ipcMain.handle("device:scan-wifi", (_event, devicePath: string) => devices.scanWifi(devicePath))
  ipcMain.handle("device:configure-wifi", (_event, input: { path: string; ssid: string; password: string }) =>
    devices.configureWifi(input.path, input.ssid, input.password))
  ipcMain.handle("device:flash", (_event, devicePath: string) => devices.flash(devicePath))
  ipcMain.handle("security:sign", (_event, message: string) => {
    if (typeof message !== "string" || message.length > 512) throw new Error("签名消息无效")
    return createHmac("sha256", config.token).update(message).digest("hex").slice(0, 16)
  })

  createWindow()
  app.on("activate", () => { if (BrowserWindow.getAllWindows().length === 0) createWindow() })
})

app.on("before-quit", () => LanController.stopAll())
app.on("window-all-closed", () => { if (process.platform !== "darwin") app.quit() })
