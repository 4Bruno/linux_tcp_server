#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <stdio.h>
#include <pthread.h>
#include <poll.h>
#include <stdarg.h>
#include <stdlib.h>
#include <unistd.h> // sysconf
#include <math.h>
#include <fcntl.h>
#include <string.h>
#include <errno.h>

#ifdef WIN32
#include <windows.h>
#elif _POSIX_C_SOURCE >= 199309L
#include <time.h>   // for nanosleep
#endif

#define TCP_BEGIN_PRIVATE_PORT (0b11 << 14)
#define TCP_END_PRIVATE_PORT (0b01 << 16) - 1
#define TCP_MAX_PRIVATE_PORTS (TCP_BEGIN_PRIVATE_PORT - TCP_END_PRIVATE_PORT)

#define logn(str, ...) sync_printf(str "\n", ## __VA_ARGS__);\

#define Assert(cond) if (!(cond)) { *(int *)0 = 0; };
#define ArrayCount(a) (sizeof(a) / sizeof(a[0]))

#define SOCKET_CHECK(func) {\
                              int result = func;\
                              if (result < 0)\
                              {\
                                logn("Error (%i) on " #func, errno);\
                                close(socketfd);\
                              }\
                            }
/*
 * TERMINOLOGY
 * 	- fd: file descriptor. socketfd == socket file descriptor
 *
 */

void
sleep_ms(int milliseconds)
{ 
  //cross-platform sleep function
#ifdef WIN32
  Sleep(milliseconds);
#elif _POSIX_C_SOURCE >= 199309L
  struct timespec ts;
  ts.tv_sec = milliseconds / 1000;
  ts.tv_nsec = (milliseconds
      % 1000) * 1000000;
  nanosleep(&ts, NULL);
#else
  if (milliseconds >= 1000)
    sleep(milliseconds
        / 1000);
  usleep((milliseconds
        % 1000)
      * 1000);
#endif
}

static pthread_mutex_t printf_mutex;

void
sync_printf(const char *format, ...)
{
  va_list args;
  va_start(args, format);

  pthread_mutex_lock(&printf_mutex);
  vprintf(format, args);
  pthread_mutex_unlock(&printf_mutex);

  va_end(args);
}

#define ASYNC_FUNC_HANDLER(name) void name(void * data)
typedef ASYNC_FUNC_HANDLER(async_func_handler);

ASYNC_FUNC_HANDLER(AsyncTest)
{
	int * val = (int *)data;
	sleep_ms(200);
	logn("Doing task %i", *val);
}

struct thread_task
{
	async_func_handler * func;
	void * data;
};

struct thread_queue
{
	//sem_t semaphore;
  pthread_cond_t condition_semaphore;
  pthread_mutex_t condition_mutex;
  /*
   * Circular buffer, there shouldn't be more than arraysize tasks
   * current_task   points to the beginning index task
   * next_task      points to the last index task
   * pending_tasks  keeps track of the workload COMPLETED
   * NOTE: 
   * substract(current_task, next_task) does not necessarily match
   * pending_tasks. This is because current_task pointer is advanced
   * before the task is performed meanwhile pending_task counter
   * is decreased only once is finished.
   */
	volatile int current_task;
	volatile int next_task;
  volatile int pending_tasks;

	struct thread_task Tasks[1024];
};

void
CompleteTasks(struct thread_queue * Queue)
{
	while (Queue->current_task != Queue->next_task)
	{
		int current_task = Queue->current_task;
		int new_current_task = (current_task + 1) & (ArrayCount(Queue->Tasks) - 1);
		if (Queue->next_task != current_task)
		{
			if (__sync_bool_compare_and_swap(&Queue->current_task, current_task, new_current_task))
			{
				struct thread_task * Task = Queue->Tasks + current_task;
				Task->func(Task->data);
        __sync_fetch_and_sub(&Queue->pending_tasks, 1);
			}
		}
	}
}

void
WaitUntilAllTasksCompleted(struct thread_queue * Queue)
{
  while (Queue->pending_tasks > 0)
  {
    CompleteTasks(Queue);
  }
}

/*
 * Single thread (1 producer many consumers)
 */
void
AddTask(struct thread_queue * Queue, async_func_handler * Func, void * Data)
{
	int next_task = Queue->next_task;
	int new_next_task = (next_task + 1) & (ArrayCount(Queue->Tasks) - 1);
	Assert(new_next_task != (volatile int)Queue->current_task);
	struct thread_task * task = Queue->Tasks + next_task;
	task->func = Func;
	task->data = Data;
	//logn("Adding task at %i. Next task now is %i", next_task, new_next_task);
	__sync_lock_test_and_set(&Queue->next_task, new_next_task);

  pthread_mutex_lock(&Queue->condition_mutex);
	int result = pthread_cond_signal(&Queue->condition_semaphore);
  __sync_fetch_and_add(&Queue->pending_tasks, 1);
	Assert(result == 0);
  pthread_mutex_unlock(&Queue->condition_mutex);
}

