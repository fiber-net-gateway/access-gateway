const mysqlDateTimePattern = /^(\d{4}-\d{2}-\d{2}) (\d{2}:\d{2}:\d{2}(?:\.\d{1,6})?)$/u

export function mysqlDateTimeToRfc3339(value: string): string {
  const match = mysqlDateTimePattern.exec(value)
  if (!match?.[1] || !match[2]) {
    throw new Error('Stored MySQL datetime is invalid')
  }
  return `${match[1]}T${match[2]}Z`
}
