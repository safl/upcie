project('upcie', 'c',
  version: '0.1.0',
  license: 'BSD-3-Clause',
  default_options: [
    'c_std=c11',
    'warning_level=2',
    'buildtype=release',
  ],
)

fs = import('fs')

conf_data = configuration_data()
if get_option('debug-logging').enabled()
  conf_data.set('UPCIE_DEBUG_ENABLED', true)
elif get_option('debug-logging').disabled()
  conf_data.set('UPCIE_DEBUG_ENABLED', false)
else
  conf_data.set('UPCIE_DEBUG_ENABLED', get_option('buildtype') == 'debug')
endif

conf = configure_file(
  configuration : conf_data,
  output : 'upcie_config.h'
)
add_project_arguments(['-include', 'upcie_config.h'], language: 'c')

install_headers(
  [
    'include/upcie/bitfield.h',
    'include/upcie/hostmem_dma.h',
    'include/upcie/hostmem.h',
    'include/upcie/hostmem_heap.h',
    'include/upcie/hostmem_hugepage.h',
    'include/upcie/mmio.h',
    'include/upcie/nvme/nvme_command.h',
    'include/upcie/nvme/nvme_controller.h',
    'include/upcie/nvme/nvme_mmio.h',
    'include/upcie/nvme/nvme_qid.h',
    'include/upcie/nvme/nvme_qpair.h',
    'include/upcie/nvme/nvme_request.h',
    'include/upcie/pci.h',
    'include/upcie/upcie.h',
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
subdir('example')
subdir('tests')
