{{/*
Expand the name of the chart.
*/}}
{{- define "fibron.name" -}}
{{- default .Chart.Name .Values.nameOverride | trunc 63 | trimSuffix "-" -}}
{{- end -}}

{{/*
Create a default fully qualified app name.
*/}}
{{- define "fibron.fullname" -}}
{{- if .Values.fullnameOverride -}}
{{- .Values.fullnameOverride | trunc 63 | trimSuffix "-" -}}
{{- else -}}
{{- $name := default .Chart.Name .Values.nameOverride -}}
{{- if contains $name .Release.Name -}}
{{- .Release.Name | trunc 63 | trimSuffix "-" -}}
{{- else -}}
{{- printf "%s-%s" .Release.Name $name | trunc 63 | trimSuffix "-" -}}
{{- end -}}
{{- end -}}
{{- end -}}

{{/*
Create chart name and version as used by the chart label.
*/}}
{{- define "fibron.chart" -}}
{{- printf "%s-%s" .Chart.Name .Chart.Version | replace "+" "_" | trunc 63 | trimSuffix "-" -}}
{{- end -}}

{{/*
Common labels shared across all resources.
*/}}
{{- define "fibron.labels" -}}
helm.sh/chart: {{ include "fibron.chart" . }}
app.kubernetes.io/managed-by: {{ .Release.Service }}
app.kubernetes.io/part-of: fibron
{{- if .Chart.AppVersion }}
app.kubernetes.io/version: {{ .Chart.AppVersion | quote }}
{{- end }}
{{- with .Values.global.commonLabels }}
{{ toYaml . }}
{{- end }}
{{- end -}}

{{/*
Component selector labels. Usage: include "fibron.selectorLabels" (dict "root" . "component" "access-server")
*/}}
{{- define "fibron.selectorLabels" -}}
app.kubernetes.io/name: {{ include "fibron.name" .root }}
app.kubernetes.io/instance: {{ .root.Release.Name }}
app.kubernetes.io/component: {{ .component }}
{{- end -}}

{{/*
Full component labels. Usage: include "fibron.componentLabels" (dict "root" . "component" "access-server")
*/}}
{{- define "fibron.componentLabels" -}}
{{ include "fibron.labels" .root }}
{{ include "fibron.selectorLabels" . }}
{{- end -}}

{{/*
ServiceAccount name.
*/}}
{{- define "fibron.serviceAccountName" -}}
{{- if .Values.serviceAccount.create -}}
{{- default (include "fibron.fullname" .) .Values.serviceAccount.name -}}
{{- else -}}
{{- default "default" .Values.serviceAccount.name -}}
{{- end -}}
{{- end -}}

{{/*
Resolve container image with optional global.imageRegistry override.
Usage: include "fibron.image" (dict "root" . "image" .Values.accessServer.image)
*/}}
{{- define "fibron.image" -}}
{{- $repo := .image.repository -}}
{{- $tag := default .root.Chart.AppVersion .image.tag -}}
{{- if .root.Values.global.imageRegistry -}}
  {{- $parts := splitList "/" $repo -}}
  {{- if and (gt (len $parts) 1) (or (contains "." (index $parts 0)) (contains ":" (index $parts 0))) -}}
    {{- $repo = printf "%s/%s" (trimSuffix "/" .root.Values.global.imageRegistry) (join "/" (slice $parts 1)) -}}
  {{- else -}}
    {{- $repo = printf "%s/%s" (trimSuffix "/" .root.Values.global.imageRegistry) $repo -}}
  {{- end -}}
{{- end -}}
{{- printf "%s:%s" $repo $tag -}}
{{- end -}}

{{/*
Render imagePullSecrets if configured.
*/}}
{{- define "fibron.imagePullSecrets" -}}
{{- with .Values.global.imagePullSecrets }}
imagePullSecrets:
{{- toYaml . | nindent 2 }}
{{- end }}
{{- end -}}

{{/*
Component resource names.
*/}}
{{- define "fibron.accessServer.fullname" -}}
{{- printf "%s-access-server" (include "fibron.fullname" .) | trunc 63 | trimSuffix "-" -}}
{{- end -}}

