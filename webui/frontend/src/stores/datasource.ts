import { defineStore } from 'pinia'
import { ref } from 'vue'
import { datasourceApi, type DataSource } from '../api'

export const useDataSourceStore = defineStore('datasource', () => {
  const datasources = ref<DataSource[]>([])
  const loading = ref(false)

  async function fetchList() {
    loading.value = true
    try {
      const res = await datasourceApi.list()
      datasources.value = res.data.data
    } finally {
      loading.value = false
    }
  }

  async function add(ds: Partial<DataSource>) {
    await datasourceApi.add(ds)
    await fetchList()
  }

  async function update(id: string, ds: Partial<DataSource>) {
    await datasourceApi.update(id, ds)
    await fetchList()
  }

  async function remove(id: string) {
    await datasourceApi.remove(id)
    await fetchList()
  }

  return { datasources, loading, fetchList, add, update, remove }
})
