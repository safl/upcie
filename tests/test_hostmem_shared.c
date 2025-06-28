#include <upcie/upcie.h>
#include <stdio.h>
#include <time.h>

struct shared_memory {
  char message[256];
  int val;
};

/**
 * Allocate a hugepage, write to it, setup a counter and wait for it to go to 0
 */
int
hugepage_allocate(struct hostmem_state *state) {
  struct hostmem_hugepage hugepage = {0};
  const size_t size = 1024 * 1024 * 256;
  struct shared_memory *shared;
  int err;

  err = hostmem_hugepage_alloc(size, &hugepage, state);
  if (err) {
    printf("# hostmem_hugepage_alloc(); err(%d)\n", err);
  }

  shared = hugepage.virt;
  shared->val = 10;

  snprintf(shared->message, sizeof(shared->message), "%s", "Hello there!");

  hostmem_hugepage_pp(&hugepage);

  while (shared->val) {
    printf("info: {pid: %d, shared: {val: %d}}\n", getpid(), shared->val);
    sleep(1);
  }

  hostmem_hugepage_free(&hugepage);

  return 0;
}

int
hugepage_import(struct hostmem_state *state, const char *path) {
  struct hostmem_hugepage hugepage = {0};
  struct shared_memory *shared;
  int err;

  err = hostmem_hugepage_import(path, &hugepage, state);
  if (err) {
    printf("# hostmem_hugepage_import(); err(%d)\n", err);
    return err;
  }

  shared = hugepage.virt;

  hostmem_hugepage_pp(&hugepage);

  printf("info: {pid: %d, shared: {message: '%s'}}\n", getpid(),
         shared->message);

  while (shared->val) {
    printf("info: {pid: %d, shared: {val: %d}}\n", getpid(), shared->val);
    shared->val -= 1;
    sleep(1);
  }

  hostmem_hugepage_free(&hugepage);

  return 0;
}

int main(int argc, const char *argv[]) {
  struct hostmem_state state = {0};
  int err;

  err = hostmem_state_init(&state);
  if (err) {
    printf("# FAILED: hostmem_state_init(); err(%d)\n", err);
    return -err;
  }

  hostmem_state_pp(&state);

  switch (argc) {
  case 1:
    err = hugepage_allocate(&state);
    break;

  case 2:
    err = hugepage_import(&state, argv[1]);
    break;

  default:
    printf("invalid #args\n");
    return EINVAL;
  }

  return err;
}
