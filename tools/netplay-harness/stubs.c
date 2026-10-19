// the vita calls netplay.c, mqtt.c and rng.c make, done with posix.
//   NP_BLOCK_UDP=1   every udp send is silently dropped (forces the relay)
//   NP_LOCAL_IP      what sceNetCtlInetGetInfo reports (default 192.168.0.10)
//   NP_MAIN_MASK     affinity mask the game thread reports (default user core 0)
//   NP_USERNAME      psn name (default "Harness")
//   NP_TCP_STALL_AT  seconds after the broker connect when tcp stops moving
//                    in both directions (a black-holed broker, sends and
//                    receives just say "try again" forever)

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/random.h>
#include <sys/socket.h>

#include <vita_stubs.h>

static int net_error(void)
{
   if (errno == EAGAIN || errno == EWOULDBLOCK)
      return (int)SCE_NET_ERROR_EAGAIN;
   return (int)(0x80410100u | (unsigned)(errno & 0xFF));
}

static void to_posix(const SceNetSockaddr *in, struct sockaddr_in *out)
{
   const SceNetSockaddrIn *a = (const SceNetSockaddrIn *)in;

   memset(out, 0, sizeof(*out));
   out->sin_family      = AF_INET;
   out->sin_port        = a->sin_port;
   out->sin_addr.s_addr = a->sin_addr.s_addr;
}

static void from_posix(const struct sockaddr_in *in, SceNetSockaddr *out, unsigned int *len)
{
   SceNetSockaddrIn *a = (SceNetSockaddrIn *)out;

   memset(a, 0, sizeof(*a));
   a->sin_len         = sizeof(*a);
   a->sin_family      = SCE_NET_AF_INET;
   a->sin_port        = in->sin_port;
   a->sin_addr.s_addr = in->sin_addr.s_addr;
   if (len)
      *len = sizeof(*a);
}

/* kernel */

SceUInt64 sceKernelGetProcessTimeWide(void)
{
   struct timespec ts;

   clock_gettime(CLOCK_MONOTONIC, &ts);
   return (SceUInt64)ts.tv_sec * 1000000ULL + (SceUInt64)(ts.tv_nsec / 1000);
}

SceUInt sceKernelGetProcessTimeLow(void)
{
   return (SceUInt)sceKernelGetProcessTimeWide();
}

int sceKernelDelayThread(SceUInt usec)
{
   usleep(usec);
   return 0;
}

#define MAX_THREADS 16

static struct {
   bool                 used;
   SceKernelThreadEntry entry;
   pthread_t            tid;
   SceSize              args;
   void                *argp;
} threads[MAX_THREADS];

static pthread_mutex_t thread_lock = PTHREAD_MUTEX_INITIALIZER;
static __thread int    my_thread_id = 1;

static void *thread_trampoline(void *p)
{
   int i = (int)(intptr_t)p;

   my_thread_id = 100 + i;
   threads[i].entry(threads[i].args, threads[i].argp);
   return NULL;
}

SceUID sceKernelCreateThread(const char *name, SceKernelThreadEntry entry, int prio,
      SceSize stack, SceUInt attr, int affinity, const void *opt)
{
   (void)name; (void)prio; (void)stack; (void)attr; (void)affinity; (void)opt;

   pthread_mutex_lock(&thread_lock);
   for (int i = 0; i < MAX_THREADS; i++)
   {
      if (!threads[i].used)
      {
         threads[i].used  = true;
         threads[i].entry = entry;
         pthread_mutex_unlock(&thread_lock);
         return i + 1;
      }
   }
   pthread_mutex_unlock(&thread_lock);
   return -1;
}

int sceKernelStartThread(SceUID thid, SceSize args, void *argp)
{
   int i = thid - 1;

   if (i < 0 || i >= MAX_THREADS || !threads[i].used)
      return -1;

   threads[i].args = args;
   threads[i].argp = argp;
   return pthread_create(&threads[i].tid, NULL, thread_trampoline, (void *)(intptr_t)i) ? -1 : 0;
}

