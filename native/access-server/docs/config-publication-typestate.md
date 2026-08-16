# Project configuration publication typestate

Project route publication uses move-only types to make readiness evidence a compile-time part of
the `RouteConfigStore` API. A compiled candidate cannot be passed directly to `commit()`.

This is repository-owned gateway policy. It does not require a Fiber library change and does not
alter the rnacos wire contract or Java compatibility rules.

## State model

```text
rnacos ConfigData
  -> parsed ProjectConfig
  -> CompiledProjectConfig
  -> PreparedProjectUpdate
       -> try_ready()   -- ignored/same-version/unload/static selector
       -> wait_ready()  -- NamingService-backed selector
  -> ReadyProjectUpdate
  -> RouteConfigStore::commit(ReadyProjectUpdate)
  -> ConfigUpdateResult + immutable AccessRouteSnapshot
```

`ProjectConfig` is the decoded compatibility model. `CompiledProjectConfig` is produced on the
compiler EventLoop and contains a complete Project candidate without owner-loop service leases.
`RouteConfigStore::prepare_compiled()` binds those leases on the Nacos owner loop and returns a
`PreparedProjectUpdate`. For a new-version candidate that could be published, it first verifies
that the compiled snapshot's embedded Project and version match the requested update. Empty and
same-version inputs retain their Java-compatible ignore semantics without inspecting a discarded
snapshot.

## Type invariants

`PreparedProjectUpdate` and `ReadyProjectUpdate`:

- have no public or default constructor;
- keep Project name, version, target status, and candidate snapshot private;
- are move-only, so one candidate cannot be copied into multiple publication attempts;
- transfer an internal validity bit on every move and fail fast if moved-from state is consumed;
- can only be created by `RouteConfigStore` or the checked Prepared-to-Ready transition.

`RouteConfigStore::commit()` accepts only `ReadyProjectUpdate`. Calling
`ProjectRouteSnapshot::wait_ready()` directly is not publication authority because it cannot create
the required Ready type.

## Checked transitions

`PreparedProjectUpdate::try_ready() &&` is the non-blocking transition. It succeeds for:

- ignored empty or null content;
- an unchanged successful version;
- an empty-Host unload candidate;
- compiled snapshots whose selectors all report `ready_for_publish()`.

Static address selectors are ready immediately. A NamingService selector deliberately never
passes this synchronous predicate, even if it has previously observed data, because readiness
must be tied to this candidate's retained lease and awaited transition.

`PreparedProjectUpdate::wait_ready() &&` transfers the Prepared value into its coroutine frame. It
awaits every selector and returns `ReadyProjectUpdate` only after all succeed. A closed, retired,
or shutting-down service returns `ProxyAddressReadyError`; no Ready value is created.

Owning the Prepared value in the coroutine is important: if a newer Project generation wins the
watcher's `when_any` race, destroying the losing awaiter also destroys the candidate and releases
all of its service-discovery leases.

## Watcher and commit ownership

The Nacos owner loop performs the following sequence:

1. reclassify the compiled version against current store state;
2. bind owner-loop-only service selectors and obtain `PreparedProjectUpdate`;
3. take the immediate transition or race asynchronous readiness against the Project revision;
4. recheck watcher state, Project identity, and generation after readiness;
5. pass the resulting `ReadyProjectUpdate` to `commit()`;
6. publish one complete global snapshot, or retain the prior snapshot on failure.

`AccessConfigWatcher::commit_ready_project()` is the single watcher publication path. It owns
publish-error reporting, successful update counters, `published_generation`, the snapshot
observer, and final Project state. A stale generation never reaches it.

`RouteConfigStore::apply()` remains a synchronous convenience for unit tests and offline/local
callers. It internally uses `try_ready()` and rejects a candidate that requires asynchronous
service readiness. It does not bypass the Ready type.

## Compatibility status semantics

| Final status       | Snapshot effect                                | Successful publication count |
| ------------------ | ---------------------------------------------- | ---------------------------- |
| `IgnoredEmpty`     | Retain the current snapshot                    | No                           |
| `VersionUnchanged` | Retain the current snapshot                    | No                           |
| `Unloaded`         | Remove the Project routes; retain last version | Yes                          |
| `Published`        | Replace/add the complete Project snapshot      | Yes                          |
| `ProjectRemoved`   | Remove Project and its remembered version      | Yes, list reconciliation     |

Decode, compile, service-ready, and global Host-conflict failures do not replace the current
snapshot. A successful rnacos read, Prepared candidate, or Ready transition is still not control
plane activation evidence; activation remains unknown until typed per-instance evidence exists.

## Scope

The TLS store is not part of this typestate. Its prepared TLS value has no asynchronous external
readiness dependency, and `commit()` reclassifies version and digest atomically on the owner loop.
The Project route typestate adds no generic executor, future, synchronization primitive, or Fiber
API.
