cpu:
	gcc -o udmabuf_import_cpu udmabuf_import_cpu.c

gpu:
	gcc -o udmabuf_import_gpu udmabuf_import_gpu.c -lcuda -I/usr/local/cuda/include

# Target-side validation programs. They compile standalone against the system
# headers, but running them needs the patched /dev/udmabuf (for UDMABUF_CREATE)
# and the loaded module at /dev/udmabuf_import.
tests: parity bounds

parity:
	gcc -o test/udmabuf_import_parity test/udmabuf_import_parity.c

bounds:
	gcc -o test/udmabuf_import_bounds test/udmabuf_import_bounds.c
