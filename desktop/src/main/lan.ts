import { createHmac, randomBytes, timingSafeEqual } from "node:crypto"
import { createSocket, type RemoteInfo, type Socket } from "node:dgram"
import { createServer, request as httpRequest, type IncomingMessage, type Server, type ServerResponse } from "node:http"
import type { AddressInfo } from "node:net"
import { networkInterfaces } from "node:os"
import type { ControlRequest, LanDevice, MonitorStatus } from "../shared/contracts"
import type { MonitorController } from "./monitor"

const controlPort = 8732
const discoveryPort = 8733
const coordinationPort = 8734
const heartbeatIntervalMs = 700
const peerMaxAgeMs = 2400
const commandCacheMs = 30000
const activeControllers = new Set<LanController>()

type PrivateInterface = { address: string; netmask: string; broadcast: string }
type Peer = { id: string; address: string; reachable: boolean; master: boolean; seenAt: number }

function ipv4Number(value: string): number | undefined {
  const parts = value.split(".").map(Number)
  if (parts.length !== 4 || parts.some((part) => !Number.isInteger(part) || part < 0 || part > 255)) return undefined
  return (((parts[0]! << 24) | (parts[1]! << 16) | (parts[2]! << 8) | parts[3]!) >>> 0)
}

function isPrivateIpv4(value: string): boolean {
  const parts = value.split(".").map(Number)
  return parts.length === 4 && (
    parts[0] === 10 ||
    (parts[0] === 172 && parts[1]! >= 16 && parts[1]! <= 31) ||
    (parts[0] === 192 && parts[1] === 168) ||
    (parts[0] === 169 && parts[1] === 254)
  )
}

function privateInterfaces(): PrivateInterface[] {
  const found: PrivateInterface[] = []
  for (const addresses of Object.values(networkInterfaces())) {
    for (const item of addresses || []) {
      if (item.family !== "IPv4" || item.internal || !isPrivateIpv4(item.address)) continue
      const address = ipv4Number(item.address)
      const mask = ipv4Number(item.netmask)
      if (address === undefined || mask === undefined) continue
      const broadcast = (address | (~mask >>> 0)) >>> 0
      found.push({
        address: item.address,
        netmask: item.netmask,
        broadcast: [broadcast >>> 24, (broadcast >>> 16) & 255, (broadcast >>> 8) & 255, broadcast & 255].join("."),
      })
    }
  }
  return found
}

function sameSubnet(remote: string, local: PrivateInterface): boolean {
  const remoteValue = ipv4Number(remote)
  const localValue = ipv4Number(local.address)
  const mask = ipv4Number(local.netmask)
  return remoteValue !== undefined && localValue !== undefined && mask !== undefined &&
    (remoteValue & mask) === (localValue & mask)
}

function safeEqual(left: string, right: string): boolean {
  const a = Buffer.from(left)
  const b = Buffer.from(right)
  return a.length === b.length && timingSafeEqual(a, b)
}

export class LanController {
  private readonly servers: Server[] = []
  private readonly coordination = new Map<string, { socket: Socket; timer: NodeJS.Timeout; network: PrivateInterface }>()
  private readonly peers = new Map<string, Peer>()
  private readonly claims = new Map<string, string>()
  private readonly commandResults = new Map<string, { wire: string; expiresAt: number }>()
  private readonly inflightCommands = new Set<string>()
  private readonly relayWaiters = new Map<string, { control: ControlRequest["control"]; resolve(value: ControlRequest["value"]): void; reject(error: Error): void; timeout: NodeJS.Timeout; repeat: NodeJS.Timeout }>()
  private heartbeatSequence = 0
  private reachable = false
  private lastProbeAt = 0
  private masterId = ""
  private isMaster = false

  constructor(private readonly token: string, private readonly desktopId: string, private readonly monitor: MonitorController) {
    activeControllers.add(this)
  }

  private sign(message: string): string {
    return createHmac("sha256", this.token).update(message).digest("hex").slice(0, 16)
  }

  async start(): Promise<void> {
    await Promise.all(privateInterfaces().flatMap((network) => [this.listen(network.address), this.coordinate(network)]))
  }

  private listen(address: string): Promise<void> {
    if (this.servers.some((server) => (server.address() as AddressInfo | null)?.address === address)) return Promise.resolve()
    const server = createServer((request, response) => { void this.handleHttp(request, response) })
    this.servers.push(server)
    return new Promise((resolve) => {
      server.once("error", () => resolve())
      server.listen(controlPort, address, resolve)
    })
  }

