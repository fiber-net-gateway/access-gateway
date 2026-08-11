interface ProjectUnavailablePageProps {
  capability: 'certificate' | 'settings'
}

const content = {
  certificate: {
    eyebrow: 'CERTIFICATE DELIVERY',
    title: 'Certificate 暂不可用',
    description:
      '证书绑定和动态 SNI 部署尚未接入 access-server。当前不会模拟“已部署”或“已激活”状态。',
  },
  settings: {
    eyebrow: 'PROJECT OPERATIONS',
    title: 'Settings 暂不可用',
    description:
      'Project 归档、下线和 HTTPS 策略尚未实现，避免在缺少完整发布语义时提供高风险操作。',
  },
}

export function ProjectUnavailablePage({ capability }: ProjectUnavailablePageProps) {
  const item = content[capability]
  return (
    <section className="unavailable-panel">
      <p className="eyebrow">{item.eyebrow}</p>
      <h2>{item.title}</h2>
      <p>{item.description}</p>
      <span className="status-chip status-chip-unknown">能力未接入</span>
    </section>
  )
}
