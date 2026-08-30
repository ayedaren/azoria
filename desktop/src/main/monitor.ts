import { execFile } from "node:child_process"
import { existsSync } from "node:fs"
import { mkdir, readFile, readdir, writeFile } from "node:fs/promises"
import path from "node:path"
import { promisify } from "node:util"
import type { ControlName, ControlRequest, InputSource, MonitorConnectionInfo, MonitorStatus, MonitorTransport } from "../shared/contracts"
import type { LocalLogger } from "./logger"

const run = promisify(execFile)
const controls: ControlName[] = ["brightness", "volume", "mute", "input"]
const transportIds: MonitorTransport[] = ["usb-hid-ddc", "video-ddc"]
export type ControlSource = "desktop-ui" | "touch" | "desktop-peer" | "internal"

interface MonitorProfile {
  id: string
  name: string
  fallback?: boolean
  match?: { displayNamePattern?: string; usbHid?: { vendorId: number; productId: number } }
  routes: Record<ControlName, MonitorTransport[]>
  usbHid?: {
    adapter: "lg-monitor-controls-v1"
    vcp: Record<ControlName, number>
    inputWriteMode: "vendor-private" | "vcp"
    inputValues: Record<InputSource, number>
  }
  ddc?: {
    inputReadValues: Record<string, InputSource>
    inputWriteValues: Record<InputSource, string>
    inputWriteFeature: "input" | "input-alt"
  }
}

interface SidecarResponse {
  ok: boolean
  result?: unknown
  error?: string
}

const initialStatus: MonitorStatus = { brightness: 50, volume: 20, mute: false, input: "usbc" }

function isInput(value: unknown): value is InputSource {
  return typeof value === "string" && ["dp1", "hdmi1", "hdmi2", "usbc"].includes(value)
}

function percentage(value: number): number {
  if (!Number.isFinite(value)) throw new Error("显示器返回值无效")
  return Math.max(0, Math.min(100, value))
}

function validateProfile(value: unknown): MonitorProfile {
  if (!value || typeof value !== "object" || Array.isArray(value)) throw new Error("配置档必须是 JSON 对象")
  const profile = value as Partial<MonitorProfile>
  if (!profile.id || !/^[a-z0-9][a-z0-9-]{1,63}$/.test(profile.id) || !profile.name || profile.name.length > 80) {
    throw new Error("配置档名称或 ID 无效")
  }
  if (!profile.routes || controls.some((control) => {
    const route = profile.routes?.[control]
    return !Array.isArray(route) || !route.length || route.some((item) => !transportIds.includes(item))
  })) throw new Error("配置档 DDC/CI 承载路径无效")
  const requested = controls.flatMap((control) => profile.routes![control])
  if (requested.includes("usb-hid-ddc") && !profile.usbHid) throw new Error("配置档缺少 USB HID DDC/CI 映射")
  if (requested.includes("video-ddc") && !profile.ddc) throw new Error("配置档缺少视频链路 DDC/CI 映射")
  if (profile.match?.displayNamePattern) {
    if (profile.match.displayNamePattern.length > 120) throw new Error("显示器匹配规则过长")
    new RegExp(profile.match.displayNamePattern, "i")
  }
  if (profile.usbHid && controls.some((control) => {
    const opcode = profile.usbHid?.vcp?.[control]
    return !Number.isInteger(opcode) || opcode! < 0 || opcode! > 255
  })) throw new Error("DDC/CI VCP 映射无效")
  if (profile.usbHid && profile.usbHid.adapter !== "lg-monitor-controls-v1") throw new Error("配置档引用了未内置的 USB HID DDC/CI 适配器")
  if (profile.match?.usbHid && (!Number.isInteger(profile.match.usbHid.vendorId) || !Number.isInteger(profile.match.usbHid.productId) ||
      profile.match.usbHid.vendorId < 0 || profile.match.usbHid.vendorId > 65535 ||
      profile.match.usbHid.productId < 0 || profile.match.usbHid.productId > 65535)) throw new Error("USB HID 识别条件无效")
  return profile as MonitorProfile
}

function collectDisplayNames(value: unknown, result: string[] = []): string[] {
  if (Array.isArray(value)) for (const item of value) collectDisplayNames(item, result)
  else if (value && typeof value === "object") {
    for (const [key, item] of Object.entries(value)) {
      if ((key === "spdisplays_display-product-name" || key === "_name") && typeof item === "string" && !item.startsWith("Apple M")) result.push(item)
      collectDisplayNames(item, result)
    }
  }
  return result
}

