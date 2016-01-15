#include "ftp.h"
#include <arpa/inet.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <malloc.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef _3DS
#include <3ds.h>
#define lstat stat
#endif
#include "console.h"

#define POLL_UNKNOWN    (~(POLLIN|POLLOUT))

#define XFER_BUFFERSIZE 32768
#define SOCK_BUFFERSIZE 32768
#define FILE_BUFFERSIZE 65536
#define CMD_BUFFERSIZE  1024
#define SOCU_ALIGN      0x1000
#define SOCU_BUFFERSIZE 0x100000
#define LISTEN_PORT     5000
#ifdef _3DS
#define DATA_PORT       (LISTEN_PORT+1)
#else
#define DATA_PORT       0 /* ephemeral port */
#endif

typedef struct ftp_session_t ftp_session_t;

#define FTP_DECLARE(x) static int x(ftp_session_t *session, const char *args)
FTP_DECLARE(ALLO);
FTP_DECLARE(APPE);
FTP_DECLARE(CDUP);
FTP_DECLARE(CWD);
FTP_DECLARE(DELE);
FTP_DECLARE(FEAT);
FTP_DECLARE(LIST);
FTP_DECLARE(MKD);
FTP_DECLARE(MODE);
FTP_DECLARE(NLST);
FTP_DECLARE(NOOP);
FTP_DECLARE(OPTS);
FTP_DECLARE(PASS);
FTP_DECLARE(PASV);
FTP_DECLARE(PORT);
FTP_DECLARE(PWD);
FTP_DECLARE(QUIT);
FTP_DECLARE(REST);
FTP_DECLARE(RETR);
FTP_DECLARE(RMD);
FTP_DECLARE(RNFR);
FTP_DECLARE(RNTO);
FTP_DECLARE(STOR);
FTP_DECLARE(STOU);
FTP_DECLARE(STRU);
FTP_DECLARE(SYST);
FTP_DECLARE(TYPE);
FTP_DECLARE(USER);

/*! session state */
typedef enum
{
  COMMAND_STATE,       /*!< waiting for a command */
  DATA_CONNECT_STATE,  /*!< waiting for connection after PASV command */
  DATA_TRANSFER_STATE, /*!< data transfer in progress */
} session_state_t;

/*! ftp session */
struct ftp_session_t
{
  char               cwd[4096]; /*!< current working directory */
  struct sockaddr_in peer_addr; /*!< peer address for data connection */
  struct sockaddr_in pasv_addr; /*!< listen address for PASV connection */
  int                cmd_fd;    /*!< socket for command connection */
  int                pasv_fd;   /*!< listen socket for PASV */
  int                data_fd;   /*!< socket for data transfer */
/*! data transfers in binary mode */
#define SESSION_BINARY (1 << 0)
/*! have pasv_addr ready for data transfer command */
#define SESSION_PASV   (1 << 1)
/*! have peer_addr ready for data transfer command */
#define SESSION_PORT   (1 << 2)
/*! data transfer in source mode */
#define SESSION_RECV   (1 << 3)
/*! data transfer in sink mode */
#define SESSION_SEND   (1 << 4)
/*! last command was RNFR and buffer contains path */
#define SESSION_RENAME (1 << 5)
  int                flags;     /*!< session flags */
  session_state_t    state;     /*!< session state */
  ftp_session_t      *next;     /*!< link to next session */
  ftp_session_t      *prev;     /*!< link to prev session */

  int      (*transfer)(ftp_session_t*);  /*! data transfer callback */
  char     buffer[XFER_BUFFERSIZE];      /*! persistent data between callbacks */
  char     tmp_buffer[XFER_BUFFERSIZE];  /*! persistent data between callbacks */
  char     file_buffer[FILE_BUFFERSIZE]; /*! stdio file buffer */
  size_t   bufferpos;                    /*! persistent buffer position between callbacks */
  size_t   buffersize;                   /*! persistent buffer size between callbacks */
  uint64_t filepos;                      /*! persistent file position between callbacks */
  uint64_t filesize;                     /*! persistent file size between callbacks */
  union
  {
    DIR    *dp;                          /*! persistent open directory pointer between callbacks */
    FILE   *fp;                          /*! persistent open file pointer between callbacks */
  };
};

/*! ftp command descriptor */
typedef struct ftp_command
{
  const char *name;                                   /*!< command name */
  int        (*handler)(ftp_session_t*, const char*); /*!< command callback */
} ftp_command_t;

static ftp_command_t ftp_commands[] =
{
#define FTP_COMMAND(x) { #x, x, }
#define FTP_ALIAS(x,y) { #x, y, }
  FTP_COMMAND(ALLO),
  FTP_COMMAND(APPE),
  FTP_COMMAND(CDUP),
  FTP_COMMAND(CWD),
  FTP_COMMAND(DELE),
  FTP_COMMAND(FEAT),
  FTP_COMMAND(LIST),
  FTP_COMMAND(MKD),
  FTP_COMMAND(MODE),
  FTP_COMMAND(NLST),
  FTP_COMMAND(NOOP),
  FTP_COMMAND(OPTS),
  FTP_COMMAND(PASS),
  FTP_COMMAND(PASV),
  FTP_COMMAND(PORT),
  FTP_COMMAND(PWD),
  FTP_COMMAND(QUIT),
  FTP_COMMAND(REST),
  FTP_COMMAND(RETR),
  FTP_COMMAND(RMD),
  FTP_COMMAND(RNFR),
  FTP_COMMAND(RNTO),
  FTP_COMMAND(STOR),
  FTP_COMMAND(STOU),
  FTP_COMMAND(STRU),
  FTP_COMMAND(SYST),
  FTP_COMMAND(TYPE),
  FTP_COMMAND(USER),
  FTP_ALIAS(XCUP, CDUP),
  FTP_ALIAS(XMKD, MKD),
  FTP_ALIAS(XPWD, PWD),
  FTP_ALIAS(XRMD, RMD),
};
/*! number of ftp commands */
static const size_t num_ftp_commands = sizeof(ftp_commands)/sizeof(ftp_commands[0]);

/*! compare ftp command descriptors
 *
 *  @param[in] p1 left side of comparison (ftp_command_t*)
 *  @param[in] p2 right side of comparison (ftp_command_t*)
 *
 *  @returns <0 if p1 <  p2
 *  @returns 0 if  p1 == p2
 *  @returns >0 if p1 >  p2
 */
static int
ftp_command_cmp(const void *p1,
                const void *p2)
{
  ftp_command_t *c1 = (ftp_command_t*)p1;
  ftp_command_t *c2 = (ftp_command_t*)p2;

  /* ordered by command name */
  return strcasecmp(c1->name, c2->name);
}

#ifdef _3DS
/*! SOC service buffer */
static u32 *SOCU_buffer = NULL;
#endif

/*! server listen address */
static struct sockaddr_in serv_addr;
/*! listen file descriptor */
static int                listenfd = -1;
#ifdef _3DS
/*! current data port */
static in_port_t          data_port = DATA_PORT;
#endif
/*! list of ftp sessions */
static ftp_session_t      *sessions = NULL;
/*! socket buffersize */
static int                sock_buffersize = SOCK_BUFFERSIZE;

/*! Allocate a new data port
 *
 *  @returns next data port
 */
static in_port_t
next_data_port(void)
{
#ifdef _3DS
  if(++data_port >= 10000)
    data_port = DATA_PORT;
  return data_port;
#else
  return 0; /* ephemeral port */
#endif
}

/*! set a socket to non-blocking
 *
 *  @param[in] fd socket
 *
 *  @returns error
 */
