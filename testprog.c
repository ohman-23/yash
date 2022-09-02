#include <stdio.h>
#include <stdlib.h>
#include <dirent.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <ctype.h>
#include <signal.h>
#include <sys/wait.h>
#include <errno.h>

int main(int arc, char **argv)
{
  printf("Started execution\n");
  sleep(5);
  printf("Computation done\n");
  sleep(3);
  return 0;
}