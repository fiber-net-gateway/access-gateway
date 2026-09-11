# Native compatibility patches

Temporary, narrowly scoped patches applied on top of the pinned
`third_party/fiber-gateway-cpp` submodule revision (`7e6930fc6b432c9f5575d969d14d249a2587dc0d`,
see `native/access-server/UPSTREAM.md`). Each patch exists because the pinned
revision contains a defect that blocks this product, the fix belongs upstream,
and waiting for a pin update is not acceptable.

Apply after `git submodule update --init --recursive`:

```sh
native/patches/apply.sh
```

The script refuses to run on any other submodule revision. After changing the
pin: re-check every patch, drop the ones the new pin already contains, rebase
the rest, and update the revision check in `apply.sh`.

## Patches

None. The pinned revision carries no compatibility patches.

## Retired patches

Four patches formerly carried here landed upstream in the reviewed range
`dfa5676c0a4e186767372ea5d2e1dd5573ba925a..e9a804053b14c206ed4a4fcd3b89e9a6789387a2`
and were dropped from this directory:

- `0001-http-h1-chunked-body-across-split-transport-reads.patch` —
  `09dd0ef` `fix(http): stream chunked bodies whose framing spans transport reads`.
  The repository-owned regression
  `ProxyExecutorTest.StreamsChunkedUpstreamWhoseFramingArrivesSeparatelyFromPayload`
  still guards the behavior from the application side.
- `0002-quic-h3-terminal-fin-strand.patch` —
  `3ccecc4` `fix(quic): encode H3 terminal FIN while body is inflight` (a superset:
  it also re-queues stranded send work on stream ACKs and keepalive backlog).
- `0003-http2-terminal-write-consumes-chain-completion.patch` —
  `d968408` `fix(http): consume chain completion on h2 terminal-only writes`.
- `0004-quic-awaiter-destruction-safe-resume.patch` —
  `1e30624` `fix(quic): make awaiter resumes retraction-safe against hard coroutine destruction`.