export class MonitorController {
  private profiles: MonitorProfile[] = []
  private profile?: MonitorProfile
  private displayName = "未检测到显示器"
  private available = new Set<MonitorTransport>()
  private routes: Record<ControlName, MonitorTransport> = {
    brightness: "unavailable", volume: "unavailable", mute: "unavailable", input: "unavailable",
  }
  private detectedAt = 0
  private lastStatus: MonitorStatus = { ...initialStatus }
  private readonly trustedControls = new Set<ControlName>()
  private readonly routeFailures: Record<ControlName, number> = { brightness: 0, volume: 0, mute: 0, input: 0 }
  private operationQueue: Promise<void> = Promise.resolve()
  private controlSequence = 0

  constructor(
    private readonly display = "1",
    private readonly bundledProfiles = path.resolve("desktop/profiles"),
    private readonly userProfiles = path.resolve("profiles"),
    private readonly sidecarBinary = path.resolve("sidecar/target/release/azoria-ddc-sidecar"),
    private readonly logger?: LocalLogger,
  ) {}

  async initialize(): Promise<void> {
    await mkdir(this.userProfiles, { recursive: true, mode: 0o700 })
    const loaded = await Promise.all([this.loadDirectory(this.bundledProfiles), this.loadDirectory(this.userProfiles)])
    const unique = new Map<string, MonitorProfile>()
    for (const profile of loaded.flat()) unique.set(profile.id, profile)
    this.profiles = [...unique.values()]
    await this.detect(true)
  }

  private async loadDirectory(directory: string): Promise<MonitorProfile[]> {
    try {
      const profiles: MonitorProfile[] = []
      for (const name of (await readdir(directory)).filter((item) => item.endsWith(".json")).sort()) {
        try { profiles.push(validateProfile(JSON.parse(await readFile(path.join(directory, name), "utf8")))) }
        catch { /* An invalid optional profile must not disable built-in profiles. */ }
      }
      return profiles
    } catch { return [] }
  }

  async importProfile(sourcePath: string): Promise<MonitorConnectionInfo> {
    const profile = validateProfile(JSON.parse(await readFile(sourcePath, "utf8")))
    await mkdir(this.userProfiles, { recursive: true, mode: 0o700 })
    await writeFile(path.join(this.userProfiles, `${profile.id}.json`), JSON.stringify(profile, null, 2), { mode: 0o600 })
    await this.initialize()
    return this.connection(true)
  }

  private async command(binary: string, args: string[], timeout = 4000): Promise<string> {
    const { stdout } = await run(binary, args, {
      timeout, maxBuffer: 128 * 1024,
      env: { PATH: "/opt/homebrew/bin:/usr/local/bin:/usr/bin:/bin" },
    })
    return stdout.trim()
  }

  private async sidecar(request: Record<string, unknown>): Promise<unknown> {
    if (!existsSync(this.sidecarBinary)) throw new Error("显示器控制组件不可用")
    let output = ""
    try {
      output = await this.command(this.sidecarBinary, [JSON.stringify(request)], 4000)
    } catch (error) {
      output = String((error as { stdout?: string }).stdout || "").trim()
      if (!output) throw new Error("显示器控制组件执行失败")
    }
    let response: SidecarResponse
    try { response = JSON.parse(output) as SidecarResponse }
    catch { throw new Error("显示器控制组件响应无效") }
    if (!response.ok) throw new Error(response.error || "DDC/CI 操作失败")
    return response.result
  }

  private displayIndex(): number {
    const value = Number.parseInt(this.display, 10)
    return Number.isInteger(value) && value > 0 ? value : 1
  }

  private vcp(control: ControlName): number {
    if (control === "brightness") return 0x10
    if (control === "volume") return 0x62
    if (control === "mute") return 0x8d
    return 0x60
  }

  private async sidecarValue(request: Record<string, unknown>): Promise<Record<string, unknown>> {
    const result = await this.sidecar(request)
    if (!result || typeof result !== "object" || Array.isArray(result)) throw new Error("DDC/CI 响应无效")
    return result as Record<string, unknown>
  }

  private get(transport: "native-ddc" | "lg-hid-ddc", vcp: number): Promise<Record<string, unknown>> {
    return this.sidecarValue({ operation: "get", transport, display: this.displayIndex(), vcp })
  }

