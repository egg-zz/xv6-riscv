#include "kernel/types.h"
#include "user.h"
#include "kernel/stat.h"

int main(int argc, char **argv){

  //PA1 TEST
  
  int pid = getpid(); 
  
	printf("pid: %d\n", pid);


  printf("\n>>>Testing getnice and setnice:\n");
  printf("initial nice value: %d\n", getnice(pid));
  setnice(pid, 40);
  printf("nice value after setting: %d\n", getnice(pid));
  
  int i;
  printf("\ngetnice test ...\n");
  for(i=-1;i<6;i++){
      printf("\tgetnice(%d) \treturn: %d\n", i, getnice(i));
  }
  printf("\tgetnice(%d) \treturn: %d\n", 1000, getnice(1000));

  printf("\nsetnice test 1...\n");
  for(i=-1;i<6;i++){
      printf("\tsetnice(%d,15) \treturn: %d\n", i, setnice(i, 15));
      printf("\tgetnice(%d) \treturn: %d\n", i, getnice(i));
  }
  printf("\tsetnice(%d,15) \treturn: %d\n", 1000, setnice(1000, 15));
  printf("\tgetnice(%d) \treturn: %d\n", 1000, getnice(1000));
  
  
  printf(">>>Testing ps:\n");
 	ps(0);

  printf(">>>Testing meminfo:\n");
  printf("available memory: %ld bytes\n", meminfo());
  
  printf(">>>Testing waitpid:\n");
  printf("wait\n");

  int pid1 = fork();
  if (pid1 == 0) {
    // child 1
    printf("start1\n");
    printf("end1\n");
    exit(10);               // exit code 10
  }

  int pid2 = fork();
  if (pid2 == 0) {
    // child 2
    printf("start2\n");
    printf("end2\n");
    exit(10);               // exit code 10
  }

  // parent
  if (waitpid(pid1) == 0){ 
    printf("done1 %d %d\n", pid1, 10);
  }

  if (waitpid(pid2) == 0) {
    printf("done2 %d %d\n", pid2, 10);
  }

	exit(0); 
}
