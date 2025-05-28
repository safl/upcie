test_sources = files(
  'test_vfioctl.c',
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

