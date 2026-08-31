import type { ControlRequest, MonitorStatus } from "../../shared/contracts"

const SERVICE = "7a6f0001-4e6d-4a9b-8f41-3c41588fee68"
const REQUEST = "7a6f0002-4e6d-4a9b-8f41-3c41588fee68"
const RESPONSE = "7a6f0003-4e6d-4a9b-8f41-3c41588fee68"

type Characteristic = {
  readValue(): Promise<DataView>
  writeValueWithResponse(value: BufferSource): Promise<void>
  value?: DataView
}

type BluetoothDeviceLike = {
  name?: string
  gatt?: {
    connected?: boolean
    connect(): Promise<{ getPrimaryService(uuid: string): Promise<{ getCharacteristic(uuid: string): Promise<unknown> }> }>
    disconnect(): void
  }
  forget?: () => Promise<void>
  addEventListener(type: string, listener: () => void, options?: { once?: boolean }): void
}

async function attachDevice(
  device: BluetoothDeviceLike,
  status: () => Promise<MonitorStatus>,
  control: (request: ControlRequest, sourceNonce: string, sourceCommandId: string) => Promise<ControlRequest["value"]>,
  onDisconnect: () => void,
): Promise<string> {
  console.info("AZORIA_BLE_CONNECT stage=gatt")
  const server = await device.gatt?.connect()
  if (!server) throw new Error("无法连接 AZORIA Touch")
  console.info("AZORIA_BLE_CONNECT stage=service")
  const service = await server.getPrimaryService(SERVICE)
  console.info("AZORIA_BLE_CONNECT stage=request-characteristic")
  const request = await service.getCharacteristic(REQUEST) as Characteristic
  console.info("AZORIA_BLE_CONNECT stage=response-characteristic")
  const response = await service.getCharacteristic(RESPONSE) as Characteristic
  const hello = "H"
  const helloSignature = await window.azoria.security.sign(hello)
  await response.writeValueWithResponse(new TextEncoder().encode(`${hello}|${helloSignature}`))
  console.info("AZORIA_BLE_CONNECT stage=authenticated")

  let lastCompletedWire = ""
  let polling = false
  let processing = false
  const processValue = async (value?: DataView) => {
    if (!value) return
    const wire = new TextDecoder().decode(value)
    if (!wire || wire === lastCompletedWire || processing) return
    processing = true
    console.info(`AZORIA_BLE_REQUEST fields=${wire.split("|").length}`)
    try {
      await handleRequest(wire, response, status, control)
      lastCompletedWire = wire
    } catch (error) {
      console.error(`AZORIA_BLE_HANDLER_FAILED ${error instanceof Error ? error.message : "unknown"}`)
    } finally {
      processing = false
    }
  }
  const poll = window.setInterval(() => {
    if (polling || processing) return
    polling = true
    void request.readValue().then((value) => processValue(value)).catch(() => undefined).finally(() => { polling = false })
  }, 100)
  device.addEventListener("gattserverdisconnected", () => {
    window.clearInterval(poll)
    onDisconnect()
  }, { once: true })
  console.info("AZORIA_BLE_CONNECT stage=ready")
  return device.name || "AZORIA Touch"
}

export async function connectBle(
  status: () => Promise<MonitorStatus>,
  control: (request: ControlRequest, sourceNonce: string, sourceCommandId: string) => Promise<ControlRequest["value"]>,
  onDisconnect: () => void,
): Promise<string> {
  const bluetooth = (navigator as Navigator & { bluetooth?: any }).bluetooth
  if (!bluetooth) throw new Error("当前 Electron 运行环境不支持蓝牙")
  try {
    const known = typeof bluetooth.getDevices === "function" ? await bluetooth.getDevices() as BluetoothDeviceLike[] : []
    const remembered = known.find((device) => device.name?.toLowerCase().includes("azoria"))
    if (remembered) {
      for (let attempt = 0; attempt < 2; attempt++) {
        try {
          console.info(`AZORIA_BLE_CONNECT stage=remembered-device attempt=${attempt + 1}`)
          return await attachDevice(remembered, status, control, onDisconnect)
        } catch {
          remembered.gatt?.disconnect()
          await new Promise((resolve) => window.setTimeout(resolve, 350))
        }
      }
      if (remembered.gatt?.connected) {
        remembered.gatt?.disconnect()
      }
    }
    console.info("AZORIA_BLE_CONNECT stage=chooser")
    const device = await bluetooth.requestDevice({
      filters: [{ namePrefix: "Azoria" }, { namePrefix: "AZORIA" }],
      optionalServices: [SERVICE],
    }) as BluetoothDeviceLike
    console.info("AZORIA_BLE_CONNECT stage=device-selected")
    return await attachDevice(device, status, control, onDisconnect)
  } catch (error) {
    const name = error instanceof DOMException ? error.name : ""
    const message = error instanceof Error ? error.message : ""
    if (name === "NotFoundError" || /cancelled|canceled/i.test(message)) {
      throw new Error("未发现 AZORIA Touch")
    }
    if (name === "SecurityError" || /permission|not allowed/i.test(message)) {
      throw new Error("蓝牙权限未开启")
    }
    if (message.startsWith("无法连接 AZORIA Touch")) throw error
    throw new Error("AZORIA Touch 蓝牙连接失败")
  }
}

async function handleRequest(
  wire: string,
  response: Characteristic,
  status: () => Promise<MonitorStatus>,
  control: (request: ControlRequest, sourceNonce: string, sourceCommandId: string) => Promise<ControlRequest["value"]>,
) {
  const fields = wire.split("|")
  if (fields.length < 5 || fields[0] !== "Q") return
  const supplied = fields.at(-1) || ""
  const unsigned = fields.slice(0, -1).join("|")
  if (await window.azoria.security.sign(unsigned) !== supplied) {
    console.warn(`AZORIA_BLE_AUTH_REJECTED id=${fields[2] || "unknown"}`)
    return
  }
  const nonce = fields[1] || ""
  const id = fields[2] || ""
  let payload = "E|protocol"
  try {
    if (fields[3] === "S" && fields.length === 5) {
      const current = await status()
      const now = new Date()
      payload = `S|${current.brightness}|${current.volume}|${current.mute ? 1 : 0}|${current.input}|${current.available === false ? 0 : 1}|${Math.floor(now.getTime() / 1000)}|${-now.getTimezoneOffset()}`
    } else if (fields[3] === "C" && fields.length === 8) {
      const rawName = fields[4] || ""
      const raw = fields[5] || "0"
      if (!["brightness", "volume", "mute", "input"].includes(rawName)) throw new Error("unsupported")
      const name = rawName as ControlRequest["control"]
      const value = name === "mute" ? raw === "1" : name === "input" ? raw : Number(raw)
      const returned = await control({ control: name, value, final: fields[6] === "1" } as ControlRequest, nonce, id)
      payload = `C|1|1|${typeof returned === "boolean" ? (returned ? 1 : 0) : returned}`
    }
  } catch {
    payload = "E|desktop"
  }
  const reply = `R|${nonce}|${id}|${payload}`
  const signature = await window.azoria.security.sign(reply)
  await response.writeValueWithResponse(new TextEncoder().encode(`${reply}|${signature}`))
  console.info(`AZORIA_BLE_RESPONSE id=${id} type=${payload[0] || "unknown"}`)
}
