"""Regenerate the checked-in rule inventory from the supplied JSF AV PDF.

Usage: python tools/extract_catalog.py /path/to/JSF-AV-rules.pdf data/rules.json
This is a maintainer tool; the released checker does not require Python or the PDF.
"""

import json
import re
import sys
from pathlib import Path

from pypdf import PdfReader


AUTOMATED = {
    "11", "12", "14", "18", "26", "29", "30", "31", "32", "33",
    "35", "41", "43", "47", "50", "51", "52", "53", "54", "59",
    "61", "99", "108", "110", "113", "126", "140", "149", "150",
    "152", "153", "154", "159", "185", "188", "189", "190", "191",
    "200", "208",
}
MODERN_NOTES = {
    "8": "Review conformance against the C++ standard selected by each compile command rather than the 2002 edition.",
    "26": "Allows #pragma once in addition to the literal directive list.",
    "27": "Allows #pragma once as an alternative header inclusion guard.",
    "32": "Allows standard C++ headers and project headers without a .h suffix.",
    "33": "Allows quoted project includes as well as angle-bracket includes.",
    "35": "Allows either a macro guard or #pragma once to prevent duplicate inclusion.",
    "53": "Allows .h, .hpp, .hh, and .hxx header suffixes.",
    "54": "Allows .cpp, .cc, .cxx, and .c++ implementation suffixes.",
    "175": "Use nullptr instead of the legacy plain-0 null pointer spelling.",
    "209": "Review use of fixed-width types from <cstdint> against the selected target and project policy.",
}


def main() -> None:
    if len(sys.argv) != 3:
        raise SystemExit(__doc__)
    pages = [page.extract_text() or "" for page in PdfReader(sys.argv[1]).pages]
    # The normative rules end on physical PDF page 63; later references are appendix examples.
    heading = re.compile(r"(?m)^AV Rule\s+(\d+(?:\.\d+)?)\b([^\n]*)\n")
    entries = []
    seen = set()
    for page_no, page in enumerate(pages[:63], start=1):
        matches = list(heading.finditer(page))
        for index, match in enumerate(matches):
            rule_id = match.group(1)
            if match.group(2).lstrip().startswith("through AV Rule"):
                continue
            if rule_id in seen:
                continue
            seen.add(rule_id)
            end = matches[index + 1].start() if index + 1 < len(matches) else len(page)
            heading_tail = re.sub(r"^\s*\([^)]*\)", "", match.group(2)).strip()
            body = heading_tail + "\n" + page[match.end() : end]
            body = re.split(
                r"(?m)^\s*(?:Rationale:|Note:|Notes:|Example:|Examples:|Exception:|"
                r"4\.\d+(?:\.\d+)*\s+[A-Z])",
                body,
                maxsplit=1,
            )[0]
            description = " ".join(body.split()).replace("\ufffd", "'")
            if not description:
                continuation = pages[page_no] if page_no < len(pages) else ""
                next_heading = heading.search(continuation)
                if next_heading:
                    continuation = continuation[: next_heading.start()]
                continuation = re.sub(r"Doc\. No\.[^\n]*|Date:[^\n]*", "", continuation)
                continuation = re.sub(r"(?m)^\s*\d{1,3}\s*$", "", continuation)
                description = " ".join(continuation.split()).replace("\ufffd", "'")
            if not description:
                raise ValueError(f"Rule {rule_id} has no extracted description")
            entries.append(
                {
                    "id": rule_id,
                    "pdf_page": page_no,
                    "description": description,
                    "detection": "automated" if rule_id in AUTOMATED else "review",
                    "modern_note": MODERN_NOTES.get(rule_id, "Same as literal profile."),
                    "review_reason": (
                        "Validated source check implemented."
                        if rule_id in AUTOMATED
                        else "No validated automatic check in this release; inspect source and external evidence."
                    ),
                }
            )
    if len(entries) != 233:
        raise ValueError(f"Expected 233 distinct rules, found {len(entries)}")
    Path(sys.argv[2]).write_text(
        json.dumps({"schema_version": 1, "rules": entries}, indent=2, ensure_ascii=False) + "\n",
        encoding="utf-8",
    )


if __name__ == "__main__":
    main()