int sceKernelWaitThreadEnd(SceUID thid, int *stat, SceUInt *timeout)
{
   int i = thid - 1;

   (void)timeout;
   if (stat)
      *stat = 0;
   if (i < 0 || i >= MAX_THREADS || !threads[i].used)
      return -1;
   return pthread_join(threads[i].tid, NULL) ? -1 : 0;
}

int sceKernelDeleteThread(SceUID thid)
{
   int i = thid - 1;

   if (i < 0 || i >= MAX_THREADS)
      return -1;

   pthread_mutex_lock(&thread_lock);
   threads[i].used = false;
   pthread_mutex_unlock(&thread_lock);
   return 0;
}

int sceKernelGetThreadId(void)
{
   return my_thread_id;
}

int sceKernelGetThreadCpuAffinityMask(SceUID thid)
{
   const char *m = getenv("NP_MAIN_MASK");

   (void)thid;
   return m ? (int)strtol(m, NULL, 0) : SCE_KERNEL_CPU_MASK_USER_0;
}

int sceKernelGetRandomNumber(void *out, unsigned size)
{
   return getrandom(out, size, 0) == (ssize_t)size ? 0 : -1;
}

/* sysmodule / apputil */

int sceSysmoduleLoadModule(int id)
{
   (void)id;
   return 0;
}

int sceAppUtilInit(SceAppUtilInitParam *init, SceAppUtilBootParam *boot)
{
   (void)init; (void)boot;
   return 0;
}

int sceAppUtilSystemParamGetString(int id, SceChar8 *buf, SceSize size)
{
   const char *name = getenv("NP_USERNAME");

   (void)id;
   snprintf((char *)buf, size, "%s", name ? name : "Harness");
   return 0;
}

/* net */

static bool      is_stream[4096];
static SceUInt64 tcp_stall_us;

static bool tcp_stalled(int s)
{
   return s >= 0 && s < 4096 && is_stream[s] && tcp_stall_us &&
         sceKernelGetProcessTimeWide() >= tcp_stall_us;
}

int sceNetInit(SceNetInitParam *param)
{
   (void)param;
   return 0;
}

int sceNetTerm(void)
{
   return 0;
}

int sceNetShowNetstat(void)
{
   return 0;
}

uint16_t sceNetHtons(uint16_t v)
{
   return htons(v);
}

uint16_t sceNetNtohs(uint16_t v)
{
   return ntohs(v);
}

int sceNetSocket(const char *name, int domain, int type, int protocol)
{
   int s;

   (void)name; (void)domain; (void)protocol;
   s = socket(AF_INET, type == SCE_NET_SOCK_STREAM ? SOCK_STREAM : SOCK_DGRAM, 0);
   if (s >= 0 && s < 4096)
      is_stream[s] = type == SCE_NET_SOCK_STREAM;
   return s < 0 ? net_error() : s;
}

int sceNetSocketClose(int s)
{
   return close(s);
}

int sceNetSocketAbort(int s, int flags)
{
   (void)flags;
   return shutdown(s, SHUT_RDWR);
}

int sceNetSetsockopt(int s, int level, int opt, const void *val, unsigned len)
{
   if (level == SCE_NET_SOL_SOCKET && opt == SCE_NET_SO_NBIO)
   {
      int fl = fcntl(s, F_GETFL);
      return fcntl(s, F_SETFL, *(const int *)val ? (fl | O_NONBLOCK) : (fl & ~O_NONBLOCK));
   }
   if (level == SCE_NET_SOL_SOCKET && opt == SCE_NET_SO_RCVBUF)
      return setsockopt(s, SOL_SOCKET, SO_RCVBUF, val, len);
   if (level == SCE_NET_IPPROTO_TCP && opt == SCE_NET_TCP_NODELAY)
      return setsockopt(s, IPPROTO_TCP, TCP_NODELAY, val, len);
   return -1;
}

int sceNetBind(int s, const SceNetSockaddr *addr, unsigned len)
{
   struct sockaddr_in sa;

   (void)len;
   to_posix(addr, &sa);
   return bind(s, (struct sockaddr *)&sa, sizeof(sa)) < 0 ? net_error() : 0;
}

