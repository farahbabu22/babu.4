#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <strings.h>
#include <time.h>
#include <sys/ipc.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/msg.h>
#include <sys/shm.h>
#include <errno.h>
#include "oss.h"

/*
* Author: Farah Babu
* SSO ID: FBKZX
* email id: fbkzx@umsystem.edu
*/

#define LOG_LINES 10000

#define IO_BOUND_PROB 30 //Set 30% as i/o bound processes

//Maximum limit either users 100 or seconds 3
#define USERS_MAX 100
#define SECONDS_MAX 3

static int max_seconds = SECONDS_MAX;
const char *log_filename = "output.log";

static const struct timespec maxTimeBetweenNewProcs = {.tv_sec = 1, .tv_nsec = 1};

static struct timespec cpuIdleTime = {.tv_sec = 0, .tv_nsec = 0};

//Turnaround and Wait time for users
static struct timespec schedulerTurnTime = {.tv_sec = 0, .tv_nsec = 0};
static struct timespec schedulerWaitTime = {.tv_sec = 0, .tv_nsec = 0};

static unsigned char bmap[processSize / 8];

//ready and blocked queue
static struct queue pq[qCOUNT];

//shared memory identifiers and pointer
static int sid = -1, qid = -1;
static struct shmem *shm = NULL;

//next process ID
static unsigned int next_id = 1;

//next time we will create a user process
static struct timespec next_start = {.tv_sec = 0, .tv_nsec = 0};

static unsigned int usersStarted = 0;
static unsigned int usersTerminated = 0;
static unsigned int usersBlocked = 0;

static unsigned int logLine = 0;



//Check a bit in byte
static int checkBmap(const int byte, const int n)
{
  return (byte & (1 << n)) >> n;
}

//Mark the bitmap as used
static void toggleBmap(const int u)
{
  int byte = u / 8;
  int mask = 1 << (u % 8);

  bmap[byte] ^= mask;
}

//Get a free bit from bitmap
static int getBmapFree()
{
  int i;

  for (i = 0; i < processSize; ++i)
  {

    int byte = i / 8;
    int bit = i % 8;

    if (checkBmap(bmap[byte], bit) == 0)
    {
      toggleBmap(i);
      return i;
    }
  }
  return -1;
}

//function to add time
static void addTime(struct timespec *a, const struct timespec *b)
{

  static const unsigned int max_ns = 1000000000;

  a->tv_sec += b->tv_sec;
  a->tv_nsec += b->tv_nsec;
  if (a->tv_nsec > max_ns)
  {
    a->tv_sec++;
    a->tv_nsec -= max_ns;
  }
}

//function to subract time or time difference
static void subTime(struct timespec *a, struct timespec *b, struct timespec *c)
{
  if (b->tv_nsec < a->tv_nsec)
  {
    c->tv_sec = b->tv_sec - a->tv_sec - 1;
    c->tv_nsec = a->tv_nsec - b->tv_nsec;
  }
  else
  {
    c->tv_sec = b->tv_sec - a->tv_sec;
    c->tv_nsec = b->tv_nsec - a->tv_nsec;
  }
}

static void divTime(struct timespec *a, const int d)
{
  a->tv_sec /= d;
  a->tv_nsec /= d;
}

static int pushQ(const int qid, const int pi)
{
  struct queue *q = &pq[qid];
  q->ids[q->len++] = pi;
  return qid;
}

//Shift the itms to the queue pop by shifting everything to the left
static int popQ(struct queue *pq, const int pos)
{
  unsigned int i;
  unsigned int u = pq->ids[pos];

  for (i = pos; i < pq->len - 1; i++)
  {
    pq->ids[i] = pq->ids[i + 1];
  }
  return u;
}

static int schedulerCreateSHM()
{

  const key_t key = ftok(keyPath, kSHM);
  if (key == -1)
  {
    perror("OSS: issue in ftok for kSHM");
    return -1;
  }

  sid = shmget(key, sizeof(struct shmem), IPC_CREAT | IPC_EXCL | S_IRWXU);
  if (sid == -1)
  {
    perror("OSS: issue in shmget for kSHM");
    return -1;
  }

  shm = (struct shmem *)shmat(sid, NULL, 0);
  if (shm == (void *)-1)
  {
    perror("OSS: issue in shmat for kSHM");
    return -1;
  }

  key_t msgKey = ftok(keyPath, kQUEUE);
  if (msgKey == -1)
  {
    perror("OSS: issue in kQueue ftok");
    return -1;
  }

  qid = msgget(msgKey, IPC_CREAT | IPC_EXCL | 0666);
  if (qid == -1)
  {
    perror("OSS: issue with msgget for kQUEUE");
    return -1;
  }

  return 0;
}



