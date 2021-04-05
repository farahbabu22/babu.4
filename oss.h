#ifndef OSS_H
#define OSS_H

/*
* Author: Farah Babu
* SSO ID: FBKZX
* email id: fbkzx@umsystem.edu
*/

//Limiting the program to total 20 processes so 20 - (oss + user) = 18 processes
#define processSize 18

#define keyPath "makefile" // Key Path to be used for ftok api call
enum ftokKeys
{
	kSHM = 1000, //Key for the shared memory
	kQUEUE		 //key for the shared queue
};

enum state
{ //New State | Ready State | Blocked State | Terminated State
	sNEW = 0,
	sREADY,
	sBLOCKED,
	sTERMINATED
};

// Messages to the log format includes message type | From Process ID | Time taken for the process to run | clock and time for i/0
struct ossMsg
{
	long mtype;
	pid_t from;

	int timeslice;

	struct timespec clock;
	struct timespec io;
};

#define MESSAGE_SIZE (sizeof(struct ossMsg) - sizeof(long))

enum qTypes
{
	qREADY = 0,
	qBLOCKED,
	qCOUNT
};

//Queue for process IDs
struct queue
{
	unsigned int ids[processSize];
	unsigned int len;
};

#define TIMESLICE 10000000 //Setting the timeslice

//Definition of Process Control Block
struct userPCB
{

	pid_t pid;
	unsigned int id;
	enum state state;

	//timers
	struct timespec t_cpu;	   //CPU Run time
	struct timespec t_sys;	   //Total Time
	struct timespec t_burst;   //Burst Time
	struct timespec t_blocked; //Blocked Time
	struct timespec t_started; //Start Time
};

//Definition of the shared memory
struct shmem
{
	struct timespec clock;
	struct userPCB users[processSize];
};

#endif
