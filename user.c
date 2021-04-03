#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <strings.h>
#include <time.h>
#include <sys/shm.h>
#include <sys/types.h>
#include <sys/ipc.h>
#include <sys/msg.h>
#include "oss.h"
#include "user.h"

static int sid = -1, qid = -1;

static struct shmem * oss_create_shm(){
	struct shmem *shm = NULL;

	key_t key = ftok(keyPath, kSHM);
	if(key == -1){
		perror("ftok");
		return NULL;
	}

	sid = shmget(key, sizeof(struct shmem), 0);
	if(sid == -1){
		perror("shmget");
		return NULL;
	}

	shm = (struct shmem*) shmat(sid, NULL, 0);
  if(shm == (void*)-1){
    perror("shmat");
    return NULL;
  }

	key = ftok(keyPath, kQUEUE);
	if(key == -1){
		perror("ftok");
		return NULL;
	}

	qid = msgget(key, 0);
	if(qid == -1){
		perror("mget");
		return NULL;
	}

  return shm;
}

static int oss_destroy_shm(struct shmem * shm){
	if(shmdt(shm) == -1){
		perror("shmdt");
		return -1;
	}
	return 0;
}

static int oss_user_simulation(const int io_bound){

  int alive = 1;

	//probability process will block for IO, depends on process type
	const int io_block_prob = (io_bound) ? IO_IO_BLOCK_PROB : CPU_IO_BLOCK_PROB;

  while(alive){

		struct ossMsg m;

		//wait for a timeslice
		m.from = getpid();
		if(msgrcv(qid, (void*)&m, MESSAGE_SIZE, m.from, 0) == -1){
			perror("mrcv");
			break;
		}

		const int timeslice = m.timeslice;
		if(timeslice == 0){	//if its time to quit
			break;
		}

		bzero(&m, sizeof(struct ossMsg));

		const int will_terminate = ((rand() % 100) < TERM_PROB) ? 1 : 0;

		if(will_terminate){
			m.timeslice = sTERMINATED;
			//use some part of the timeslice
			m.clock.tv_nsec = rand() % timeslice;

			alive = 0;

		}else{	//it won't terminate

			//decide to use entire timeslice or block on event
			const int will_interrupt = ((rand() % 100) < io_block_prob) ? 1 : 0;

			if(will_interrupt){
				m.timeslice = sBLOCKED;	//tell OSS we are waiting for the event

				//amount of timeslice used, before being blocked
				m.clock.tv_nsec = rand() % timeslice;

				//tell OSS, when event will happen
				m.io.tv_sec = rand() % EVENT_R;
				m.io.tv_nsec = rand() % EVENT_S;

			}else{	//use entire timeslice
				m.timeslice = sREADY;
				m.clock.tv_nsec = timeslice;
			}

		}

		//inform oss what we choose to do
		m.mtype = getppid();
		m.from = getpid();
		if(msgsnd(qid, (void*)&m, MESSAGE_SIZE, 0) == -1){
			perror("msnd");
			break;
		}
  }

	return 0;
}

int main(const int argc, char * const argv[]){

	if(argc != 2){
		fprintf(stderr, "Usage: ./user [IO_BOUND=0|1]\n");
		return EXIT_FAILURE;
	}

	const int io_bound = atoi(argv[1]);	//1 if true

	srand(getpid() + io_bound);

	struct shmem *shm = oss_create_shm();
  if(shm == NULL){
    return EXIT_FAILURE;
	}

  oss_user_simulation(io_bound);

  if(oss_destroy_shm(shm) == -1){
		return EXIT_FAILURE;
	}

  return EXIT_SUCCESS;
}