  private coordinate(network: PrivateInterface): Promise<void> {
    if (this.coordination.has(network.address)) return Promise.resolve()
    return new Promise((resolve) => {
      const socket = createSocket({ type: "udp4", reuseAddr: true })
      let settled = false
      const finish = () => { if (!settled) { settled = true; resolve() } }
      socket.on("message", (message, remote) => { void this.handleCoordination(socket, network, message, remote) })
      socket.once("error", finish)
      socket.bind(coordinationPort, network.address, () => {
        socket.setBroadcast(true)
        const send = () => { void this.sendHeartbeat(socket, network) }
        const timer = setInterval(send, heartbeatIntervalMs)
        this.coordination.set(network.address, { socket, timer, network })
        send()
        finish()
      })
    })
  }

  private async sendHeartbeat(socket: Socket, network: PrivateInterface): Promise<void> {
    const now = Date.now()
    if (now - this.lastProbeAt >= 1400) {
      this.lastProbeAt = now
      try { this.reachable = await this.monitor.reachable() }
      catch { this.reachable = false }
    }
    if (!this.reachable && this.isMaster) this.releaseMaster(socket, network)
    this.peers.set(this.desktopId, { id: this.desktopId, address: network.address, reachable: this.reachable, master: false, seenAt: Date.now() })
    this.refreshMaster()
    const self = this.peers.get(this.desktopId)
    if (self) self.master = this.isMaster
    const unsigned = `AZORIA_DESKTOP_HEARTBEAT_V1|${this.desktopId}|${network.address}|${this.reachable ? 1 : 0}|${this.isMaster ? 1 : 0}|${++this.heartbeatSequence}`
    const heartbeat = Buffer.from(`${unsigned}|${this.sign(unsigned)}`)
    socket.send(heartbeat, coordinationPort, network.broadcast)
    socket.send(heartbeat, discoveryPort, network.broadcast)
    for (const [id, peer] of this.peers) if (Date.now() - peer.seenAt > peerMaxAgeMs) this.peers.delete(id)
    for (const [id, result] of this.commandResults) if (result.expiresAt <= Date.now()) this.commandResults.delete(id)
  }

  private refreshMaster(): string {
    const now = Date.now()
    const candidates = [...this.peers.values()]
      .filter((peer) => peer.reachable && now - peer.seenAt <= peerMaxAgeMs)
      .sort((left, right) => left.id.localeCompare(right.id))
    this.masterId = candidates[0]?.id || ""
    this.isMaster = this.masterId === this.desktopId
    return this.masterId
  }

  private releaseMaster(socket: Socket, network: PrivateInterface): void {
    if (!this.isMaster && this.masterId !== this.desktopId) return
    this.isMaster = false
    this.masterId = ""
    const self = this.peers.get(this.desktopId)
    if (self) { self.reachable = false; self.master = false }
    this.broadcast(socket, network, `AZORIA_DESKTOP_RELEASE_V1|${this.desktopId}|${this.heartbeatSequence}`)
  }

  private broadcast(socket: Socket, network: PrivateInterface, unsigned: string): string {
    const wire = `${unsigned}|${this.sign(unsigned)}`
    socket.send(Buffer.from(wire), coordinationPort, network.broadcast)
    return wire
  }

  private parseControl(control: string, encoded: string): ControlRequest["value"] | undefined {
    if (control === "input") return ["dp1", "hdmi1", "hdmi2", "usbc"].includes(encoded) ? encoded as ControlRequest["value"] : undefined
    if (control === "mute") return encoded === "1" ? true : encoded === "0" ? false : undefined
    const value = Number(encoded)
    return Number.isInteger(value) && value >= 0 && value <= 100 ? value : undefined
  }

