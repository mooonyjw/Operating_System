#include "types.h"
#include "user.h"
#include "stat.h"
#include "sched.h"

#define NUM_LOOP 10000

int main(int argc, char* argv[]){
	int p, i;
	p = fork();
	if(p==0){
		for(i =0; i<NUM_LOOP; i++){
			printf(1, "Child\n");
			yield();
		}
	}
	else{
		for(i=0; i<NUM_LOOP; i++){
			printf(1, "Parent\n");
			yield();	
		}
	}
	exit();
}
