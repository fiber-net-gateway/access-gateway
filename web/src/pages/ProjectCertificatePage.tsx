import { useCallback, useEffect, useState } from 'react'
import { Link } from 'react-router'

import { fetchProjectCertificate } from '../api/client'
import type { ProjectCertificateResolutionView } from '../api/types'
import { useProjectContext } from './ProjectLayout'

const resolutionLabel: Record<ProjectCertificateResolutionView['resolutionStatus'], string> = {
  matched: '已自动匹配',
  uncovered: '没有覆盖证书',
  conflict: '匹配冲突',
}

export function ProjectCertificatePage() {
  const { project } = useProjectContext()
  const [resolution, setResolution] = useState<ProjectCertificateResolutionView | null>(null)
  const [errorMessage, setErrorMessage] = useState<string | null>(null)

  const load = useCallback(
    async (signal?: AbortSignal): Promise<void> => {
      setResolution(await fetchProjectCertificate(project.id, signal))
    },
    [project.id],
  )

  useEffect(() => {
    const controller = new AbortController()
    void load(controller.signal).catch((error: unknown) => {
      if (!controller.signal.aborted) {
        setErrorMessage(error instanceof Error ? error.message : '加载 Project 证书解析失败')
      }
    })
    return () => controller.abort()
  }, [load])

  return (
    <section className="project-subpage" aria-labelledby="project-certificate-title">
      <header className="subpage-header">
        <div>
          <p className="eyebrow">PROJECT / CERTIFICATE</p>
          <h2 id="project-certificate-title">Certificate</h2>
          <p>根据 Project 域名与逻辑证书的稳定 DNS 范围自动解析，不再保存逐项目绑定。</p>
        </div>
        <span className="status-chip status-chip-unknown">运行时部署未接入</span>
      </header>

      {errorMessage ? (
        <div className="error-banner" role="alert">
          <strong>加载未完成</strong>
          <span>{errorMessage}</span>
        </div>
      ) : null}

      <div className="project-settings-grid">
        <section className="settings-panel">
          <header>
            <div>
              <p className="eyebrow">AUTOMATIC RESOLUTION</p>
              <h3>{resolution?.certificate?.name ?? '尚未解析到唯一证书'}</h3>
            </div>
            <span
              className={`status-chip status-chip-${resolution?.resolutionStatus === 'matched' ? 'ready' : 'unknown'}`}
            >
              {resolution ? resolutionLabel[resolution.resolutionStatus] : '解析中'}
            </span>
          </header>

          {resolution?.certificate ? (
            <div className="certificate-resolution-detail">
              <p>{resolution.certificate.managedDnsNames.join(' · ')}</p>
              <dl>
                <div>
                  <dt>当前逻辑版本</dt>
                  <dd>V{resolution.certificate.currentVersion.version}</dd>
                </div>
                <div>
                  <dt>事实状态</dt>
                  <dd>{resolution.certificate.currentVersion.status}</dd>
                </div>
                <div>
                  <dt>有效期至</dt>
                  <dd>
                    {new Date(resolution.certificate.currentVersion.notAfter).toLocaleString()}
                  </dd>
                </div>
                <div>
                  <dt>当前指纹</dt>
                  <dd>{resolution.certificate.currentVersion.fingerprintSha256}</dd>
                </div>
                <div>
                  <dt>实例状态</dt>
                  <dd>动态证书部署未接入</dd>
                </div>
              </dl>
            </div>
          ) : resolution?.resolutionStatus === 'conflict' ? (
            <div className="certificate-match-conflict" role="alert">
              <p>以下逻辑证书具有相同优先级，控制面不会任意选择：</p>
              <ul>
                {resolution.matches.map((certificate) => (
                  <li key={certificate.id}>
                    {certificate.name} · {certificate.managedDnsNames.join(', ')}
                  </li>
                ))}
              </ul>
            </div>
          ) : (
            <p className="settings-description">
              当前库存中没有 DNS 范围覆盖 {project.domain} 的逻辑证书。
            </p>
          )}

          <div className="form-actions">
            <span>证书更新后，匹配该逻辑证书的所有 Project 会自动指向新版本。</span>
            <Link className="button-secondary inline-button-link" to="/certificates">
              管理证书与版本
            </Link>
          </div>
        </section>

        <section className="settings-panel">
          <header>
            <div>
              <p className="eyebrow">SELECTION RULES</p>
              <h3>选择规则</h3>
            </div>
          </header>
          <ol className="certificate-selection-rules">
            <li>规范化域名后优先匹配 exact DNS 名称。</li>
            <li>没有 exact 时匹配仅覆盖一层子域名的 wildcard。</li>
            <li>同优先级出现多个候选时报告冲突，不按上传时间猜测。</li>
            <li>上传新版本不能删除逻辑证书既有的管理范围。</li>
          </ol>
        </section>
      </div>
    </section>
  )
}