static int
ftp_set_socket_nonblocking(int fd)
{
  int rc, flags;

  flags = fcntl(fd, F_GETFL, 0);
  if(flags == -1)
  {
    console_print(RED "fcntl: %d %s\n" RESET, errno, strerror(errno));
    return -1;
  }

  rc = fcntl(fd, F_SETFL, flags | O_NONBLOCK);
  if(rc != 0)
  {
    console_print(RED "fcntl: %d %s\n" RESET, errno, strerror(errno));
    return -1;
  }

  return 0;
}

/*! set socket options
 *
 *  @param[in] fd socket
 */
static void
ftp_set_socket_options(int fd)
{
  int rc;

  /* it's okay if this fails */
  rc = setsockopt(fd, SOL_SOCKET, SO_RCVBUF,
                  &sock_buffersize, sizeof(sock_buffersize));
  if(rc != 0)
  {
    console_print(RED "setsockopt: %d %s\n" RESET, errno, strerror(errno));
  }

  /* it's okay if this fails */
  rc = setsockopt(fd, SOL_SOCKET, SO_SNDBUF,
                  &sock_buffersize, sizeof(sock_buffersize));
  if(rc != 0)
  {
    console_print(RED "setsockopt: %d %s\n" RESET, errno, strerror(errno));
  }
}

/*! close a socket
 *
 *  @param[in] fd        socket to close
 *  @param[in] connected whether this socket is connected
 */
static void
ftp_closesocket(int fd, int connected)
{
  int                rc;
  struct sockaddr_in addr;
  socklen_t          addrlen = sizeof(addr);

  if(connected)
  {
    /* get peer address and print */
    rc = getpeername(fd, (struct sockaddr*)&addr, &addrlen);
    if(rc != 0)
    {
      console_print(RED "getpeername: %d %s\n" RESET, errno, strerror(errno));
      console_print(YELLOW "closing connection to fd=%d\n" RESET, fd);
    }
    else
      console_print(YELLOW "closing connection to %s:%u\n" RESET,
                    inet_ntoa(addr.sin_addr), ntohs(addr.sin_port));

    /* shutdown connection */
    rc = shutdown(fd, SHUT_RDWR);
    if(rc != 0)
      console_print(RED "shutdown: %d %s\n" RESET, errno, strerror(errno));
  }

  /* close socket */
  rc = close(fd);
  if(rc != 0)
    console_print(RED "close: %d %s\n" RESET, errno, strerror(errno));
}

/*! close command socket on ftp session
 *
 *  @param[in] session ftp session
 */
static void
ftp_session_close_cmd(ftp_session_t *session)
{
  /* close command socket */
  ftp_closesocket(session->cmd_fd, 1);
  session->cmd_fd = -1;
}

/*! close listen socket on ftp session
 *
 *  @param[in] session ftp session
 */
static void
ftp_session_close_pasv(ftp_session_t *session)
{
  console_print(YELLOW "stop listening on %s:%u\n" RESET,
                inet_ntoa(session->pasv_addr.sin_addr),
                ntohs(session->pasv_addr.sin_port));

  /* close pasv socket */
  ftp_closesocket(session->pasv_fd, 0);
  session->pasv_fd = -1;
}

/*! close data socket on ftp session
 *
 *  @param[in] session ftp session */
static void
ftp_session_close_data(ftp_session_t *session)
{
  /* close data connection */
  ftp_closesocket(session->data_fd, 1);
  session->data_fd = -1;

  /* clear send/recv flags */
  session->flags &= ~(SESSION_RECV|SESSION_SEND);
}

/*! close open file for ftp session
 *
 *  @param[in] session ftp session
 */
static void
ftp_session_close_file(ftp_session_t *session)
{
  int rc;

  rc = fclose(session->fp);
  if(rc != 0)
    console_print(RED "fclose: %d %s\n" RESET, errno, strerror(errno));
  session->fp = NULL;
}

/*! open file for reading for ftp session
 *
 *  @param[in] session ftp session
 *
 *  @returns -1 for error
 */
static int
ftp_session_open_file_read(ftp_session_t *session)
{
  int         rc;
  struct stat st;

  /* open file in read mode */
  session->fp = fopen(session->buffer, "rb");
  if(session->fp == NULL)
  {
    console_print(RED "fopen '%s': %d %s\n" RESET, session->buffer, errno, strerror(errno));
    return -1;
  }

  /* it's okay if this fails */
  errno = 0;
  rc = setvbuf(session->fp, session->file_buffer, _IOFBF, FILE_BUFFERSIZE);
  if(rc != 0)
  {
    console_print(RED "setvbuf: %d %s\n" RESET, errno, strerror(errno));
  }

  /* get the file size */
  rc = fstat(fileno(session->fp), &st);
  if(rc != 0)
  {
    console_print(RED "fstat '%s': %d %s\n" RESET, session->buffer, errno, strerror(errno));
    ftp_session_close_file(session);
    return -1;
  }
  session->filesize = st.st_size;

  /* reset file position */
  /* TODO: support REST command */
  session->filepos = 0;

  return 0;
}

/*! read from an open file for ftp session
 *
 *  @param[in] session ftp session
 *
 *  @returns bytes read
 */
static ssize_t
ftp_session_read_file(ftp_session_t *session)
{
  ssize_t rc;

  /* read file at current position */
  rc = fread(session->buffer, 1, sizeof(session->buffer), session->fp);
  if(rc < 0)
  {
    console_print(RED "fread: %d %s\n" RESET, errno, strerror(errno));
    return -1;
  }

  /* adjust file position */
  session->filepos += rc;

  return rc;
}

/*! open file for writing for ftp session
 *
 *  @param[in] session ftp session
 *
 *  @returns -1 for error
 *
 *  @note truncates file
 */
static int
ftp_session_open_file_write(ftp_session_t *session)
{
  int rc;

  /* open file in write and create mode with truncation */
  session->fp = fopen(session->buffer, "wb");
  if(session->fp == NULL)
  {
    console_print(RED "fopen '%s': %d %s\n" RESET, session->buffer, errno, strerror(errno));
    return -1;
  }

  /* it's okay if this fails */
  errno = 0;
  rc = setvbuf(session->fp, session->file_buffer, _IOFBF, FILE_BUFFERSIZE);
  if(rc != 0)
  {
    console_print(RED "setvbuf: %d %s\n" RESET, errno, strerror(errno));
  }

  /* reset file position */
  /* TODO: support REST command */
  session->filepos = 0;

  return 0;
}

/*! write to an open file for ftp session
 *
 *  @param[in] session ftp session
 *
 *  @returns bytes written
 */
static ssize_t
ftp_session_write_file(ftp_session_t *session)
{
  ssize_t rc;

  /* write to file at current position */
  rc = fwrite(session->buffer + session->bufferpos,
              1, session->buffersize - session->bufferpos,
              session->fp);
  if(rc < 0)
  {
    console_print(RED "fwrite: %d %s\n" RESET, errno, strerror(errno));
    return -1;
  }
  else if(rc == 0)
    console_print(RED "fwrite: wrote 0 bytes\n" RESET);

  /* adjust file position */
  session->filepos += rc;

  return rc;
}

/*! close current working directory for ftp session
 *
 *   @param[in] session ftp session
 */
static void
ftp_session_close_cwd(ftp_session_t *session)
{
  int rc;

  /* close open directory pointer */
  rc = closedir(session->dp);
  if(rc != 0)
    console_print(RED "closedir: %d %s\n" RESET, errno, strerror(errno));
  session->dp = NULL;
}

