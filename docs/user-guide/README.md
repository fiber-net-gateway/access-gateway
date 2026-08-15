# Access Gateway user guide

The maintained user guide is available in two languages and three topics:

| Topic                    | 简体中文                                         | English                                                     |
| ------------------------ | ------------------------------------------------ | ----------------------------------------------------------- |
| Route rules and usage    | [路由规则与详细用法](zh-CN/routing.md)           | [Route rules and usage](en/routing.md)                      |
| Script route usage       | [脚本路由用法](zh-CN/script-routing.md)          | [Script route usage](en/script-routing.md)                  |
| Script language and APIs | [脚本语法与 API 参考](zh-CN/script-reference.md) | [Script language and API reference](en/script-reference.md) |

The Markdown files are the source of truth. Run `npm run docs:build` from the repository root to
produce the checked-in HTML fragments under `docs/user-guide/html/`. The Console imports those
fragments and exposes them under `/docs/zh-CN/...` and `/docs/en/...`. Run `npm run docs:check` to
verify that generated HTML matches its source.

These guides describe the repository-owned access-server and the pinned Fiber revision
`0fda7764bf94944aca4b674ab5ab311184703118`. Production script-corpus differential verification and
the final cutover gates remain incomplete; the guide does not claim production cutover readiness.
