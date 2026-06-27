"""Console-script entry points for the uPCIe docs tooling."""

from __future__ import annotations

import subprocess
import sys
from pathlib import Path


def _docs_root() -> Path:
    """Locate the docs root: the directory containing ``src/conf.py``.

    The commands are expected to run from inside ``upcie/docs/`` (the
    directory holding this ``tooling/`` package and the ``src/`` Sphinx
    tree). Falls back to the parent in case the user invoked from
    ``upcie/docs/tooling``.
    """
    cwd = Path.cwd()
    for candidate in (cwd, cwd.parent):
        if (candidate / "src" / "conf.py").exists():
            return candidate
    sys.exit("upcie-docs: could not find src/conf.py; run from the docs directory (e.g. upcie/docs)")


def _run_doxygen(root: Path) -> None:
    """Generate the Doxygen XML that Breathe renders into the API reference."""
    # Doxygen does not create a nested OUTPUT_DIRECTORY, so ensure it exists
    # (the docs/_build tree is absent on a clean checkout, e.g. in CI).
    (root / "_build" / "doxy").mkdir(parents=True, exist_ok=True)
    subprocess.run(["doxygen", "doxy.cfg"], cwd=root, check=True)


def build_html() -> None:
    root = _docs_root()
    src = root / "src"
    out = root / "_build" / "html"
    _run_doxygen(root)
    subprocess.run(
        [sys.executable, "-m", "sphinx", "-W", "-b", "html", str(src), str(out)],
        check=True,
    )


def build_pdf() -> None:
    root = _docs_root()
    src = root / "src"
    latex_out = root / "_build" / "latex"
    _run_doxygen(root)
    subprocess.run(
        [sys.executable, "-m", "sphinx", "-b", "latex", str(src), str(latex_out)],
        check=True,
    )
    subprocess.run(["make"], cwd=latex_out, check=True)


def serve() -> None:
    root = _docs_root()
    src = root / "src"
    out = root / "_build" / "html"
    _run_doxygen(root)
    subprocess.run(
        [sys.executable, "-m", "sphinx_autobuild", "--port", "8000", str(src), str(out)],
        check=True,
    )
