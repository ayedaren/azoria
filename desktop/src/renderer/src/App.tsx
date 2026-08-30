import { useCallback, useEffect, useMemo, useRef, useState } from "react"
import { Bluetooth, Cable, Check, FileUp, Radar, RefreshCw, Router, Settings, ShieldCheck, Sun, Volume2, VolumeX, Zap } from "lucide-react"
import type { ControlName, FirmwareImage, InputSource, LanDevice, MonitorConnectionInfo, MonitorStatus, MonitorTransport, UsbDevice } from "../../shared/contracts"
import { connectBle } from "./ble"
import { AzoriaDesktopBrand } from "./components/brand"
import { AlertDialog, AlertDialogAction, AlertDialogCancel, AlertDialogContent, AlertDialogDescription, AlertDialogFooter, AlertDialogHeader, AlertDialogTitle, AlertDialogTrigger } from "@/components/ui/alert-dialog"
import { Badge } from "@/components/ui/badge"
import { Button } from "@/components/ui/button"
import { Card, CardContent, CardDescription, CardHeader, CardTitle } from "@/components/ui/card"
import { Dialog, DialogContent, DialogHeader, DialogTitle, DialogTrigger } from "@/components/ui/dialog"
import { Label } from "@/components/ui/label"
import { Select, SelectContent, SelectItem, SelectTrigger, SelectValue } from "@/components/ui/select"
import { Separator } from "@/components/ui/separator"
import { Slider } from "@/components/ui/slider"
import { Switch } from "@/components/ui/switch"
import { Tabs, TabsContent, TabsList, TabsTrigger } from "@/components/ui/tabs"

const emptyStatus: MonitorStatus = { brightness: 50, volume: 20, mute: false, input: "usbc" }
const emptyConnection: MonitorConnectionInfo = { displayName: "未检测到显示器", profileId: "generic-ddc", profileName: "通用 DDC/CI 显示器", summary: "未连接", availableTransports: [], routes: { brightness: "unavailable", volume: "unavailable", mute: "unavailable", input: "unavailable" } }
const inputs: Array<{ value: InputSource; label: string }> = [
  { value: "dp1", label: "DisplayPort" }, { value: "hdmi1", label: "HDMI 1" },
  { value: "hdmi2", label: "HDMI 2" }, { value: "usbc", label: "USB-C" },
]

function MetricSlider({ icon, label, value, disabled, onCommit, onEditingChange }: { icon: React.ReactNode; label: string; value: number; disabled: boolean; onCommit(value: number): void; onEditingChange(editing: boolean): void }) {
  const [local, setLocal] = useState(value)
  useEffect(() => setLocal(value), [value])
  return <div className="grid grid-cols-[120px_1fr_48px] items-center gap-5 py-6">
    <div className="flex items-center gap-3 text-sm text-zinc-300">{icon}<span>{label}</span></div>
    <Slider value={[local]} max={100} step={1} disabled={disabled} onValueChange={(next) => { onEditingChange(true); setLocal(next[0] ?? 0) }} onValueCommit={(next) => { onEditingChange(false); onCommit(next[0] ?? 0) }} />
    <span className="text-right font-mono text-sm tabular-nums text-white">{local}</span>
  </div>
}

function transportLabel(transport: MonitorTransport) {
  if (transport === "usb-hid-ddc") return "USB HID → DDC/CI"
  if (transport === "video-ddc") return "视频链路 → DDC/CI"
  return "不可用"
}