/*! open current working directory for ftp session
 *
 *  @param[in] session ftp session
 *
 *  @return -1 for failure
 */
static int
ftp_session_open_cwd(ftp_session_t *session)
{
  /* open current working directory */
  session->dp = opendir(session->cwd);
  if(session->dp == NULL)
  {
    console_print(RED "opendir '%s': %d %s\n" RESET, session->cwd, errno, strerror(errno));
    return -1;
  }

  return 0;
}

/*! set state for ftp session
 *
 *  @param[in] session ftp session
 *  @param[in] state   state to set
 */
static void
ftp_session_set_state(ftp_session_t   *session,
                      session_state_t state)
{
  session->state = state;

  switch(state)
  {
    case COMMAND_STATE:
      /* close pasv and data sockets */
      if(session->pasv_fd >= 0)
        ftp_session_close_pasv(session);
      if(session->data_fd >= 0)
        ftp_session_close_data(session);
      break;

    case DATA_CONNECT_STATE:
      /* close data socket; we are listening for a new one */
      if(session->data_fd >= 0)
        ftp_session_close_data(session);
      break;

    case DATA_TRANSFER_STATE:
      /* close pasv socket; we are connecting for a new one */
      if(session->pasv_fd >= 0)
        ftp_session_close_pasv(session);
  }
}

static void
ftp_session_transfer(ftp_session_t *session)
{
  int rc;
  do
  {
    rc = session->transfer(session);
  } while(rc == 0);
}

__attribute__((format(printf,3,4)))
/*! send ftp response to ftp session's peer
 *
 *  @param[in] session ftp session
 *  @param[in] code    response code
 *  @param[in] fmt     format string
 *  @param[in] ...     format arguments
 *
 *  returns bytes send to peer
 */
static ssize_t
ftp_send_response(ftp_session_t *session,
                  int           code,
                  const char    *fmt, ...)
{
  static char buffer[CMD_BUFFERSIZE];
  ssize_t     rc, to_send;
  va_list     ap;

  /* print response code and message to buffer */
  va_start(ap, fmt);
  if(code != 211)
    rc = sprintf(buffer, "%d ", code);
  else
    rc = sprintf(buffer, "%d- ", code);
  rc += vsnprintf(buffer+rc, sizeof(buffer)-rc, fmt, ap);
  va_end(ap);

  if(rc >= sizeof(buffer))
  {
    /* couldn't fit message; just send code */
    console_print(RED "%s: buffersize too small\n" RESET, __func__);
    rc = sprintf(buffer, "%d\r\n", code);
  }

  /* send response */
  to_send = rc;
  console_print(GREEN "%s" RESET, buffer);
  rc = send(session->cmd_fd, buffer, to_send, 0);
  if(rc < 0)
    console_print(RED "send: %d %s\n" RESET, errno, strerror(errno));
  else if(rc != to_send)
    console_print(RED "only sent %u/%u bytes\n" RESET,
                  (unsigned int)rc, (unsigned int)to_send);

  return rc;
}

/*! destroy ftp session
 *
 *  @param[in] session ftp session
 */
static ftp_session_t*
ftp_session_destroy(ftp_session_t *session)
{
  ftp_session_t *next = session->next;

  /* close all sockets */
  if(session->cmd_fd >= 0)
    ftp_session_close_cmd(session);
  if(session->pasv_fd >= 0)
    ftp_session_close_pasv(session);
  if(session->data_fd >= 0)
    ftp_session_close_data(session);

  /* unlink from sessions list */
  if(session->next)
    session->next->prev = session->prev;
  if(session == sessions)
    sessions = session->next;
  else
  {
    session->prev->next = session->next;
    if(session == sessions->prev)
      sessions->prev = session->prev;
  }

  /* deallocate */
  free(session);

  return next;
}

/*! allocate new ftp session
 *
 *  @param[in] listen_fd socket to accept connection from
 */
static void
ftp_session_new(int listen_fd)
{
  ssize_t            rc;
  int                new_fd;
  ftp_session_t      *session;
  struct sockaddr_in addr;
  socklen_t          addrlen = sizeof(addr);

  /* accept connection */
  new_fd = accept(listen_fd, (struct sockaddr*)&addr, &addrlen);
  if(new_fd < 0)
  {
    console_print(RED "accept: %d %s\n" RESET, errno, strerror(errno));
    return;
  }

  console_print(CYAN "accepted connection from %s:%u\n" RESET,
                inet_ntoa(addr.sin_addr), ntohs(addr.sin_port));

  /* allocate a new session */
  session = (ftp_session_t*)malloc(sizeof(ftp_session_t));
  if(session == NULL)
  {
    console_print(RED "failed to allocate session\n" RESET);
    ftp_closesocket(new_fd, 1);
    return;
  }

  /* initialize session */
  memset(session->cwd, 0, sizeof(session->cwd));
  strcpy(session->cwd, "/");
  session->peer_addr.sin_addr.s_addr = INADDR_ANY;
  session->cmd_fd   = new_fd;
  session->pasv_fd  = -1;
  session->data_fd  = -1;
  session->flags    = 0;
  session->state    = COMMAND_STATE;
  session->next     = NULL;
  session->prev     = NULL;
  session->transfer = NULL;

  /* link to the sessions list */
  if(sessions == NULL)
  {
    sessions = session;
    session->prev = session;
  }
  else
  {
    sessions->prev->next = session;
    session->prev        = sessions->prev;
    sessions->prev       = session;
  }

  /* copy socket address to pasv address */
  addrlen = sizeof(session->pasv_addr);
  rc = getsockname(new_fd, (struct sockaddr*)&session->pasv_addr, &addrlen);
  if(rc != 0)
  {
    console_print(RED "getsockname: %d %s\n" RESET, errno, strerror(errno));
    ftp_send_response(session, 451, "Failed to get connection info\r\n");
    ftp_session_destroy(session);
    return;
  }

  session->cmd_fd = new_fd;

  /* send initiator response */
  rc = ftp_send_response(session, 200, "Hello!\r\n");
  if(rc <= 0)
    ftp_session_destroy(session);
}

/*! accept PASV connection for ftp session
 *
 *  @param[in] session ftp session
 *
 *  @returns -1 for failure
 */
static int
ftp_session_accept(ftp_session_t *session)
{
  int                rc, new_fd;
  struct sockaddr_in addr;
  socklen_t          addrlen = sizeof(addr);

  if(session->flags & SESSION_PASV)
  {
    /* clear PASV flag */
    session->flags &= ~SESSION_PASV;

    /* tell the peer that we're ready to accept the connection */
    ftp_send_response(session, 150, "Ready\r\n");

    /* accept connection from peer */
    new_fd = accept(session->pasv_fd, (struct sockaddr*)&addr, &addrlen);
    if(new_fd < 0)
    {
      console_print(RED "accept: %d %s\n" RESET, errno, strerror(errno));
      ftp_session_set_state(session, COMMAND_STATE);
      ftp_send_response(session, 425, "Failed to establish connection\r\n");
      return -1;
    }

    rc = ftp_set_socket_nonblocking(new_fd);
    if(rc != 0)
    {
      ftp_closesocket(new_fd, 1);
      ftp_session_set_state(session, COMMAND_STATE);
      ftp_send_response(session, 425, "Failed to establish connection\r\n");
      return -1;
    }

    console_print(CYAN "accepted connection from %s:%u\n" RESET,
                  inet_ntoa(addr.sin_addr), ntohs(addr.sin_port));

    ftp_session_set_state(session, DATA_TRANSFER_STATE);
    session->data_fd = new_fd;

    return 0;
  }
  else
  {
    /* peer didn't send PASV command */
    ftp_send_response(session, 503, "Bad sequence of commands\r\n");
    return -1;
  }
}

