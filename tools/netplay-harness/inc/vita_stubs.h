// just enough of the vitasdk surface for netplay.c, mqtt.c and rng.c to
// build and run on linux. every psp2/... header in inc/ pulls this in.
// the values only have to agree with stubs.c, not with the real sdk

#ifndef VITA_STUBS_H
#define VITA_STUBS_H

#include <stdint.h>
#include <stddef.h>

typedef int          SceUID;
typedef unsigned int SceSize;
typedef unsigned int SceUInt;
typedef int64_t      SceInt64;
typedef uint64_t     SceUInt64;
typedef unsigned char SceChar8;

/* kernel */
#define SCE_KERNEL_CPU_MASK_USER_0   0x00010000
#define SCE_KERNEL_CPU_MASK_USER_1   0x00020000
#define SCE_KERNEL_CPU_MASK_USER_2   0x00040000
#define SCE_KERNEL_CPU_MASK_USER_ALL 0x00070000

typedef int (*SceKernelThreadEntry)(SceSize args, void *argp);

SceUInt64 sceKernelGetProcessTimeWide(void);
SceUInt   sceKernelGetProcessTimeLow(void);
int       sceKernelDelayThread(SceUInt usec);
SceUID    sceKernelCreateThread(const char *name, SceKernelThreadEntry entry, int prio,
                                SceSize stack, SceUInt attr, int affinity, const void *opt);
int       sceKernelStartThread(SceUID thid, SceSize args, void *argp);
int       sceKernelWaitThreadEnd(SceUID thid, int *stat, SceUInt *timeout);
int       sceKernelDeleteThread(SceUID thid);
int       sceKernelGetThreadId(void);
int       sceKernelGetThreadCpuAffinityMask(SceUID thid);
int       sceKernelGetRandomNumber(void *out, unsigned size);

/* sysmodule / apputil */
#define SCE_SYSMODULE_NET            1
#define SCE_SYSTEM_PARAM_ID_USERNAME 1

typedef struct { int dummy; } SceAppUtilInitParam;
typedef struct { int dummy; } SceAppUtilBootParam;

int sceSysmoduleLoadModule(int id);
int sceAppUtilInit(SceAppUtilInitParam *init, SceAppUtilBootParam *boot);
int sceAppUtilSystemParamGetString(int id, SceChar8 *buf, SceSize size);

/* net */
#define SCE_NET_AF_INET       2
#define SCE_NET_SOCK_STREAM   1
#define SCE_NET_SOCK_DGRAM    2
#define SCE_NET_IPPROTO_TCP   6
#define SCE_NET_IPPROTO_UDP   17
#define SCE_NET_SOL_SOCKET    0xFFFF
#define SCE_NET_SO_RCVBUF     0x1002
#define SCE_NET_SO_NBIO       0x1100
#define SCE_NET_TCP_NODELAY   1
#define SCE_NET_MSG_DONTWAIT  0x80
#define SCE_NET_INADDR_ANY    0

#define SCE_NET_ERROR_EAGAIN  0x80410123

typedef struct { uint32_t s_addr; } SceNetInAddr;

typedef struct {
   uint8_t      sin_len;
   uint8_t      sin_family;
   uint16_t     sin_port;
   SceNetInAddr sin_addr;
   uint16_t     sin_vport;
   char         sin_zero[6];
} SceNetSockaddrIn;

typedef struct {
   uint8_t sa_len;
   uint8_t sa_family;
   uint8_t sa_data[14];
} SceNetSockaddr;

typedef struct {
   void *memory;
   int   size;
   int   flags;
} SceNetInitParam;

int      sceNetInit(SceNetInitParam *param);
int      sceNetTerm(void);
int      sceNetShowNetstat(void);
uint16_t sceNetHtons(uint16_t v);
uint16_t sceNetNtohs(uint16_t v);
int      sceNetSocket(const char *name, int domain, int type, int protocol);
int      sceNetSocketClose(int s);
int      sceNetSocketAbort(int s, int flags);
int      sceNetSetsockopt(int s, int level, int opt, const void *val, unsigned len);
int      sceNetBind(int s, const SceNetSockaddr *addr, unsigned len);
int      sceNetGetsockname(int s, SceNetSockaddr *addr, unsigned int *len);
int      sceNetConnect(int s, const SceNetSockaddr *addr, unsigned len);
int      sceNetSend(int s, const void *buf, unsigned len, int flags);
int      sceNetRecv(int s, void *buf, unsigned len, int flags);
int      sceNetSendto(int s, const void *buf, unsigned len, int flags,
                      const SceNetSockaddr *to, unsigned tolen);
int      sceNetRecvfrom(int s, void *buf, unsigned len, int flags,
                        SceNetSockaddr *from, unsigned int *fromlen);
int      sceNetResolverCreate(const char *name, void *param, int flags);
int      sceNetResolverStartNtoa(int rid, const char *host, SceNetInAddr *addr,
                                 int timeout, int retry, int flags);
int      sceNetResolverDestroy(int rid);
int      sceNetResolverAbort(int rid, int flags);

/* netctl */
#define SCE_NETCTL_INFO_GET_IP_ADDRESS 14

typedef union {
   char ip_address[16];
   char pad[256];
} SceNetCtlInfo;

int sceNetCtlInit(void);
void sceNetCtlTerm(void);
int sceNetCtlInetGetInfo(int code, SceNetCtlInfo *info);

#endif
