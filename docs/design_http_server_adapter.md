# `http_server.c` as an adapter — the decisions that still bind

**Status: current.** The restructure is done (2026-09-05): the enum→string
accessors moved to their owning modules, `-Werror=switch` is scoped per-source
so a new enumerator without a name fails the build, and `whyText` reaches
`/api/pack/status`. That plan and its acceptance record have been deleted.

Three things survive it, because each is a decision that would otherwise be
re-litigated.

## 1. An enum's name belongs to the module that owns the enum

`http_server.c` and `cmd_parser.c` had each grown their own spelling of
`ePackCondition`, and the HTTP side emitted `ePackAbsentReason` as a bare
integer while the CLI rendered it as a sentence — so the most diagnostic field
the pack module has was reachable only from a console needing physical access,
which on a tunnel-only board is backwards.

There are now fourteen accessors (`Pack_CondName`, `Crash_TypeName`,
`Modbus_PortName`, `SysMon_StateName`, `NvDb_UserName`, …). Write each as a
`switch` whose fallback sits **after** the switch, never in a `default:` — a
`default:` satisfies `-Wswitch` and defeats the whole mechanism. Scope
`-Werror=switch` per-source (`ENUM_NAME_SOURCES` in `CMakeLists.txt`); never
globally, because lwIP, FreeRTOS, the HAL, trice and wireguard-lwip share
`CMAKE_C_FLAGS`. Range-check and return `"?"`, never `NULL`.

Adding one is about a day's work and it is where this class of fix goes.

---

## 2. Serialisation — decided, do not re-open

Whether status rendering should move behind a byte sink
(`Modbus_ConfigExport(fModbusByteSink, ctx)` is the in-tree precedent) was
reviewed and **answered no**. Recorded so it is not revisited:

1. **A sink cannot produce `Content-Length` for live status.**
   `handle_modbus_cfg_download` (1969-1991) makes the sink work by running the
   export *twice* — legal only because "export is a pure function of the
   region". Status is not pure: `Pack_GetState` recomputes `age_ms[]` from
   `osKernelGetTickCount()` on every call, so two passes disagree in digit
   count. The alternatives are buffer anyway (today's `pvPortMalloc` plus an
   indirection — zero gain), snapshot 8 × `sPackState` ≈ 960 B first (a *new*
   1 KB allocation), or drop `Content-Length` (a wire change on every status
   endpoint). There is no fourth.
2. **JSON key names are the envelope.** Moving them into `pack.c` would swap
   one ownership violation for its mirror image and give `pack.h` a
   compatibility obligation to every HTTP client. A config export serialises
   the module's own persisted document; a status view is a projection chosen by
   a consumer, and MQTT/HA will want a different one.

So this file keeps building its own JSON — using `Json_Cat` from `Shared/Json/`, not raw `snprintf`.

---

## 3. The file's size — a separate question, deliberately deferred

Splitting the file, replacing the 66-branch chain with a route table, and
moving the ~279-line embedded UI out are all defensible. **None is in scope
here**, for one reason: they relocate coupling without removing it, and doing
them first would make the ownership change in §2 harder to review.

If §2 and the JSON task both land, the file shrinks as a *consequence*. Judge
the remainder then, against evidence rather than against the line count. The
plumbing (`sConnStream`, `cs_*`, `send_all`, `header_value`,
`parse_content_length`) is genuinely separable and would be the natural first
cut if one is wanted — it is real HTTP, has no domain knowledge, and is the
part most likely to be reused if a second listener ever exists.

---

## 4. Not yet verified on hardware

Run it on hardware: `GET /api/pack/status` for `whyText`, `GET
/api/crash/latest` for an unchanged `type` and `state`, `pack status` on the
CLI for `why=online`, and the web UI's pack card for the reason beside a
non-`online` condition.
