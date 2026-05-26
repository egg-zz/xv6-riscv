// user/pa4test.c
#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

// skeleton에서 제공한 swapstat()이 있다고 가정
// 없다면, swapstat.c에서 extern 선언을 참고해서 맞춰주면 됨.
extern void swapstat(int *nr_read, int *nr_write);

#define PGSIZE 4096

int
main(int argc, char *argv[])
{
  int r0, w0, r1, w1;
  int pages = 0;
  char *base, *p;

  // 1. 시작 시점의 swap I/O 카운터 확인
  swapstat(&r0, &w0);
  printf("pa4test: start: swap read=%d write=%d\n", r0, w0);

  // 현재 브레이크(힙 시작 주소)
  base = sbrk(0);

  // 2. sbrk로 페이지를 가능한 한 많이 할당하면서
  //    각 페이지에 패턴을 기록하여 실제 할당되도록 만듦
  while (1) {
    p = sbrk(PGSIZE);
    if (p == (char *)-1) {
      printf("pa4test: sbrk failed at %d pages\n", pages);
      break;
    }

    // 이 페이지에 고유한 패턴 기록
    // 앞 바이트와 마지막 바이트에 서로 다른 값 저장
    p[0] = (char)(pages & 0xff);
    p[PGSIZE - 1] = (char)(~pages);

    pages++;
  }

  printf("pa4test: allocated %d pages\n", pages);

  // 3. 여러 번 메모리를 비연속적으로 접근해서
  //    LRU + swap_in이 잘 동작하도록 부담을 줌
  for (int pass = 0; pass < 3; pass++) {
    for (int i = 0; i < pages; i += 7) {  // 7칸씩 건너뛰면서 랜덤-ish 접근
      char *q = base + (uint64)i * PGSIZE;
      // 읽기 + 간단한 쓰기 (다시 원래 값으로 복구)
      char v = q[0];
      q[0] = v ^ 0x5a;
      q[0] = v;
    }
  }

  // 4. 스왑 I/O 카운터 다시 확인
  swapstat(&r1, &w1);
  printf("pa4test: after stress: swap read=%d(+%d) write=%d(+%d)\n",
         r1, r1 - r0, w1, w1 - w0);

  // 5. 모든 페이지의 패턴 검증
  int ok = 1;
  for (int i = 0; i < pages; i++) {
    char *q = base + (uint64)i * PGSIZE;
    char a = q[0];
    char b = q[PGSIZE - 1];
    char expect_a = (char)(i & 0xff);
    char expect_b = (char)(~i);

    if (a != expect_a || b != expect_b) {
      printf("pa4test: data mismatch at page %d: "
             "got (a=%d, b=%d) expect (a=%d, b=%d)\n",
             i, (int)a, (int)b, (int)expect_a, (int)expect_b);
      ok = 0;
      break;
    }
  }

  // 6. 사용했던 메모리 회수
  if (pages > 0) {
    sbrk(-((uint64)pages * PGSIZE));
  }

  if (!ok) {
    printf("pa4test: FAILED\n");
    exit(1);
  }

  printf("pa4test: PASSED (pages=%d, swap_read_delta=%d, swap_write_delta=%d)\n",
         pages, r1 - r0, w1 - w0);
  exit(0);
}
