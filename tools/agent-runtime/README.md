# Audastra Agent runtime

Native Qt owns the settings, conversation view and live editor tools. This small
NDJSON worker owns the PI model/tool loop; it never opens or writes map files.

The locked runtime uses `@earendil-works/pi-agent-core` and `pi-ai` 1.0.0 (MIT),
Node >=22.19.0 and esbuild 0.28.2. Run `npm ci --ignore-scripts` and `npm run build`.
`dist/worker.bundle.mjs` is a standalone Node ESM bundle for the four supported
protocols. The build collects the actual included dependency notices and fails
when a notice is missing. Node and its complete LICENSE are packaged separately.
PI license was obtained from its npm release gitHead
`a13d35a742c6ef8462812a28fbe1d8c8b7431c32`; the standardwebhooks notice comes from
its 1.1.1 gitHead `b4d2c14fc5b4ccff3ff271e3b087dff812254c59`.

`start` input contains selected provider/model, in-memory key, tools, system prompt,
prompt and prior messages. The host waits for `ready`, dispatches `tool_call` through
the same live editor broker as HTTP, and returns `tool_result`. `stop` aborts the
model and pending tool wait; already completed editor transactions remain intact.
Keys are absent from argv/environment, normal settings and event/history output.

Events include streamed text, actual tool lifecycle, token usage, session state and
run result. Token usage is the provider's report; cost is unavailable rather than
the zero placeholder PI needs internally. Context compression removes only complete
old user rounds and keeps the tool request/result pairs; full host records remain.

Tests use an original loopback SSE fixture without real accounts or author keys:
`npm test`. To test the portable bundle, set `FORGE_RUNTIME_TEST_ENTRY` to
`../dist/worker.bundle.mjs` for that test process. These tests prove the protocol/tool
loop, not a particular paid provider or Warcraft game behavior.
