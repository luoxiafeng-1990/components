import { spawn, spawnSync } from 'node:child_process'
import { existsSync, readFileSync, writeFileSync } from 'node:fs'
import type { IncomingMessage, ServerResponse } from 'node:http'
import { defineConfig, type Plugin } from 'vite'
import vue from '@vitejs/plugin-vue'

const DEFAULT_VENDOR_SOURCE = [
  'deb http://172.16.1.193:6520/ubuntu noble main',
  'deb https://mirrors.aliyun.com/ubuntu-ports noble main universe',
].join('\n')

const VENDOR_SOURCE_PATH = '/etc/apt/sources.list'

type CpuSnapshot = { total: number; busy: number }

let prevCpu: CpuSnapshot | null = null
let prevCores: CpuSnapshot[] = []

function apiOk(data: unknown, message = 'success') {
  return JSON.stringify({ code: 0, message, data })
}

function apiError(message: string, code = 1) {
  return JSON.stringify({ code, message, data: null })
}

function sendJson(res: ServerResponse, statusCode: number, payload: string) {
  res.statusCode = statusCode
  res.setHeader('Content-Type', 'application/json; charset=utf-8')
  res.end(payload)
}

function runShell(command: string) {
  const result = spawnSync('bash', ['-lc', command], {
    encoding: 'utf-8',
    maxBuffer: 10 * 1024 * 1024,
  })
  return {
    stdout: (result.stdout || '').trim(),
    stderr: (result.stderr || '').trim(),
    status: result.status ?? 0,
  }
}

function readBody(req: IncomingMessage): Promise<string> {
  return new Promise((resolve, reject) => {
    const chunks: Buffer[] = []
    req.on('data', (chunk) => chunks.push(Buffer.from(chunk)))
    req.on('end', () => resolve(Buffer.concat(chunks).toString('utf-8')))
    req.on('error', reject)
  })
}

function parseMeminfo() {
  const raw = readFileSync('/proc/meminfo', 'utf-8')
  const map = new Map<string, number>()
  raw.split('\n').forEach((line) => {
    const match = line.match(/^([^:]+):\s+(\d+)/)
    if (match) map.set(match[1], Number(match[2]))
  })
  const total = map.get('MemTotal') || 0
  const free = map.get('MemFree') || 0
  const available = map.get('MemAvailable') || 0
  const buffers = map.get('Buffers') || 0
  const cached = map.get('Cached') || 0
  const used = total - available
  const usage = total > 0 ? (used / total) * 100 : 0

  return {
    total_mb: Math.round(total / 1024),
    used_mb: Math.round(used / 1024),
    free_mb: Math.round(free / 1024),
    available_mb: Math.round(available / 1024),
    buffers_mb: Math.round(buffers / 1024),
    cached_mb: Math.round(cached / 1024),
    usage_percent: Math.round(usage * 100) / 100,
  }
}

function parseCpuStats() {
  const raw = readFileSync('/proc/stat', 'utf-8')
  const lines = raw.split('\n').filter((line) => line.startsWith('cpu'))
  const parseLine = (line: string): CpuSnapshot => {
    const nums = line.trim().split(/\s+/).slice(1).map((v) => Number(v))
    const total = nums.reduce((sum, value) => sum + value, 0)
    const idle = (nums[3] || 0) + (nums[4] || 0)
    return { total, busy: total - idle }
  }

  return {
    total: parseLine(lines[0] || 'cpu 0 0 0 0 0 0 0 0'),
    cores: lines.slice(1).map(parseLine),
  }
}

function diffUsage(current: CpuSnapshot, previous: CpuSnapshot | null) {
  if (!previous) return 0
  const totalDelta = current.total - previous.total
  const busyDelta = current.busy - previous.busy
  if (totalDelta <= 0) return 0
  return Math.max(0, Math.min(100, (busyDelta / totalDelta) * 100))
}

function parseCpuUsage() {
  const current = parseCpuStats()
  const usage = diffUsage(current.total, prevCpu)
  const perCore = current.cores.map((core, index) => ({
    core: index,
    usage_percent: Math.round(diffUsage(core, prevCores[index] || null) * 100) / 100,
  }))

  prevCpu = current.total
  prevCores = current.cores

  return {
    usage_percent: Math.round(usage * 100) / 100,
    cores: current.cores.length,
    per_core: perCore,
  }
}

