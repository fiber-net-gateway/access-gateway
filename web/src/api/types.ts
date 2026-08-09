export interface HealthResponse {
  status: 'ok'
  service: 'access-gateway-console-api'
  version: string
}

export type ApiConnectionState = 'loading' | 'online' | 'offline'
