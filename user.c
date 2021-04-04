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

static int schedulerDestroySHM(struct shmem *shm)
{
	if (shmdt(shm) == -1)
	{
		perror("USER: Issue with shmdt");
		return -1;
	}
	return 0;
}



static struct shmem *schedulerCreateSHM()
{
	struct shmem *shm = NULL;

	key_t key = ftok(keyPath, kSHM);
	if (key == -1)
	{
		perror("USER: Issue with ftok");
		return NULL;
	}

	sid = shmget(key, sizeof(struct shmem), 0);
	if (sid == -1)
	{
		perror("USER: Issue with shmget");
		return NULL;
	}

	shm = (struct shmem *)shmat(sid, NULL, 0);
	if (shm == (void *)-1)
	{
		perror("USER: Issue with shmat");
		return NULL;
	}

	key = ftok(keyPath, kQUEUE);
	if (key == -1)
	{
		perror("USER: Issue ftok kQUEUE");
		return NULL;
	}

	qid = msgget(key, 0);
	if (qid == -1)
	{
		perror("USER: Issue with mget");
		return NULL;
	}

	return shm;
}



/*
* Wait for timeslice and receive message and process it
*/
static int schedulerUserSimulate(const int io_bound)
{

	int alive = 1;

	const int io_block_prob = (io_bound) ? IO_IO_BLOCK_PROB : CPU_IO_BLOCK_PROB;

	while (alive)
	{

		struct ossMsg m;


		m.from = getpid();
		if (msgrcv(qid, (void *)&m, MESSAGE_SIZE, m.from, 0) == -1)
		{
			perror("mrcv");
			break;
		}

		const int timeslice = m.timeslice;
		if (timeslice == 0)
		{ 
			break;
		}

		bzero(&m, sizeof(struct ossMsg));

		const int willTerminate = ((rand() % 100) < TERM_PROB) ? 1 : 0;

		if (willTerminate) // terminated successfully
		{
			m.timeslice = sTERMINATED;

			m.clock.tv_nsec = rand() % timeslice;

			alive = 0;
		}
		else
		{ 


			const int will_interrupt = ((rand() % 100) < io_block_prob) ? 1 : 0;

			if (will_interrupt)
			{
				m.timeslice = sBLOCKED; //Set as blocked

				
				m.clock.tv_nsec = rand() % timeslice;

				
				m.io.tv_sec = rand() % EVENT_R;
				m.io.tv_nsec = rand() % EVENT_S;
			}
			else
			{ 
				m.timeslice = sREADY;
				m.clock.tv_nsec = timeslice;
			}
		}

		
		m.mtype = getppid();
		m.from = getpid();
		if (msgsnd(qid, (void *)&m, MESSAGE_SIZE, 0) == -1)
		{
			perror("USER: error with msnd");
			break;
		}
	}

	return 0;
}

int main(const int argc, char *const argv[])
{

	if (argc != 2)
	{
		fprintf(stderr, "USER: Please supply arguments like ./user [IO_BOUND=0|1]\n");
		return EXIT_FAILURE;
	}

	const int ioBound = atoi(argv[1]); //1 if true

	srand(getpid() + ioBound);

	struct shmem *shm = schedulerCreateSHM();
	if (shm == NULL)
	{
		return EXIT_FAILURE;
	}

	schedulerUserSimulate(ioBound);

	if (schedulerDestroySHM(shm) == -1)
	{
		return EXIT_FAILURE;
	}

	return EXIT_SUCCESS;
}
