#include "ssu_backup.h"

int period;		
char pathname[PATH_MAX];
char filename[PATH_MAX];
int option = 0;
int file_num = 1;
int log_fd;
pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;

//for -d option, thread management
typedef struct thread_struct
{
	struct thread_struct *next;
	pthread_t tid;
	char *data;
} ssu_thread;

ssu_thread *HEAD = NULL, *TAIL = NULL;

char *dir_list[MAX_DIR]; // management directory(subdirectory) list		 
int dir_cnt = 0;				 // num of dir
time_t dir_time_now[MAX_DIR] = {0}; // mtime of dir list
time_t dir_time_new;
char prog_name[64] = {0};
//for -d option, first file will be normal log,
//but, after first step, log about new file, will be modify log
bool isnewfile = false; 
int startfile_fin = 0;

int kill_daemon_pid(); //find daemon process and send exit signal
void siganl_handler(int signo);	// USR1 signal handler
int which_number(char *str);
void check_option(); // check option and handle error
int daemon_init(void); // create daemon process
char *strTohex(char *old_str); // convert pathname to hex code
char *getTimestamp(char *hex_str); // get time stamp from hex code file name
void daemon_error(char *str); // for syslog
void backup_function(char *name); // main backup function
void file_backup(char *name); // backup file
void thread_function(void *arg); // for pthread_creat() argument to backup_function
void write_log(char *name, int size, time_t mt, int index); // for logging

char **getBackupList(char *name, int *ret); // get list of files becked up
void directory_backup(char *link); // directory backup function

void recovery(); // for option -r
void compare();	// for option -c
ssu_thread *make_thread(char *data);
int check_list(char *data);
void delete_list(char *name);

int main(int argc, char *argv[]){

	if (argc < 3){
		fprintf(stderr, "usage : ssu_backup <pathname> <period> [option]\n");
		exit(1);
	}

	umask(0);

	realpath(argv[1], pathname);
	strcpy(filename, argv[1]);
	strcpy(prog_name, argv[0]);

	if (access(argv[1], F_OK) != 0){
		fprintf(stderr, "usage : Target file not exist\n");
		exit(1);
	}

	char opt;
	while ((opt = getopt(argc, argv, "mn:dcr")) != -1){
		switch (opt){
			case 'm':
				option |= OPT_M;
				break;
			case 'n':
				if(optarg == NULL){
					fprintf(stderr, "usage : option -n N, N is Integer\n");
					exit(1);
				}

				for (int i = 0; i < strlen(optarg); i++){
					if (optarg[i] < '0' || optarg[i] > '9'){
						fprintf(stderr, "usage : option -n N, N is Integer\n");
						exit(1);
					}
				}
				option |= OPT_N;
				file_num = atoi(optarg);
				break;
			case 'd':
				option |= OPT_D;
				break;
			case 'c':
				option |= OPT_C;
				break;
			case 'r':
				option |= OPT_R;
				break;
		}
	}

	period = atoi(argv[argc - 1]);

	if (!(option & OPT_R) && !(option & OPT_C))
		if (period > 10 || period < 2){
			fprintf(stderr, "usage : period will be 3 <= p <= 10\n");
			exit(1);
		}

	check_option();

	//handle of option -n and -m is in backup_funtion
	if (option & OPT_C || option & OPT_R){ // option -r or -c
		chdir("backup"); // set working directory for backup
		if (option & OPT_R)
			recovery();
		if (option & OPT_C)
			compare();
	}
	else if (option & OPT_D){ // option -d 
		struct stat st;
		time_t old = 0, new = 0;

		while (1){
			if (access(pathname, F_OK) != 0){
				write_log(pathname, 0, 0, 3);
				exit(0);
			}
			stat(pathname, &st);
			new = st.st_mtime; 
			if (new != old){ 
				directory_backup(pathname);
				old = new;
			}

			for (int i = 0; i < dir_cnt; i++){
				if (access(dir_list[i], F_OK) == 0) {
					stat(dir_list[i], &st); 
					dir_time_new = st.st_mtime;
					if (dir_time_now[i] != dir_time_new) 
					{
						directory_backup(dir_list[i]);
						dir_time_now[i] = dir_time_new;
					}
				}
			}
			if(!isnewfile){
				int startfile_cnt = 0;
				ssu_thread *cur;
				for (cur = HEAD; cur != NULL; cur = cur->next) 
				{
					startfile_cnt++;
				}
				while(startfile_cnt != startfile_fin)
					;
				isnewfile = true;
			}
		}
	}
	else{ // regular file backup
		backup_function(pathname);
	}
}

