import axios from 'axios'

const api = axios.create({
  baseURL: '/api',
  timeout: 8000,
  headers: { 'Content-Type': 'application/json' },
})

const slowApi = axios.create({
  baseURL: '/api',
  timeout: 60000,
  headers: { 'Content-Type': 'application/json' },
})

api.interceptors.response.use(
  (response) => {
    const data = response.data
    if (data.code !== undefined && data.code !== 0) {
      return Promise.reject(new Error(data.message || '请求失败'))
    }
    return response
  },
  (error) => {
    return Promise.reject(error)
  }
)

slowApi.interceptors.response.use(
  (response) => {
    const data = response.data
    if (data.code !== undefined && data.code !== 0) {
      return Promise.reject(new Error(data.message || '请求失败'))
    }
    return response
  },
  (error) => {
    return Promise.reject(error)
  }
)

export default api

// ===== DataSource API =====

export interface DataSource {
  id: string
  name: string
  type: 'FILE' | 'RTSP' | 'BUFFER'
  path: string
  buffer_count: number
  max_frames: number
  loop: boolean
  created_at: string
  status: string
  /** RTSP 模式下保存的多地址候选列表 */
  rtsp_urls?: string[]
}

export interface RtspProbeItem {
  url: string
  playable: boolean
}

export const datasourceApi = {
  list: () => api.get<{ code: number; data: DataSource[] }>('/datasources'),
  add: (ds: Partial<DataSource>) => api.post('/datasources', ds),
  update: (id: string, ds: Partial<DataSource>) => api.put(`/datasources/${id}`, ds),
  remove: (id: string) => api.delete(`/datasources/${id}`),
  probe: (id: string) => api.get(`/datasources/${id}/probe`),
  /**
   * 顺序探测多路 RTSP（服务器端每路最多约 95s 墙钟 + 10s 解码意图，axios 超时按路数拉长）
   */
  probeRtspUrls: (urls: string[]) =>
    slowApi.post<{ code: number; data: RtspProbeItem[] }>(
      '/datasources/rtsp-probe',
      { urls },
      {
        timeout: Math.max(120_000, urls.length * 100_000 + 30_000),
      }
    ),
  previewUrl: (id: string) => `/api/datasources/${id}/preview`,
}

// ===== Worker API =====

export interface DecoderConfig {
  name: string | null
  enable_hardware: boolean
  decode_threads: number
}

export interface Worker {
  id: string
  name: string
  datasource_id: string
  datasource_name: string
  state: string
  worker_type: string
  decoder: DecoderConfig
  created_at: string
  consumers: string[]
  consumers_config: Consumer[]
}

export interface WorkerStatus {
  id: string
  state: string
  fps: number
  decoded_frames: number
  dropped_frames: number
  uptime_seconds: number
  buffer_pool: { total: number; free: number; filled: number }
  consumers: Consumer[]
  command_line: string
  output: string
}

export const workerApi = {
  list: () => api.get<{ code: number; data: Worker[] }>('/workers'),
  create: (w: { name: string; datasource_id: string; worker_type?: string; loop?: boolean; decoder?: Partial<DecoderConfig>; consumers?: { type: string; config?: Record<string, any> }[] }) =>
    api.post('/workers', w),
  update: (id: string, w: Partial<{ name: string; datasource_id: string; worker_type: string; decoder: Partial<DecoderConfig>; consumers: { type: string; config?: Record<string, any> }[] }>) =>
    api.put(`/workers/${id}`, w),
  remove: (id: string) => api.delete(`/workers/${id}`),
  start: (id: string) => api.post(`/workers/${id}/start`),
  stop: (id: string) => api.post(`/workers/${id}/stop`),
  stopAll: () => api.post('/workers/stop-all'),
  status: (id: string) => api.get<{ code: number; data: WorkerStatus }>(`/workers/${id}/status`),
}

// ===== Consumer API =====

export interface Consumer {
  id: string
  type: string
  state: string
  config: Record<string, any>
}

export const consumerApi = {
  list: (workerId: string) =>
    api.get<{ code: number; data: Consumer[] }>(`/workers/${workerId}/consumers`),
  add: (workerId: string, consumer: { type: string; config?: Record<string, any> }) =>
    api.post(`/workers/${workerId}/consumers`, consumer),
  remove: (workerId: string, consumerId: string) =>
    api.delete(`/workers/${workerId}/consumers/${consumerId}`),
  update: (workerId: string, consumerId: string, config: Record<string, any>) =>
    api.put(`/workers/${workerId}/consumers/${consumerId}`, { config }),
}

