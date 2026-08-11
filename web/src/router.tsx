import { Navigate, createBrowserRouter } from 'react-router'

import App from './App'
import { NotFoundPage } from './pages/NotFoundPage'
import { ProjectLayout } from './pages/ProjectLayout'
import { ProjectReleasesPage } from './pages/ProjectReleasesPage'
import { ProjectRoutesPage } from './pages/ProjectRoutesPage'
import { ProjectUnavailablePage } from './pages/ProjectUnavailablePage'
import { ProjectVersionsPage } from './pages/ProjectVersionsPage'
import { ProjectsIndexPage } from './pages/ProjectsIndexPage'

export const appRoutes = [
  {
    path: '/',
    element: <App />,
    children: [
      { index: true, element: <Navigate replace to="/projects" /> },
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
              { path: 'certificate', element: <ProjectUnavailablePage capability="certificate" /> },
              { path: 'releases', element: <ProjectReleasesPage /> },
              { path: 'settings', element: <ProjectUnavailablePage capability="settings" /> },
            ],
          },
        ],
      },
      { path: '*', element: <NotFoundPage /> },
    ],
  },
]

export const router = createBrowserRouter(appRoutes)
