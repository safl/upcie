BUILD_DIR ?= builddir
VERSION ?= 0.2.5

.PHONY: all setup build test install uninstall clean docs

all: clean setup build install test

bump:
	@python3 -c "import sys, re, pathlib; [p.write_text(re.sub(r'(@version)\s+.*', rf'\1 {sys.argv[1]}', p.read_text()), encoding='utf-8') for p in pathlib.Path('.').rglob('*.h') if '@version' in p.read_text()]" $(VERSION)

setup:
	meson setup $(BUILD_DIR)

build:
	meson compile -C $(BUILD_DIR)

install:
	meson install -C $(BUILD_DIR)

guest:
	@cd cijoe && cijoe \
		--config configs/cijoe-config.toml \
		--workflow workflows/guest_setup.yaml \
		-l \
		-m

test:
	@cd cijoe && cijoe \
		--config configs/cijoe-config.toml \
		--workflow workflows/guest_test.yaml \
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
