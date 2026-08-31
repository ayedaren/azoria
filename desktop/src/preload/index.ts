import { contextBridge, ipcRenderer } from "electron"
import type { ControlRequest, DesktopApi } from "../shared/contracts"

const api: DesktopApi = {
  monitor: {
    status: () => ipcRenderer.invoke("monitor:status"),
    relayStatus: () => ipcRenderer.invoke("monitor:relay-status"),
    control: (request: ControlRequest) => ipcRenderer.invoke("monitor:control", request),
    relayControl: (request: ControlRequest, sourceNonce: string, sourceCommandId: string) =>
      ipcRenderer.invoke("monitor:relay-control", request, sourceNonce, sourceCommandId),
    connection: () => ipcRenderer.invoke("monitor:connection"),
    importProfile: () => ipcRenderer.invoke("monitor:import-profile"),
  },
  device: {
    listUsb: () => ipcRenderer.invoke("device:list-usb"),
    discoverLan: () => ipcRenderer.invoke("device:discover-lan"),
    listLan: () => ipcRenderer.invoke("device:list-lan"),
    verifyUsb: (path: string) => ipcRenderer.invoke("device:verify-usb", path),
    scanWifi: (path: string) => ipcRenderer.invoke("device:scan-wifi", path),
    configureWifi: (path: string, ssid: string, password: string) =>
      ipcRenderer.invoke("device:configure-wifi", { path, ssid, password }),
    prepareBle: (path: string) => ipcRenderer.invoke("device:prepare-ble", path),
    selectFirmware: () => ipcRenderer.invoke("device:select-firmware"),
    flash: (path: string, firmwarePath: string, expectedSha256: string) =>
      ipcRenderer.invoke("device:flash", { path, firmwarePath, expectedSha256 }),
  },
  security: {
    sign: (message: string) => ipcRenderer.invoke("security:sign", message),
  },
}

contextBridge.exposeInMainWorld("azoria", api)
