export type InputSource = "dp1" | "hdmi1" | "hdmi2" | "usbc"
export type ControlName = "brightness" | "volume" | "mute" | "input"
export type MonitorTransport = "usb-hid-ddc" | "video-ddc" | "unavailable"

export interface MonitorStatus {
  brightness: number
  volume: number
  mute: boolean
  input: InputSource
}

export interface ControlRequest {
  control: ControlName
  value: number | boolean | InputSource
  final?: boolean
}

export interface MonitorConnectionInfo {
  displayName: string
  profileId: string
  profileName: string
  summary: string
  availableTransports: MonitorTransport[]
  routes: Record<ControlName, MonitorTransport>
}

export interface UsbDevice {
  path: string
  name: string
  verified: boolean
  vendorId?: string
  productId?: string
  serialNumber?: string
  manufacturer?: string
}

export interface LanDevice {
  id: string
  name: string
  address: string
  firmware: string
  paired: boolean
}

export interface DesktopApi {
  monitor: {
    status(): Promise<MonitorStatus>
    relayStatus(): Promise<MonitorStatus>
    control(request: ControlRequest): Promise<MonitorStatus>
    relayControl(request: ControlRequest, sourceNonce: string, sourceCommandId: string): Promise<ControlRequest["value"]>
    connection(): Promise<MonitorConnectionInfo>
    importProfile(): Promise<MonitorConnectionInfo | null>
  }
  device: {
    listUsb(): Promise<UsbDevice[]>
    discoverLan(): Promise<LanDevice[]>
    verifyUsb(path: string): Promise<{ chip: string; mac: string }>
    scanWifi(path: string): Promise<Array<{ ssid: string; rssi: number; secure: boolean }>>
    configureWifi(path: string, ssid: string, password: string): Promise<void>
    flash(path: string): Promise<void>
  }
  security: {
    sign(message: string): Promise<string>
  }
}