void check_option(){
	struct stat st;
	stat(pathname, &st);

	// make backup directory
	if (access("backup", F_OK))
		mkdir("backup", 0777);

	// option -d error
	if(option & OPT_D){
		if(!S_ISDIR(st.st_mode)){
			fprintf(stderr, "usage : option -d need directory\n");
			exit(1);
		}
	}

	// option -c, -r error
	if (option & OPT_C || option & OPT_R){
		if(!(option == OPT_C || option == OPT_R)){
			fprintf(stderr, "option -c and -r cannot be used with other option\n");
			exit(1);
		}
	}


	kill_daemon_pid(); // kill before daemon process

	// make log file
	if (access("backup_log", F_OK) != 0) 
		log_fd = open("backup_log", O_CREAT | O_TRUNC | O_WRONLY, 0777);
	else
		log_fd = open("backup_log", O_WRONLY | O_APPEND);

	// if option -c or -r, do not make daemon
	if (!(option & OPT_C || option & OPT_R)) 
		daemon_init();


}

int kill_daemon_pid(void){//find daemon process and send exit signal
	DIR *dp;
	struct dirent *dir;
	int pid, curpid;
	FILE *fp;

	char buf[128], line[1024], tag[128], name[128];

	if ((dp = opendir("/proc/")) == NULL) {
		fprintf(stderr, "opendir error for /proc/\n");
		exit(1);
	}

	curpid = getpid();
	while((dir = readdir(dp)))
	{
		// get pid from dir_name
		pid = which_number(dir->d_name);

		if(pid == -1 || pid == curpid)
			continue;

		//get information of process status (/proc/pid/status file)
		snprintf(buf, 100, "/proc/%d/status", pid);
		fp = fopen(buf, "r");
		if(fp == NULL)
			continue;
		fgets(line, 1024, fp);
		fclose(fp);

		//get prog_name from file info
		sscanf(line, "%s %s", tag, name);
		//for delete "./", prog_name + 2 
		if(!strcmp(name, prog_name+2))
		{
			closedir(dp);
			printf("Send signal to ssu_backup process<%d>\n", pid);
			kill(pid, SIGUSR1);
			return pid;
		}
	}
	closedir(dp);
	return -1;
}

int which_number(char *str){
	int len, i;

	len = strlen(str);
	for(i=0; i<len; i++)
	{
		if(str[i] < '0' || str[i] > '9')
			return -1;
	}

	return atoi(str);
}


int daemon_init(void){
	pid_t pid;
	int fd, maxfd;

	printf("Daemon process initialization.\n");

	if ((pid = fork()) < 0) {
		fprintf(stderr, "fork error\n");
		exit(1);
	}
	else if(pid != 0)
		exit(0);

	pid = getpid();
	printf("process %d running as ssu_backup daemon.\n", pid);
	setsid();
	signal(SIGTTIN, SIG_IGN);
	signal(SIGTTOU, SIG_IGN);
	signal(SIGTSTP, SIG_IGN);
	maxfd = getdtablesize();

	for (fd = 0; fd < maxfd; fd++)
		close(fd);

	umask(0);


	chdir("backup"); // set working directory for backup
	fd = open("/dev/null", O_RDWR);
	signal(SIGUSR1, siganl_handler);
	dup(0);
	dup(0);
	if (access("../backup_log", F_OK) != 0) 
		log_fd = open("../backup_log", O_CREAT | O_TRUNC | O_WRONLY, 0777);
	else
		log_fd = open("../backup_log", O_WRONLY | O_APPEND); 
	return 0;
}

void daemon_error(char *str){
	openlog("ssu_backup", LOG_PID, LOG_LPR);
	syslog(LOG_ERR, "%s", str);
	closelog();
	exit(1);
}


void backup_function(char *name){ 
	struct stat src_sc;
	time_t old = 0, new = 0;
	int i = 0;
	int check = 0;

	while (true)
	{
		sleep(period);

		if (access(name, F_OK) != 0){
			write_log(name, 0, new, 3);
			if (option & OPT_D) {// if it is option -d backup, it is pthread, for exit, use pthread_exit()
				//before exit, tihs thread struct must be deleted
				delete_list(name);
				pthread_exit(0);
			}
			else
				exit(0);
		}

		stat(name, &src_sc);
		new = src_sc.st_mtime;

		if (option & OPT_M && new == old) // option -m
			continue;

		if (option & OPT_N) { // option -n N
			int cnt = 0;
			char **list = getBackupList(name, &cnt); // get list of files backed up
			struct stat lis;

			// delete old files (N - current file num)
			for (i = 0; i <= cnt - file_num; i++){
				stat(list[i], &lis);
				write_log(name, lis.st_size, lis.st_ctime, 4);
				unlink(list[i]);
			}

			for (i = 0; i < cnt; i++)
				free(list[i]);
			free(list);
		}

		

		if (old == 0){
			if(!isnewfile)
				write_log(name, src_sc.st_size, new, 1);
			else
				write_log(name, src_sc.st_size, new, 2);
		}
		else if(old == new)
			write_log(name, src_sc.st_size, new, 1);
		else
			write_log(name, src_sc.st_size, new, 2);

		file_backup(name);
		old = new;
		if((option & OPT_D) && !isnewfile)
			startfile_fin++;
	}
}

