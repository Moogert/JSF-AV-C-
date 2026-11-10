# Writing C++ with the JSF AV standard

Use this guide with the complete standard and the target project's requirements. It describes a working method; it does not establish compliance by itself.

## Establish the target before choosing a design

Record the compiler, target platform, C++ language level, selected profile, and applicable project requirements. The default `literal` profile follows the reference PDF. `modern` changes only the interpretations listed in each catalog entry's `modern_note`; it is not blanket permission to use every modern feature.

The checker itself is a desktop C++17 application. Its use of facilities such as exceptions and dynamic allocation is not evidence that those facilities are permitted in a target governed by the standard.

## Turn rules into concrete implementation decisions

Consult `data/rules.json` for every rule's ID, PDF page, description, detection status, and profile note. Use `jsfav explain ID --format json` to retrieve an entry. Read the full rule and surrounding explanations in the original PDF before relying on an exception or an ambiguous extracted description.

For each change, consider the applicable rules across these areas:

| Area | Questions to resolve |
|---|---|
| Requirements and design | What requirement does the change satisfy? What design and review evidence is required? |
| Interfaces and data | Are ownership, initialization, type ranges, lifetimes, and valid states explicit? |
| Functions and control flow | Does each function have a clear responsibility and meet applicable size and complexity constraints? Are paths bounded and understandable? |
| Language features | Are casts, macros, allocation, exceptions, inheritance, and library facilities permitted for this target? |
| Source conventions | Do names, includes, file suffixes, comments, and layout follow the selected interpretation? |
| Verification | What source checks, tests, design reviews, and approval records establish the required properties? |

These questions are prompts for reviewing the entire standard, not a replacement subset of its 233 rule IDs. Consult review entries even when automatic checks are quiet.

## Write comments that preserve engineering intent

Document public interfaces where callers find them. Include applicable preconditions, valid ranges and units, returned values, ownership transfers, lifetime requirements, failure behavior, and concurrency constraints. Explain algorithms and tradeoffs that a maintainer cannot infer safely from the code.

Use `//` comments for first-party C++ when following the literal comment rule. Keep rationale close to the decision. For an exception, identify the affected rule, scope, reason, and real approval reference if one exists. Do not invent a reference. Keep user-facing guides focused on using the tool and maintainer documentation focused on its implementation.

```cpp
// Caller retains ownership of samples; the buffer must outlive this call.
// sample_count is the number of initialized elements, not the buffer capacity.
// Project review must establish the permitted count range before deployment.
```

This illustrates useful contract wording, not an approved contract for a particular system. Replace assumptions with verified project requirements. Comments cannot establish runtime safety or replace tests.

## Verify and report the actual evidence

1. Build the target with its configured flags and refresh `compile_commands.json`.
2. Run `jsfav scan PROJECT -p BUILD --format json --output findings.json` using the agreed profile. Specify `--profile modern` only when selected.
3. Inspect `partial`, `coverage.executedAutomatedRules`, `summary`, and individual findings. An analysis error must be resolved or reported as incomplete work.
4. Correct supported violations, examine allowed exceptions, and review all rules requiring project or human evidence. Review items remain unresolved until evidence is recorded independently.
5. Apply only authorized, scoped deviations with actual justification and approval metadata. Suppressed findings remain visible in the report.
6. Report the affected rule IDs, checks performed, manual evidence, deviations, and remaining gaps. A successful process exit is not proof of compliance.

For maintaining automatic checks, add targeted compliant and violating cases plus relevant exception cases. Include negative cases that could otherwise produce false positives. Keep the catalog, extraction tool, fixtures, and documentation consistent.