  private async handleCoordination(socket: Socket, network: PrivateInterface, message: Buffer, remote: RemoteInfo): Promise<void> {
    if (!sameSubnet(remote.address, network) || message.length > 512) return
    const fields = message.toString("utf8").split("|")
    const supplied = fields.at(-1) || ""
    const unsigned = fields.slice(0, -1).join("|")
    if (!safeEqual(this.sign(unsigned), supplied)) return

    if (fields[0] === "AZORIA_DESKTOP_HEARTBEAT_V1" && fields.length === 7) {
      const id = fields[1] || ""
      if (!/^[0-9a-f]{32}$/i.test(id) || fields[2] !== remote.address || !["0", "1"].includes(fields[3] || "") ||
          !["0", "1"].includes(fields[4] || "") || !/^\d+$/.test(fields[5] || "")) return
      this.peers.set(id, { id, address: remote.address, reachable: fields[3] === "1", master: fields[4] === "1", seenAt: Date.now() })
      this.refreshMaster()
      return
    }

    if (fields[0] === "AZORIA_DESKTOP_RELEASE_V1" && fields.length === 4) {
      const released = fields[1] || ""
      if (released === this.masterId) this.masterId = ""
      const peer = this.peers.get(released)
      if (peer) peer.master = false
      return
    }

    if (fields[0] === "AZORIA_DESKTOP_CLAIM_V1" && fields.length === 7) {
      const key = `${fields[1]}:${fields[2]}:${fields[3]}`
      const claimant = fields[4] || ""
      if (!/^[0-9a-f]{32}$/i.test(claimant)) return
      const current = this.claims.get(key)
      if (!current || claimant.localeCompare(current) < 0) this.claims.set(key, claimant)
      return
    }

    if (fields[0] === "AZORIA_DESKTOP_RESULT_V1" && fields.length === 10) {
      const key = `${fields[1]}:${fields[2]}:${fields[3]}`
      this.commandResults.set(key, { wire: message.toString("utf8"), expiresAt: Date.now() + commandCacheMs })
      if (fields[5] === "1" && /^[0-9a-f]{32}$/i.test(fields[4] || "")) {
        this.masterId = fields[4]!
        this.isMaster = this.masterId === this.desktopId
      }
      const waiter = this.relayWaiters.get(key)
      if (waiter) {
        clearTimeout(waiter.timeout)
        clearInterval(waiter.repeat)
        this.relayWaiters.delete(key)
        const value = fields[5] === "1" && fields[7] === waiter.control ? this.parseControl(waiter.control, fields[8] || "") : undefined
        if (value === undefined) waiter.reject(new Error("没有 Desktop 能执行当前 DDC/CI 命令"))
        else waiter.resolve(value)
      }
      return
    }

    if (fields[0] !== "AZORIA_TOUCH_COMMAND_V1" || fields.length !== 8) return
    const [touchId, bootNonce, commandId, control, encodedValue, finalValue] = fields.slice(1, 7)
    if (!/^[0-9A-F]{12}$/i.test(touchId || "") || !/^[0-9a-f]{8}$/i.test(bootNonce || "") || !/^\d+$/.test(commandId || "") ||
        !["brightness", "volume", "mute", "input"].includes(control || "") || !["0", "1"].includes(finalValue || "")) return
    const value = this.parseControl(control!, encodedValue!)
    if (value === undefined) return
    const key = `${touchId}:${bootNonce}:${commandId}`
    const cached = this.commandResults.get(key)
    if (cached) {
      socket.send(Buffer.from(cached.wire), discoveryPort, remote.address)
      return
    }
    if (this.inflightCommands.has(key)) return
    this.inflightCommands.add(key)
    const master = this.refreshMaster()
    const peers = [...this.peers.keys()].sort()
    const rank = Math.max(0, peers.indexOf(this.desktopId))
    const delay = master === this.desktopId ? 0 : master ? 850 + rank * 120 : 120 + rank * 180
    void this.consumeCommand(socket, network, remote.address, {
      touchId: touchId!, bootNonce: bootNonce!, commandId: commandId!, control: control as ControlRequest["control"],
      value, final: finalValue === "1", finalValue: finalValue!, key, delay,
    })
  }

  relayControl(request: ControlRequest, sourceNonce: string, sourceCommandId: string): Promise<ControlRequest["value"]> {
    if (!/^[0-9a-f]{8}$/i.test(sourceNonce) || !/^\d+$/.test(sourceCommandId)) throw new Error("蓝牙转发命令标识无效")
    const entry = this.coordination.values().next().value as { socket: Socket; network: PrivateInterface } | undefined
    if (!entry) throw new Error("没有可用的局域网接口")
    const encoded = typeof request.value === "boolean" ? (request.value ? "1" : "0") : String(request.value)
    if (this.parseControl(request.control, encoded) === undefined) throw new Error("蓝牙转发命令值无效")
    const touchId = this.desktopId.slice(0, 12).toUpperCase()
    const key = `${touchId}:${sourceNonce.toLowerCase()}:${sourceCommandId}`
    const unsigned = `AZORIA_TOUCH_COMMAND_V1|${touchId}|${sourceNonce.toLowerCase()}|${sourceCommandId}|${request.control}|${encoded}|${request.final === false ? 0 : 1}`
    return new Promise((resolve, reject) => {
      const send = () => this.broadcast(entry.socket, entry.network, unsigned)
      const repeat = setInterval(send, 350)
      const timeout = setTimeout(() => {
        clearInterval(repeat)
        this.relayWaiters.delete(key)
        reject(new Error("局域网内没有 Desktop 完成命令"))
      }, 9000)
      this.relayWaiters.set(key, { control: request.control, resolve, reject, timeout, repeat })
      send()
    })
  }

