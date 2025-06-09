project('upcie', 'c',
  version: '0.1.0',
  license: 'BSD-3-Clause'
)

fs = import('fs')

incdir = include_directories('include')

install_headers('include/hostmem.h')
install_headers('include/pci.h')
install_headers('include/vfioctl.h')

# Optional dependency for other Meson projects
upcie_dep = declare_dependency(
  include_directories: incdir
)

# Add the test programs
subdir('scripts')
subdir('tests')
