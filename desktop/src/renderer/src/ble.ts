import type { ControlRequest, MonitorStatus } from "../../shared/contracts"

const SERVICE = "7a6f0001-4e6d-4a9b-8f41-3c41588fee68"
const REQUEST = "7a6f0002-4e6d-4a9b-8f41-3c41588fee68"
const RESPONSE = "7a6f0003-4e6d-4a9b-8f41-3c41588fee68"

type Characteristic = {
  startNotifications(): Promise<Characteristic>
  addEventListener(type: string, listener: (event: Event) => void): void
  writeValueWithResponse(value: BufferSource): Promise<void>
  value?: DataView
}

export async function connectBle(
  status: () => Promise<MonitorStatus>,
  control: (request: ControlRequest, sourceNonce: string, sourceCommandId: string) => Promise<ControlRequest["value"]>,
): Promise<string> {
  const bluetooth = (navigator as Navigator & { bluetooth?: any }).bluetooth
  if (!bluetooth) throw new Error("当前 Electron 运行环境不支持蓝牙")
  try {
    const device = await bluetooth.requestDevice({ filters: [{ services: [SERVICE] }] })
    const server = await device.gatt?.connect()
    if (!server) throw new Error("无法连接 AZORIA Touch")
    const service = await server.getPrimaryService(SERVICE)
    const request = await service.getCharacteristic(REQUEST) as Characteristic
    const response = await service.getCharacteristic(RESPONSE) as Characteristic

    request.addEventListener("characteristicvaluechanged", (event: Event) => {
      const value = (event.target as unknown as Characteristic).value
      if (!value) return
      const wire = new TextDecoder().decode(value)
      void handleRequest(wire, response, status, control)
    })
    await request.startNotifications()
    return device.name || "AZORIA Touch"
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
  if (await window.azoria.security.sign(unsigned) !== supplied) return
  const nonce = fields[1] || ""
  const id = fields[2] || ""
  let payload = "E|protocol"
  try {
    if (fields[3] === "S" && fields.length === 5) {
      const current = await status()
      payload = `S|${current.brightness}|${current.volume}|${current.mute ? 1 : 0}|${current.input}`
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
}
