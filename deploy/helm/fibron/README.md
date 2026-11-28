# Fibron (Access Gateway) Helm Chart

Production-ready Kubernetes Helm chart for deploying **Fibron** (`access-server` C++23 data plane and the management Console control plane).

## Components

| Component             | Default Workload                                  | Container Image                                    | Purpose                                                                                                                       |
| :-------------------- | :------------------------------------------------ | :------------------------------------------------- | :---------------------------------------------------------------------------------------------------------------------------- |
| `access-server`       | `StatefulSet` (or `Deployment`)                   | `ghcr.io/fiber-net-gateway/access-server`          | HTTP/1.1, HTTP/2, HTTP/3 (QUIC), WebSocket data plane (`8443/tcp`, `8443/udp`, `8000/tcp`) + private status port (`8001/tcp`) |
| `console-migrate`     | `Job` (Helm `pre-install,pre-upgrade` hook)       | `ghcr.io/fiber-net-gateway/access-gateway-console` | Checksum-verified MySQL schema migrations                                                                                     |
| `console-api`         | `Deployment` + `Service` + optional `Ingress`     | `ghcr.io/fiber-net-gateway/access-gateway-console` | Fastify control-plane API, React SPA, and embedded Native Validator                                                           |
| `console-publisher`   | `Deployment`                                      | `ghcr.io/fiber-net-gateway/access-gateway-console` | Leased R-Nacos release publication worker                                                                                     |
| `console-activation`  | `Deployment` (optional)                           | `ghcr.io/fiber-net-gateway/access-gateway-console` | Leased per-instance activation evidence collector (isolated from Console API)                                                 |
| Monitoring (optional) | `ServiceMonitor` + Grafana Dashboard `ConfigMap`s | —                                                  | Prometheus Operator scraping and Grafana sidecar dashboards synced from `deploy/grafana/`                                     |

## Prerequisites

- Kubernetes **1.26+** and Helm **3.12+**
- External **MySQL 8.4** (required when `console.enabled=true`)
- External **R-Nacos** (Nacos-compatible config & naming server)
- Optional: **Prometheus Operator** (`monitoring.coreos.com/v1`) for `ServiceMonitor` and **Grafana sidecar** for dashboard auto-discovery

## Lifecycle & Current Limitations

Keep the repository's operational boundaries in mind when planning a rollout:

- **Three distinct lifecycle states:** Console draft persistence, R-Nacos publication/readback, and per-instance activation on `access-server` are tracked independently. Until `accessServer.activationEvidence` and `console.activationCollector` are configured, Release activation reports `unknown`.
- **Console authentication:** OIDC is not implemented yet. With MySQL enabled, `AUTH_MODE=oidc` refuses to start, so the Console currently runs with `AUTH_MODE=development` (which sets `NODE_ENV=development` as enforced by `server/src/config/env.ts`). Keep `console.api.service.type=ClusterIP` and place it behind a private VPN or authenticating reverse proxy (such as `oauth2-proxy`).
- **Data-plane cutover gate:** Refer to `native/access-server/docs/cutover-evidence-gate.md` for current differential verification and production cutover status.

## Secret Management (No Hardcoded Secrets)

This chart intentionally contains **no default passwords, encryption keys, or bearer tokens**. Create Secrets beforehand (or sync them via External Secrets Operator, Sealed Secrets, or Vault) and reference them via `existingSecret`.

### 1. Required Console Secrets (`console.enabled=true`)

```bash
kubectl -n fibron create secret generic fibron-mysql-credentials \
  --from-literal=password='<strong-mysql-password>'

kubectl -n fibron create secret generic fibron-document-encryption \
  --from-literal=keyBase64="$(openssl rand -base64 32)"
```

### 2. Optional R-Nacos Auth Secret

```bash
kubectl -n fibron create secret generic fibron-nacos-auth \
  --from-literal=username='<nacos-username>' \
  --from-literal=password='<nacos-password>'
```

### 3. Optional Per-Instance Activation Evidence Secrets

Each `access-server` replica must have a **stable, environment-unique ID** and a **unique 32–512 byte printable ASCII bearer token** (never reuse one token across replicas). With `accessServer.kind=StatefulSet` and release name `fibron`, Pods are named `fibron-access-server-0`, `fibron-access-server-1`, etc., and reachable over the headless Service at `http://<pod>.fibron-access-server-headless.<ns>.svc.cluster.local:8001/v1/activation-evidence`.

```bash
TOKEN_0="$(openssl rand -hex 32)"
TOKEN_1="$(openssl rand -hex 32)"

# Secret mounted ONLY by the access-server render-config initContainer:
kubectl -n fibron create secret generic fibron-access-server-activation-tokens \
  --from-literal="fibron-access-server-0=${TOKEN_0}" \
  --from-literal="fibron-access-server-1=${TOKEN_1}"

# Secret mounted ONLY by the activation-collector Deployment (NEVER Console API):
kubectl -n fibron create secret generic fibron-activation-targets \
  --from-literal=targetsJson="[
    {\"environmentCode\":\"prod\",\"instanceKey\":\"fibron-access-server-0\",\"endpoint\":\"http://fibron-access-server-0.fibron-access-server-headless.fibron.svc.cluster.local:8001/v1/activation-evidence\",\"token\":\"${TOKEN_0}\"},
    {\"environmentCode\":\"prod\",\"instanceKey\":\"fibron-access-server-1\",\"endpoint\":\"http://fibron-access-server-1.fibron-access-server-headless.fibron.svc.cluster.local:8001/v1/activation-evidence\",\"token\":\"${TOKEN_1}\"}
  ]"
```