char *strTohex(char *old_str){
	char date[12] = {0};
	time_t timer;
	struct tm *t;

	timer = time(NULL);	
	t = localtime(&timer);

	char *result = (char *)calloc(sizeof(char), PATH_MAX);

	if (strlen(old_str) > 255)
		daemon_error("pathname is too long");

	for (int i = 0; i < strlen(old_str); i++)
	{
		char temp[3];
		sprintf(temp, "%x", old_str[i]);
		strcat(result, temp);
	}

	// make hex code with timestamp
	sprintf(date, "_%02d%02d%02d%02d%02d", t->tm_mon + 1, t->tm_mday, t->tm_hour, t->tm_min, t->tm_sec);
	strcat(result, date);

	return result;
}

void file_backup(char *name)
{
	int length;
	int fd;
	int nfd;
	char *data;
	struct stat src_st;


	fd = open(name, O_RDONLY);
	if(option & OPT_R){
		// in option -r, it recover file from backup file(user choose)
		nfd = open(pathname, O_CREAT | O_TRUNC | O_WRONLY, 0666); 
	}
	else{
		nfd = open(strTohex(name), O_CREAT | O_TRUNC | O_WRONLY, 0666); 
	}

	stat(name, &src_st);
	data = (char *)malloc(src_st.st_size);

	while ((length = read(fd, data, src_st.st_size)) > 0)
		write(nfd, data, length);

	close(fd);
	close(nfd);
	free(data);
}


char **getBackupList(char *name, int *ret){ 
	char *real = strTohex(name);
	int length = strlen(real);
	struct dirent **namelist;

	int cnt = scandir(".", &namelist, NULL, alphasort);
	char **list = (char **)malloc(cnt * sizeof(char *));
	int many = 0;

	for (int i = 0; i < cnt; i++)
	{
		// skip '.', '..' and different name length files
		if ((!strcmp(namelist[i]->d_name, ".") || !strcmp(namelist[i]->d_name, "..")))
			continue;
		if (strlen(namelist[i]->d_name) != length)
			continue;

		for (int j = 0; j < length; j++)
		{
			if (real[j] == '_')
			{
				list[many] = (char *)malloc(sizeof(char) * strlen(namelist[i]->d_name)); 
				strcpy(list[many], namelist[i]->d_name);
				many++;
				break;
			}
			if (real[j] != namelist[i]->d_name[j])
				break;
		}
	}

	for (int i = 0; i < cnt; i++)
		free(namelist[i]); 
	free(namelist);
	free(real);

	*ret = many; // return number of list by pointer
	return list; // return list
}

void compare()
{
	int ret;
	char **file_list = getBackupList(pathname, &ret);
	if (ret == 0)
	{
		fprintf(stderr, "There is no backup file! <%s>", filename);
		exit(0);
	}
	int select;
	printf("<Compare with backup file[%s%s]>\n", filename, getTimestamp(file_list[ret - 1])); 
	pid_t pid;
	if ((pid = fork()) == 0)
	{
		execl("/usr/bin/diff", "diff", pathname, file_list[ret - 1], NULL);
		exit(0);
	}
	else
		wait(0);
}

void recovery()
{
	int ret;
	char **file_list = getBackupList(pathname, &ret); 
	int select;
	if (ret == 0)
	{
		fprintf(stderr, "There is no backup file! <%s>", filename);
		exit(0);
	}
	printf("<%s backup list>\n", filename);
	printf("0 : exit\n");
	for (int i = 0; i < ret; i++)
		printf("%d : %s%s\n", i + 1, filename, getTimestamp(file_list[i]));

	printf("input : ");

	scanf("%d", &select);
	if (select == 0 || select > ret) 
	{
		printf("Cancel recover\n");
		exit(0);
	}
	printf("Recovery backup file...\n");
	printf("[%s]\n", filename);
	fflush(stdout);

	unlink(pathname); 
	file_backup(file_list[select - 1]);

	pid_t pid;
	if ((pid = fork()) == 0)
	{
		execl("/bin/cat", "cat", pathname, NULL);
		exit(0);
	}
	else
		wait(0);
}

char *getTimestamp(char *hex_str)
{
	int i;
	for (i = 0; hex_str[i] != '_'; i++);

	return hex_str + i;
}

