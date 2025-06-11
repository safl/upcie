test_sources = files(
  'test_vfioctl.c',
  'test_hostmem_shared.c',
  'test_hostmem_heap.c',
  'test_pci_bars.c',
  'test_pci_scan.c',
  'test_nvme_init.c',
)

incdir = include_directories('../include')

foreach src : test_sources
  bin_name = fs.stem(src)
  exe = executable(
    bin_name,
    src,
    include_directories: incdir,
    install: true,
  )
endforeach