export default function App() {
  const [tab, setTab] = useState("control")
  const [developerMode, setDeveloperMode] = useState(() => localStorage.getItem("azoria.developerMode") === "1")
  const [status, setStatus] = useState(emptyStatus)
  const [connection, setConnection] = useState(emptyConnection)
  const [online, setOnline] = useState(false)
  const [busy, setBusy] = useState(false)
  const [pendingControls, setPendingControls] = useState<Set<ControlName>>(() => new Set())
  const editingRef = useRef(false)
  const pendingCountRef = useRef(0)
  const initializedRef = useRef(false)
  const [message, setMessage] = useState("正在连接显示器")
  const [usb, setUsb] = useState<UsbDevice[]>([])
  const [lanDevices, setLanDevices] = useState<LanDevice[]>([])
  const [selectedUsb, setSelectedUsb] = useState("")
  const [networks, setNetworks] = useState<Array<{ ssid: string; rssi: number; secure: boolean }>>([])
  const [ssid, setSsid] = useState("")
  const [password, setPassword] = useState("")
  const [bleName, setBleName] = useState("")
  const [bleConnecting, setBleConnecting] = useState(false)
  const [verifiedChip, setVerifiedChip] = useState("")
  const [firmware, setFirmware] = useState<FirmwareImage | null>(null)

  useEffect(() => {
    localStorage.setItem("azoria.developerMode", developerMode ? "1" : "0")
    if (!developerMode && tab === "developer") setTab("touch")
  }, [developerMode, tab])

  const refresh = useCallback(async () => {
    try { setStatus(await window.azoria.monitor.status()); setOnline(true); setMessage("显示器已连接") }
    catch (error) { setOnline(false); setMessage(error instanceof Error ? error.message : "显示器未连接") }
    try { setConnection(await window.azoria.monitor.connection()) } catch { /* Keep the last detected route visible. */ }
  }, [])
  const detectUsb = useCallback(async () => {
    const devices = await window.azoria.device.listUsb(); setUsb(devices)
    setSelectedUsb((current) => devices.some((device) => device.path === current) ? current : devices[0]?.path || "")
    setVerifiedChip("")
  }, [])
  const discoverLan = useCallback(async () => {
    setBusy(true)
    try {
      const devices = await window.azoria.device.discoverLan()
      setLanDevices(devices)
      setMessage(devices.length ? `已连接 ${devices.length} 台 AZORIA Touch` : "局域网内未发现 AZORIA Touch")
    } catch (error) {
      setMessage(error instanceof Error ? error.message : "局域网搜索失败")
    } finally {
      setBusy(false)
    }
  }, [])
  useEffect(() => {
    if (!initializedRef.current) {
      initializedRef.current = true
      void refresh()
      void detectUsb()
      void discoverLan()
    }
    const timer = window.setInterval(() => {
      if (!editingRef.current && pendingCountRef.current === 0) void refresh()
    }, 30000)
    return () => window.clearInterval(timer)
  }, [refresh, detectUsb, discoverLan])

  const setControl = async (control: ControlName, value: number | boolean | InputSource) => {
    pendingCountRef.current++
    setPendingControls((current) => new Set(current).add(control))
    try {
      const next = await window.azoria.monitor.control({ control, value, final: true })
      setStatus((current) => ({ ...current, [control]: next[control] }))
      setOnline(true)
      setMessage("已保存到显示器")
    }
    catch (error) { setMessage(error instanceof Error ? error.message : "控制失败") }
    finally {
      pendingCountRef.current = Math.max(0, pendingCountRef.current - 1)
      setPendingControls((current) => { const next = new Set(current); next.delete(control); return next })
    }
  }
  const selectedDevice = useMemo(() => usb.find((device) => device.path === selectedUsb), [usb, selectedUsb])
  const scanWifi = () => { setBusy(true); void window.azoria.device.scanWifi(selectedUsb).then(setNetworks).catch((error) => setMessage(String(error))).finally(() => setBusy(false)) }
  const saveWifi = () => { setBusy(true); void window.azoria.device.configureWifi(selectedUsb, ssid, password).then(() => { setPassword(""); setMessage("AZORIA Touch Wi‑Fi 配置完成") }).catch((error) => setMessage(String(error))).finally(() => setBusy(false)) }
  const connectBluetooth = () => {
    setBleConnecting(true)
    void connectBle(
      () => window.azoria.monitor.relayStatus(),
      (request, nonce, commandId) => window.azoria.monitor.relayControl(request, nonce, commandId),
    ).then((name) => {
      setBleName(name)
      setMessage("AZORIA Touch 蓝牙已连接")
    }).catch((error) => setMessage(error instanceof Error ? error.message : "蓝牙连接失败"))
      .finally(() => setBleConnecting(false))
  }
  const importProfile = () => { void window.azoria.monitor.importProfile().then((result) => { if (result) { setConnection(result); setMessage(`已加载配置表：${result.profileName}`); void refresh() } }).catch((error) => setMessage(error instanceof Error ? error.message : "配置表加载失败")) }
  const selectFirmware = () => {
    void window.azoria.device.selectFirmware().then((selected) => {
      if (selected) {
        setFirmware(selected)
        setMessage(`已选择固件 ${selected.version}`)
      }
    }).catch((error) => setMessage(error instanceof Error ? error.message : "固件选择失败"))
  }

  return <div className="min-h-screen bg-black text-white selection:bg-white selection:text-black">
    <header className="drag-region flex h-20 items-center justify-between border-b border-white/10 px-8 pl-24">
      <AzoriaDesktopBrand />
      <div className="no-drag flex items-center gap-3">
        <Badge variant="outline" className="h-8 gap-2 border-white/10 bg-zinc-950 px-3 font-normal text-zinc-300"><span className={`h-1.5 w-1.5 rounded-full ${online ? "bg-white" : "bg-zinc-700"}`} />{message}</Badge>
        <Button variant="outline" size="icon" className="border-white/10 bg-black" onClick={() => void refresh()}><RefreshCw /></Button>
        <Dialog><DialogTrigger asChild><Button variant="outline" size="icon" className="border-white/10 bg-black"><Settings /></Button></DialogTrigger><DialogContent><DialogHeader><DialogTitle>AZORIA Desktop 设置</DialogTitle></DialogHeader><div className="flex items-center justify-between rounded-lg border border-white/10 p-4"><Label htmlFor="developer-mode">开发者模式</Label><Switch id="developer-mode" checked={developerMode} onCheckedChange={setDeveloperMode} /></div><div className="flex items-center justify-between rounded-lg border border-white/10 p-4"><Label>显示器配置表</Label><Button variant="outline" onClick={importProfile}><FileUp />加载</Button></div></DialogContent></Dialog>
      </div>
    </header>

    <main className="mx-auto max-w-[1180px] px-8 py-8">
      <Tabs value={tab} onValueChange={setTab}>
        <TabsList className="mb-6 bg-zinc-950">
          <TabsTrigger value="control">显示器</TabsTrigger>
          <TabsTrigger value="touch">AZORIA Touch</TabsTrigger>
          {developerMode && <TabsTrigger value="developer">开发者</TabsTrigger>}
        </TabsList>

        <TabsContent value="control" className="grid gap-5 lg:grid-cols-[1.45fr_.7fr]">
          <Card><CardHeader><CardTitle>显示器控制</CardTitle><CardDescription>{connection.displayName}</CardDescription></CardHeader><CardContent><MetricSlider icon={<Sun />} label="亮度" value={status.brightness} disabled={pendingControls.has("brightness") || !online} onEditingChange={(editing) => { editingRef.current = editing }} onCommit={(value) => void setControl("brightness", value)} /><Separator /><MetricSlider icon={<Volume2 />} label="音量" value={status.volume} disabled={pendingControls.has("volume") || !online} onEditingChange={(editing) => { editingRef.current = editing }} onCommit={(value) => void setControl("volume", value)} /></CardContent></Card>
          <div className="grid gap-5"><Card><CardHeader><CardTitle>输入源</CardTitle></CardHeader><CardContent className="grid grid-cols-2 gap-2">{inputs.map((input) => <Button key={input.value} variant={status.input === input.value ? "default" : "outline"} className="h-12" disabled={pendingControls.has("input") || !online} onClick={() => void setControl("input", input.value)}>{status.input === input.value && <Check />}{input.label}</Button>)}</CardContent></Card><Card><CardContent className="pt-6"><Button variant={status.mute ? "default" : "outline"} className="h-12 w-full" disabled={pendingControls.has("mute") || !online} onClick={() => void setControl("mute", !status.mute)}>{status.mute ? <VolumeX /> : <Volume2 />}{status.mute ? "取消静音" : "静音"}</Button></CardContent></Card></div>
        </TabsContent>

        <TabsContent value="touch" className="grid gap-5 lg:grid-cols-2">
          <Card><CardHeader><CardTitle>AZORIA Touch</CardTitle></CardHeader><CardContent className="space-y-4">{lanDevices.length ? <div className="space-y-2">{lanDevices.map((device) => <div key={device.id} className="rounded-lg border border-white/10 bg-black p-5"><div className="flex items-center justify-between"><p className="font-medium">{device.name}</p><Badge variant="outline" className="border-white/10"><Check />已连接</Badge></div></div>)}</div> : selectedDevice?.verified ? <div className="rounded-lg border border-white/10 bg-black p-5"><div className="flex items-center justify-between"><div><p className="font-medium">{selectedDevice.name}</p><p className="mt-1 text-sm text-zinc-500">USB 已连接</p></div><Badge variant="outline" className="border-white/10"><Check />已识别</Badge></div></div> : <div className="rounded-lg border border-dashed border-white/10 p-8 text-center text-sm text-zinc-500">未发现 AZORIA Touch</div>}<div className="grid grid-cols-2 gap-2"><Button variant="outline" disabled={busy} onClick={() => void discoverLan()}><Radar />搜索局域网</Button><Button variant="outline" disabled={bleConnecting} onClick={connectBluetooth}><Bluetooth />{bleName ? "蓝牙已连接" : bleConnecting ? "正在连接" : "连接蓝牙"}</Button></div></CardContent></Card>
          <Card><CardHeader><CardTitle className="flex items-center gap-2"><Router />Wi‑Fi 配置</CardTitle></CardHeader><CardContent className="space-y-3"><Button variant="outline" className="w-full" disabled={!selectedDevice?.verified} onClick={scanWifi}>扫描 2.4 GHz 网络</Button><Select value={ssid} onValueChange={setSsid}><SelectTrigger><SelectValue placeholder="选择 Wi‑Fi" /></SelectTrigger><SelectContent>{networks.map((network) => <SelectItem key={network.ssid} value={network.ssid}>{network.secure ? "加密 · " : "开放 · "}{network.ssid} · {network.rssi} dBm</SelectItem>)}</SelectContent></Select><input className="h-10 w-full rounded-md border border-white/10 bg-black px-3 text-sm outline-none focus:border-white/30" type="password" value={password} onChange={(event) => setPassword(event.target.value)} placeholder="Wi‑Fi 密码" /><Button className="w-full" disabled={!selectedDevice?.verified || !ssid || busy} onClick={saveWifi}>保存并连接</Button></CardContent></Card>
        </TabsContent>

        {developerMode && <TabsContent value="developer" className="grid gap-5 lg:grid-cols-2">
          <Card><CardHeader><CardTitle className="flex items-center gap-2"><Cable />固件刷写</CardTitle></CardHeader><CardContent className="space-y-4">{usb.length > 1 && <Select value={selectedUsb} onValueChange={(value) => { setSelectedUsb(value); setVerifiedChip("") }}><SelectTrigger><SelectValue /></SelectTrigger><SelectContent>{usb.map((device) => <SelectItem key={device.path} value={device.path}>{device.name}</SelectItem>)}</SelectContent></Select>}{selectedDevice ? <div className="rounded-lg border border-white/10 bg-black p-4 text-sm text-zinc-400"><div className="flex items-center justify-between gap-3"><p className="text-white">{selectedDevice.name}</p><Badge variant="outline" className="border-white/10">{verifiedChip || (selectedDevice.verified ? "USB 身份已验证" : "ESP32-S3 候选设备")}</Badge></div><p className="mt-2 font-mono text-xs">{selectedDevice.path}</p></div> : <div className="rounded-lg border border-dashed border-white/10 p-8 text-center text-sm text-zinc-500">没有可刷写的 AZORIA Touch</div>}<Button variant="outline" className="w-full" onClick={selectFirmware}><FileUp />{firmware ? `${firmware.name} · ${firmware.version}` : "选择固件"}</Button><AlertDialog><AlertDialogTrigger asChild><Button className="w-full" disabled={!selectedUsb || !firmware || busy}><Zap />刷写固件</Button></AlertDialogTrigger><AlertDialogContent><AlertDialogHeader><AlertDialogTitle>刷写 AZORIA Touch {firmware?.version}？</AlertDialogTitle><AlertDialogDescription>目标 {verifiedChip || "ESP32-S3"}，固件校验通过后写入。</AlertDialogDescription></AlertDialogHeader><AlertDialogFooter><AlertDialogCancel>取消</AlertDialogCancel><AlertDialogAction onClick={() => { if (!firmware) return; setBusy(true); void window.azoria.device.verifyUsb(selectedUsb).then((identity) => { setVerifiedChip(identity.chip); return window.azoria.device.flash(selectedUsb, firmware.path, firmware.sha256) }).then(() => { setMessage("固件刷写完成"); void detectUsb(); void discoverLan() }).catch((error) => setMessage(error instanceof Error ? error.message : "身份校验或刷写失败")).finally(() => setBusy(false)) }}>校验并开始刷写</AlertDialogAction></AlertDialogFooter></AlertDialogContent></AlertDialog></CardContent></Card>
          <Card><CardHeader><CardTitle className="flex items-center gap-2"><ShieldCheck />开发诊断</CardTitle></CardHeader><CardContent className="space-y-3 text-sm"><div className="flex justify-between rounded-lg border border-white/10 p-4"><span className="text-zinc-400">显示器配置表</span><span>{connection.profileName}</span></div>{([['亮度','brightness'],['音量','volume'],['静音','mute'],['输入源','input']] as const).map(([label, control]) => <div key={control} className="flex justify-between rounded-lg border border-white/10 p-4"><span className="text-zinc-400">{label}路径</span><span>{transportLabel(connection.routes[control])}</span></div>)}<div className="flex justify-between rounded-lg border border-white/10 p-4"><span className="text-zinc-400">USB 候选过滤</span><span>Espressif + MAC</span></div><div className="flex justify-between rounded-lg border border-white/10 p-4"><span className="text-zinc-400">刷写前校验</span><span>ESP32-S3 + MAC 一致</span></div><div className="flex justify-between rounded-lg border border-white/10 p-4"><span className="text-zinc-400">公网访问</span><span>已阻止</span></div></CardContent></Card>
        </TabsContent>}
      </Tabs>
    </main>
  </div>
}
