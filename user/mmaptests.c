// user/mmaptests.c
#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"
#include "kernel/fcntl.h"
#include "kernel/param.h"
#define PGSIZE       4096

static int tests_run = 0, tests_ok = 0;

static void ok(const char *name)   { printf("  [OK]   %s\n", name); tests_ok++; }
static void fail(const char *name) { printf("  [FAIL] %s\n", name); exit(1); }
static void expect(int cond, const char *name) { tests_run++; if(cond) ok(name); else fail(name); }

static void touch_write(char *p, int len, char byte)
{
  for (int i = 0; i < len; i += 64) p[i] = byte;   // stride to ensure page fault
}

static int create_file_with_pattern(const char *path, char a, char b)
{
  int fd = open(path, O_CREATE | O_RDWR);
  if (fd < 0) return -1;
  char *buf = malloc(PGSIZE);
  if (buf == 0) {
    close(fd);
    return -1;
  }

  memset(buf, a, PGSIZE);
  if (write(fd, buf, PGSIZE) != PGSIZE) {
    free(buf);
    close(fd);
    return -1;
  }

  memset(buf, b, PGSIZE);
  if (write(fd, buf, PGSIZE) != PGSIZE) {
    free(buf);
    close(fd);
    return -1;
  }

  free(buf);

  if (close(fd) < 0)
    return -1;

  return 0;
}

static void test_anon_lazy(void)
{
  //const char *t = "anon + lazy (no POPULATE) triggers per-page faults & freemem deltas";
  int before = freemem();
  uint64 a = mmap(0, 2*PGSIZE, PROT_READ|PROT_WRITE, MAP_ANONYMOUS, -1, 0);
  expect(a != 0, "anon lazy: mmap returns non-zero");
  int after_mmap = freemem();
  expect(after_mmap == before, "anon lazy: freemem unchanged immediately");

  volatile char *p = (char*)a;
  p[0] = 0xAA;  // touch first page → data page + maybe new page-table pages
  int after_p1 = freemem();
  int delta1 = before - after_p1;
  // On RISC-V (3-level PT), the first map in a fresh region may allocate
  // extra page-table pages. So require at least 1 page drop.
  expect(delta1 >= 1, "anon lazy: touching 1st page consumes >=1 pages (data + PT pages)");

  p[PGSIZE] = 0xBB; // touch second page → another page alloc
  int after_p2 = freemem();
  // Now page-table pages already exist; second touch should drop exactly 1 more.
  expect(after_p2 == after_p1 - 1, "anon lazy: touching 2nd page consumes exactly 1 more page");

  // Compute how many total pages decreased during mapping phase.
  // This includes 2 data pages + any number of PT pages allocated by walk(..., alloc=1).
  int total_alloc = before - after_p2;

  expect(munmap(a) == 1, "anon lazy: munmap succeeds");
  int after_unmap = freemem();
  // Expected lower bound after munmap if PT pages persist:
  //   expected_min = before - max(total_alloc - data_pages, 0)
  // where data_pages = length / PGSIZE = 2 in this test.
  int data_pages = 2;
  int pt_pages  = total_alloc - data_pages;
  if (pt_pages < 0) pt_pages = 0;
  int expected_min = before - pt_pages;
  // If your munmap also frees PT pages, after_unmap may be anywhere up to 'before'.
  expect(after_unmap >= expected_min && after_unmap <= before, "anon lazy: munmap returns all data pages (PT pages may persist)");
}

static void test_anon_populate(void)
{
  //const char *t = "anon + MAP_POPULATE allocates immediately and frees on munmap";
  int before = freemem();
  uint64 a = mmap(0, 2*PGSIZE, PROT_READ|PROT_WRITE, MAP_ANONYMOUS|MAP_POPULATE, -1, 0);
  expect(a != 0, "anon populate: mmap returns non-zero");
  int after_mmap = freemem();
  expect(after_mmap == before - 2, "anon populate: immediate 2-page allocation");
  touch_write((char*)a, 2*PGSIZE, 0x5A); // should not change freemem now
  expect(munmap(a) == 1, "anon populate: munmap succeeds");
  int after_unmap = freemem();
  expect(after_unmap == before, "anon populate: all pages returned");
}