function parseFilesystem() {
  const { stdout: dfRaw } = runShell('df -BM --output=source,size,used,avail,pcent,target 2>/dev/null')
  const { stdout: lsblk } = runShell('lsblk -o NAME,SIZE,TYPE,MOUNTPOINT,FSTYPE 2>/dev/null')
  const partitions = dfRaw
    .split('\n')
    .slice(1)
    .map((line) => line.trim())
    .filter(Boolean)
    .map((line) => {
      const cols = line.split(/\s+/)
      return {
        device: cols[0] || '',
        size_mb: parseInt((cols[1] || '0').replace(/\D/g, ''), 10) || 0,
        used_mb: parseInt((cols[2] || '0').replace(/\D/g, ''), 10) || 0,
        avail_mb: parseInt((cols[3] || '0').replace(/\D/g, ''), 10) || 0,
        percent: parseInt((cols[4] || '0').replace(/\D/g, ''), 10) || 0,
        mount: cols.slice(5).join(' '),
      }
    })
    .filter((item) => item.device.startsWith('/dev/') || item.device === 'tmpfs' || item.device === 'overlay')

  return { partitions, lsblk }
}

function parseInfo() {
  const version = runShell('tps-version 2>/dev/null').stdout || '未安装 tps-version'
  const kernel = runShell('uname -r 2>/dev/null').stdout
  const arch = runShell('uname -m 2>/dev/null').stdout
  const hostname = runShell('hostname 2>/dev/null').stdout
  const boardModel = runShell('cat /sys/firmware/devicetree/base/model 2>/dev/null').stdout
  const cpuModel =
    runShell("grep -m1 'model name' /proc/cpuinfo 2>/dev/null | cut -d: -f2 | xargs").stdout ||
    runShell("grep -m1 'isa' /proc/cpuinfo 2>/dev/null | cut -d: -f2 | xargs").stdout
  const cpuCores = runShell('nproc 2>/dev/null').stdout
  const uptime = Number((runShell("cut -d' ' -f1 /proc/uptime 2>/dev/null").stdout || '0').split('.')[0] || '0')

  return {
    tps_version: version,
    kernel,
    arch,
    hostname,
    board_model: boardModel,
    uptime_seconds: uptime,
    cpu_model: cpuModel,
    cpu_cores: cpuCores,
  }
}

function parseMetrics() {
  const smi = runShell('tps-smi 2>/dev/null').stdout
  const npuUsageMatch = smi.match(/(\d+(?:\.\d+)?)\s*%/)
  return {
    timestamp: new Date().toISOString(),
    cpu: parseCpuUsage(),
    memory: parseMeminfo(),
    npu: {
      available: Boolean(smi),
      raw_output: smi,
      usage_percent: npuUsageMatch ? Number(npuUsageMatch[1]) : 0,
    },
    network: [],
    codec: {
      decode: { fps: 0 },
      encode: { fps: 0 },
      raw_output: smi,
    },
  }
}

function parseDmaMemory() {
  return {
    cma_total: runShell("grep CmaTotal /proc/meminfo 2>/dev/null | awk '{print $2}'").stdout,
    cma_free: runShell("grep CmaFree /proc/meminfo 2>/dev/null | awk '{print $2}'").stdout,
    dma_buf: runShell('sed -n "1,60p" /sys/kernel/debug/dma_buf/bufinfo 2>/dev/null').stdout,
    umap_media_mem: runShell('sed -n "1,40p" /proc/umap/media-mem 2>/dev/null').stdout,
    tps_smi_memory: runShell("tps-smi 2>/dev/null | rg -i 'mem|dma|pool'").stdout,
  }
}

