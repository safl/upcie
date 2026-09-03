cpu:
	gcc -o dmabuf_import_cpu dmabuf_import_cpu.c

gpu:
	gcc -o dmabuf_import_gpu dmabuf_import_gpu.c -lcuda -I/usr/local/cuda/include

gpu-hip:
	gcc -o dmabuf_import_gpu_hip dmabuf_import_gpu_hip.c -I/opt/rocm/include \
		-L/opt/rocm/lib -lamdhip64

# Target-side validation program. It compiles standalone against the system
# headers, but running it needs the stock /dev/udmabuf (for UDMABUF_CREATE)
# and the loaded module at /dev/dmabuf_import.
tests: bounds

bounds:
	gcc -o test/dmabuf_import_bounds test/dmabuf_import_bounds.c
