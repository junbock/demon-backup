#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <syslog.h>
#include <string.h>
#include <dirent.h>
#include <time.h>
#include <wait.h>
#include <pthread.h>
#include <sys/stat.h>

#define true 1
#define false 0
typedef int bool;

#define OPT_M 0x0001
#define OPT_N 0x0002
#define OPT_D 0x0004
#define OPT_C 0x0008
#define OPT_R 0x0010

#define MAX_DIR	32