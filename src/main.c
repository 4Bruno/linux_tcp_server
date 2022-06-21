#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <sys/stat.h>
#include <stdio.h>
#include <stdlib.h>
#include <syslog.h>
#include <pthread.h>
#include <poll.h>
#include <stdarg.h>
#include <unistd.h> // sysconf
#include <fcntl.h>
#include <string.h>
#include <errno.h>
// BEGIN SIGTERM handling
#include <signal.h> 
#include <sys/signalfd.h>
// END SIGTERM handling

#ifdef WIN32
#include <windows.h>
#elif _POSIX_C_SOURCE >= 199309L
#include <time.h>   // for nanosleep
#endif

#define TCP_BEGIN_PRIVATE_PORT (0b11 << 14)
#define TCP_END_PRIVATE_PORT (0b01 << 16) - 1
#define TCP_MAX_PRIVATE_PORTS (TCP_BEGIN_PRIVATE_PORT - TCP_END_PRIVATE_PORT)

#define logn(str, ...) sync_printf(str "\n", ## __VA_ARGS__);
#define logerr(str, ...) sync_printf(str "\n", ## __VA_ARGS__);
#if 0
#define logerr_(str, file, line, func,  ...) sync_printf("[" #file "," #line "," #func "] Error: " str , ## __VA_ARGS__);
#define logerr(str, ...) logerr_(str , __FILE__ , __LINE__ , __func__ , ## __VA_ARGS__);
#endif

#define Assert(cond) if (!(cond)) { *(int *)0 = 0; };
#define ArrayCount(a) (int)(sizeof(a) / sizeof(a[0]))

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
static FILE * log_file;