## Installation

### Evaluation / Local Cluster

```bash
kubectl create namespace fibron
# Create fibron-mysql-secret and fibron-encryption-secret first (see above).
helm upgrade --install fibron deploy/helm/fibron \
  --namespace fibron \
  -f deploy/helm/fibron/examples/values-eval.yaml
```

### Production

```bash
helm upgrade --install fibron deploy/helm/fibron \
  --namespace fibron \
  --create-namespace \
  -f my-production-values.yaml
```

See [examples/values-production.yaml](examples/values-production.yaml) for a full multi-replica HA template (StatefulSet, separate HTTP/3 UDP Service, Activation Evidence, ServiceMonitor, Grafana dashboards, and NetworkPolicy).

### Data-Plane-Only Deployment

To run `access-server` in an edge cluster separate from the Console:

```bash
helm upgrade --install fibron-edge deploy/helm/fibron \
  --namespace fibron-edge \
  --set console.enabled=false \
  --set 'externalNacos.addresses={10.20.0.10,10.20.0.11}'
```

## Operational Notes

### Nacos IP Literals & Startup Rendering

`access-server` strictly validates `NACOS_SERVER_ADDRESSES` as IP literals. The chart uses a `render-config` initContainer with a memory-backed `emptyDir` (`/run/access-server`) so that:

- Explicit IP literals in `externalNacos.addresses` are written directly;
- Optional Kubernetes Service DNS names in `externalNacos.hosts` are resolved to IP literals at Pod start;
- `ACCESS_SERVER_ADVERTISE_ADDRESS` is populated from the Pod's `status.podIP`;
- Secrets (`NACOS_PASSWORD`, per-Pod `ACCESS_SERVER_ACTIVATION_TOKEN`) are written with mode `0600` in memory and never stored in a `ConfigMap`;
- The main `access-server` container executes `/usr/local/bin/access-server /run/access-server/access-server.env` directly as PID 1 with `readOnlyRootFilesystem: true` and handles `SIGTERM` gracefully.

### HTTP/3 (UDP 8443) & LoadBalancers

- `accessServer.service.http3Mode=combined`: exposes `8443/tcp` and `8443/udp` on the same Service (requires Kubernetes 1.26+ mixed-protocol support in your LB controller, e.g. MetalLB or Cilium).
- `accessServer.service.http3Mode=separate`: creates a dedicated UDP Service (`<fullname>-access-server-http3`) for cloud LoadBalancers that disallow mixed TCP/UDP Services; annotate both Services to share an external IP so `Alt-Svc` advertises the same endpoint.
- `accessServer.service.externalTrafficPolicy=Local`: recommended for `LoadBalancer`/`NodePort` to preserve real client source IPs for route CIDR policies, WAF rules, and edge-function canary sampling.

### Private Status Port (`8001/tcp`)

Port `8001` serves unauthenticated `/metrics` and token-authenticated `/v1/activation-evidence`. It is **never** exposed on the public traffic Service; it is only attached to `<fullname>-access-server-metrics` (`ClusterIP`) and `<fullname>-access-server-headless` (`ClusterIP: None`). Enable `networkPolicy.enabled=true` to enforce ingress restrictions to port `8001`.

### CPU Quota and HTTP Worker Count

`access-server` automatically sizes its HTTP EventLoop worker pool from the container cgroup CPU quota (`accessServer.resources.limits.cpu`). Use whole-core CPU limits in production (for example, `cpu: "2"` or `cpu: "4"`).

### Grafana Dashboards Sync

Dashboard JSON files under `deploy/helm/fibron/dashboards/` are synced from `deploy/grafana/` so `helm package` remains self-contained. After modifying `deploy/grafana/*.json`, run:

```bash
npm run helm:sync-dashboards
npm run helm:check-dashboards
```

## Key Values Reference