{{- define "fibron.accessServer.headlessName" -}}
{{- printf "%s-headless" (include "fibron.accessServer.fullname" .) | trunc 63 | trimSuffix "-" -}}
{{- end -}}

{{- define "fibron.accessServer.metricsServiceName" -}}
{{- printf "%s-metrics" (include "fibron.accessServer.fullname" .) | trunc 63 | trimSuffix "-" -}}
{{- end -}}

{{- define "fibron.accessServer.http3ServiceName" -}}
{{- printf "%s-http3" (include "fibron.accessServer.fullname" .) | trunc 63 | trimSuffix "-" -}}
{{- end -}}

{{- define "fibron.console.fullname" -}}
{{- printf "%s-console" (include "fibron.fullname" .) | trunc 63 | trimSuffix "-" -}}
{{- end -}}

{{- define "fibron.console.apiName" -}}
{{- printf "%s-api" (include "fibron.console.fullname" .) | trunc 63 | trimSuffix "-" -}}
{{- end -}}

{{- define "fibron.console.publisherName" -}}
{{- printf "%s-publisher" (include "fibron.console.fullname" .) | trunc 63 | trimSuffix "-" -}}
{{- end -}}

{{- define "fibron.console.activationName" -}}
{{- printf "%s-activation" (include "fibron.console.fullname" .) | trunc 63 | trimSuffix "-" -}}
{{- end -}}

{{- define "fibron.console.migrationName" -}}
{{- printf "%s-migrate" (include "fibron.console.fullname" .) | trunc 63 | trimSuffix "-" -}}
{{- end -}}

{{/*
Validate access-server values fail-fast during template rendering.
*/}}
{{- define "fibron.validateAccessServer" -}}
{{- $as := .Values.accessServer -}}
{{- $cfg := $as.config -}}
{{- if and (not $cfg.tls.enabled) (not $cfg.plain.enabled) -}}
  {{- fail "accessServer.config: at least one of tls.enabled or plain.enabled must be true" -}}
{{- end -}}
{{- if and $cfg.tls.enabled $cfg.plain.enabled (eq (int $cfg.tls.listenPort) (int $cfg.plain.listenPort)) -}}
  {{- fail "accessServer.config: tls.listenPort and plain.listenPort must not collide" -}}
{{- end -}}
{{- if and $cfg.tls.enabled (eq (int $cfg.tls.listenPort) (int $cfg.status.listenPort)) -}}
  {{- fail "accessServer.config: tls.listenPort and status.listenPort must not collide" -}}
{{- end -}}
{{- if and $cfg.plain.enabled (eq (int $cfg.plain.listenPort) (int $cfg.status.listenPort)) -}}
  {{- fail "accessServer.config: plain.listenPort and status.listenPort must not collide" -}}
{{- end -}}
{{- if and (eq $cfg.registration.listener "plain") (not $cfg.plain.enabled) -}}
  {{- fail "accessServer.config.registration.listener=plain requires accessServer.config.plain.enabled=true (set listener=tls for TLS-only deployments)" -}}
{{- end -}}
{{- if and (eq $cfg.registration.listener "tls") (not $cfg.tls.enabled) -}}
  {{- fail "accessServer.config.registration.listener=tls requires accessServer.config.tls.enabled=true" -}}
{{- end -}}
{{- if and (eq (len .Values.externalNacos.addresses) 0) (eq (len .Values.externalNacos.hosts) 0) -}}
  {{- fail "externalNacos: set at least one IP literal in externalNacos.addresses or DNS hostname in externalNacos.hosts when accessServer.enabled=true" -}}
{{- end -}}
{{- if and (eq $cfg.upstreamTls.mode "custom_ca") (not $cfg.upstreamTls.customCaExistingConfigMap) -}}
  {{- fail "accessServer.config.upstreamTls.customCaExistingConfigMap is required when upstreamTls.mode=custom_ca" -}}
{{- end -}}
{{- if and (eq $cfg.dns.mode "override") (eq (len $cfg.dns.servers) 0) -}}
  {{- fail "accessServer.config.dns.servers (1-3 IPs) is required when dns.mode=override" -}}
{{- end -}}
{{- if and (eq $cfg.clientMetadata.mode "trusted_proxy") (eq (len $cfg.clientMetadata.trustedProxyCidrs) 0) -}}
  {{- fail "accessServer.config.clientMetadata.trustedProxyCidrs is required when clientMetadata.mode=trusted_proxy" -}}
{{- end -}}
{{- if $as.activationEvidence.enabled -}}
  {{- if not $as.activationEvidence.existingSecret -}}
    {{- fail "accessServer.activationEvidence.existingSecret is required when accessServer.activationEvidence.enabled=true (never hardcode bearer tokens in values.yaml)" -}}
  {{- end -}}
  {{- if and (eq $as.kind "Deployment") (gt (int $as.replicaCount) 1) -}}
    {{- fail "accessServer.kind=Deployment with replicaCount > 1 is incompatible with activationEvidence.enabled=true (each instance requires a unique stable ID and token; use StatefulSet)" -}}
  {{- end -}}
  {{- if gt (int $as.replicaCount) 64 -}}
    {{- fail "accessServer.replicaCount must not exceed 64 when activationEvidence.enabled=true (collector target cap)" -}}
  {{- end -}}
{{- end -}}
{{- if and $as.hpa.enabled (ne $as.kind "Deployment") -}}
  {{- fail "accessServer.hpa.enabled=true is only supported when accessServer.kind=Deployment" -}}
{{- end -}}
{{- end -}}