struct socket_connection
{
  struct sockaddr_in ext_addr;
  socklen_t ext_addr_size;
  int fd;
};

struct server
{
#define MAX_OPEN_CONNECTIONS 12
  int socketfd;
  struct socket_connection connections[MAX_OPEN_CONNECTIONS];
  pthread_mutex_t nfds_mutex;
  int nfds;
  struct pollfd fds[MAX_OPEN_CONNECTIONS];

  pthread_cond_t condition_connection_released;
  pthread_mutex_t mutex_connection;

  struct thread_queue queue;

  int keep_alive;
};

void
OnConnectionLost(struct server * server,int connection_index)
{
  pthread_mutex_lock(&server->mutex_connection);
  pthread_cond_signal(&server->condition_connection_released);
  pthread_mutex_unlock(&server->mutex_connection);
}


ASYNC_FUNC_HANDLER(AsyncHandleClient)
{
  struct server * server = (struct server *)data;
  struct pollfd * fds = server->fds;
  int timeout = 30 * 1000 * 6;
  char buffer[80];
  for (;;)
  {
    int number_of_events = poll(fds, server->nfds,timeout);

    if (number_of_events < 0)
    {
      logn("Poll() returns error %i",number_of_events);
      Assert(0);
    }
    else if (number_of_events == 0)
    {
      logn("Poll() timeout");
      Assert(0);
    }
    else
    {
      int handled_responses = 0;
      for (int i = 0;
           i < server->nfds;
           /* pointer advances if not swap */)
      {
        if (fds[i].revents == 0)
        {
          ++i;
          continue;
        }
        else if (fds[i].revents == POLLIN)
        {
          for (;;)
          {
            int result = recv(fds[i].fd, buffer, sizeof(buffer), 0);
            if (result < 0)
            {
              if (errno != EWOULDBLOCK)
              {
                logn("recv() error");
                Assert(0);
              }
              // nothing to read
              break;
            }
            // conn closed
            else if (result == 0)
            {
              pthread_mutex_lock(&server->nfds_mutex);
              if (i != server->nfds)
              {
                close(fds[i].fd);
                // swap last
                memcpy(fds + i,fds + (server->nfds - 1), sizeof(struct pollfd));
                memcpy(server->connections + i, server->connections + (server->nfds - 1), sizeof(struct socket_connection));
              }
              server->nfds -= 1;
              pthread_mutex_unlock(&server->nfds_mutex);
            }
            logn("%s",buffer);
          }
        }
        if (handled_responses >= number_of_events)
        {
          break;
        }
      }
    }
  }
}

void
IncomingConnectionsHandler(struct server * server)
{
  if (server->nfds < ArrayCount(server->connections))
  {
    int timeout = 30 * 1000 * 6;
    struct pollfd pollfd[1];
    pollfd[0].fd = server->socketfd;
    pollfd[0].events = POLLIN;

    // blocking
    logn("[Thread %lu] Waiting on incoming connections", pthread_self());
    int number_of_events = poll(pollfd, 1, timeout);
    if (number_of_events < 0)
    {
      logn("Poll() returns error %i",number_of_events);
      Assert(0);
    }
    else if (number_of_events == 0)
    {
      logn("Poll() timeout");
      Assert(0);
    }
    else
    {
      for (;;)
      {
        struct sockaddr_in addr;
        socklen_t addr_size = sizeof(struct sockaddr);
        int new_fd = accept(server->socketfd, (struct sockaddr *)&addr, &addr_size);
        if (new_fd < 0)
        {
          if (errno != EWOULDBLOCK)
          {
            logn("Error (%i) on accept() [Thread %lu]",errno,pthread_self());
            Assert(0);
          }
          break;
        }
        pthread_mutex_lock(&server->nfds_mutex);
        struct socket_connection * new_connection = server->connections + server->nfds;
        new_connection->ext_addr_size = sizeof(struct sockaddr_in);
        new_connection->fd = new_fd;
        server->fds[server->nfds].events = POLLIN;
        server->fds[server->nfds].revents = 0;
        server->fds[server->nfds].fd = new_fd;
        server->nfds += 1;
        pthread_mutex_unlock(&server->nfds_mutex);
        logn("New connection received %i", new_connection->ext_addr.sin_addr.s_addr);
      }
    }
  }
}