void
sync_printf(const char *format, ...)
{
  va_list args;
  va_start(args, format);

  pthread_mutex_lock(&printf_mutex);
#if DEBUG
  vprintf(format, args);
#else
  vfprintf(log_file,format,args);
  fflush(log_file);
#endif
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
  __attribute__((aligned (8)))
#define MAX_OPEN_CONNECTIONS 12
  int socket_fd;
  int signal_fd;
  struct socket_connection connections[MAX_OPEN_CONNECTIONS];
  pthread_mutex_t nfds_mutex;
  int nfds;
  int fds_begin_client_index;
  struct pollfd fds[MAX_OPEN_CONNECTIONS];
  int max_open_connections;

  pthread_cond_t condition_connection_released;
  pthread_mutex_t mutex_connection;

  struct thread_queue * queue;

  int default_timeout;
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

#if 0
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
#endif

int
max_val(int a, int b)
{
  int result = a > b ? a : b;
  return result;
}

#define TCP_SERVER_NONBLOCKING 1
int
CreateServer(struct server * server, int port, int tcp_server_type, int signal_fd, struct thread_queue * ThreadQueue)
{

  memset(server,0,sizeof(struct server));

  server->socket_fd = socket(AF_INET, SOCK_STREAM, 0);
  if (server->socket_fd > 0)
  {
    struct sockaddr_in my_addr = {0};
    memset(&my_addr, 0 , sizeof(my_addr));
    my_addr.sin_family = AF_INET;
    my_addr.sin_port = htons(port);
    my_addr.sin_addr.s_addr = INADDR_ANY;

    // re-use socket
    int optval = 0;
    if (setsockopt(server->socket_fd,SOL_SOCKET,SO_REUSEADDR,(char *)&optval, sizeof(optval)) == 0)
    {
      if (tcp_server_type & TCP_SERVER_NONBLOCKING)
      {
        int socketfd_flags = fcntl(server->socket_fd, F_GETFL);
        fcntl(server->socket_fd, F_SETFL, socketfd_flags | O_NONBLOCK);
      }

      if (bind(server->socket_fd, (struct sockaddr *)&my_addr, sizeof(struct sockaddr)) == 0)
      {
        if(listen(server->socket_fd, MAX_OPEN_CONNECTIONS) == 0)
        {
          server->keep_alive = 1;
          server->default_timeout = 30 * 1000 * 6;

          // reserved fd
          int fd_reserved[] = {
            server->socket_fd,  // accept connections
            signal_fd           // SIGTERM signals
          };

          for (int i = 0; i < ArrayCount(fd_reserved);++i)
          {
            struct pollfd * pollfd = server->fds + server->nfds++;
            pollfd->fd = fd_reserved[i];
            pollfd->events = POLLIN;
          }

          if (ThreadQueue != NULL)
          {
            server->queue = ThreadQueue;
          }
          server->fds_begin_client_index = server->nfds;
        }
        else
        {
          logerr("listen()");
        }
      }
      else
      {
        logerr("bind()");
      }
    }
    else
    {
      logerr("setsockopt()");
    }
  }
  else
  {
    logerr("socket()");
  }

  int server_creation_error = (server->keep_alive == 0);

  return server_creation_error;
}

int
Daemonize(const char * log_file_name)
{
  int result = 0;

  // daemon fork its own process
  pid_t pid, sid;
  pid = fork();

  if (pid < 0)
  {
    printf("Error on fork()\n");
    exit(EXIT_FAILURE);
  }

  if (pid > 0) 
  {
    printf("Success fork with pid %i\n",pid);
    exit(EXIT_SUCCESS);
  }

  // full access to files generated by daemon
  umask(0);

  // logs
  pthread_mutex_init(&printf_mutex,NULL);

  log_file = fopen(log_file_name, "wt+");

  sid = setsid();
  if (sid < 0 )
  {
    logn("Error on setid()");
    exit(EXIT_FAILURE);
  }

  if (chdir("/") < 0)
  {
    logn("Error on chdir(""/"")");
    exit (EXIT_FAILURE);
  }

  // disable std
#if DISABLE_STDIO
  close(STDIN_FILENO);
  close(STDOUT_FILENO);
  close(STDERR_FILENO);
#endif

  return result;
}



int
main()
{

  const char * log_file_name = "./linux_tcp_server.log";
#if DAEMON
  printf("Log file is %s", log_file_name);
  Daemonize(log_file_name);
#else
  // logs
  log_file = fopen(log_file_name, "wt+");
  pthread_mutex_init(&printf_mutex,NULL);
#endif

  logerr("test()");

  /* THREAD POOL */
  const int max_threads = 40;
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

  for (int i = 0; i < count_threads; ++i)
  {
    pthread_t * thread = list_threads + i;
    int thread_id =	pthread_create(thread, NULL,ThreadHandler, &primary_thread_queue);
    Assert(thread_id == 0);
  }

  /* SIGTERM handlers */
  sigset_t sigset;
  struct signalfd_siginfo siginfo;
  int signal_fd;

  if (sigemptyset(&sigset) != 0)
  {
    logerr("sigemptyset()");
    return 0;
  }
  if (sigaddset(&sigset, SIGTERM) != 0)
  {
    logerr("sigaddset()");
    return 0;
  }
  if (sigprocmask(SIG_SETMASK,&sigset, NULL) != 0)
  {
    logerr("sigprocmask()");
    return 0;
  }

  signal_fd = signalfd(-1, &sigset, SFD_NONBLOCK);

  struct server server;
  if (CreateServer(&server,
                   TCP_BEGIN_PRIVATE_PORT, 
                   TCP_SERVER_NONBLOCKING, 
                   signal_fd,
                   &primary_thread_queue) != 0)
  {
    logn("Failed to create server. Terminating.");
    return 0;
  }

  char buffer[80];
  while (server.keep_alive)
  {
    int number_of_events = poll(server.fds, server.nfds,server.default_timeout);
    logn("Poll() returns with %i events", number_of_events);

    if (number_of_events < 0)
    {
      logn("Poll() returns error %i",number_of_events);
      server.keep_alive = 0;
    }
    else if (number_of_events == 0)
    {
      logn("Poll() timeout");
      server.keep_alive = 0;
    }
    else
    {
      int handled_responses = 0;

      struct pollfd * server_pollfd = server.fds + 0;
      // new connections
      if (server_pollfd->revents & POLLIN)
      {
        int new_fd = -1;
        do
        {
          struct sockaddr_in addr;
          socklen_t addr_size = sizeof(struct sockaddr);
          logn("Invoking accept()");
          new_fd = accept(server_pollfd->fd, (struct sockaddr *)&addr, &addr_size);
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

          if (server.nfds >= MAX_OPEN_CONNECTIONS)
          {
            // http://man7.org/linux/man-pages/man2/poll.2.html#DESCRIPTION
            // if fd is < 0 is ignored
            server.fds[0].fd = -server.fds[0].fd;
          }

          //pthread_mutex_unlock(&server.nfds_mutex);
          logn("New connection received %i", new_connection->ext_addr.sin_addr.s_addr);
        } while (new_fd != -1);
      }

      // SIGTERM
      if (server.fds[1].revents & POLLIN)
      {
        logn("SIGTERM received");
        if (read(server.fds[1].fd, &siginfo, sizeof(siginfo)) != sizeof(siginfo))
        {
          logerr("read() signal_fd file returns data bigger than siginfo size");
        }
        server.keep_alive = 0;
      }

      // Client connections
      for (int i = server.fds_begin_client_index;
           i < server.nfds;
           ++i)
      {
        struct pollfd * test_pollfd = server.fds + i;
        if (test_pollfd->revents == 0)
        {
          continue;
        }
        else if (test_pollfd->revents == POLLIN)
        {
          int connection_is_alive = 1;
          do
          {
            int result = recv(test_pollfd->fd, buffer, sizeof(buffer), 0);

            if (result < 0)
            {
              if (errno != EWOULDBLOCK)
              {
                logerr("recv()");
                connection_is_alive = 0;
                break;
              }
              // nothing to read
              logn("Nothing to recv() break");
              break;
            }

            if (result == 0 || (connection_is_alive == 0))
            {
              connection_is_alive = 0;
              //pthread_mutex_lock(&server.nfds_mutex);
              if (i != server.nfds)
              {
                close(test_pollfd->fd);
                // swap last
                struct pollfd * last_pollfd = server.fds + (server.nfds - 1);
                memcpy(test_pollfd, last_pollfd, sizeof(struct pollfd));
                memcpy(server.connections + i, server.connections + (server.nfds - 1), sizeof(struct socket_connection));
                i -= 1; // dont adv pointer
              }

              // check if max capacity was reached and accept new connections
              if (server.nfds >= MAX_OPEN_CONNECTIONS)
              {
                // http://man7.org/linux/man-pages/man2/poll.2.html#DESCRIPTION
                // if fd is < 0 is ignored
                Assert(server_pollfd->fd < 0);
                server_pollfd->fd = -server_pollfd->fd;
              }

              // only now decrement
              server.nfds -= 1;
              if (server.nfds == server.fds_begin_client_index)
              {
                // close server if no more connections
                logn("All connections closed");
              }

              //pthread_mutex_unlock(&server.nfds_mutex);
            }
            logn("Client sends message: %s",buffer);
          } while (connection_is_alive);
        } // fd with data
        
        if (handled_responses >= number_of_events)
        {
          break;
        }
      } // for each client fd
    } // polls any event
  } // while server alive

  logn("Shutting down server");
  fclose(log_file);

  for (int i = 0; i < server.nfds; ++i)
  {
    close(server.fds[i].fd);
  }

  return 0;
}
