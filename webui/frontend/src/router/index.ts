import { createRouter, createWebHashHistory } from 'vue-router'

const router = createRouter({
  history: createWebHashHistory(),
  routes: [
    {
      path: '/',
      redirect: '/system',
    },
    {
      path: '/system',
      name: 'System',
      component: () => import('../views/SystemInfoView.vue'),
    },
    {
      path: '/datasources',
      redirect: '/datasources/add',
    },
    {
      path: '/datasources/add',
      name: 'DataSourcesAdd',
      component: () => import('../views/DataSourceView.vue'),
    },
    {
      path: '/datasources/rtsp-verify',
      name: 'DataSourcesRtspVerify',
      component: () => import('../views/RtspVerifyView.vue'),
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