/*! connect to peer for ftp session
 *
 *  @param[in] session ftp session
 *
 *  @returns -1 for failure
 */
static int
ftp_session_connect(ftp_session_t *session)
{
  int rc;

  /* clear PORT flag */
  session->flags &= ~SESSION_PORT;

  /* create a new socket */
  session->data_fd = socket(AF_INET, SOCK_STREAM, 0);
  if(session->data_fd < 0)
  {
    console_print(RED "socket: %d %s\n" RESET, errno, strerror(errno));
    return -1;
  }

  /* set socket options */
  ftp_set_socket_options(session->data_fd);

  /* connect to peer */
  rc = connect(session->data_fd, (struct sockaddr*)&session->peer_addr,
               sizeof(session->peer_addr));
  if(rc != 0)
  {
    console_print(RED "connect: %d %s\n" RESET, errno, strerror(errno));
    ftp_closesocket(session->data_fd, 0);
    session->data_fd = -1;
    return -1;
  }

  rc = ftp_set_socket_nonblocking(session->data_fd);
  if(rc != 0)
    return -1;

  console_print(CYAN "connected to %s:%u\n" RESET,
                inet_ntoa(session->peer_addr.sin_addr),
                ntohs(session->peer_addr.sin_port));

  return 0;
}

/*! read command for ftp session
 *
 *  @param[in] session ftp session
 */
static void
ftp_session_read_command(ftp_session_t *session)
{
  static char   buffer[CMD_BUFFERSIZE];
  ssize_t       rc;
  char          *args;
  ftp_command_t key, *command;

  memset(buffer, 0, sizeof(buffer));

  /* retrieve command */
  rc = recv(session->cmd_fd, buffer, sizeof(buffer), 0);
  if(rc < 0)
  {
    /* error retrieving command */
    console_print(RED "recv: %d %s\n" RESET, errno, strerror(errno));
    ftp_session_close_cmd(session);
    return;
  }
  if(rc == 0)
  {
    /* peer closed connection */
    ftp_session_close_cmd(session);
    return;
  }
  else
  {
    /* split into command and arguments */
    /* TODO: support partial transfers */
    buffer[sizeof(buffer)-1] = 0;

    args = buffer;
    while(*args && *args != '\r' && *args != '\n')
      ++args;
    *args = 0;
    
    args = buffer;
    while(*args && !isspace((int)*args))
      ++args;
    if(*args)
      *args++ = 0;

    /* look up the command */
    key.name = buffer;
    command = bsearch(&key, ftp_commands,
                      num_ftp_commands, sizeof(ftp_command_t),
                      ftp_command_cmp);

    /* execute the command */
    if(command == NULL)
    {
      ftp_send_response(session, 502, "invalid command -> %s %s\r\n",
                        key.name, args);
    }
    else
    {
      /* clear RENAME flag for all commands except RNTO */
      if(strcasecmp(command->name, "RNTO") != 0)
        session->flags &= ~SESSION_RENAME;
      command->handler(session, args);
    }
  }
}

/*! poll sockets for ftp session
 *
 *  @param[in] session ftp session
 *
 *  @returns next session
 */
static ftp_session_t*
ftp_session_poll(ftp_session_t *session)
{
  int           rc;
  struct pollfd pollinfo;

  switch(session->state)
  {
    case COMMAND_STATE:
      /* we are waiting to read a command */
      pollinfo.fd      = session->cmd_fd;
      pollinfo.events  = POLLIN;
      pollinfo.revents = 0;
      break;

    case DATA_CONNECT_STATE:
      /* we are waiting for a PASV connection */
      pollinfo.fd      = session->pasv_fd;
      pollinfo.events  = POLLIN;
      pollinfo.revents = 0;
      break;

    case DATA_TRANSFER_STATE:
      /* we need to transfer data */
      pollinfo.fd = session->data_fd;
      if(session->flags & SESSION_RECV)
        pollinfo.events = POLLIN;
      else
        pollinfo.events = POLLOUT;
      pollinfo.revents = 0;
      break;
  }

  /* poll the selected socket */
  rc = poll(&pollinfo, 1, 0);
  if(rc < 0)
    console_print(RED "poll: %d %s\n" RESET, errno, strerror(errno));
  else if(rc > 0)
  {
    if(pollinfo.revents != 0)
    {
      /* handle event */
      switch(session->state)
      {
        case COMMAND_STATE:
          if(pollinfo.revents & POLL_UNKNOWN)
            console_print(YELLOW "cmd_fd: revents=0x%08X\n" RESET, pollinfo.revents);

          /* we need to read a new command */
          if(pollinfo.revents & (POLLERR|POLLHUP))
            ftp_session_close_cmd(session);
          else if(pollinfo.revents & POLLIN)
            ftp_session_read_command(session);
          break;

        case DATA_CONNECT_STATE:
          if(pollinfo.revents & POLL_UNKNOWN)
            console_print(YELLOW "pasv_fd: revents=0x%08X\n" RESET, pollinfo.revents);

          /* we need to accept the PASV connection */
          if(pollinfo.revents & (POLLERR|POLLHUP))
          {
            ftp_session_set_state(session, COMMAND_STATE);
            ftp_send_response(session, 426, "Data connection failed\r\n");
          }
          else if(pollinfo.revents & POLLIN)
          {
            if(ftp_session_accept(session) != 0)
              ftp_session_set_state(session, COMMAND_STATE);
          }
          break;

        case DATA_TRANSFER_STATE:
          if(pollinfo.revents & POLL_UNKNOWN)
            console_print(YELLOW "data_fd: revents=0x%08X\n" RESET, pollinfo.revents);

          /* we need to transfer data */
          if(pollinfo.revents & (POLLERR|POLLHUP))
          {
            ftp_session_set_state(session, COMMAND_STATE);
            ftp_send_response(session, 426, "Data connection failed\r\n");
          }
          else if(pollinfo.revents & (POLLIN|POLLOUT))
            ftp_session_transfer(session);
          break;
      }
    }
  }

  /* still connected to peer; return next session */
  if(session->cmd_fd >= 0)
    return session->next;

  /* disconnected from peer; destroy it and return next session */
  return ftp_session_destroy(session);
}