static int startUserPCB()
{

  char buf[10];

  const int u = getBmapFree(); //get the free bitmap for the user process
  if (u == -1)
  {
    return EXIT_SUCCESS;
  }

  struct userPCB *usr = &shm->users[u];

  /*
    using random function to decide if the user process is IO bound or CPU bound
  */
  const int io_bound = ((rand() % 100) <= IO_BOUND_PROB) ? 1 : 0;

  const pid_t pid = fork();

  switch (pid)
  {

  case -1:
    perror("OSS: error in forking the process");
    return EXIT_FAILURE;
    break;

  case 0:

    snprintf(buf, sizeof(buf), "%d", io_bound); //Set the arguments in the buf variable

    execl("./user", "./user", buf, NULL); //spawn the child user process

    perror("OSS: error in execl");
    exit(EXIT_FAILURE);
    break;

  default:
    ++usersStarted;

    usr->id = next_id++;
    usr->pid = pid;

    //save start time of user
    usr->t_started = shm->clock;

    //add to ready queue
    usr->state = sREADY;
    pushQ(qREADY, u);

    ++logLine;
    printf("OSS: Generating process with PID %u and putting it in ready queue at time %lu:%lu\n",
           usr->id, shm->clock.tv_sec, shm->clock.tv_nsec);
    break;
  }

  return EXIT_SUCCESS;
}

//Send a 0 timeslice to users to make them exit
static void stopUserProcess()
{
  struct ossMsg m;

  int i;
  for (i = 0; i < processSize; i++)
  {
    if (shm->users[i].pid > 0)
    {

      m.timeslice = 0;
      m.mtype = shm->users[i].pid;
      m.from = getpid();
      if (msgsnd(qid, (void *)&m, MESSAGE_SIZE, 0) == -1)
      {
        break;
      }

      waitpid(shm->users[i].pid, NULL, 0); //wait for the process to exit
      shm->users[i].pid = 0;
    }
  }
}

static int advanceTimer()
{

  struct timespec t = {.tv_sec = 1, .tv_nsec = 0}; //amount to update
  addTime(&shm->clock, &t);

  if ((shm->clock.tv_sec >= next_start.tv_sec) ||
      ((shm->clock.tv_sec == next_start.tv_sec) &&
       (shm->clock.tv_nsec >= next_start.tv_nsec)))
  {
    next_start.tv_sec = rand() % maxTimeBetweenNewProcs.tv_sec;
    next_start.tv_nsec = rand() % maxTimeBetweenNewProcs.tv_nsec;

    addTime(&next_start, &shm->clock);

    if (usersStarted < USERS_MAX)
    {
      startUserPCB();
    }
  }
  return 0;
}

//Clear the user process control block, on termination
static void clearUserPCB(const int u)
{

  struct timespec res;
  struct userPCB *usr = &shm->users[u];

  ++usersTerminated;

  //system time = current - started
  subTime(&usr->t_started, &shm->clock, &usr->t_sys);

  //turnaround time = system  / num users
  addTime(&schedulerTurnTime, &usr->t_sys);

  //wait time = system - cpu
  subTime(&usr->t_cpu, &usr->t_sys, &res);
  addTime(&schedulerWaitTime, &res);

  bzero(usr, sizeof(struct userPCB));

  //change state to new (unused)
  usr->state = sNEW;

  toggleBmap(u); //mark the usr as free
}



