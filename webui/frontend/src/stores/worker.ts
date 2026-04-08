import { defineStore } from 'pinia'
import { ref } from 'vue'
import { workerApi, consumerApi, type Worker, type Consumer } from '../api'

export const useWorkerStore = defineStore('worker', () => {
  const workers = ref<Worker[]>([])
  const loading = ref(false)

  async function fetchList() {
    loading.value = true
    try {
      const res = await workerApi.list()
      workers.value = res.data.data
    } finally {
      loading.value = false
    }
  }

  async function create(params: { name: string; datasource_id: string; worker_type?: string; consumers?: { type: string; config?: Record<string, any> }[] }) {
    await workerApi.create(params)
    await fetchList()
  }

  async function update(id: string, params: Record<string, any>) {
    await workerApi.update(id, params)
    await fetchList()
  }

  async function remove(id: string) {
    await workerApi.remove(id)
    await fetchList()
  }

  async function start(id: string) {
    await workerApi.start(id)
    await fetchList()
  }

  async function stop(id: string) {
    await workerApi.stop(id)
    await fetchList()
  }

  async function addConsumer(workerId: string, consumer: { type: string; config?: Record<string, any> }) {
    await consumerApi.add(workerId, consumer)
    await fetchList()
  }

  async function removeConsumer(workerId: string, consumerId: string) {
    await consumerApi.remove(workerId, consumerId)
    await fetchList()
  }

  return { workers, loading, fetchList, create, update, remove, start, stop, addConsumer, removeConsumer }
})
