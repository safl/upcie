"""Sphinx configuration for uPCIe documentation."""

from __future__ import annotations

import re
from pathlib import Path

project = "uPCIe"
author = "Simon A. F. Lund"
copyright = f"2026, {author}"

# Single source of truth: the project version lives in meson.build.
_meson = (Path(__file__).resolve().parents[2] / "meson.build").read_text()
_match = re.search(r"version:\s*'([^']+)'", _meson)
release = _match.group(1) if _match else "0.0.0"
version = release

extensions = [
    "myst_parser",
    "sphinx_copybutton",
    "breathe",
]

myst_enable_extensions = [
    "deflist",
    "fieldlist",
    "tasklist",
    "linkify",
    "colon_fence",
]

source_suffix = {".md": "markdown"}
master_doc = "index"

templates_path = ["_templates"]
exclude_patterns = ["_build", "Thumbs.db", ".DS_Store"]

# Breathe: render the C headers from the Doxygen XML emitted under _build/doxy.
breathe_projects = {project: str(Path(__file__).resolve().parents[1] / "_build" / "doxy" / "xml")}
breathe_default_project = project
breathe_domain_by_extension = {"h": "c"}

html_theme = "furo"
html_title = "User-space PCIe libraries"
html_static_path = ["_static"]
html_logo = "_static/upcie.png"
html_theme_options = {
    "sidebar_hide_name": True,
}
