import { createRouter, createWebHashHistory } from 'vue-router'

const router = createRouter({
  history: createWebHashHistory(),
  routes: [
    {
      path: '/',
      redirect: '/datasources',
    },
    {
      path: '/datasources',
      name: 'DataSources',
      component: () => import('../views/DataSourceView.vue'),
    },
    {
      path: '/workers',
      name: 'Workers',
      component: () => import('../views/WorkerView.vue'),
    },
    {
      path: '/preview',
      name: 'Preview',
      component: () => import('../views/PreviewView.vue'),
    },
  ],
})

export default router