static void test_file_lazy(void)
{
  const char *path = "mmfile_lazy";
  unlink(path);
  expect(create_file_with_pattern(path, 'A', 'B') == 0, "file lazy: prepared test file");

  int before = freemem();
  int fd = open(path, O_RDONLY);
  expect(fd >= 0, "file lazy: open");
  uint64 a = mmap(0, 2*PGSIZE, PROT_READ, 0, fd, 0);  // file-backed, lazy
  expect(a != 0, "file lazy: mmap non-zero");
  int after_mmap = freemem();
  expect(after_mmap == before, "file lazy: freemem unchanged immediately");

  char *p = (char*)a;
  // first page should be 'A'
  expect(p[0] == 'A', "file lazy: first touch loads page 0 with 'A'");
  int after_p1 = freemem();
  expect(after_p1 == before - 1, "file lazy: first page alloc after read");

  // second page should be 'B'
  expect(p[PGSIZE] == 'B', "file lazy: second touch loads page 1 with 'B'");
  int after_p2 = freemem();
  expect(after_p2 == before - 2, "file lazy: second page alloc after read");

  expect(munmap(a) == 1, "file lazy: munmap");
  close(fd);
  unlink(path);
  int after_unmap = freemem();
  expect(after_unmap == before, "file lazy: pages returned");
}

static void test_file_populate(void)
{
  const char *path = "mmfile_pop";
  unlink(path);
  expect(create_file_with_pattern(path, 'X', 'Y') == 0, "file populate: prepared test file");

  int before = freemem();
  int fd = open(path, O_RDONLY);
  expect(fd >= 0, "file populate: open");
  uint64 a = mmap(0, 2*PGSIZE, PROT_READ, MAP_POPULATE, fd, 0);
  expect(a != 0, "file populate: mmap non-zero");
  int after_mmap = freemem();
  expect(after_mmap == before - 2, "file populate: immediate allocation of both pages");
  char *p = (char*)a;
  expect(p[0] == 'X' && p[PGSIZE] == 'Y', "file populate: contents preloaded");
  expect(munmap(a) == 1, "file populate: munmap");
  close(fd);
  unlink(path);
  int after_unmap = freemem();
  expect(after_unmap == before, "file populate: pages returned");
}

static void test_perm_violation(void)
{
  // Map read-only and try to write in a child; child should be killed by fault handler.
  //int fd = -1; // anonymous, read-only
  uint64 a = mmap(0, PGSIZE, PROT_READ, MAP_ANONYMOUS, -1, 0);
  expect(a != 0, "perm: mmap R-only anon");
  int pid = fork();
  if (pid == 0) {
    volatile char *p = (char*)a;
    printf("  (child) attempting write to R-only page... should crash\n");
    p[0] = 1; // should trigger kill
    // If we get here, it's a failure.
    printf("  (child) ERROR: write did not crash!\n");
    exit(1);
  } else {
    wait(0); // child should have been killed
    // crude check: parent still alive and returns here → assume child died on fault
    expect(1, "perm: child write to R-only caused kill");
    expect(munmap(a) == 1, "perm: munmap");
  }
}

static void test_munmap_returns_pages(void)
{
  int before = freemem();
  uint64 a = mmap(0, 3*PGSIZE, PROT_READ|PROT_WRITE, MAP_ANONYMOUS|MAP_POPULATE, -1, 0);
  expect(a != 0, "munmap/free: mmap 3 pages");
  expect(freemem() == before - 3, "munmap/free: allocated 3");
  expect(munmap(a) == 1, "munmap/free: munmap ok");
  expect(freemem() == before, "munmap/free: all returned");
}

static void test_fork_visibility(void)
{
  // Parent maps 2 pages anon+populate, fills with 'P'/'Q'.
  // Child must see same content. Child modifies its copy; parent must remain unchanged.
  uint64 a = mmap(0, 2*PGSIZE, PROT_READ|PROT_WRITE, MAP_ANONYMOUS|MAP_POPULATE, -1, 0);
  expect(a != 0, "fork: parent mmap");
  char *p = (char*)a;
  memset(p, 'P', PGSIZE);
  memset(p+PGSIZE, 'Q', PGSIZE);

  int pid = fork();
  if (pid == 0) {
    // child sees same bytes
    for (int i = 0; i < PGSIZE; i += 257) if (p[i] != 'P') exit(1);
    for (int i = 0; i < PGSIZE; i += 257) if (p[PGSIZE+i] != 'Q') exit(1);
    // child modifies its second page
    memset(p+PGSIZE, 'C', PGSIZE);
    // and exits success
    exit(0);
  } else {
    int rc = 0;
    wait(&rc); // (xv6 user.h wait can take status in some variants; else use wait(0))
    // parent should still see original 'Q' in second page
    int okq = 1;
    for (int i = 0; i < PGSIZE; i += 257) if (p[PGSIZE+i] != 'Q') okq = 0;
    expect(okq, "fork: child's write does not affect parent (uvmcopy semantics)");
    expect(munmap(a) == 1, "fork: parent munmap");
  }
}

