interface ProjectUnavailablePageProps {
  capability: 'settings'
}

const content = {
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