/*! initialize ftp subsystem */
int
ftp_init(void)
{
  int rc;

#ifdef _3DS
  Result  ret;

#if ENABLE_LOGGING
  /* open log file */
  FILE *fp = freopen("/ftbrony.log", "wb", stderr);
  if(fp == NULL)
  {
    console_print(RED "freopen: 0x%08X\n" RESET, errno);
    goto stderr_fail;
  }

  /* truncate log file */
  if(ftruncate(fileno(fp), 0) != 0)
  {
    console_print(RED "ftruncate: 0x%08X\n" RESET, errno);
    goto ftruncate_fail;
  }
#endif

  /* allocate buffer for SOC service */
  SOCU_buffer = (u32*)memalign(SOCU_ALIGN, SOCU_BUFFERSIZE);
  if(SOCU_buffer == NULL)
  {
    console_print(RED "memalign: failed to allocate\n" RESET);
    goto memalign_fail;
  }

  /* initialize SOC service */
  ret = socInit(SOCU_buffer, SOCU_BUFFERSIZE);
  if(ret != 0)
  {
    console_print(RED "socInit: 0x%08X\n" RESET, (unsigned int)ret);
    goto soc_fail;
  }
#endif

  /* allocate socket to listen for clients */
  listenfd = socket(AF_INET, SOCK_STREAM, 0);
  if(listenfd < 0)
  {
    console_print(RED "socket: %d %s\n" RESET, errno, strerror(errno));
    ftp_exit();
    return -1;
  }

  /* set socket options */
  ftp_set_socket_options(listenfd);

  /* get address to listen on */
  serv_addr.sin_family      = AF_INET;
#ifdef _3DS
  serv_addr.sin_addr.s_addr = gethostid();
  serv_addr.sin_port        = htons(LISTEN_PORT);
#else
  serv_addr.sin_addr.s_addr = INADDR_ANY;
  serv_addr.sin_port        = htons(LISTEN_PORT);
#endif

  /* reuse address */
  {
    int yes = 1;
    rc = setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    if(rc != 0)
    {
      console_print(RED "setsockopt: %d %s\n" RESET, errno, strerror(errno));
      ftp_exit();
      return -1;
    }
  }

  /* bind socket to listen address */
  rc = bind(listenfd, (struct sockaddr*)&serv_addr, sizeof(serv_addr));
  if(rc != 0)
  {
    console_print(RED "bind: %d %s\n" RESET, errno, strerror(errno));
    ftp_exit();
    return -1;
  }

  /* listen on socket */
  rc = listen(listenfd, 5);
  if(rc != 0)
  {
    console_print(RED "listen: %d %s\n" RESET, errno, strerror(errno));
    ftp_exit();
    return -1;
  }

  /* print server address */
#ifdef _3DS
  console_set_status("\n" GREEN STATUS_STRING " "
                     YELLOW "IP:"   CYAN "%s "
                     YELLOW "Port:" CYAN "%u"
                     RESET,
                     inet_ntoa(serv_addr.sin_addr),
                     ntohs(serv_addr.sin_port));
#else
  {
    char      hostname[128];
    socklen_t addrlen = sizeof(serv_addr);
    rc = getsockname(listenfd, (struct sockaddr*)&serv_addr, &addrlen);
    if(rc != 0)
    {
      console_print(RED "getsockname: %d %s\n" RESET, errno, strerror(errno));
      ftp_exit();
      return -1;
    }

    rc = gethostname(hostname, sizeof(hostname));
    if(rc != 0)
    {
      console_print(RED "gethostname: %d %s\n" RESET, errno, strerror(errno));
      ftp_exit();
      return -1;
    }

    console_set_status(GREEN STATUS_STRING " "
                       YELLOW "IP:"   CYAN "%s "
                       YELLOW "Port:" CYAN "%u"
                       RESET,
                       hostname,
                       ntohs(serv_addr.sin_port));
  }
#endif

  return 0;

#ifdef _3DS
soc_fail:
  free(SOCU_buffer);

memalign_fail:
#ifdef ENABLE_LOGGING
ftruncate_fail:
  if(fclose(stderr) != 0)
    console_print(RED "fclose: 0x%08X\n" RESET, errno);

stderr_fail:
#endif
  return -1;

#endif
}

/*! deinitialize ftp subsystem */
void
ftp_exit(void)
{
#ifdef _3DS
  Result ret;
#endif

  /* clean up all sessions */
  while(sessions != NULL)
    ftp_session_destroy(sessions);

  /* stop listening for new clients */
  if(listenfd >= 0)
    ftp_closesocket(listenfd, 0);

#ifdef _3DS
  /* deinitialize SOC service */
  ret = socExit();
  if(ret != 0)
    console_print(RED "socExit: 0x%08X\n" RESET, (unsigned int)ret);
  free(SOCU_buffer);

#ifdef ENABLE_LOGGING
  /* close log file */
  if(fclose(stderr) != 0)
    console_print(RED "fclose: 0x%08X\n" RESET, errno);

#endif

#endif
}

int
ftp_loop(void)
{
  int           rc;
  struct pollfd pollinfo;
  ftp_session_t *session;

  pollinfo.fd      = listenfd;
  pollinfo.events  = POLLIN;
  pollinfo.revents = 0;

  rc = poll(&pollinfo, 1, 0);
  if(rc < 0)
  {
    console_print(RED "poll: %d %s\n" RESET, errno, strerror(errno));
    return -1;
  }
  else if(rc > 0)
  {
    if(pollinfo.revents & POLLIN)
    {
      ftp_session_new(listenfd);
    }
    else
    {
      console_print(YELLOW "listenfd: revents=0x%08X\n" RESET, pollinfo.revents);
    }
  }

  session = sessions;
  while(session != NULL)
    session = ftp_session_poll(session);

#ifdef _3DS
  hidScanInput();
  if(hidKeysDown() & KEY_B)
    return -1;
#endif

  return 0;
}

static void
cd_up(ftp_session_t *session)
{
  char *slash = NULL, *p;

  for(p = session->cwd; *p; ++p)
  {
    if(*p == '/')
      slash = p;
  }
  *slash = 0;
  if(strlen(session->cwd) == 0)
    strcat(session->cwd, "/");
}

static int
validate_path(const char *args)
{
  const char *p;

  /* make sure no path components are '..' */
  p = args;
  while((p = strstr(p, "/..")) != NULL)
  {
    if(p[3] == 0 || p[3] == '/')
      return -1;
  }

  /* make sure there are no '//' */
  if(strstr(args, "//") != NULL)
    return -1;

  return 0;
}

static int
build_path(ftp_session_t *session,
           const char    *args)
{
  int  rc;
  char *p;

  memset(session->buffer, 0, sizeof(session->buffer));

  if(validate_path(args) != 0)
  {
    errno = EINVAL;
    return -1;
  }

  if(args[0] == '/')
  {
    if(strlen(args) > sizeof(session->buffer)-1)
    {
      errno = ENAMETOOLONG;
      return -1;
    }
    strncpy(session->buffer, args, sizeof(session->buffer));
  }
  else
  {
    if(strcmp(session->cwd, "/") == 0)
      rc = snprintf(session->buffer, sizeof(session->buffer), "/%s",
                    args);
    else
      rc = snprintf(session->buffer, sizeof(session->buffer), "%s/%s",
                    session->cwd, args);

    if(rc >= sizeof(session->buffer))
    {
      errno = ENAMETOOLONG;
      return -1;
    }
  }

  p = session->buffer + strlen(session->buffer);
  while(p > session->buffer && *--p == '/')
    *p = 0;

  if(strlen(session->buffer) == 0)
    strcpy(session->buffer, "/");

  return 0;
}