#if 0
ASYNC_FUNC_HANDLER(AsyncIncomingConnectionsHandler)
{
  struct server * server = (struct server *)data;
  while (1)
  {
    if (server->nfds < ArrayCount(server->connections))
    {
      struct socket_connection * new_connection = server->connections + server->nfds;

      new_connection->ext_addr_size = sizeof(struct sockaddr_in);
      logn("[Thread %lu] Waiting on incoming connections", pthread_self());
      new_connection->fd = accept(server->socketfd, (struct sockaddr *)&new_connection->ext_addr, &new_connection->ext_addr_size);
      logn("New connection received %i", new_connection->ext_addr.sin_addr.s_addr);

      if ( new_connection->fd == -1)
      {
        logn("Error on accept() [Thread %lu]",pthread_self());
        Assert(0);
      }
      else
      {
        // dispatch connection
        Assert(0); // thisrequires mutex or other way to prevent nfds errors as we are deleting while handling client
        __sync_fetch_and_add(&server->nfds, 1);
        AddTask(server->queue, AsyncHandleClient, &server->client_queue);
      }
    }
    else
    {
      pthread_mutex_lock(&server->mutex_connection);
      pthread_cond_wait(&server->condition_connection_released, &server->mutex_connection);
    }
  }
  return NULL;
}
#endif

void *
ThreadHandler(void * data)
{
	struct thread_queue * Queue = (struct thread_queue *)data;

	for (;;)
	{
		CompleteTasks(Queue);
		logn("[Thread %lu] No task found. Thread to sleep.",pthread_self());
#if 1
    pthread_mutex_lock(&Queue->condition_mutex);
    pthread_cond_wait( &Queue->condition_semaphore, &Queue->condition_mutex );
    pthread_mutex_unlock(&Queue->condition_mutex);
#else
    sleep_ms(3000);
    // problem:
    // wait invoked after task added
    pthread_cond_wait( &Queue->condition_semaphore, &Queue->condition_mutex );
#endif
		logn("[Thread %lu] Sempahore activated",pthread_self());
	}
  logn("should not happen");
}



#if 0
void *
memset(void *dest, register int val, register size_t len)
{
	register unsigned char *ptr = (unsigned char*)dest;
	while (len-- > 0)
		*ptr++ = val;
	return dest;
}
#endif

void
TestThread(struct thread_queue * Queue)
{
  int task_data[15];
  srand(time(NULL));

  for (int i = 0; i < ArrayCount(task_data); ++i)
  {
    int * val = task_data + i;
    *val = Queue->next_task;
    AddTask(Queue, AsyncTest, (void *)val);
    int r = (rand() + 1) % 3;
  }
  WaitUntilAllTasksCompleted(Queue);
}

int
max_val(int a, int b)
{
  int result = a > b ? a : b;
  return result;
}

