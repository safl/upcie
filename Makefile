BUILD_DIR ?= builddir
VERSION ?= 0.4.0

.PHONY: all config build test install uninstall clean docs

all: clean config build install test

bump:
ifndef NEW_VERSION
	$(error Usage: make bump NEW_VERSION=x.y.z)
endif
	@echo "Bumping version to $(NEW_VERSION)"
	@sed -i "s/^\(VERSION ?= \).*/\1$(NEW_VERSION)/" Makefile
	@sed -i "s/\(version: '\)[^']*'/\1$(NEW_VERSION)'/" meson.build
	@python3 -c "import sys, re, pathlib; [p.write_text(re.sub(r'(@version)\s+.*', rf'\1 {sys.argv[1]}', p.read_text()), encoding='utf-8') for p in pathlib.Path('.').rglob('*.h') if '@version' in p.read_text()]" $(NEW_VERSION)
	@echo "Done"

config:
	meson setup $(BUILD_DIR)

config-debug:
	meson setup $(BUILD_DIR) --buildtype=debug

build:
	meson compile -C $(BUILD_DIR)

install:
	meson install -C $(BUILD_DIR)

guest:
	@cd cijoe && cijoe \
		tasks/guest_setup.yaml \
		--config configs/cijoe-config.toml \
		-l \
		-m

test:
	@cd cijoe && cijoe \
		tasks/guest_test.yaml \
		--config configs/cijoe-config.toml \
		-l \
		-m

uninstall:
	cd $(BUILD_DIR) && meson --internal uninstall

#
# This assumes a pipx-based Python environment is available e.g.:
# 
#   pipx install sphinx
#   pipx inject sphinx breathe
#   pipx inject sphinx furo
# 
docs:
	doxygen docs/Doxyfile
	cd docs; make html
	

clean:
	rm -rf $(BUILD_DIR)