void directory_backup(char *link)
{
	struct dirent **namelist;
	int cont_cnt = scandir(link, &namelist, NULL, alphasort);

	for (int i = 0; i < cont_cnt; i++)
	{
		if ((!strcmp(namelist[i]->d_name, ".") || !strcmp(namelist[i]->d_name, "..")))
			continue; 

		char *resource = (char *)malloc(PATH_MAX);
		struct stat src;
		sprintf(resource, "%s/%s", link, namelist[i]->d_name);
		stat(resource, &src);

		if (S_ISDIR(src.st_mode)) // search subdirectory 
		{
			int k;
			for (k = 0; k < dir_cnt; k++)
			{
				if (strcmp(dir_list[k], resource) == 0) 
					break;
			}
			if (k == dir_cnt) 
			{
				dir_list[k] = resource;
				dir_cnt++;
			}
			directory_backup(resource); 
		}
		else // regular file
		{
			if (check_list(resource)) // check file already in list
			{
				if (HEAD == NULL)
				{
					HEAD = make_thread(resource);
					TAIL = HEAD;
				}
				else
				{
					TAIL->next = make_thread(resource);
					TAIL = TAIL->next;
				}
			}
		}
	}

	for (int i = 0; i < cont_cnt; i++)
		free(namelist[i]); 
	free(namelist);

	return;
}

void write_log(char *name, int size, time_t mt, int index)
{
	char *melong = (char *)malloc(PATH_MAX);
	time_t timer = time(NULL);		 
	struct tm *t = localtime(&timer);
	char temp[14];
	sprintf(temp, "%02d%02d %02d:%02d:%02d", t->tm_mon + 1, t->tm_mday, t->tm_hour, t->tm_min, t->tm_sec);
	struct tm *tt = localtime(&mt);
	int k;

	for (k = strlen(name) - 1; k >= 0; k--)
	{
		if (name[k] == '/')
			break;
	}
	k++;

	switch (index)
	{
	case 1:
		sprintf(melong, "[%s] %s [size:%d/mtime:%02d%02d %02d:%02d:%02d]\n", temp, name + k, size, tt->tm_mon + 1, tt->tm_mday, tt->tm_hour, tt->tm_min, tt->tm_sec);
		break;
	case 2:
		sprintf(melong, "[%s] %s is modified [size:%d/mtime:%02d%02d %02d:%02d:%02d]\n", temp, name + k, size, tt->tm_mon + 1, tt->tm_mday, tt->tm_hour, tt->tm_min, tt->tm_sec);
		break;
	case 3:
		sprintf(melong, "[%s] %s is deleted\n", temp, name + k);
		break;
	case 4:
		sprintf(melong, "[%s] Delete backup [%s, size:%d, btime:%02d%02d %02d:%02d:%02d]\n", temp, name + k, size, tt->tm_mon + 1, tt->tm_mday, tt->tm_hour, tt->tm_min, tt->tm_sec);
		break;
	}
	if (option & OPT_D) // in option -d, need synchronization about log file write,
	{
		pthread_mutex_lock(&mutex);
		write(log_fd, melong, strlen(melong));
		pthread_mutex_unlock(&mutex);
	}
	else
		write(log_fd, melong, strlen(melong));
}

void thread_function(void *arg){
	char *data = (char *)arg; 
	backup_function(data); 
}

ssu_thread *make_thread(char *data){
	ssu_thread *temp = (ssu_thread *)malloc(sizeof(ssu_thread));
	temp->data = data; 
	temp->next = NULL;

	if(pthread_create(&(temp->tid), NULL, (void *)(&thread_function), (void *)data) != 0){
		daemon_error("pthread_create error");
	}
	return temp;
}

int check_list(char *data)
{
	ssu_thread *cur;
	for (cur = HEAD; cur != NULL; cur = cur->next) 
	{
		if (strcmp(cur->data, data) == 0)
			return 0; 
	}
	return 1; 
}

void delete_list(char *name){
	ssu_thread *cur;
	for (cur = HEAD; cur != NULL; cur = cur->next) 
	{
		if (strcmp(cur->data, name) == 0){
			cur->data[0] = '\0';
			return;
		}
	}
	return;
}

void siganl_handler(int signo)
{
	char melong[PATH_MAX] = {0};
	time_t timer = time(NULL);
	struct tm *t = localtime(&timer);
	sprintf(melong, "[%02d%02d %02d:%02d:%02d] %s<pid:%d> exit\n", t->tm_mon + 1, t->tm_mday, t->tm_hour, t->tm_min, t->tm_sec, prog_name + 2, getpid()); 
	pthread_mutex_lock(&mutex);
	write(log_fd, melong, strlen(melong));
	pthread_mutex_unlock(&mutex);
	exit(0);
}