function parseDebPackages() {
  const installedRaw = runShell("dpkg-query -W -f='${Package}\\t${Version}\\t${Architecture}\\t${db:Status-Status}\\t${binary:Summary}\\n' 2>/dev/null").stdout
  const availableRaw = runShell("apt list --all-versions 2>/dev/null | tail -n +2").stdout
  const packages: Record<string, { versions: Array<{ version: string; status: string }>; description: string; arch: string }> = {}

  installedRaw.split('\n').filter(Boolean).forEach((line) => {
    const [pkg, ver, arch, status, ...rest] = line.split('\t')
    if (!pkg || !ver) return
    if (!packages[pkg]) {
      packages[pkg] = { versions: [], description: rest.join('\t') || '', arch: arch || '' }
    }
    packages[pkg].versions.push({ version: ver, status: status || 'installed' })
  })

  availableRaw.split('\n').filter(Boolean).forEach((line) => {
    const slash = line.indexOf('/')
    if (slash <= 0) return
    const pkg = line.slice(0, slash)
    const rest = line.slice(line.indexOf(' ', slash) + 1).trim()
    const ver = rest.split(/\s+/)[0]
    if (!pkg || !ver) return
    if (!packages[pkg]) {
      packages[pkg] = { versions: [], description: '', arch: '' }
    }
    if (!packages[pkg].versions.some((item) => item.version === ver)) {
      packages[pkg].versions.push({
        version: ver,
        status: line.includes('[installed') ? 'installed' : 'available',
      })
    }
  })

  return { packages }
}

function systemApiPlugin(): Plugin {
  return {
    name: 'system-api-dev-plugin',
    configureServer(server) {
      server.middlewares.use(async (req, res, next) => {
        const url = req.url || ''
        if (!url.startsWith('/api/system/')) return next()

        try {
          if (req.method === 'GET' && url === '/api/system/info') {
            return sendJson(res, 200, apiOk(parseInfo()))
          }
          if (req.method === 'GET' && url === '/api/system/metrics') {
            return sendJson(res, 200, apiOk(parseMetrics()))
          }
          if (req.method === 'GET' && url === '/api/system/filesystem') {
            return sendJson(res, 200, apiOk(parseFilesystem()))
          }
          if (req.method === 'GET' && url === '/api/system/dma-memory') {
            return sendJson(res, 200, apiOk(parseDmaMemory()))
          }
          if (req.method === 'GET' && url === '/api/system/debs') {
            return sendJson(res, 200, apiOk(parseDebPackages()))
          }
          if (req.method === 'GET' && url === '/api/system/apt-source') {
            const source = existsSync(VENDOR_SOURCE_PATH) ? readFileSync(VENDOR_SOURCE_PATH, 'utf-8').trim() : DEFAULT_VENDOR_SOURCE
            return sendJson(res, 200, apiOk({ source }))
          }
          if (req.method === 'POST' && url === '/api/system/apt-source') {
            const body = await readBody(req)
            const parsed = JSON.parse(body || '{}') as { source?: string }
            const source = (parsed.source || '').trim()
            if (!source) {
              return sendJson(res, 400, apiError('source 参数必填'))
            }
            try {
              writeFileSync(VENDOR_SOURCE_PATH, `${source}\n`, 'utf-8')
            } catch (error: any) {
              return sendJson(res, 500, apiError(error?.message || '写入 /etc/apt/sources.list 失败'))
            }
            const updateResult = runShell('apt-get update 2>&1')
            const pkgs = parseDebPackages()
            return sendJson(res, 200, apiOk({ output: updateResult.stdout, packages: pkgs.packages }, '数据源已写入 /etc/apt/sources.list，已获取最新包列表'))
          }
          if (req.method === 'POST' && url === '/api/system/deb/install') {
            const body = await readBody(req)
            const parsed = JSON.parse(body || '{}') as { package?: string; version?: string }
            const pkg = (parsed.package || '').trim()
            const version = (parsed.version || '').trim()
            if (!pkg) {
              return sendJson(res, 400, apiError('package 参数必填'))
            }
            const spec = version ? `${pkg}=${version}` : pkg
            const child = spawn('bash', ['-lc', `apt-get install -y ${spec} >/tmp/taco-vendor-apt-install.log 2>&1`], {
              detached: true,
              stdio: 'ignore',
            })
            child.unref()
            return sendJson(res, 200, apiOk({ output: 'apt-get install started' }, `已后台触发安装 ${spec}`))
          }
        } catch (error: any) {
          return sendJson(res, 500, apiError(error?.message || 'system api dev plugin failed'))
        }

        return next()
      })
    },
  }
}

export default defineConfig({
  plugins: [vue(), systemApiPlugin()],
  server: {
    port: 3001,
    strictPort: true,
    proxy: {
      '/api': {
        target: 'http://localhost:8090',
        changeOrigin: true,
      },
    },
  },
  build: {
    outDir: '../dist',
    emptyOutDir: true,
  },
})
