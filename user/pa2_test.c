#include "kernel/types.h"
#include "user.h"
#include "kernel/stat.h"

int main(int argc, char **argv){
	printf(">>> Testing fair scheduling:\n");

  int range = 100000;
  int a = 1;

  int pid1 = fork();

  if (pid1 > 0) {
      //parent proc
      setnice(getpid(), 0);
      for (int i = 0; i < range; i++) { //some work
          for (int j = 0; j < range; j++) {
              a = a * 10 + 1;
          }
      }

      // print all process stats
      ps(0);
      // wait for child to finish
      wait(0);
  } else if (pid1 == 0) {
      //child proc
      setnice(getpid(), 10);
      for (int i = 0; i < range; i++) { //some work
          for (int j = 0; j < range; j++) {
              a = a * 10 + 1;
          }
      }
      //printf("child process is over.\n");
      exit(0);
  }
  //printf("it's all over.\n");
  exit(0);
}