  private set(transport: "native-ddc" | "lg-hid-ddc", vcp: number, value: number): Promise<Record<string, unknown>> {
    return this.sidecarValue({ operation: "set", transport, display: this.displayIndex(), vcp, value })
  }

  private async detectDisplayName(): Promise<string> {
    try {
      const payload = JSON.parse(await this.command("system_profiler", ["SPDisplaysDataType", "-json"], 8000))
      return collectDisplayNames(payload)[0] || "外接显示器"
    } catch { return "外接显示器" }
  }

  private async probeHidDdc(): Promise<{ vendorId: number; productId: number } | undefined> {
    try {
      const result = await this.sidecarValue({ operation: "probe", transport: "lg-hid-ddc" })
      return { vendorId: Number(result.vendor_id), productId: Number(result.product_id) }
    } catch { return undefined }
  }

  private async probeVideoDdc(): Promise<boolean> {
    try {
      const result = await this.get("native-ddc", 0x10)
      return Number.isFinite(Number(result.current))
    } catch { return false }
  }

  private async detect(force = false): Promise<void> {
    if (!force && this.profile && Date.now() - this.detectedAt < 5000) return
    this.detectedAt = Date.now()
    const [name, hid, videoDdc] = await Promise.all([this.detectDisplayName(), this.probeHidDdc(), this.probeVideoDdc()])
    this.displayName = name
    this.profile = this.profiles.find((candidate) => !candidate.fallback && (
      (hid && candidate.match?.usbHid?.vendorId === hid.vendorId && candidate.match.usbHid.productId === hid.productId) ||
      (candidate.match?.displayNamePattern && new RegExp(candidate.match.displayNamePattern, "i").test(name))
    )) || this.profiles.find((candidate) => candidate.fallback) || this.profiles[0]
    if (!this.profile) throw new Error("没有可用的显示器配置档")
    const profileAcceptsHid = Boolean(hid && this.profile.usbHid?.adapter === "lg-monitor-controls-v1" &&
      this.profile.match?.usbHid?.vendorId === hid.vendorId && this.profile.match.usbHid.productId === hid.productId)
    this.available = new Set<MonitorTransport>([
      ...(profileAcceptsHid ? ["usb-hid-ddc" as const] : []),
      ...(videoDdc ? ["video-ddc" as const] : []),
    ])
    for (const control of controls) {
      this.routes[control] = this.profile.routes[control].find((item) => this.available.has(item)) || "unavailable"
    }
    if (!this.available.size) this.displayName = "未检测到显示器"
  }

  private label(transport: MonitorTransport): string {
    if (transport === "usb-hid-ddc") return "USB HID → DDC/CI"
    if (transport === "video-ddc") return "视频链路 → DDC/CI"
    return "不可用"
  }

  async connection(force = false): Promise<MonitorConnectionInfo> {
    await this.detect(force)
    const active = [...new Set(controls.map((control) => this.routes[control]).filter((item) => item !== "unavailable"))]
    return {
      displayName: this.displayName,
      profileId: this.profile!.id,
      profileName: this.profile!.name,
      summary: active.length ? active.map((item) => this.label(item)).join(" + ") : "未连接",
      availableTransports: [...this.available],
      routes: { ...this.routes },
    }
  }

  private candidates(control: ControlName): MonitorTransport[] {
    const route = this.profile?.routes[control] || []
    const ordered = [this.routes[control], ...route]
    return ordered.filter((item, index) => item !== "unavailable" && this.available.has(item) && ordered.indexOf(item) === index)
  }

  private enqueue<T>(operation: () => Promise<T>): Promise<T> {
    const result = this.operationQueue.then(operation, operation)
    this.operationQueue = result.then(() => undefined, () => undefined)
    return result
  }

  private valuesMatch(control: ControlName, left: number | boolean | InputSource, right: number | boolean | InputSource): boolean {
    if ((control === "brightness" || control === "volume") && typeof left === "number" && typeof right === "number") return Math.abs(left - right) <= 1
    return left === right
  }

