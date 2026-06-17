/** 浏览器本地持久化：候选 RTSP 地址列表（多行文本） */
export const RTSP_URLS_LS_KEY = 'webui_rtsp_candidate_urls'
/** 各 URL 上次「检测可用性」在服务器端解码的结果 */
export const RTSP_PROBE_CACHE_LS_KEY = 'webui_rtsp_probe_playable'

/** 同一 RTSP URL 在设备侧允许的最大并发客户端数（单机摄像头常见为 1） */
export const RTSP_MAX_CONCURRENT_PER_URL = 1
/** 与后端 RTSP 探测一致：服务器上对每路 ffmpeg 解码秒数 */
export const RTSP_DECODE_PROBE_SECONDS = 10

export function normalizeRtspUrl(p: string): string {
  return p.trim()
}

export function parseRtspLines(text: string): string[] {
  const out: string[] = []
  const seen = new Set<string>()
  for (const line of text.split(/\r?\n/)) {
    const u = line.trim()
    if (!u) continue
    if (seen.has(u)) continue
    seen.add(u)
    out.push(u)
  }
  return out
}

export function loadRtspUrlsFromStorage(): string {
  try {
    return localStorage.getItem(RTSP_URLS_LS_KEY) || ''
  } catch {
    return ''
  }
}

export function saveRtspUrlsToStorage(text: string): void {
  try {
    localStorage.setItem(RTSP_URLS_LS_KEY, text)
  } catch {
    /* ignore */
  }
}

export function loadRtspProbeCacheFromStorage(): Record<string, boolean | undefined> {
  try {
    const raw = localStorage.getItem(RTSP_PROBE_CACHE_LS_KEY)
    if (!raw) return {}
    const parsed = JSON.parse(raw) as Record<string, boolean>
    const next: Record<string, boolean | undefined> = {}
    for (const [k, v] of Object.entries(parsed)) {
      if (v === true || v === false) {
        next[normalizeRtspUrl(k)] = v
      }
    }
    return next
  } catch {
    return {}
  }
}

export function saveRtspProbeCacheToStorage(
  map: Record<string, boolean | undefined>
): void {
  try {
    const clean: Record<string, boolean> = {}
    for (const [k, v] of Object.entries(map)) {
      if (v === true || v === false) clean[k] = v
    }
    localStorage.setItem(RTSP_PROBE_CACHE_LS_KEY, JSON.stringify(clean))
  } catch {
    /* ignore */
  }
}