{{/*
Validate Console values fail-fast during template rendering.
*/}}
{{- define "fibron.validateConsole" -}}
{{- $c := .Values.console -}}
{{- if not .Values.externalMysql.host -}}
  {{- fail "externalMysql.host is required when console.enabled=true" -}}
{{- end -}}
{{- if not .Values.externalMysql.existingSecret -}}
  {{- fail "externalMysql.existingSecret is required when console.enabled=true (do not hardcode MySQL passwords in values.yaml)" -}}
{{- end -}}
{{- if not $c.documentEncryption.existingSecret -}}
  {{- fail "console.documentEncryption.existingSecret is required when console.enabled=true (provide a 32-byte base64 AES-256-GCM key via Secret)" -}}
{{- end -}}
{{- if $c.activationCollector.enabled -}}
  {{- $ac := $c.activationCollector -}}
  {{- if not $ac.existingSecret -}}
    {{- fail "console.activationCollector.existingSecret is required when console.activationCollector.enabled=true (provide ACTIVATION_TARGETS_JSON via Secret)" -}}
  {{- end -}}
  {{- if le (int $ac.evidenceTtlMillis) (int $ac.pollIntervalMillis) -}}
    {{- fail "console.activationCollector.evidenceTtlMillis must exceed console.activationCollector.pollIntervalMillis" -}}
  {{- end -}}
  {{- if lt (int $ac.leaseMillis) (mul (int $ac.requestTimeoutMillis) 2) -}}
    {{- fail "console.activationCollector.leaseMillis must be at least twice console.activationCollector.requestTimeoutMillis" -}}
  {{- end -}}
  {{- if gt (int $ac.maxProjects) (mul (int $ac.maxPages) 256) -}}
    {{- fail "console.activationCollector.maxPages * 256 must be >= console.activationCollector.maxProjects" -}}
  {{- end -}}
{{- end -}}
{{- end -}}

{{/*
Shared secret env vars required by all Console processes (MySQL password & document encryption key).
Never includes ACTIVATION_TARGETS_JSON.
*/}}
{{- define "fibron.console.secretEnv" -}}
- name: MYSQL_PASSWORD
  valueFrom:
    secretKeyRef:
      name: {{ .Values.externalMysql.existingSecret | quote }}
      key: {{ .Values.externalMysql.passwordKey | quote }}
- name: DOCUMENT_ENCRYPTION_KEY_BASE64
  valueFrom:
    secretKeyRef:
      name: {{ .Values.console.documentEncryption.existingSecret | quote }}
      key: {{ .Values.console.documentEncryption.keyBase64Key | quote }}
{{- end -}}

{{/*
Compute NODE_ENV for Console workloads in alignment with server/src/config/env.ts.
*/}}
{{- define "fibron.console.nodeEnv" -}}
{{- if eq .Values.console.auth.mode "development" -}}
development
{{- else -}}
production
{{- end -}}
{{- end -}}