static int schedulerRunUser()
{

  struct ossMsg m;

  const int u = popQ(&pq[qREADY], 0); //Getting the first process
  pq[qREADY].len--;                   //drop from q

  struct userPCB *usr = &shm->users[u];

  bzero(&m, sizeof(struct ossMsg));

  ++logLine;
  printf("OSS: Running process with PID %u from ready queue at time %lu:%lu\n", usr->id, shm->clock.tv_sec, shm->clock.tv_nsec);

  m.timeslice = TIMESLICE;
  m.mtype = usr->pid;
  m.from = getpid();

  //Send message to the queue
  if ((msgsnd(qid, (void *)&m, MESSAGE_SIZE, 0) == -1) ||
      (msgrcv(qid, (void *)&m, MESSAGE_SIZE, getpid(), 0) == -1))
  {
    perror("OSS: Error in sending and receiving message");
    return -1;
  }

  const int new_state = m.timeslice;

  switch (new_state)
  {

  case sREADY:

    usr->state = sREADY;

    //get the burst time
    usr->t_burst.tv_sec = 0;
    usr->t_burst.tv_nsec = m.clock.tv_nsec;

    addTime(&shm->clock, &usr->t_burst); //add the burst time

    //update how long user ran on cpu
    addTime(&usr->t_cpu, &usr->t_burst);

    ++logLine;
    printf("OSS: Receiving that process with PID %u ran for %lu nanoseconds\n", usr->id, usr->t_burst.tv_nsec);
    if (m.clock.tv_nsec != TIMESLICE)
    {
      ++logLine;
      printf("OSS: not using its entire quantum\n");
    }

    ++logLine;
    printf("OSS: Putting process with PID %u into end of queue\n", usr->id);
    pushQ(qREADY, u); //push back at end of ready queue

    break;

  case sBLOCKED:
    usr->state = sBLOCKED;

    //start the burst before the i/o block
    usr->t_burst.tv_sec = 0;
    usr->t_burst.tv_nsec = m.clock.tv_nsec;

    addTime(&usr->t_cpu, &usr->t_burst);


    //set the event time to process control block
    usr->t_blocked.tv_sec = m.io.tv_sec;
    usr->t_blocked.tv_nsec = m.io.tv_nsec;

    addTime(&usr->t_blocked, &shm->clock); //add clock to wait time

    ++logLine;
    usersBlocked++;
    printf("OSS: Receiving that process with PID %u is blocking for event(%li:%li) %li:%li\n",
           usr->id, usr->t_blocked.tv_sec, usr->t_blocked.tv_nsec, shm->clock.tv_sec, shm->clock.tv_nsec);

    //insert at blocked queue
    ++logLine;
    printf("OSS: Putting process with PID %u into queue %d\n", usr->id, qBLOCKED);
    pushQ(qBLOCKED, u);
    break;

  case sTERMINATED:
    usr->state = sTERMINATED;
    usr->t_burst.tv_sec = 0;
    usr->t_burst.tv_nsec = m.clock.tv_nsec;
    addTime(&usr->t_cpu, &usr->t_burst); //add burst to total cpu time

    ++logLine;
    printf("OSS: Receiving that process with PID %u has terminated\n", usr->id);

    clearUserPCB(u);

    break;

  default:
    printf("OSS: Invalid response from user\n");
    break;
  }
  return 1;
}

// Run a user from the READY queue
static int scheduleUsers()
{

  static struct timespec t_idle = {.tv_sec = 0, .tv_nsec = 0};
  static int printfFlag = 0;
  struct timespec a, b, c;

  //No process is ready in the queue
  if (pq[qREADY].len == 0)
  {

    if (printfFlag == 0)
    {
      ++logLine;
      printf("OSS: No ready process for scheduleUsers %li:%li\n", shm->clock.tv_sec, shm->clock.tv_nsec);
    }

    //get the time idle
    t_idle = shm->clock;
    

    printfFlag = 1;
    return 0;
  }
  else if (printfFlag == 1)
  { //if we are in idle mode
    printfFlag = 0;

    //calculate idle time
    subTime(&t_idle, &shm->clock, &b);
    addTime(&cpuIdleTime, &b);

    t_idle.tv_sec = 0;
    t_idle.tv_nsec = 0;
  }

  clock_gettime(CLOCK_REALTIME, &a);

  
  schedulerRunUser(); //allocate process to CPU

  clock_gettime(CLOCK_REALTIME, &b);

  subTime(&a, &b, &c); 

  addTime(&shm->clock, &c); //How long did the process run

  ++logLine;
  printf("OSS: total time this schedule was %lu nanoseconds\n", c.tv_nsec);
  return 0;
}

//check blocked queue and move users to ready, if their time has come
static void unblockUsers()
{
  unsigned int i;
  for (i = 0; i < pq[qBLOCKED].len; ++i)
  {

    //get index of first block user
    const int u = pq[qBLOCKED].ids[i];

    struct userPCB *usr = &shm->users[u];

    //check if its time to unblock
    if ((usr->t_blocked.tv_sec < shm->clock.tv_sec) ||

        ((usr->t_blocked.tv_sec == shm->clock.tv_sec) &&
         (usr->t_blocked.tv_nsec <= shm->clock.tv_nsec)))
    {

      //make the user ready
      usr->state = sREADY;
      usr->t_blocked.tv_sec = 0;
      usr->t_blocked.tv_nsec = 0;

      ++logLine;
      printf("OSS: Unblocked PID %d at %lu:%lu\n", usr->id, shm->clock.tv_sec, shm->clock.tv_nsec);

      //pop from blocked queue
      popQ(&pq[qBLOCKED], i);
      pq[qBLOCKED].len--;

      //push at end of ready queue
      pushQ(qREADY, u);
    }
  }
}