int
main()
{
  pthread_mutex_init(&printf_mutex,NULL);

  int keep_alive = 1;

  int default_port = TCP_BEGIN_PRIVATE_PORT;
  int socketfd = 0;
  int on = 0;

  const int max_threads = 40; // capped by number_of_processors
  pthread_t list_threads[max_threads];
  pthread_cond_t  condition_cond  = PTHREAD_COND_INITIALIZER;
  pthread_mutex_t condition_mutex = PTHREAD_MUTEX_INITIALIZER;

  struct thread_queue primary_thread_queue;
  primary_thread_queue.condition_semaphore = condition_cond;
  primary_thread_queue.condition_mutex = condition_mutex;
  primary_thread_queue.current_task = 0;
  primary_thread_queue.next_task = 0;
  memset(&primary_thread_queue.Tasks[0], 0, sizeof(primary_thread_queue.Tasks));

  int count_threads = max_val(4,(int)sysconf(_SC_NPROCESSORS_ONLN));

  // reserve 1 thread to handle new connections
  for (int i = 0; i < count_threads; ++i)
  {
    pthread_t * thread = list_threads + i;
    int thread_id =	pthread_create(thread, NULL,ThreadHandler, &primary_thread_queue);
  }

#if 0
  socketfd = socket(AF_INET6, SOCK_STREAM, 0);
  if (socketfd == -1)
  {
    logn("Error on socket()");
    return 0;
  }
  struct sockaddr_in6 my_addr = {0};
  memset(&my_addr, 0 , sizeof(my_addr));
  my_addr.sin6_family = AF_INET6;
  my_addr.sin6_port = htons(default_port);
  //my_addr.sin_addr.s_addr = INADDR_ANY;
  memcpy(&my_addr.sin6_addr, &in6addr_any, sizeof(in6addr_any));
#else
  socketfd = socket(AF_INET, SOCK_STREAM, 0);
  if (socketfd == -1)
  {
    logn("Error on socket()");
    return 0;
  }
  struct sockaddr_in my_addr = {0};
  memset(&my_addr, 0 , sizeof(my_addr));
  my_addr.sin_family = AF_INET;
  logn("Port %i",default_port);
  my_addr.sin_port = htons(default_port);
  my_addr.sin_addr.s_addr = INADDR_ANY;
#endif

  // re-use socket
  SOCKET_CHECK(setsockopt(socketfd,SOL_SOCKET,SO_REUSEADDR,(char *)&on, sizeof(on)));


  // non blocking
#if 1
  int socketfd_flags = fcntl(socketfd, F_GETFL);
  fcntl(socketfd, F_SETFL, socketfd_flags | O_NONBLOCK);
#endif

  SOCKET_CHECK(bind(socketfd, (struct sockaddr *)&my_addr, sizeof(struct sockaddr)));
  SOCKET_CHECK(listen(socketfd, MAX_OPEN_CONNECTIONS));

  struct server server;
  server.keep_alive = 1;
  server.nfds = 0;
  server.socketfd = socketfd;
  memset(server.connections, 0,sizeof(server.connections));
  memset(server.fds, 0 , sizeof(server.fds));

#if 0
  //AddTask(&primary_thread_queue, AsyncIncomingConnectionsHandler, (void *)&server);
  //AddTask(&primary_thread_queue, AsyncHandleClient, (void *)&server);
  while (server.keep_alive)
  {
    IncomingConnectionsHandler(&server);
  }
#else
  server.nfds += 1;
  server.fds[0].fd = socketfd;
  server.fds[0].events = POLLIN;
  int timeout = 30 * 1000 * 6;
  char buffer[80];
  while (server.keep_alive)
  {
    int number_of_events = poll(server.fds, server.nfds,timeout);
    logn("Poll() returns with %i events", number_of_events);

    if (number_of_events < 0)
    {
      logn("Poll() returns error %i",number_of_events);
      Assert(0);
    }
    else if (number_of_events == 0)
    {
      logn("Poll() timeout");
      Assert(0);
    }
    else
    {
      int handled_responses = 0;
      for (int i = 0;
           i < server.nfds;
           ++i)
      {
        if (server.fds[i].revents == 0)
        {
          continue;
        }
        else if (server.fds[i].revents == POLLIN)
        {
            // new connections
            if (server.fds[i].fd == socketfd)
            {
              int new_fd = -1;
              do
              {
                struct sockaddr_in addr;
                socklen_t addr_size = sizeof(struct sockaddr);
                logn("Invoking accept()");
                new_fd = accept(server.fds[i].fd, (struct sockaddr *)&addr, &addr_size);
                logn("Accept() returns %i", new_fd);
                if (new_fd < 0)
                {
                  if (errno != EWOULDBLOCK)
                  {
                    logn("Error (%i) on accept() [Thread %lu]",errno,pthread_self());
                    Assert(0);
                  }
                  break;
                }
                //pthread_mutex_lock(&server.nfds_mutex);
                struct socket_connection * new_connection = server.connections + server.nfds;
                new_connection->ext_addr_size = sizeof(struct sockaddr_in);
                new_connection->fd = new_fd;
                server.fds[server.nfds].events = POLLIN;
                server.fds[server.nfds].revents = 0;
                server.fds[server.nfds].fd = new_fd;
                server.nfds += 1;
                //pthread_mutex_unlock(&server.nfds_mutex);
                logn("New connection received %i", new_connection->ext_addr.sin_addr.s_addr);
              } while (new_fd != -1);
            }
            // rec msg
            else
            {
              int connection_is_alive = 1;
              do
              {
                int result = recv(server.fds[i].fd, buffer, sizeof(buffer), 0);
                if (result < 0)
                {
                  if (errno != EWOULDBLOCK)
                  {
                    logn("recv() error");
                    Assert(0);
                  }
                  // nothing to read
                  logn("Nothing to recv() break");
                  break;
                }
                // conn closed
                else if (result == 0)
                {
                  connection_is_alive = 0;
                  //pthread_mutex_lock(&server.nfds_mutex);
                  if (i != server.nfds)
                  {
                    close(server.fds[i].fd);
                    // swap last
                    memcpy(server.fds + i,server.fds + (server.nfds - 1), sizeof(struct pollfd));
                    memcpy(server.connections + i, server.connections + (server.nfds - 1), sizeof(struct socket_connection));
                    i -= 1; // dont adv pointer
                  }
                  server.nfds -= 1;
                  if (server.nfds == 1)
                  {
                    // close server if no more connections
                    logn("Connections dropped to 0. Shutting down server");
                    server.keep_alive = 0;
                  }
                  //pthread_mutex_unlock(&server.nfds_mutex);
                }
                logn("%s",buffer);
              } while (connection_is_alive);
            }
        } // fd with data
        if (handled_responses >= number_of_events)
        {
          break;
        }
      } // for each fd
    } // polls any event
  } // while server alive
#endif

  logn("Shutting down server");

  for (int i = 0; i < server.nfds; ++i)
  {
    close(server.connections[i].fd);
  }

  close(socketfd);

  return 0;
}
