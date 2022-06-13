#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <stdio.h>
#include <pthread.h>
#include <semaphore.h>

#define TCP_BEGIN_PRIVATE_PORT (0x11 << 14)
#define TCP_END_PRIVATE_PORT (0x01 << 16) - 1
#define TCP_MAX_PRIVATE_PORTS (TCP_BEGIN_PRIVATE_PORT - TCP_END_PRIVATE_PORT)

#define logn(str, ...) printf(str "\n", ## __VA_ARGS__)
#define Assert(cond) if (!(cond)) { *(int *)0 = 0; };
#define ArrayCount(a) (sizeof(a) / sizeof(a[0]))

/*
 * TERMINOLOGY
 * 	- fd: file descriptor. socketfd == socket file descriptor
 *
 */

#define ASYNC_FUNC_HANDLER(name) void name(void * data)
typedef ASYNC_FUNC_HANDLER(async_func_handler);

struct async_socket_handler
{
	struct sockaddr_in addr;
};

ASYNC_FUNC_HANDLER(AsyncSocketAcceptedHandler)
{
	async_socket_handler * SocketData = (async_socket_handler *)data;
	logn("Socket value %i", SocketData->addr);
}
ASYNC_FUNC_HANDLER(AsyncTest)
{
	int * val = (int *)data;
	sleep(1);
	logn("Doing task %i", *val);
}

struct thread_task
{
	async_func_handler * func;
	void * data;
};

struct thread_queue
{
	sem_t semaphore;
	volatile int current_task;
	volatile int next_task;

	thread_task Tasks[1024];
};

void
CompleteTasks(thread_queue * Queue)
{
	while (Queue->current_task != Queue->next_task)
	{
		int current_task = Queue->current_task;
		int new_current_task = (current_task + 1) & (ArrayCount(Queue->Tasks) - 1);
		if (Queue->next_task != current_task)
		{
			if (__sync_bool_compare_and_swap(&Queue->current_task, current_task, new_current_task))
			{
				thread_task * Task = Queue->Tasks + current_task;
				Task->func(Task->data);
			}
		}

	}
}

/*
 * Single thread (1 producer many consumers)
 */
void
AddTask(thread_queue * Queue, async_func_handler * Func, void * Data)
{
	int next_task = Queue->next_task;
	int new_next_task = (next_task + 1) & (ArrayCount(Queue->Tasks) - 1);
	Assert(new_next_task != Queue->current_task);
	thread_task * task = Queue->Tasks + next_task;
	task->func = Func;
	task->data = Data;
	logn("Adding task at %i. Next task now is %i", next_task, new_next_task);
	__sync_lock_test_and_set(&Queue->next_task, new_next_task);
#if 0
	int result = sem_post(&Queue->semaphore);
	Assert(result == 0);
#endif
}

void *
ThreadHandler(void * data)
{
	struct thread_queue * Queue = (thread_queue *)data;

	for (;;)
	{
		CompleteTasks(Queue);
		logn("No task found. Thread to sleep");
		sem_wait(&Queue->semaphore);
		logn("Sempahore activated");
	}
  logn("should not happen");
}



void *
memset(void *dest, register int val, register size_t len)
{
	register unsigned char *ptr = (unsigned char*)dest;
	while (len-- > 0)
		*ptr++ = val;
	return dest;
}

main()
{
	int keep_alive = true;

	int default_port = TCP_BEGIN_PRIVATE_PORT;
	const int max_open_connections = 10;
	struct sockaddr_in my_addr = {0};
	struct sockaddr_in list_ext_addr[max_open_connections] = {0};
	socklen_t list_ext_addr_size[max_open_connections] = {0};
	int socketfd = 0;
	int list_fd[max_open_connections] = {0};
	int count_open_connections = 0;
	pthread_t list_threads[max_open_connections] = {0};
	sem_t thread_semaphore = {0};

	sem_init(&thread_semaphore, 0, 0);
	struct thread_queue primary_thread_queue;
	primary_thread_queue.semaphore = thread_semaphore;
	primary_thread_queue.current_task = 0;
	primary_thread_queue.next_task = 0;
	memset(&primary_thread_queue.Tasks[0], 0, sizeof(primary_thread_queue.Tasks));

	for (int i = 0; i < max_open_connections; ++i)
	{
		pthread_t * thread = list_threads + i;
		int thread_id =	pthread_create(thread, NULL,ThreadHandler, &primary_thread_queue);
	}

	int task_data[80];
	for (int i = 0; i < ArrayCount(task_data); ++i)
	{
		int * val = task_data + i;
		*val = i;
		AddTask(&primary_thread_queue, AsyncTest, (void *)val);
		//sleep(1);
	}

	for (int i = 0; i < 5; ++i)
  {
    if (sem_post(&thread_semaphore) != 0)
    {
      Assert(0);
    }
  }

	//CompleteTasks(&primary_thread_queue);

#if 0
	socketfd = socket(AF_INET, SOCK_STREAM, 0);
	if (socketfd == -1)
	{
		logn("Error on socket()");
		return 0;
	}

	my_addr.sin_family = AF_INET;
	my_addr.sin_port = htons(default_port);
	my_addr.sin_addr.s_addr = INADDR_ANY;

	if(bind(socketfd, (struct sockaddr *)&my_addr, sizeof(struct sockaddr)) == -1)
	{
		logn("Error on bind()"); 
		return 0;
	}

	if(listen(socketfd, max_open_connections) == -1)
	{
		logn("Error on listen()");
		return 0;
	}

	while (keep_alive)
	{
		if (count_open_connections < max_open_connections)
		{
			socklen_t		* ext_addr_size = list_ext_addr_size + count_open_connections;
			struct sockaddr_in 	* ext_addr 	= list_ext_addr + count_open_connections;
			int 			* new_fd 	= list_fd + count_open_connections;

			*ext_addr_size = sizeof(struct sockaddr_in);

			// locks
			*new_fd = accept(socketfd, (struct sockaddr *)ext_addr, ext_addr_size);

			if (*new_fd == -1)
			{
				logn("Error on accept()");
				keep_alive = false;
			}
			else
			{
				// dispatch connection
			}
		}
	}


	for (int i = 0; i < count_open_connections; ++i)
	{
		close(list_fd[i]);
	}
	close(socketfd);
#endif

	return 0;
}