//Check if log lines are above maximum
static void checkLog()
{
  if (logLine >= LOG_LINES)
  {
    printf("OSS: Redirecting log to /dev/null\n");
    stdout = freopen("/dev/null", "w", stdout);
  }
}

/*
Schedule user processes
Run until all users have terminated
*/
static void ossSchedule()
{

  while (usersTerminated < USERS_MAX)
  {
    advanceTimer();
    unblockUsers();
    scheduleUsers();
    checkLog();
  }
}

static void getHelp()
{
  printf("oss [-h] [-s t] [-l f]\n");
  printf("-h Describe how the project should be run and then terminate\n");
  printf("-s t Indicate how many seconds before the system terminates\n");
  printf("-l f Specify a particular name for the log file\n");
}

static int parseProgramOptions(const int argc, char *const argv[])
{
  int opt;

  while ((opt = getopt(argc, argv, "hs:l:")) > 0)
  {
    switch (opt)
    {
    case 'h':
      getHelp();
      return -1;

    case 's':
      max_seconds = atoi(optarg);
      break;

    case 'l':
      log_filename = optarg;
      break;

    default:
      getHelp();
      return -1;
    }
  }

  stdout = freopen(log_filename, "w", stdout);
  if (stdout == NULL)
  {
    perror("freopen");
    return -1;
  }

  return 0;
}

static void printResults()
{

  //we show the average for turnaround and wait time
  divTime(&schedulerTurnTime, usersStarted);
  divTime(&schedulerWaitTime, usersStarted);

  if (logLine >= LOG_LINES)
  {
    stdout = freopen(log_filename, "w", stdout);
  }

  printf("*********************************REPORT**********************************************************************\n");
  printf("***\t\tTotal time %lu:%lu\n", shm->clock.tv_sec, shm->clock.tv_nsec);
  printf("***\t\tTurnaround time: %lu:%lu\n", schedulerTurnTime.tv_sec, schedulerTurnTime.tv_nsec);
  printf("***\t\tWait time: %lu:%lu\n", schedulerWaitTime.tv_sec, schedulerWaitTime.tv_nsec);
  printf("***\t\tOSS idle time: %lu:%lu\n", cpuIdleTime.tv_sec, cpuIdleTime.tv_nsec);

  printf("******Schedule Run Information\n");
  printf("***\t\tBlocked processes: %u\n", usersBlocked);
  printf("***\t\tStarted processes: %u\n", usersStarted);
  printf("***\t\tTerminated processes: %u\n", usersTerminated);
  printf("*********************************PROGRAM COMPLETED SUCCESSFULLY**********************************************\n");
}

static void destorySchedulerSHM()
{

  if (shm != NULL)
  {
    if (shmdt(shm) == -1)
    {
      perror("OSS: Error in shmdt");
    }
    shm = NULL;
  }

  if (sid > 0)
  {
    if (shmctl(sid, IPC_RMID, NULL) == -1)
    {
      perror("OSS: Error in shmctl");
    }
    sid = 0;
  }

  if (qid > 0)
  {
    if (msgctl(qid, IPC_RMID, NULL) == -1)
    {
      perror("OSS: Error in msgctl");
    }
    qid = -1;
  }
}

//Called at exit of OSS
static void cleanUpOSS()
{
  //stop all users
  stopUserProcess();
  //clear shared memory
  destorySchedulerSHM();
  exit(EXIT_SUCCESS);
}

static void catchSignalHandler(int sig)
{
  printf("OSS: Signaled with %d %li:%li\n", sig, shm->clock.tv_sec, shm->clock.tv_nsec);
  printResults();
  cleanUpOSS();
}

int main(const int argc, char *const argv[])
{

  //Intialize signals
  signal(SIGINT, catchSignalHandler);
  signal(SIGTERM, catchSignalHandler);
  signal(SIGALRM, catchSignalHandler);
  signal(SIGCHLD, SIG_IGN);

  srand(getpid());

  if ((parseProgramOptions(argc, argv) < 0) ||
      (schedulerCreateSHM() < 0))
  {
    return EXIT_FAILURE;
  }

  //clear shared memory, bitmap and queues
  bzero(shm, sizeof(struct shmem));
  bzero(bmap, sizeof(bmap));
  bzero(pq, sizeof(struct queue) * qCOUNT);

  alarm(max_seconds);
  atexit(destorySchedulerSHM);

  ossSchedule();
  printResults();
  cleanUpOSS();

  return EXIT_SUCCESS;
}