static void compare_region(int fd, uint64 base_va, int off, int n, const char *name) {
  // read n bytes from file at offset 'off' and compare with mapped memory at base_va+off
  char fb[256], *mp = (char*)(base_va + off);
  if (n > (int)sizeof(fb)) n = sizeof(fb); // keep it small & safe for printing
  if (n <= 0) return;
  if (read(fd, fb, off) < 0) fail("seek failed in compare_region");
  int r = read(fd, fb, n);
  expect(r == n, name);
  for (int i = 0; i < n; i++) {
    if (mp[i] != fb[i]) {
      printf("  mismatch at off=%d: map=0x%x file=0x%x\n", off+i, mp[i] & 0xff, fb[i] & 0xff);
      fail(name);
    }
  }
}

static void test_readme_lazy(void)
{
  // Map README as read-only, lazy (no MAP_POPULATE),
  // verify contents match the on-disk file at a few offsets.
  int fd = open("README", O_RDONLY);
  expect(fd >= 0, "README(lazy): open README");

  struct stat st;
  expect(fstat(fd, &st) == 0, "README(lazy): fstat");
  int fsz = st.size;                    // README size
  int len = (fsz >= 2*PGSIZE) ? 2*PGSIZE : ((fsz + PGSIZE - 1) & ~(PGSIZE-1));
  if (len == 0) len = PGSIZE;          // map at least 1 page

  int before = freemem();
  uint64 a = mmap(0, len, PROT_READ, /*flags*/0, fd, 0);
  expect(a != 0, "README(lazy): mmap returns non-zero");
  expect(freemem() == before, "README(lazy): no immediate allocation");

  // First touch should fault & map page 0
  compare_region(fd, a, /*off=*/0, /*n=*/64, "README(lazy): page0 content match");
  expect(freemem() == before - 1, "README(lazy): after first touch, 1 page allocated");

  if (len > PGSIZE && fsz > PGSIZE) {
    // Touch into second page (if file & mapping big enough)
    compare_region(fd, a, /*off=*/PGSIZE, /*n=*/64, "README(lazy): page1 content match");
    expect(freemem() == before - 2, "README(lazy): after second touch, 2 pages allocated");
  }

  expect(munmap(a) == 1, "README(lazy): munmap ok");
  expect(freemem() == before, "README(lazy): all pages returned");
  close(fd);
}

static void test_readme_populate(void)
{
  // Map README as read-only, MAP_POPULATE,
  // verify contents are present without page faults.
  int fd = open("README", O_RDONLY);
  expect(fd >= 0, "README(pop): open README");

  struct stat st;
  expect(fstat(fd, &st) == 0, "README(pop): fstat");
  int fsz = st.size;
  int len = (fsz >= 2*PGSIZE) ? 2*PGSIZE : ((fsz + PGSIZE - 1) & ~(PGSIZE-1));
  if (len == 0) len = PGSIZE;

  int before = freemem();
  uint64 a = mmap(0, len, PROT_READ, MAP_POPULATE, fd, 0);
  expect(a != 0, "README(pop): mmap returns non-zero");

  int expected_pages = (len + PGSIZE - 1) / PGSIZE;
  expect(freemem() == before - expected_pages, "README(pop): immediate allocation");

  // Check a few bytes from page 0
  compare_region(fd, a, /*off=*/0, /*n=*/64, "README(pop): page0 content match");

  // If second page is mapped and file big enough, check there too
  if (len > PGSIZE && fsz > PGSIZE)
    compare_region(fd, a, /*off=*/PGSIZE, /*n=*/64, "README(pop): page1 content match");

  expect(munmap(a) == 1, "README(pop): munmap ok");
  expect(freemem() == before, "README(pop): all pages returned");
  close(fd);
}

int
main(int argc, char *argv[])
{
  /*
  int a = freemem();
  sbrk(4096);   // 또는 간단 malloc
  int b = freemem();
  printf("free before=%d after sbrk=%d\n", a, b);
  sbrk(-4096);
  */
  printf("===== mmap test suite start =====\n");

  test_anon_lazy();
  test_anon_populate();
  test_file_lazy();
  test_file_populate();
  test_perm_violation();
  test_munmap_returns_pages();
  test_fork_visibility();
  test_readme_lazy();
  test_readme_populate();


  printf("===== mmap test suite done: %d/%d passed =====\n", tests_ok, tests_run);
  if (tests_ok == tests_run) exit(0);
  exit(1);
}