  async relayStatus(): Promise<MonitorStatus> {
    const master = this.refreshMaster()
    if (master === this.desktopId) return this.monitor.status()
    const now = Date.now()
    const peer = (master ? this.peers.get(master) : undefined) || [...this.peers.values()]
      .filter((candidate) => candidate.reachable && now - candidate.seenAt <= peerMaxAgeMs)
      .sort((left, right) => left.id.localeCompare(right.id))[0]
    if (!peer || !isPrivateIpv4(peer.address)) throw new Error("局域网内没有可用的 Desktop")
    return new Promise((resolve, reject) => {
      const request = httpRequest({
        host: peer.address, port: controlPort, path: "/v1/status", method: "GET",
        headers: { Authorization: `Bearer ${this.token}` }, timeout: 3500,
      }, (response) => {
        const chunks: Buffer[] = []
        let length = 0
        response.on("data", (chunk) => {
          length += Buffer.byteLength(chunk)
          if (length <= 4096) chunks.push(Buffer.from(chunk))
        })
        response.on("end", () => {
          if (response.statusCode !== 200 || length > 4096) return reject(new Error("Desktop 状态读取失败"))
          try { resolve(JSON.parse(Buffer.concat(chunks).toString("utf8")) as MonitorStatus) }
          catch { reject(new Error("Desktop 状态响应无效")) }
        })
      })
      request.on("timeout", () => request.destroy(new Error("Desktop 状态读取超时")))
      request.on("error", reject)
      request.end()
    })
  }

  private async consumeCommand(
    socket: Socket,
    network: PrivateInterface,
    touchAddress: string,
    command: { touchId: string; bootNonce: string; commandId: string; control: ControlRequest["control"]; value: ControlRequest["value"]; final: boolean; finalValue: string; key: string; delay: number },
  ): Promise<void> {
    try {
      if (command.delay) await new Promise((resolve) => setTimeout(resolve, command.delay))
      if (this.commandResults.has(command.key)) return
      const currentMaster = this.refreshMaster()
      if (currentMaster && currentMaster !== this.desktopId) return
      this.reachable = await this.monitor.reachable()
      this.peers.set(this.desktopId, { id: this.desktopId, address: network.address, reachable: this.reachable, master: this.isMaster, seenAt: Date.now() })
      if (!this.reachable) {
        this.releaseMaster(socket, network)
        return
      }
      this.claims.set(command.key, this.desktopId)
      this.broadcast(socket, network, `AZORIA_DESKTOP_CLAIM_V1|${command.touchId}|${command.bootNonce}|${command.commandId}|${this.desktopId}|${this.heartbeatSequence}`)
      await new Promise((resolve) => setTimeout(resolve, 120))
      if (this.commandResults.has(command.key) || this.claims.get(command.key) !== this.desktopId) return
      const status = await this.monitor.control({ control: command.control, value: command.value, final: command.final })
      const returned = status[command.control as keyof typeof status]
      const encoded = typeof returned === "boolean" ? (returned ? "1" : "0") : String(returned)
      this.isMaster = true
      this.masterId = this.desktopId
      const self = this.peers.get(this.desktopId)
      if (self) self.master = true
      const result = `AZORIA_DESKTOP_RESULT_V1|${command.touchId}|${command.bootNonce}|${command.commandId}|${this.desktopId}|1|${command.finalValue}|${command.control}|${encoded}`
      const wire = this.broadcast(socket, network, result)
      this.commandResults.set(command.key, { wire, expiresAt: Date.now() + commandCacheMs })
      socket.send(Buffer.from(wire), discoveryPort, touchAddress)
    } catch {
      this.reachable = false
      this.releaseMaster(socket, network)
    } finally {
      this.claims.delete(command.key)
      this.inflightCommands.delete(command.key)
    }
  }

  private authorized(request: IncomingMessage): boolean {
    const remote = request.socket.remoteAddress?.replace(/^::ffff:/, "") || ""
    return isPrivateIpv4(remote) && safeEqual(request.headers.authorization || "", `Bearer ${this.token}`)
  }

  private json(response: ServerResponse, status: number, body: unknown): void {
    response.writeHead(status, { "Content-Type": "application/json", "Cache-Control": "no-store" })
    response.end(JSON.stringify(body))
  }