  private async read(control: ControlName, transport: MonitorTransport): Promise<number | boolean | InputSource> {
    if (transport === "video-ddc") {
      const result = await this.get("native-ddc", this.vcp(control))
      const current = Number(result.current)
      if (control === "brightness" || control === "volume") return percentage(current)
      if (control === "mute") return current === 1
      const raw = String(current)
      return this.profile?.ddc?.inputReadValues[raw] || this.lastStatus.input
    }
    if (transport === "usb-hid-ddc" && this.profile?.usbHid) {
      const result = await this.get("lg-hid-ddc", this.profile.usbHid.vcp[control])
      const current = Number(result.current)
      if (control === "brightness" || control === "volume") return percentage(current)
      if (control === "mute") return current === 1
      return Object.entries(this.profile.usbHid.inputValues).find(([, code]) => code === current)?.[0] as InputSource || this.lastStatus.input
    }
    throw new Error("DDC/CI 承载路径不可用")
  }

  private async readWithFallback(control: ControlName): Promise<number | boolean | InputSource> {
    let lastError: unknown
    const current = this.routes[control]
    if (current !== "unavailable" && this.available.has(current)) {
      try {
        const value = await this.read(control, current)
        this.routeFailures[control] = 0
        return value
      } catch (error) {
        lastError = error
        this.routeFailures[control]++
        if (this.routeFailures[control] < 3) throw error
      }
    }
    for (const transport of this.candidates(control).filter((item) => item !== current)) {
      try {
        const value = await this.read(control, transport)
        this.routes[control] = transport
        this.routeFailures[control] = 0
        return value
      } catch (error) { lastError = error }
    }
    throw lastError instanceof Error ? lastError : new Error(`${control} 没有可用的 DDC/CI 路径`)
  }

  private async readStable(control: ControlName): Promise<number | boolean | InputSource> {
    const first = await this.readWithFallback(control)
    const previous = this.lastStatus[control]
    if (this.trustedControls.has(control) && this.valuesMatch(control, first, previous)) return first
    await new Promise((resolve) => setTimeout(resolve, 80))
    const second = await this.readWithFallback(control)
    if (!this.valuesMatch(control, first, second)) throw new Error(`${control} 状态读取不稳定`)
    this.trustedControls.add(control)
    return second
  }

  private async readStatus(): Promise<MonitorStatus> {
    await this.detect()
    let success = 0
    for (const control of controls) {
      try {
        const value = await this.readStable(control)
        if (control === "brightness" && typeof value === "number") this.lastStatus.brightness = value
        else if (control === "volume" && typeof value === "number") this.lastStatus.volume = value
        else if (control === "mute" && typeof value === "boolean") this.lastStatus.mute = value
        else if (control === "input" && isInput(value)) this.lastStatus.input = value
        success++
      } catch { /* A display can expose only a subset of DDC/CI VCP features. */ }
    }
    if (!success) {
      await this.detect(true)
      throw new Error("未检测到可用的 DDC/CI 连接")
    }
    return { ...this.lastStatus }
  }

  status(): Promise<MonitorStatus> {
    return this.enqueue(() => this.readStatus())
  }

  private async checkReachable(): Promise<boolean> {
    await this.detect(true)
    for (const transport of this.candidates("brightness")) {
      try {
        const first = await this.read("brightness", transport)
        await new Promise((resolve) => setTimeout(resolve, 80))
        const second = await this.read("brightness", transport)
        if (!this.valuesMatch("brightness", first, second)) continue
        this.routes.brightness = transport
        return true
      } catch { /* Only the Desktop on the active display input is eligible. */ }
    }
    return false
  }

  reachable(): Promise<boolean> {
    return this.enqueue(() => this.checkReachable())
  }

  private async write(control: ControlName, value: ControlRequest["value"], transport: MonitorTransport): Promise<void> {
    if (transport === "video-ddc" && this.profile?.ddc) {
      if ((control === "brightness" || control === "volume") && typeof value === "number") await this.set("native-ddc", this.vcp(control), value)
      else if (control === "mute" && typeof value === "boolean") await this.set("native-ddc", this.vcp(control), value ? 1 : 2)
      else if (control === "input" && isInput(value)) await this.set("native-ddc", this.vcp(control), Number(this.profile.ddc.inputWriteValues[value]))
      else throw new Error("DDC/CI 控制值无效")
      return
    }
    if (transport === "usb-hid-ddc" && this.profile?.usbHid) {
      if (control === "input" && isInput(value) && this.profile.usbHid.inputWriteMode === "vendor-private") {
        await this.sidecar({ operation: "input", transport: "lg-hid-ddc", source: value })
      }
      else if (control === "input" && isInput(value)) await this.set("lg-hid-ddc", this.profile.usbHid.vcp.input, this.profile.usbHid.inputValues[value])
      else if (control === "mute" && typeof value === "boolean") await this.set("lg-hid-ddc", this.profile.usbHid.vcp.mute, value ? 1 : 2)
      else if ((control === "brightness" || control === "volume") && typeof value === "number") await this.set("lg-hid-ddc", this.profile.usbHid.vcp[control], value)
      else throw new Error("DDC/CI 控制值无效")
      return
    }
    throw new Error("DDC/CI 承载路径不可用")
  }

