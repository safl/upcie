example_sources = files(
  'upcie_nvme_driver.c',
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