static int
list_transfer(ftp_session_t *session)
{
  ssize_t rc;

  if(session->bufferpos == session->buffersize)
  {
    struct stat   st;
    struct dirent *dent = readdir(session->dp);
    if(dent == NULL)
    {
      ftp_session_close_cwd(session);
      ftp_session_set_state(session, COMMAND_STATE);
      ftp_send_response(session, 226, "OK\r\n");
      return -1;
    }

    if(strcmp(dent->d_name, ".") == 0 || strcmp(dent->d_name, "..") == 0)
      return 0;

    if(strcmp(session->cwd, "/") == 0)
      snprintf(session->buffer, sizeof(session->buffer),
               "/%s", dent->d_name);
    else
      snprintf(session->buffer, sizeof(session->buffer),
               "%s/%s", session->cwd, dent->d_name);
    rc = lstat(session->buffer, &st);
    if(rc != 0)
    {
      console_print(RED "stat '%s': %d %s\n" RESET, session->buffer, errno, strerror(errno));
      ftp_session_close_cwd(session);
      ftp_session_set_state(session, COMMAND_STATE);
      ftp_send_response(session, 550, "unavailable\r\n");
      return -1;
    }

    session->buffersize =
        sprintf(session->buffer,
                "%crwxrwxrwx 1 3DS 3DS %llu Jan 1 1970 %s\r\n",
                S_ISDIR(st.st_mode) ? 'd' :
                S_ISLNK(st.st_mode) ? 'l' : '-',
                (unsigned long long)st.st_size,
                dent->d_name);
    session->bufferpos = 0;
  }

  rc = send(session->data_fd, session->buffer + session->bufferpos,
            session->buffersize - session->bufferpos, 0);
  if(rc <= 0)
  {
    if(rc < 0)
    {
      if(errno == EWOULDBLOCK)
        return -1;
      console_print(RED "send: %d %s\n" RESET, errno, strerror(errno));
    }
    else
      console_print(YELLOW "send: %d %s\n" RESET, ECONNRESET, strerror(ECONNRESET));

    ftp_session_close_cwd(session);
    ftp_session_set_state(session, COMMAND_STATE);
    ftp_send_response(session, 426, "Connection broken during transfer\r\n");
    return -1;
  }

  session->bufferpos += rc;
  return 0;
}

static int
retrieve_transfer(ftp_session_t *session)
{
  ssize_t rc;

  if(session->bufferpos == session->buffersize)
  {
    rc = ftp_session_read_file(session);
    if(rc <= 0)
    {
      ftp_session_close_file(session);
      ftp_session_set_state(session, COMMAND_STATE);
      if(rc < 0)
        ftp_send_response(session, 451, "Failed to read file\r\n");
      else
        ftp_send_response(session, 226, "OK\r\n");
      return -1;
    }

    session->bufferpos  = 0;
    session->buffersize = rc;
  }

  rc = send(session->data_fd, session->buffer + session->bufferpos,
            session->buffersize - session->bufferpos, 0);
  if(rc <= 0)
  {
    if(rc < 0)
    {
      if(errno == EWOULDBLOCK)
        return -1;
      console_print(RED "send: %d %s\n" RESET, errno, strerror(errno));
    }
    else
      console_print(YELLOW "send: %d %s\n" RESET, ECONNRESET, strerror(ECONNRESET));

    ftp_session_close_file(session);
    ftp_session_set_state(session, COMMAND_STATE);
    ftp_send_response(session, 426, "Connection broken during transfer\r\n");
    return -1;
  }

  session->bufferpos += rc;
  return 0;
}