int sceNetGetsockname(int s, SceNetSockaddr *addr, unsigned int *len)
{
   struct sockaddr_in sa;
   socklen_t sl = sizeof(sa);

   if (getsockname(s, (struct sockaddr *)&sa, &sl) < 0)
      return net_error();
   from_posix(&sa, addr, len);
   return 0;
}

int sceNetConnect(int s, const SceNetSockaddr *addr, unsigned len)
{
   struct sockaddr_in sa;

   (void)len;
   to_posix(addr, &sa);
   if (connect(s, (struct sockaddr *)&sa, sizeof(sa)) < 0)
      return net_error();
   if (getenv("NP_TCP_STALL_AT"))
      tcp_stall_us = sceKernelGetProcessTimeWide() + (SceUInt64)(atof(getenv("NP_TCP_STALL_AT")) * 1e6);
   return 0;
}

int sceNetSend(int s, const void *buf, unsigned len, int flags)
{
   ssize_t n;

   if (tcp_stalled(s))
   {
      // a real blocking send would just sit here
      if (!(flags & SCE_NET_MSG_DONTWAIT))
         for (;;)
            pause();
      return (int)SCE_NET_ERROR_EAGAIN;
   }

   n = send(s, buf, len, MSG_NOSIGNAL | ((flags & SCE_NET_MSG_DONTWAIT) ? MSG_DONTWAIT : 0));
   return n < 0 ? net_error() : (int)n;
}

int sceNetRecv(int s, void *buf, unsigned len, int flags)
{
   ssize_t n;

   if (tcp_stalled(s))
      return (int)SCE_NET_ERROR_EAGAIN;

   n = recv(s, buf, len, (flags & SCE_NET_MSG_DONTWAIT) ? MSG_DONTWAIT : 0);
   return n < 0 ? net_error() : (int)n;
}

int sceNetSendto(int s, const void *buf, unsigned len, int flags,
      const SceNetSockaddr *to, unsigned tolen)
{
   struct sockaddr_in sa;
   ssize_t n;

   (void)tolen;
   if (getenv("NP_BLOCK_UDP"))
      return (int)len;

   to_posix(to, &sa);
   n = sendto(s, buf, len, flags & SCE_NET_MSG_DONTWAIT ? MSG_DONTWAIT : 0,
         (struct sockaddr *)&sa, sizeof(sa));
   return n < 0 ? net_error() : (int)n;
}

int sceNetRecvfrom(int s, void *buf, unsigned len, int flags,
      SceNetSockaddr *from, unsigned int *fromlen)
{
   struct sockaddr_in sa;
   socklen_t sl = sizeof(sa);
   ssize_t n = recvfrom(s, buf, len, flags & SCE_NET_MSG_DONTWAIT ? MSG_DONTWAIT : 0,
         (struct sockaddr *)&sa, &sl);

   if (n < 0)
      return net_error();
   if (from)
      from_posix(&sa, from, fromlen);
   return (int)n;
}

int sceNetResolverCreate(const char *name, void *param, int flags)
{
   (void)name; (void)param; (void)flags;
   return 1;
}

int sceNetResolverStartNtoa(int rid, const char *host, SceNetInAddr *addr,
      int timeout, int retry, int flags)
{
   struct addrinfo hints, *res;

   (void)rid; (void)timeout; (void)retry; (void)flags;
   memset(&hints, 0, sizeof(hints));
   hints.ai_family = AF_INET;

   if (getaddrinfo(host, NULL, &hints, &res) != 0)
      return -1;

   addr->s_addr = ((struct sockaddr_in *)res->ai_addr)->sin_addr.s_addr;
   freeaddrinfo(res);
   return 0;
}

int sceNetResolverDestroy(int rid)
{
   (void)rid;
   return 0;
}

int sceNetResolverAbort(int rid, int flags)
{
   (void)rid; (void)flags;
   return 0;
}

/* netctl */

int sceNetCtlInit(void)
{
   return 0;
}

void sceNetCtlTerm(void)
{
}

int sceNetCtlInetGetInfo(int code, SceNetCtlInfo *info)
{
   const char *ip = getenv("NP_LOCAL_IP");

   (void)code;
   snprintf(info->ip_address, sizeof(info->ip_address), "%s", ip ? ip : "192.168.0.10");
   return 0;
}
