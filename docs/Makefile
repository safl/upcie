.DEFAULT_GOAL := help

.PHONY: help deps html serve clean

help:
	@echo "uPCIe docs:"
	@echo "  deps    pipx install ./tooling  (provides upcie-docs-{serve,build-html})"
	@echo "  html    upcie-docs-build-html   -> _build/html/"
	@echo "  serve   upcie-docs-serve        (live rebuild on http://localhost:8000)"
	@echo "  clean   remove _build/"

deps:
	pipx install ./tooling

html:
	upcie-docs-build-html

serve:
	upcie-docs-serve

clean:
	rm -rf _build
