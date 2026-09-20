#!/usr/bin/env python3
"""Point the README's Deploy on EdgeZ button at the current repository."""

from __future__ import annotations

import os
import re
from pathlib import Path
from urllib.parse import quote


repository = os.environ.get("GITHUB_REPOSITORY", "")
if not re.fullmatch(r"[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+", repository):
    raise SystemExit("GITHUB_REPOSITORY must be in owner/repository format")

readme = Path("README.md")
contents = readme.read_text(encoding="utf-8")
deploy_url = (
    "https://appwrite.edgez.ai/console/deploy?repo="
    + quote(f"https://github.com/{repository}", safe="")
)
pattern = re.compile(
    r"(\[!\[Deploy on EdgeZ\]\([^\n)]+\)\]\()"
    r"https://appwrite\.edgez\.ai/console/deploy\?repo=[^\s)]+"
    r"(\))"
)
updated, replacements = pattern.subn(rf"\g<1>{deploy_url}\g<2>", contents)

if replacements != 1:
    raise SystemExit(f"expected one Deploy on EdgeZ button, found {replacements}")

if updated != contents:
    readme.write_text(updated, encoding="utf-8")
    print(f"Updated Deploy on EdgeZ button for {repository}")
else:
    print(f"Deploy on EdgeZ button already targets {repository}")
