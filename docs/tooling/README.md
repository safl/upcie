# upcie-docs

Build and dev-server tooling for the uPCIe project documentation.

Provides three console scripts:

- `upcie-docs-serve`: live-rebuild dev server on `http://localhost:8000`.
- `upcie-docs-build-html`: one-shot HTML build to `docs/_build/html/`.
- `upcie-docs-build-pdf`: one-shot PDF build via LaTeX to
  `docs/_build/latex/upcie.pdf`.

## Install

From the uPCIe repo root:

```bash
pipx install ./docs/tooling
```

The PDF build additionally requires a LaTeX distribution (`texlive`
variants on Linux, `latexmk` via MacTeX on macOS).
