# Native compatibility patches

Temporary, narrowly scoped patches applied on top of the pinned
`third_party/fiber-gateway-cpp` submodule revision (`dfa5676c0a4e186767372ea5d2e1dd5573ba925a`,
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

- `0001-http-h1-chunked-body-across-split-transport-reads.patch` — HTTP/1
  chunked bodies were corrupted or aborted when chunk framing crossed transport
  read boundaries (chunk-size line split mid-line, or framing arriving without
  payload in the same read). Three cooperating defects: `BodyParser::remaining()`
  surfaced a partial chunk size while the size line was incomplete (framing bytes
  leaked into the decoded body), `advance_chunked_body` (both client and server
  sides) treated buffered-but-incomplete framing as a protocol error, and the
  chunked branch of `read_body`/`read_request_body` could return an empty,
  non-complete body which `http::pipe_http_body` rejects as a Validate-phase
  invariant violation, aborting proxied responses. Observed in production as
  `curl -H 'Accept-Encoding: gzip'` on dynamically compressed assets returning
  200 with a zero-byte body and HTTP/2 RST_STREAM(CANCEL) in browsers.
  Regression test: `ProxyExecutorTest.StreamsChunkedUpstreamWhoseFramingArrivesSeparatelyFromPayload`.
  Upstream contribution: pending (root-cause report handed off for
  `fiber-net-gateway/fiber-gateway-cpp`).

- `0002-quic-h3-terminal-fin-strand.patch` — the terminal fin-only STREAM frame
  of an HTTP/3 response was refused by `QuicStreamSendQueue::encode_stream_frame`
  while body extents were still inflight (`buffered_bytes() > 0` includes
  inflight), so it could only encode after the peer ACKed the body — and
  nothing re-arms the outbound datagram build after that ACK on an otherwise
  idle connection (ACK processing releases extents without re-queueing the
  stream, and the keepalive timer bails when pending send work exists). The
  FIN strands in `pending_frames`: the client receives the response head and
  the full body but the stream never ends. Observed in production as proxied
  no-Content-Length responses over a warm idle h3 connection taking 15s in the
  browser (SPA timeout) while the server journal logs 8-12ms `result=success`;
  reproduced with headless Chrome netlog (fin absent; QUIC session dies at the
  29s idle timeout). Fix: a fin-only frame at `offset = final_size` is
  RFC-legal regardless of inflight data (offsets are absolute, peers reassemble
  out of order), so the guard now only defers while *ready* (unsent) data could
  still carry the FIN instead.
  Regression test: `QuicStreamSendQueueTest.FinOnlyFrameEncodesWhileBodyInflight`.
  Upstream contribution: pending (root-cause report handed off for
  `fiber-net-gateway/fiber-gateway-cpp`).