  private async body(request: IncomingMessage): Promise<Record<string, unknown>> {
    const chunks: Buffer[] = []
    let length = 0
    for await (const chunk of request) {
      const value = Buffer.from(chunk)
      length += value.length
      if (length > 2048) throw new Error("请求内容过大")
      chunks.push(value)
    }
    const parsed = JSON.parse(Buffer.concat(chunks).toString("utf8")) as unknown
    if (!parsed || typeof parsed !== "object" || Array.isArray(parsed)) throw new Error("请求格式无效")
    return parsed as Record<string, unknown>
  }

  private async handleHttp(request: IncomingMessage, response: ServerResponse): Promise<void> {
    if (!this.authorized(request)) return this.json(response, 401, { ok: false, error: "unauthorized" })
    try {
      if (request.method === "GET" && request.url === "/v1/status") return this.json(response, 200, await this.monitor.status())
      if (request.method === "POST" && request.url === "/v1/control") {
        if (this.refreshMaster() !== this.desktopId || !this.reachable) return this.json(response, 409, { ok: false, error: "not active DDC/CI host" })
        const payload = await this.body(request)
        const control = payload.control
        if (!["brightness", "volume", "mute", "input"].includes(String(control))) return this.json(response, 400, { ok: false, error: "unsupported control" })
        const status = await this.monitor.control(payload as unknown as ControlRequest)
        return this.json(response, 200, { accepted: true, confirmed: payload.final !== false, value: status[control as keyof typeof status] })
      }
      if (request.method === "POST" && request.url === "/v1/device/register") {
        await this.body(request)
        return this.json(response, 200, { ok: true })
      }
      return this.json(response, 404, { ok: false, error: "not found" })
    } catch (error) {
      return this.json(response, 400, { ok: false, error: error instanceof Error ? error.message : "request failed" })
    }
  }

  async discover(): Promise<LanDevice[]> {
    await this.start()
    const results = await Promise.all(privateInterfaces().map((network) => this.discoverOn(network)))
    const devices = new Map<string, LanDevice>()
    for (const result of results.flat()) devices.set(result.id, result)
    return [...devices.values()]
  }

  private discoverOn(network: PrivateInterface): Promise<LanDevice[]> {
    return new Promise((resolve) => {
      const socket = createSocket("udp4")
      const nonce = randomBytes(12).toString("hex")
      const unsignedProbe = `AZORIA_DESKTOP_DISCOVER_V1|${nonce}`
      const probe = Buffer.from(`${unsignedProbe}|${this.sign(unsignedProbe)}`)
      const devices = new Map<string, LanDevice>()
      const pending = new Map<string, { id: string; name: string; firmware: string }>()
      let finished = false
      const finish = () => {
        if (finished) return
        finished = true
        socket.close()
        resolve([...devices.values()])
      }
      socket.on("message", (message, remote) => {
        if (!sameSubnet(remote.address, network)) return
        const fields = message.toString("utf8").split("|")
        const supplied = fields.at(-1) || ""
        const unsigned = fields.slice(0, -1).join("|")
        if (!safeEqual(this.sign(unsigned), supplied) || fields[1] !== nonce) return
        if (fields[0] === "AZORIA_TOUCH_V1" && fields.length === 6) {
          const id = fields[2] || ""
          const name = fields[3] || ""
          const firmware = fields[4] || ""
          if (!/^[0-9A-F]{12}$/i.test(id) || !/^azoria-touch-[a-z0-9-]{1,32}$/i.test(name)) return
          pending.set(remote.address, { id, name, firmware })
          const config = `AZORIA_DESKTOP_CONFIG_V1|${nonce}|${network.address}|${controlPort}`
          socket.send(Buffer.from(`${config}|${this.sign(config)}`), discoveryPort, remote.address)
        } else if (fields[0] === "AZORIA_TOUCH_CONFIGURED_V1" && fields.length === 4) {
          const found = pending.get(remote.address)
          if (!found || found.id !== fields[2]) return
          devices.set(found.id, { ...found, address: remote.address, paired: true })
        }
      })
      socket.on("error", finish)
      socket.bind(0, network.address, () => {
        socket.setBroadcast(true)
        socket.send(probe, discoveryPort, network.broadcast)
        setTimeout(finish, 1200)
      })
    })
  }

  stop(): void {
    for (const server of this.servers) server.close()
    this.servers.length = 0
    for (const { socket, timer } of this.coordination.values()) {
      clearInterval(timer)
      socket.close()
    }
    this.coordination.clear()
    for (const waiter of this.relayWaiters.values()) {
      clearTimeout(waiter.timeout)
      clearInterval(waiter.repeat)
      waiter.reject(new Error("Desktop 已停止"))
    }
    this.relayWaiters.clear()
    activeControllers.delete(this)
  }

  static stopAll(): void {
    for (const controller of [...activeControllers]) controller.stop()
  }
}
