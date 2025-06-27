#include <upcie/upcie.h>

int
func_printer(struct pci_func *func, void *callback_arg)
{
	(void)callback_arg; ///< For compiler-warnings...

	pci_func_pr(func);

	return PCI_SCAN_ACTION_RELEASE_FUNC;
}

int
main(void)
{
	int err;

	err = pci_scan(func_printer, NULL);
	if (err) {
		perror("pci_scan()");
	}

	return -err;
}
