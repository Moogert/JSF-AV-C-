# Agent use of `jsfav`

For interactive local use, a user can launch the browser UI with `jsfav serve` (or the packaged start script). It is localhost-only; the browser API lives under `/api/v1/` and requires the per-session `X-JSFAV-Token` from the printed URL. The UI supports scan, review, and JSON/SARIF export. Its session is temporary and never edits project files. Agents should continue using the CLI below for deterministic, noninteractive jobs.

1. Find the project's `compile_commands.json`. If absent, run a limited path scan and report `partial: true` to the user.
2. Run `jsfav scan PROJECT_PATH -p BUILD_DIR --format json --output findings.json`. Use `--profile modern` only when the task asks for the documented modern interpretation.
3. Read `summary` and `coverage` first. Read each `result.classification`: `violation` is an implemented source check, `review` needs independent verification, and `coverage-warning` limits what the scan established. A `suppressed: true` violation is still a finding with deviation metadata.
4. Use `jsfav explain RULE_ID --format json` for a rule's source page, description, detection status, and modern interpretation. The original PDF controls if extraction is ambiguous.
5. Never report that the project passed all 233 rules based on a zero process exit code. Exit 0 means the scan completed; exit 2 means input or analysis failed.

The commands are noninteractive. JSON and SARIF have stable top-level schema version 1 and SARIF version 2.1.0. Quote paths containing spaces. Keep the compile database current after build changes.
