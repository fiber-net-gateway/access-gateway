import { Navigate, createBrowserRouter } from 'react-router'

import App from './App'
import { CertificatesPage } from './pages/CertificatesPage'
import { NotFoundPage } from './pages/NotFoundPage'
import { ProjectLayout } from './pages/ProjectLayout'
import { ProjectNetworkPolicyPage } from './pages/ProjectNetworkPolicyPage'
import { ProjectReleasesPage } from './pages/ProjectReleasesPage'
import { ProjectRoutesPage } from './pages/ProjectRoutesPage'
import { ProjectSettingsPage } from './pages/ProjectSettingsPage'
import { ProjectVersionsPage } from './pages/ProjectVersionsPage'
import { ProjectsIndexPage } from './pages/ProjectsIndexPage'

export const appRoutes = [
  {
    path: '/',
    element: <App />,
    children: [
      { index: true, element: <Navigate replace to="/projects" /> },
      { path: 'certificates', element: <CertificatesPage /> },
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
              { path: 'network-policy', element: <ProjectNetworkPolicyPage /> },
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
