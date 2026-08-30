import { appendFile, mkdir, rename, stat, unlink } from "node:fs/promises"
import { homedir } from "node:os"
import path from "node:path"

const maxFileBytes = 2 * 1024 * 1024
const retainedFiles = 3

export type LogFields = Record<string, string | number | boolean | null | undefined>
type LogLevel = "info" | "warn" | "error"

export class LocalLogger {
  private readonly file: string
  private queue: Promise<void> = Promise.resolve()

  constructor(private readonly directory: string) {
    this.file = path.join(directory, "azoria-desktop.jsonl")
  }

  async initialize(): Promise<void> {
    await mkdir(this.directory, { recursive: true, mode: 0o700 })
    await this.rotateIfNeeded(0)
  }

  info(event: string, fields: LogFields = {}): void {
    this.write("info", event, fields)
  }

  warn(event: string, fields: LogFields = {}): void {
    this.write("warn", event, fields)
  }

  error(event: string, fields: LogFields = {}): void {
    this.write("error", event, fields)
  }

  private write(level: LogLevel, event: string, fields: LogFields): void {
    const record: Record<string, string | number | boolean | null> = {
      timestamp: new Date().toISOString(),
      level,
      event,
    }
    for (const [key, value] of Object.entries(fields)) {
      if (value === undefined) continue
      record[key] = typeof value === "string" ? this.sanitize(value) : value
    }
    const line = `${JSON.stringify(record)}\n`
    this.queue = this.queue.then(async () => {
      await this.rotateIfNeeded(Buffer.byteLength(line))
      await appendFile(this.file, line, { encoding: "utf8", mode: 0o600 })
    }).catch(() => undefined)
  }

  private sanitize(value: string): string {
    const home = homedir()
    const withoutHome = home && value.includes(home) ? value.split(home).join("<home>") : value
    return withoutHome.replace(/[\r\n\t]+/g, " ").slice(0, 320)
  }

  private async rotateIfNeeded(incomingBytes: number): Promise<void> {
    let size = 0
    try { size = (await stat(this.file)).size }
    catch { return }
    if (size + incomingBytes <= maxFileBytes) return
    for (let index = retainedFiles; index >= 1; index--) {
      const destination = `${this.file}.${index}`
      const source = index === 1 ? this.file : `${this.file}.${index - 1}`
      try { await unlink(destination) } catch { /* Missing rotations are expected. */ }
      try { await rename(source, destination) } catch { /* Missing rotations are expected. */ }
    }
  }
}