| Key                                                               | Type            | Default                                                       | Description                                                                     |
| :---------------------------------------------------------------- | :-------------- | :------------------------------------------------------------ | :------------------------------------------------------------------------------ |
| `global.imageRegistry`                                            | string          | `""`                                                          | Optional container registry override                                            |
| `global.imagePullSecrets`                                         | list            | `[]`                                                          | Image pull secrets attached to all Pods                                         |
| `serviceAccount.create`                                           | bool            | `true`                                                        | Create a dedicated ServiceAccount (`automountServiceAccountToken: false`)       |
| `externalNacos.addresses`                                         | list            | `[]`                                                          | Static Nacos IPv4/IPv6 literals for `access-server`                             |
| `externalNacos.hosts`                                             | list            | `[]`                                                          | Nacos DNS hostnames resolved to IPs by `render-config` initContainer            |
| `externalNacos.httpPort` / `grpcPort`                             | int             | `8848` / `9848`                                               | R-Nacos HTTP and gRPC ports                                                     |
| `externalNacos.auth.existingSecret`                               | string          | `""`                                                          | Optional Secret with Nacos `username` / `password` keys                         |
| `externalNacos.endpointOverride`                                  | string          | `""`                                                          | Optional `PUBLICATION_NACOS_ENDPOINT` origin override for Console               |
| `externalMysql.host` / `port` / `database` / `user`               | string/int      | `""` / `3306` / `access_gateway` / `access_gateway`           | External MySQL 8.4 connection parameters                                        |
| `externalMysql.sslMode`                                           | string          | `required`                                                    | MySQL TLS mode (`required` or `disabled`)                                       |
| `externalMysql.existingSecret`                                    | string          | `""`                                                          | **Required** when `console.enabled=true`; holds MySQL password                  |
| `accessServer.enabled`                                            | bool            | `true`                                                        | Deploy `access-server` data plane                                               |
| `accessServer.kind`                                               | string          | `StatefulSet`                                                 | `StatefulSet` (required for multi-replica activation evidence) or `Deployment`  |
| `accessServer.replicaCount`                                       | int             | `2`                                                           | Number of `access-server` replicas                                              |
| `accessServer.image.repository` / `tag`                           | string          | `ghcr.io/fiber-net-gateway/access-server` / `latest`          | Data-plane container image (pin immutable tags in production)                   |
| `accessServer.config.networkEntry`                                | string          | `internet`                                                    | Required entry network (`X-Entry`; e.g. `internet`, `vdi`, `desktop`)           |
| `accessServer.config.tls.enabled` / `listenPort` / `http3Enabled` | bool/int        | `true` / `8443` / `true`                                      | TLS + HTTP/2 (TCP) and HTTP/3 (UDP) listener settings                           |
| `accessServer.config.plain.enabled` / `listenPort`                | bool/int        | `true` / `8000`                                               | Plaintext HTTP/1.1 listener settings                                            |
| `accessServer.config.status.listenPort`                           | int             | `8001`                                                        | Private status listener port (`/metrics` and `/v1/activation-evidence`)         |
| `accessServer.config.upstreamTls.mode`                            | string          | `system_ca`                                                   | Outbound TLS verification (`system_ca`, `custom_ca`, `legacy_insecure`)         |
| `accessServer.config.clientMetadata.mode`                         | string          | `direct`                                                      | Client IP trust (`direct`, `trusted_proxy`, `legacy_headers`)                   |
| `accessServer.activationEvidence.enabled` / `existingSecret`      | bool/string     | `false` / `""`                                                | Enable per-instance `/v1/activation-evidence` with per-Pod token Secret         |
| `accessServer.service.type` / `http3Mode`                         | string          | `ClusterIP` / `combined`                                      | Traffic Service type and UDP 8443 exposure (`combined`, `separate`, `disabled`) |
| `console.enabled`                                                 | bool            | `true`                                                        | Deploy Console control plane                                                    |
| `console.image.repository` / `tag`                                | string          | `ghcr.io/fiber-net-gateway/access-gateway-console` / `latest` | Console container image                                                         |
| `console.auth.mode`                                               | string          | `development`                                                 | Authentication mode (`development`; `oidc` is not yet implemented)              |
| `console.documentEncryption.existingSecret`                       | string          | `""`                                                          | **Required** when `console.enabled=true`; holds 32-byte base64 AES-256-GCM key  |
| `console.prometheus.url`                                          | string          | `""`                                                          | Optional `PROMETHEUS_URL` for Console WAF hit statistics                        |
| `console.migration.enabled` / `useHelmHook`                       | bool            | `true` / `true`                                               | Run MySQL migrations via `pre-install,pre-upgrade` Job                          |
| `console.api.replicaCount` / `service.type` / `ingress.enabled`   | int/string/bool | `2` / `ClusterIP` / `false`                                   | Console API + SPA deployment and exposure                                       |
| `console.publicationWorker.enabled` / `replicaCount`              | bool/int        | `true` / `1`                                                  | Leased R-Nacos publication worker                                               |
| `console.activationCollector.enabled` / `existingSecret`          | bool/string     | `false` / `""`                                                | Leased activation evidence collector and its `ACTIVATION_TARGETS_JSON` Secret   |
| `monitoring.serviceMonitor.enabled`                               | bool            | `false`                                                       | Create Prometheus Operator `ServiceMonitor` for port `8001`                     |
| `monitoring.grafanaDashboards.enabled`                            | bool            | `false`                                                       | Create Grafana sidecar `ConfigMap`s from synced `deploy/grafana/` JSONs         |
| `monitoring.grafanaDashboards.prometheusDatasourceUid`            | string          | `""`                                                          | Optional fixed datasource UID (default keeps `${datasource}` variable)          |
| `networkPolicy.enabled`                                           | bool            | `false`                                                       | Enforce NetworkPolicy restricting port `8001` and Console API                   |
