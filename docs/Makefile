.DEFAULT_GOAL := help

.PHONY: help deps html pdf serve clean

help:
	@echo "uPCIe docs:"
	@echo "  deps    pipx install ./tooling  (provides upcie-docs-{serve,build-html,build-pdf})"
	@echo "  html    upcie-docs-build-html   -> _build/html/"
	@echo "  pdf     upcie-docs-build-pdf    -> _build/latex/upcie.pdf  (requires LaTeX)"
	@echo "  serve   upcie-docs-serve        (live rebuild on http://localhost:8000)"
	@echo "  clean   remove _build/"

deps:
	pipx install ./tooling

html:
	upcie-docs-build-html

pdf:
	upcie-docs-build-pdf

serve:
	upcie-docs-serve

clean:
	rm -rf _build
