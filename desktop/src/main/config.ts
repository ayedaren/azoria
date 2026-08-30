import { randomBytes } from "node:crypto"
import { readFile, writeFile } from "node:fs/promises"
import path from "node:path"

export interface AppConfig {
  token: string
  display: string
  desktopId: string
}

export async function loadConfig(userData: string): Promise<AppConfig> {
  const configPath = path.join(userData, "config.json")
  try {
    const parsed = JSON.parse(await readFile(configPath, "utf8")) as Partial<AppConfig>
    if (typeof parsed.token === "string" && parsed.token.length >= 20) {
      const config = {
        token: parsed.token,
        display: parsed.display || "1",
        desktopId: typeof parsed.desktopId === "string" && /^[0-9a-f]{32}$/i.test(parsed.desktopId)
          ? parsed.desktopId.toLowerCase()
          : randomBytes(16).toString("hex"),
      }
      if (config.desktopId !== parsed.desktopId) {
        await writeFile(configPath, JSON.stringify(config, null, 2), { mode: 0o600 })
      }
      return config
    }
  } catch {
    // First launch creates a device-local secret below.
  }
  const config = { token: randomBytes(24).toString("hex"), display: "1", desktopId: randomBytes(16).toString("hex") }
  await writeFile(configPath, JSON.stringify(config, null, 2), { mode: 0o600 })
  return config
}
