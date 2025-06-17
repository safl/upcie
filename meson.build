project('upcie', 'c',
  version: '0.1.0',
  license: 'BSD-3-Clause'
)

fs = import('fs')

install_headers(
  [
    'include/upcie/upcie.h',
    'include/upcie/hostmem.h',
    'include/upcie/hostmem_hugepage.h',
    'include/upcie/hostmem_dma.h',
    'include/upcie/hostmem_heap.h',
    'include/upcie/mmio.h',
    'include/upcie/nvme.h',
    'include/upcie/nvme_controller.h',
    'include/upcie/nvme_queue.h',
    'include/upcie/pci.h',
    'include/upcie/vfioctl.h',
  ],
  subdir: 'upcie'
)

# Optional dependency for other Meson projects
upcie_dep = declare_dependency(
  include_directories: include_directories('include')
)

# Add the test programs
subdir('scripts')
subdir('tests')
