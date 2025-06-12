driver_script = custom_target(
  'pcie-device-driver-script',
  input: 'devbind.py',
  output: 'devbind',
  command: [find_program('cp'), '@INPUT@', '@OUTPUT@'],
  install: true,
  install_dir: get_option('bindir')
)

hugepages_script = custom_target(
  'hugepages-inspect-and-setup-script',
  input: 'hugepages.py',
  output: 'hugepages',
  command: [find_program('cp'), '@INPUT@', '@OUTPUT@'],
  install: true,
  install_dir: get_option('bindir')
)