// ===== Preview API =====

export const previewApi = {
  streamUrl: (workerId: string) => `/api/preview/stream/${workerId}`,
  snapshotUrl: (workerId: string, quality = 80) =>
    `/api/preview/snapshot/${workerId}?quality=${quality}`,
  grid: (layout = '3x3') => api.get(`/preview/grid?layout=${layout}`),
  compositeStreamUrl: () => `/api/preview/composite/stream`,
  compositeSnapshotUrl: () => `/api/preview/composite/snapshot`,
}

// ===== FileSystem API =====

export interface FileEntry {
  name: string
  path: string
  type: 'file' | 'directory'
  size_bytes: number
  modified_at: string
  extension: string
}

export const filesystemApi = {
  browse: (path = '/', filter = 'all') =>
    api.get<{ code: number; data: { current_path: string; parent_path: string; entries: FileEntry[] } }>(
      `/filesystem/browse?path=${encodeURIComponent(path)}&filter=${filter}`
    ),
}

// ===== System API =====

export interface SystemInfo {
  tps_version: string
  kernel: string
  arch: string
  hostname: string
  board_model: string
  uptime_seconds: number
  cpu_model: string
  cpu_cores: string
}

export interface NetworkInterface {
  name: string
  ip: string
  rx_bytes: number
  tx_bytes: number
  rx_rate_kbps: number
  tx_rate_kbps: number
  rx_packets: number
  tx_packets: number
  rx_errors: number
  tx_errors: number
}

export interface SystemMetrics {
  timestamp: string
  cpu: { usage_percent: number; cores: number; per_core?: { core: number; usage_percent: number }[] }
  memory: {
    total_mb: number; used_mb: number; free_mb: number
    available_mb: number; buffers_mb: number; cached_mb: number
    usage_percent: number
  }
  npu: { available: boolean; raw_output: string; usage_percent: number }
  network: NetworkInterface[]
  codec: { decode: Record<string, any>; encode: Record<string, any>; raw_output: string }
}

const metricsApi = axios.create({
  baseURL: '/api',
  timeout: 15000,
  headers: { 'Content-Type': 'application/json' },
})

metricsApi.interceptors.response.use(
  (response) => {
    const data = response.data
    if (data.code !== undefined && data.code !== 0) {
      return Promise.reject(new Error(data.message || '请求失败'))
    }
    return response
  },
  (error) => Promise.reject(error)
)

export const systemApi = {
  info: () => api.get<{ code: number; data: SystemInfo }>('/system/info'),
  metrics: () => metricsApi.get<{ code: number; data: SystemMetrics }>('/system/metrics'),
  cpu: () => api.get('/system/cpu'),
  memory: () => api.get('/system/memory'),
  hwModule: (module: string) => api.get(`/system/hw/${module}`),
  clocks: () => api.get('/system/clocks'),
  interrupts: () => api.get('/system/interrupts'),
  gpio: () => api.get('/system/gpio'),
  debs: () => slowApi.get('/system/debs'),
  installDeb: (pkg: string, version: string) =>
    slowApi.post('/system/deb/install', { package: pkg, version }),
  filesystem: () => api.get('/system/filesystem'),
  aptSource: () => api.get('/system/apt-source'),
  updateAptSource: (source: string) => slowApi.post('/system/apt-source', { source }),
  dmaMem: () => api.get('/system/dma-memory'),
}

// ===== Config API =====

export const configApi = {
  exportConfig: () => api.get('/config/export'),
  importConfig: (data: any, mode = 'replace') =>
    api.post(`/config/import?mode=${mode}`, data),
}

// ===== Recording API =====

export interface Recording {
  id: string
  datasource_id: string
  file_path: string
  format: string
  duration_seconds: number
  file_size_bytes: number
  created_at: string
}

export const recordingApi = {
  list: (datasourceId?: string) =>
    api.get<{ code: number; data: Recording[] }>(
      `/recordings${datasourceId ? `?datasource_id=${datasourceId}` : ''}`
    ),
  remove: (id: string) => api.delete(`/recordings/${id}`),
  playUrl: (id: string) => `/api/recordings/${id}/play`,
}
