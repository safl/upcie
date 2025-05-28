driver_script = custom_target(
  'vfioctl-dirver-script',
  input: 'devbind.py',
  output: 'devbind',
  command: [find_program('cp'), '@INPUT@', '@OUTPUT@'],
  install: true,
  install_dir: get_option('bindir')
)
