import type {
  FieldPacket,
  Pool,
  PoolConnection,
  QueryResult,
  ResultSetHeader,
  RowDataPacket,
} from 'mysql2/promise'
import type { ExecuteValues } from 'mysql2'

export type { ResultSetHeader, RowDataPacket }

export interface SqlExecutor {
  execute<T extends QueryResult>(sql: string, values?: ExecuteValues): Promise<[T, FieldPacket[]]>
}

export type DatabasePool = Pool
export type DatabaseConnection = PoolConnection
