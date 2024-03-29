#include "types.h"
#include "stat.h"
#include "user.h"

int main(int argc, char *argv[]){
	int mypid = getpid();
	printf(1, "My student id is 2022066953\nMy pid is %d", mypid);
	
	char *buf = "\nMy gpid is";
	int ret_val;
	ret_val = getgpid(buf);
	printf(1, " %d\n", ret_val);
	exit();
};
