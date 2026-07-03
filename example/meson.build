example_sources = files(
  'upcie_nvme_driver.c',
  'dmamem_memfd_smoketest.c',
  'dmamem_nvme_identify_smoketest.c',
  'dmamem_vfio_bar_smoketest.c',
  'dmamem_nvme_vram_smoketest.c',
)

incdir = include_directories('../include', '.')

foreach src : example_sources
  bin_name = fs.stem(src)
  exe = executable(
    bin_name,
    src,
    include_directories: incdir,
    install: true,
  )
endforeach