static int
store_transfer(ftp_session_t *session)
{
  ssize_t rc;

  if(session->bufferpos == session->buffersize)
  {
    rc = recv(session->data_fd, session->buffer, sizeof(session->buffer), 0);
    if(rc <= 0)
    {
      if(rc < 0)
      {
        if(errno == EWOULDBLOCK)
          return -1;
        console_print(RED "recv: %d %s\n" RESET, errno, strerror(errno));
      }

      ftp_session_close_file(session);
      ftp_session_set_state(session, COMMAND_STATE);

      if(rc == 0)
      {
        ftp_send_response(session, 226, "OK\r\n");
      }
      else
        ftp_send_response(session, 426, "Connection broken during transfer\r\n");
      return -1;
    }

    session->bufferpos  = 0;
    session->buffersize = rc;
  }

  rc = ftp_session_write_file(session);
  if(rc <= 0)
  {
    ftp_session_close_file(session);
    ftp_session_set_state(session, COMMAND_STATE);
    ftp_send_response(session, 451, "Failed to write file\r\n");
    return -1;
  }

  session->bufferpos += rc;
  return 0;
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 *                                                                           *
 *                          F T P   C O M M A N D S                          *
 *                                                                           *
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

FTP_DECLARE(ALLO)
{
  console_print(CYAN "%s %s\n" RESET, __func__, args ? args : "");

  ftp_session_set_state(session, COMMAND_STATE);

  return ftp_send_response(session, 202, "superfluous command\r\n");
}

FTP_DECLARE(APPE)
{
  /* TODO */
  console_print(CYAN "%s %s\n" RESET, __func__, args ? args : "");

  ftp_session_set_state(session, COMMAND_STATE);

  return ftp_send_response(session, 502, "unavailable\r\n");
}

FTP_DECLARE(CDUP)
{
  console_print(CYAN "%s %s\n" RESET, __func__, args ? args : "");

  ftp_session_set_state(session, COMMAND_STATE);

  cd_up(session);

  return ftp_send_response(session, 200, "OK\r\n");
}

FTP_DECLARE(CWD)
{
  console_print(CYAN "%s %s\n" RESET, __func__, args ? args : "");

  ftp_session_set_state(session, COMMAND_STATE);

  if(strcmp(args, "..") == 0)
  {
    cd_up(session);
    return ftp_send_response(session, 200, "OK\r\n");
  }

  if(build_path(session, args) != 0)
    return ftp_send_response(session, 553, "%s\r\n", strerror(errno));
   
  {
    struct stat st;
    int         rc;

    rc = stat(session->buffer, &st);
    if(rc != 0)
    {
      console_print(RED "stat '%s': %d %s\n" RESET, session->buffer, errno, strerror(errno));
      return ftp_send_response(session, 550, "unavailable\r\n");
    }

    if(!S_ISDIR(st.st_mode))
      return ftp_send_response(session, 553, "not a directory\r\n");
  }

  strncpy(session->cwd, session->buffer, sizeof(session->cwd));
 
  return ftp_send_response(session, 200, "OK\r\n");
}

FTP_DECLARE(DELE)
{
  int rc;

  console_print(CYAN "%s %s\n" RESET, __func__, args ? args : "");

  ftp_session_set_state(session, COMMAND_STATE);

  if(build_path(session, args) != 0)
    return ftp_send_response(session, 553, "%s\r\n", strerror(errno));

  rc = unlink(session->buffer);
  if(rc != 0)
  {
    console_print(RED "unlink: %d %s\n" RESET, errno, strerror(errno));
    return ftp_send_response(session, 550, "failed to delete file\r\n");
  }

  return ftp_send_response(session, 250, "OK\r\n");
}

FTP_DECLARE(FEAT)
{
  console_print(CYAN "%s %s\n" RESET, __func__, args ? args : "");

  ftp_session_set_state(session, COMMAND_STATE);

  return ftp_send_response(session, 211, "\r\n UTF8\r\n211 End\r\n");
}

FTP_DECLARE(LIST)
{
  ssize_t rc;

  console_print(CYAN "%s %s\n" RESET, __func__, args ? args : "");

  if(ftp_session_open_cwd(session) != 0)
  {
    ftp_session_set_state(session, COMMAND_STATE);
    return ftp_send_response(session, 550, "unavailable\r\n");
  }
  
  if(session->flags & SESSION_PORT)
  {
    ftp_session_set_state(session, DATA_TRANSFER_STATE);
    rc = ftp_session_connect(session);
    if(rc != 0)
    {
      ftp_session_set_state(session, COMMAND_STATE);
      return ftp_send_response(session, 425, "can't open data connection\r\n");
    }

    session->flags &= ~(SESSION_RECV|SESSION_SEND);
    session->flags |= SESSION_SEND;

    session->transfer   = list_transfer;
    session->bufferpos  = 0;
    session->buffersize = 0;

    return ftp_send_response(session, 150, "Ready\r\n");
  }
  else if(session->flags & SESSION_PASV)
  {
    session->flags &= ~(SESSION_RECV|SESSION_SEND);
    session->flags |= SESSION_SEND;

    session->transfer   = list_transfer;
    session->bufferpos  = 0;
    session->buffersize = 0;

    ftp_session_set_state(session, DATA_CONNECT_STATE);
    return 0;
  }

  ftp_session_set_state(session, COMMAND_STATE);
  return ftp_send_response(session, 503, "Bad sequence of commands\r\n");
}

FTP_DECLARE(MKD)
{
  int rc;

  console_print(CYAN "%s %s\n" RESET, __func__, args ? args : "");

  ftp_session_set_state(session, COMMAND_STATE);

  if(build_path(session, args) != 0)
    return ftp_send_response(session, 553, "%s\r\n", strerror(errno));

  rc = mkdir(session->buffer, 0755);
  if(rc != 0)
  {
    console_print(RED "mkdir: %d %s\n" RESET, errno, strerror(errno));
    return ftp_send_response(session, 550, "failed to create directory\r\n");
  }

  return ftp_send_response(session, 250, "OK\r\n");
}

FTP_DECLARE(MODE)
{
  console_print(CYAN "%s %s\n" RESET, __func__, args ? args : "");

  ftp_session_set_state(session, COMMAND_STATE);

  if(strcasecmp(args, "S") == 0)
    return ftp_send_response(session, 200, "OK\r\n");

  return ftp_send_response(session, 504, "unavailable\r\n");
}

FTP_DECLARE(NLST)
{
  /* TODO */
  console_print(CYAN "%s %s\n" RESET, __func__, args ? args : "");

  ftp_session_set_state(session, COMMAND_STATE);

  return ftp_send_response(session, 504, "unavailable\r\n");
}

FTP_DECLARE(NOOP)
{
  console_print(CYAN "%s %s\n" RESET, __func__, args ? args : "");
  return ftp_send_response(session, 200, "OK\r\n");
}

FTP_DECLARE(OPTS)
{
  console_print(CYAN "%s %s\n" RESET, __func__, args ? args : "");

  ftp_session_set_state(session, COMMAND_STATE);

  if(strcasecmp(args, "UTF8") == 0
  || strcasecmp(args, "UTF8 ON") == 0
  || strcasecmp(args, "UTF8 NLST") == 0)
    return ftp_send_response(session, 200, "OK\r\n");

  return ftp_send_response(session, 504, "invalid argument\r\n");
}

FTP_DECLARE(PASS)
{
  console_print(CYAN "%s %s\n" RESET, __func__, args ? args : "");

  ftp_session_set_state(session, COMMAND_STATE);

  return ftp_send_response(session, 230, "OK\r\n");
}

FTP_DECLARE(PASV)
{
  int       rc;
  char      buffer[INET_ADDRSTRLEN + 10];
  char      *p;
  in_port_t port;

  console_print(CYAN "%s %s\n" RESET, __func__, args ? args : "");

  memset(buffer, 0, sizeof(buffer));

  ftp_session_set_state(session, COMMAND_STATE);

  session->flags &= ~(SESSION_PASV|SESSION_PORT);

  session->pasv_fd = socket(AF_INET, SOCK_STREAM, 0);
  if(session->pasv_fd < 0)
  {
    console_print(RED "socket: %d %s\n" RESET, errno, strerror(errno));
    return ftp_send_response(session, 451, "\r\n");
  }

  ftp_set_socket_options(session->pasv_fd);

  session->pasv_addr.sin_port = htons(next_data_port());

#ifdef _3DS
  console_print(YELLOW "binding to %s:%u\n" RESET,
                inet_ntoa(session->pasv_addr.sin_addr),
                ntohs(session->pasv_addr.sin_port));
#endif
  rc = bind(session->pasv_fd, (struct sockaddr*)&session->pasv_addr,
            sizeof(session->pasv_addr));
  if(rc != 0)
  {
    console_print(RED "bind: %d %s\n" RESET, errno, strerror(errno));
    ftp_session_close_pasv(session);
    return ftp_send_response(session, 451, "\r\n");
  }

  rc = listen(session->pasv_fd, 5);
  if(rc != 0)
  {
    console_print(RED "listen: %d %s\n" RESET, errno, strerror(errno));
    ftp_session_close_pasv(session);
    return ftp_send_response(session, 451, "\r\n");
  }

#ifndef _3DS
  {
    socklen_t addrlen = sizeof(session->pasv_addr);
    rc = getsockname(session->pasv_fd, (struct sockaddr*)&session->pasv_addr,
                     &addrlen);
    if(rc != 0)
    {
      console_print(RED "getsockname: %d %s\n" RESET, errno, strerror(errno));
      ftp_session_close_pasv(session);
      return ftp_send_response(session, 451, "\r\n");
    }
  }
#endif

  console_print(YELLOW "listening on %s:%u\n" RESET,
                inet_ntoa(session->pasv_addr.sin_addr),
                ntohs(session->pasv_addr.sin_port));

  session->flags |= SESSION_PASV;

  port = ntohs(session->pasv_addr.sin_port);
  strcpy(buffer, inet_ntoa(session->pasv_addr.sin_addr));
  sprintf(buffer+strlen(buffer), ",%u,%u",
          port >> 8, port & 0xFF);
  for(p = buffer; *p; ++p)
  {
    if(*p == '.')
      *p = ',';
  }

  return ftp_send_response(session, 227, "%s\r\n", buffer);
}

FTP_DECLARE(PORT)
{
  char               *addrstr, *p, *portstr;
  int                commas = 0, rc;
  short              port = 0;
  unsigned long      val;
  struct sockaddr_in addr;

  console_print(CYAN "%s %s\n" RESET, __func__, args ? args : "");

  ftp_session_set_state(session, COMMAND_STATE);

  session->flags &= ~(SESSION_PASV|SESSION_PORT);

  addrstr = strdup(args);
  if(addrstr == NULL)
    return ftp_send_response(session, 425, "%s\r\n", strerror(ENOMEM));

  for(p = addrstr; *p; ++p)
  {
    if(*p == ',')
    {
      if(commas != 3)
        *p = '.';
      else
      {
        *p = 0;
        portstr = p+1;
      }
      ++commas;
    }
  }

  if(commas != 5)
  {
    free(addrstr);
    return ftp_send_response(session, 501, "%s\r\n", strerror(EINVAL));
  }

  rc = inet_aton(addrstr, &addr.sin_addr);
  if(rc == 0)
  {
    free(addrstr);
    return ftp_send_response(session, 501, "%s\r\n", strerror(EINVAL));
  }

  val  = 0;
  port = 0;
  for(p = portstr; *p; ++p)
  {
    if(!isdigit((int)*p))
    {
      if(p == portstr || *p != '.' || val > 0xFF)
      {
        free(addrstr);
        return ftp_send_response(session, 501, "%s\r\n", strerror(EINVAL));
      }
      port <<= 8;
      port  += val;
      val    = 0;
    }
    else
    {
      val *= 10;
      val += *p - '0';
    }
  }

  if(val > 0xFF || port > 0xFF)
  {
    free(addrstr);
    return ftp_send_response(session, 501, "%s\r\n", strerror(EINVAL));
  }
  port <<= 8;
  port  += val;

  addr.sin_family = AF_INET;
  addr.sin_port   = htons(port);

  free(addrstr);

  memcpy(&session->peer_addr, &addr, sizeof(addr));

  session->flags |= SESSION_PORT;

  return ftp_send_response(session, 200, "OK\r\n");
}

FTP_DECLARE(PWD)
{
  console_print(CYAN "%s %s\n" RESET, __func__, args ? args : "");

  ftp_session_set_state(session, COMMAND_STATE);

  return ftp_send_response(session, 257, "\"%s\"\r\n", session->cwd);
}

FTP_DECLARE(QUIT)
{
  console_print(CYAN "%s %s\n" RESET, __func__, args ? args : "");

  ftp_send_response(session, 221, "disconnecting\r\n");
  ftp_session_close_cmd(session);

  return 0;
}

FTP_DECLARE(REST)
{
  /* TODO */
  console_print(CYAN "%s %s\n" RESET, __func__, args ? args : "");

  ftp_session_set_state(session, COMMAND_STATE);

  return ftp_send_response(session, 502, "unavailable\r\n");
}

FTP_DECLARE(RETR)
{
  int rc;

  console_print(CYAN "%s %s\n" RESET, __func__, args ? args : "");

  if(build_path(session, args) != 0)
  {
    rc = errno;
    ftp_session_set_state(session, COMMAND_STATE);
    return ftp_send_response(session, 553, "%s\r\n", strerror(rc));
  }

  if(ftp_session_open_file_read(session) != 0)
  {
    ftp_session_set_state(session, COMMAND_STATE);
    return ftp_send_response(session, 450, "failed to open file\r\n");
  }

  if(session->flags & SESSION_PORT)
  {
    ftp_session_set_state(session, DATA_TRANSFER_STATE);
    rc = ftp_session_connect(session);
    if(rc != 0)
    {
      ftp_session_set_state(session, COMMAND_STATE);
      return ftp_send_response(session, 425, "can't open data connection\r\n");
    }

    session->flags &= ~(SESSION_RECV|SESSION_SEND);
    session->flags |= SESSION_SEND;

    session->transfer   = retrieve_transfer;
    session->bufferpos  = 0;
    session->buffersize = 0;

    return ftp_send_response(session, 150, "Ready\r\n");
  }
  else if(session->flags & SESSION_PASV)
  {
    session->flags &= ~(SESSION_RECV|SESSION_SEND);
    session->flags |= SESSION_SEND;

    session->transfer   = retrieve_transfer;
    session->bufferpos  = 0;
    session->buffersize = 0;

    ftp_session_set_state(session, DATA_CONNECT_STATE);
    return 0;
  }

  ftp_session_set_state(session, COMMAND_STATE);
  return ftp_send_response(session, 503, "Bad sequence of commands\r\n");
}

FTP_DECLARE(RMD)
{
  int rc;

  console_print(CYAN "%s %s\n" RESET, __func__, args ? args : "");

  ftp_session_set_state(session, COMMAND_STATE);

  if(build_path(session, args) != 0)
    return ftp_send_response(session, 553, "%s\r\n", strerror(errno));

  rc = rmdir(session->buffer);
  if(rc != 0)
  {
    console_print(RED "rmdir: %d %s\n" RESET, errno, strerror(errno));
    return ftp_send_response(session, 550, "failed to delete directory\r\n");
  }

  return ftp_send_response(session, 250, "OK\r\n");
}

FTP_DECLARE(RNFR)
{
  int         rc;
  struct stat st;
  console_print(CYAN "%s %s\n" RESET, __func__, args ? args : "");

  ftp_session_set_state(session, COMMAND_STATE);

  if(build_path(session, args) != 0)
    return ftp_send_response(session, 553, "%s\r\n", strerror(errno));

  rc = lstat(session->buffer, &st);
  if(rc != 0)
  {
    console_print(RED "lstat: %d %s\n" RESET, errno, strerror(errno));
    return ftp_send_response(session, 450, "no such file or directory\r\n");
  }

  session->flags |= SESSION_RENAME;

  return ftp_send_response(session, 350, "OK\r\n");
}

FTP_DECLARE(RNTO)
{
  int rc;

  console_print(CYAN "%s %s\n" RESET, __func__, args ? args : "");

  ftp_session_set_state(session, COMMAND_STATE);

  if(!(session->flags & SESSION_RENAME))
    return ftp_send_response(session, 503, "Bad sequence of commands\r\n");

  session->flags &= ~SESSION_RENAME;

  memcpy(session->tmp_buffer, session->buffer, XFER_BUFFERSIZE);

  if(build_path(session, args) != 0)
    return ftp_send_response(session, 554, "%s\r\n", strerror(errno));

  rc = rename(session->tmp_buffer, session->buffer);
  if(rc != 0)
  {
    console_print(RED "rename: %d %s\n" RESET, errno, strerror(errno));
    return ftp_send_response(session, 550, "failed to rename file/directory\r\n");
  }

  return ftp_send_response(session, 250, "OK\r\n");
}

FTP_DECLARE(STOR)
{
  int rc;

  console_print(CYAN "%s %s\n" RESET, __func__, args ? args : "");

  if(build_path(session, args) != 0)
  {
    rc = errno;
    ftp_session_set_state(session, COMMAND_STATE);
    return ftp_send_response(session, 553, "%s\r\n", strerror(rc));
  }

  if(ftp_session_open_file_write(session) != 0)
  {
    ftp_session_set_state(session, COMMAND_STATE);
    return ftp_send_response(session, 450, "failed to open file\r\n");
  }

  if(session->flags & SESSION_PORT)
  {
    ftp_session_set_state(session, DATA_TRANSFER_STATE);
    rc = ftp_session_connect(session);
    if(rc != 0)
    {
      ftp_session_set_state(session, COMMAND_STATE);
      return ftp_send_response(session, 425, "can't open data connection\r\n");
    }

    session->flags &= ~(SESSION_RECV|SESSION_SEND);
    session->flags |= SESSION_RECV;

    session->transfer   = store_transfer;
    session->bufferpos  = 0;
    session->buffersize = 0;

    return ftp_send_response(session, 150, "Ready\r\n");
  }
  else if(session->flags & SESSION_PASV)
  {
    session->flags &= ~(SESSION_RECV|SESSION_SEND);
    session->flags |= SESSION_RECV;

    session->transfer   = store_transfer;
    session->bufferpos  = 0;
    session->buffersize = 0;

    ftp_session_set_state(session, DATA_CONNECT_STATE);
    return 0;
  }

  ftp_session_set_state(session, COMMAND_STATE);
  return ftp_send_response(session, 503, "Bad sequence of commands\r\n");
}

FTP_DECLARE(STOU)
{
  console_print(CYAN "%s %s\n" RESET, __func__, args ? args : "");

  ftp_session_set_state(session, COMMAND_STATE);

  return ftp_send_response(session, 502, "unavailable\r\n");
}

FTP_DECLARE(STRU)
{
  console_print(CYAN "%s %s\n" RESET, __func__, args ? args : "");

  ftp_session_set_state(session, COMMAND_STATE);

  if(strcasecmp(args, "F") == 0)
    return ftp_send_response(session, 200, "OK\r\n");

  return ftp_send_response(session, 504, "unavailable\r\n");
}

FTP_DECLARE(SYST)
{
  console_print(CYAN "%s %s\n" RESET, __func__, args ? args : "");

  ftp_session_set_state(session, COMMAND_STATE);

  return ftp_send_response(session, 215, "UNIX Type: L8\r\n");
}

FTP_DECLARE(TYPE)
{
  console_print(CYAN "%s %s\n" RESET, __func__, args ? args : "");

  ftp_session_set_state(session, COMMAND_STATE);

  return ftp_send_response(session, 200, "OK\r\n");
}

FTP_DECLARE(USER)
{
  console_print(CYAN "%s %s\n" RESET, __func__, args ? args : "");

  ftp_session_set_state(session, COMMAND_STATE);

  return ftp_send_response(session, 230, "OK\r\n");
}
