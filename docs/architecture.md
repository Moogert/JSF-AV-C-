# Architecture and maintenance notes

## Scanner pipeline

`src/main.cpp` owns CLI parsing, catalog loading, source selection, analysis, and output serialization. A scan validates deviations, collects source files, runs file/line checks, optionally parses compilation commands with libclang, adds explicit review items, sorts and deduplicates findings, and serializes text, JSON, or SARIF.

The catalog and `tools/extract_catalog.py` must agree on automated status and modern interpretations. Updating the JSON alone can be undone by regeneration. Preserve the complete rule inventory even when analysis cannot establish a rule's compliance.

Finding identity consists of rule, classification, source path, line, and column. This avoids duplicating findings from headers included by multiple translation units. Deviation matching keeps the original result and attaches supplied metadata rather than deleting evidence.

Parsing temporarily changes the process working directory to honor relative compiler arguments. Standalone CLI scanning is sequential. Do not introduce concurrent in-process parsing without removing or coordinating that process-wide state. The web server avoids sharing this state by running the CLI in a child process.

Without a compilation database, supported lexical checks run and skipped semantic checks become review items. An unparsed header can also make the scan partial. The current `executedAutomatedRules` value uses a coarse partial/full distinction; inspect coverage warnings when interpreting a partial scan. Do not present it as proof that each rule ran on every file.

## Local server and browser

`src/server.cpp` serves the files in `web/` and a versioned API. The server binds to IPv4 loopback on port 8765. API handlers check the local Host header and session token. Static assets are public on that loopback address; project data requires authorization.

There is one retained job. A mutex protects job state, results, and project scope. Starting a second job while one is running returns a conflict. A worker invokes the existing CLI with explicit arguments, captures its output in memory through a pipe, and publishes either results or an operational error. The browser polls for completion; there is no cancellation or persisted scan history.

Windows process arguments require escaping compatible with `CreateProcessW`. Linux uses `execv` with an argument vector. Neither scan path uses a shell command assembled from user input. Preserve these boundaries when extending launch behavior.

Source previews canonicalize the requested file and check that it remains beneath the selected project, including after symlink resolution. Do not replace that check with a string prefix comparison. The browser renders source and findings as text, not HTML.

The web export path builds SARIF from retained JSON rather than rescanning. Changes to result fields or SARIF semantics should be reviewed in both `src/main.cpp` and `src/server.cpp` so CLI and browser exports continue to agree.

## Build and verification

CMake builds the executable, copies runtime assets beside it, and installs platform launchers, the catalog, web assets, Clang resources, runtime dependencies, and license notices into packages. `tests/test_cli.py` covers scanner behavior; `tests/test_server.py` covers API authorization, real scans, previews, exports, and invalid projects. Native dialog behavior and visual layout still need desktop review when changed.

The server tests set `JSFAV_NO_BROWSER=1` to avoid opening a desktop browser. They use port 8765, so a running UI instance can prevent the test server from starting. Stop that instance before running the tests.
