/* Wait for a subprocess to finish, twice.
   The first call must wait in the usual way and return the exit code.
   The second wait call must return -1 immediately. */

#include <syscall.h>
#include "tests/lib.h"
#include "tests/main.h"

void
test_main (void) 
{
  pid_t child;
  if ((child = fork ("child-simple"))){
    // printf("fork 끝\n");
    msg ("wait(exec()) = %d", wait (child));
    // printf("첫 번째 wait 끝\n");
    msg ("wait(exec()) = %d", wait (child));
    // printf("두 번째 wait 끝\n");
  } else {
    exec ("child-simple");
  }
}
