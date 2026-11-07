import { Navigate, createBrowserRouter } from 'react-router'

import App from './App'
import { CertificatesPage } from './pages/CertificatesPage'
import { NotFoundPage } from './pages/NotFoundPage'
import { ProjectLayout } from './pages/ProjectLayout'
import { ProjectHostPolicyPage } from './pages/ProjectHostPolicyPage'
import { ProjectNetworkPolicyPage } from './pages/ProjectNetworkPolicyPage'
import { ProjectReleasesPage } from './pages/ProjectReleasesPage'
import { ProjectRoutesPage } from './pages/ProjectRoutesPage'
import { ProjectSettingsPage } from './pages/ProjectSettingsPage'
import { ProjectVersionsPage } from './pages/ProjectVersionsPage'
import { ProjectWafPolicyPage } from './pages/ProjectWafPolicyPage'
import { ProjectsIndexPage } from './pages/ProjectsIndexPage'
import { WafEdgeFunctionsPage } from './pages/WafEdgeFunctionsPage'
import { WafLayout } from './pages/WafLayout'
import { WafPolicyPage } from './pages/WafPolicyPage'
import { WafReleasesPage } from './pages/WafReleasesPage'

export const appRoutes = [
  {
    path: '/',
    element: <App />,
    children: [
      { index: true, element: <Navigate replace to="/projects" /> },
      { path: 'certificates', element: <CertificatesPage /> },
      {
        path: 'docs',
        children: [
          { index: true, element: <Navigate replace to="zh-CN/routing" /> },
          {
            path: ':language/:topic',
            lazy: async () => {
              const { DocumentationPage } = await import('./pages/DocumentationPage')
              return { Component: DocumentationPage }
            },
          },
        ],
      },
      {
        path: 'waf',
        element: <WafLayout />,
        children: [
          { index: true, element: <WafPolicyPage /> },
          { path: 'releases', element: <WafReleasesPage /> },
          { path: 'edge-functions', element: <WafEdgeFunctionsPage /> },
        ],
      },
      {
        path: 'projects',
        children: [
          { index: true, element: <ProjectsIndexPage /> },
          {
            path: ':projectId',
            element: <ProjectLayout />,
            children: [
              { index: true, element: <Navigate replace to="routes" /> },
              { path: 'routes', element: <ProjectRoutesPage /> },
              { path: 'versions', element: <ProjectVersionsPage /> },
              { path: 'host-policy', element: <ProjectHostPolicyPage /> },
              { path: 'network-policy', element: <ProjectNetworkPolicyPage /> },
              { path: 'waf-policy', element: <ProjectWafPolicyPage /> },
              { path: 'releases', element: <ProjectReleasesPage /> },
              { path: 'settings', element: <ProjectSettingsPage /> },
            ],
          },
        ],
      },
      { path: '*', element: <NotFoundPage /> },
    ],
  },
]

export const router = createBrowserRouter(appRoutes)