  private async applyControl(request: ControlRequest, source: ControlSource): Promise<MonitorStatus> {
    const { control, value } = request
    if ((control === "brightness" || control === "volume") && (typeof value !== "number" || !Number.isInteger(value) || value < 0 || value > 100)) throw new Error("数值控制必须是 0–100 的整数")
    if (control === "mute" && typeof value !== "boolean") throw new Error("静音值无效")
    if (control === "input" && !isInput(value)) throw new Error("输入源无效")
    const operation = ++this.controlSequence
    const startedAt = Date.now()
    this.logger?.info("control.request", {
      operation, source, control, requested: String(value), final: request.final !== false,
      profile: this.profile?.id,
    })
    try {
      await this.detect()
    } catch (error) {
      const failure = error instanceof Error ? error : new Error("显示器检测失败")
      this.logger?.error("control.failed", {
        operation, source, control, requested: String(value), stage: "detection",
        durationMs: Date.now() - startedAt, error: failure.message,
      })
      throw failure
    }
    let lastError: unknown
    for (const transport of this.candidates(control)) {
      const routeStartedAt = Date.now()
      this.logger?.info("control.route_attempt", {
        operation, control, transport, display: this.display, profile: this.profile?.id,
      })
      try {
        await this.write(control, value, transport)
        this.routes[control] = transport
        let confirmed = value
        let verification: "skipped" | "matched" | "unavailable" | "mismatch" = request.final === false ? "skipped" : "unavailable"
        if (request.final !== false) {
          try {
            const first = await this.read(control, transport)
            if (this.valuesMatch(control, first, value)) {
              confirmed = first
              verification = "matched"
            }
            else {
              await new Promise((resolve) => setTimeout(resolve, 80))
              const second = await this.read(control, transport)
              if (this.valuesMatch(control, first, second) && this.valuesMatch(control, second, value)) {
                confirmed = second
                verification = "matched"
              } else {
                verification = "mismatch"
                this.logger?.warn("control.readback_mismatch", {
                  operation, control, transport, requested: String(value), first: String(first), second: String(second),
                })
              }
            }
          }
          catch (error) {
            this.logger?.warn("control.readback_failed", {
              operation, control, transport, error: error instanceof Error ? error.message : "unknown",
            })
          }
        }
        if (control === "brightness" && typeof confirmed === "number") this.lastStatus.brightness = confirmed
        else if (control === "volume" && typeof confirmed === "number") this.lastStatus.volume = confirmed
        else if (control === "mute" && typeof confirmed === "boolean") this.lastStatus.mute = confirmed
        else if (control === "input" && isInput(confirmed)) this.lastStatus.input = confirmed
        this.trustedControls.add(control)
        this.routeFailures[control] = 0
        this.logger?.info("control.success", {
          operation, source, control, requested: String(value), confirmed: String(confirmed), transport,
          display: this.display, profile: this.profile?.id, verification,
          routeDurationMs: Date.now() - routeStartedAt, durationMs: Date.now() - startedAt,
        })
        return { ...this.lastStatus }
      } catch (error) {
        lastError = error
        this.logger?.warn("control.route_failed", {
          operation, control, transport, durationMs: Date.now() - routeStartedAt,
          error: error instanceof Error ? error.message : "unknown",
        })
      }
    }
    const failure = lastError instanceof Error ? lastError : new Error("没有可用的 DDC/CI 承载路径")
    this.logger?.error("control.failed", {
      operation, source, control, requested: String(value), durationMs: Date.now() - startedAt, error: failure.message,
    })
    throw failure
  }

  control(request: ControlRequest, source: ControlSource = "internal"): Promise<MonitorStatus> {
    return this.enqueue(() => this.applyControl(request, source))
  }
}
