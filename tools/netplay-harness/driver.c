// one side of a match, run as two processes on one pc against the real
// broker (under a test topic root, see build.sh). mirrors main.c's flow:
// room -> join -> sync a fake machine hash -> lockstep frames.
//
//   driver host|guest [frames]
//   NP_HIDE_IP=1   relay only, like Options -> Hide My IP Online
//   NP_BUSY_MS=n   burn n ms of cpu per frame, like a heavy game
//   NP_ROM_CRC=x   rom crc to claim (default 0x3f432e29)
//
// at the end both print "result: frames N crc XXXXXXXX", which has to
// match between the two, plus the time it took

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>

#include <vita_stubs.h>
#include "netplay.h"

#define STATE_SIZE 353988
#define FRAME_US   16667ULL

static uint32_t crc32_update(uint32_t crc, const void *data, size_t len)
{
   const uint8_t *p = data;

   crc = ~crc;
   while (len--)
   {
      crc ^= *p++;
      for (int k = 0; k < 8; k++)
         crc = (crc >> 1) ^ (0xEDB88320u & -(crc & 1));
   }
   return ~crc;
}

static uint16_t input_for(unsigned frame, int side)
{
   uint32_t x = frame * 2654435761u ^ (side ? 0xA5A5A5A5u : 0x5A5A5A5Au);

   x ^= x >> 13;
   x *= 0x9E3779B1u;
   x ^= x >> 16;
   // hold buttons for a few frames like a person would
   return (uint16_t)(((frame / 7) * 31 + x) & 0x0FFF);
}

static void burn(unsigned ms)
{
   uint64_t end = sceKernelGetProcessTimeWide() + ms * 1000ULL;
   volatile uint32_t spin = 0;

   while (sceKernelGetProcessTimeWide() < end)
      spin++;
}

static void idle(void)
{
   usleep(16000);
}

static int fail(const char *why)
{
   printf("result: FAILED (%s)\n", why);
   netplay_shutdown();
   return 1;
}

int main(int argc, char **argv)
{
   netplay_options_t opt;
   netplay_event_t ev = NETPLAY_EVENT_NONE;
   bool host;
   unsigned frames = (argc > 2) ? (unsigned)atoi(argv[2]) : 900;
   unsigned busy = getenv("NP_BUSY_MS") ? (unsigned)atoi(getenv("NP_BUSY_MS")) : 0;
   uint32_t rom_crc = getenv("NP_ROM_CRC") ? (uint32_t)strtoul(getenv("NP_ROM_CRC"), NULL, 0) : 0x3f432e29u;
   uint8_t *state, digest[NETPLAY_DIGEST_SIZE];
   unsigned delay;
   uint32_t crc = 0;
   uint64_t deadline, start;
   unsigned f;

   if (argc < 2 || (strcmp(argv[1], "host") && strcmp(argv[1], "guest")))
   {
      fprintf(stderr, "usage: %s host|guest [frames]\n", argv[0]);
      return 2;
   }

   host = !strcmp(argv[1], "host");
   setvbuf(stdout, NULL, _IOLBF, 0);

   memset(&opt, 0, sizeof(opt));
   opt.hide_ip = getenv("NP_HIDE_IP") != NULL;

   if (netplay_init(&opt) != NETPLAY_INIT_OK)
      return fail("init");

   deadline = sceKernelGetProcessTimeWide() + 20000000ULL;
   while (netplay_lobby_state() == NETPLAY_LOBBY_CONNECTING && sceKernelGetProcessTimeWide() < deadline)
      usleep(10000);
   if (netplay_lobby_state() != NETPLAY_LOBBY_ONLINE)
      return fail("lobby");

   printf("%s: lobby online as %s\n", argv[1], netplay_local_name());

   if (host)
   {
      netplay_create_room("selftest", "xmvsfu.zip", rom_crc);
      deadline = sceKernelGetProcessTimeWide() + 90000000ULL;

      while ((ev = netplay_lobby_poll()) != NETPLAY_EVENT_CONNECTED)
      {
         if (ev == NETPLAY_EVENT_LOBBY_LOST || sceKernelGetProcessTimeWide() > deadline)
            return fail("nobody joined");
         usleep(16000);
      }
   }
   else
   {
      netplay_room_t rooms[NETPLAY_MAX_ROOMS];
      int n = 0, pick = -1;

      netplay_browse("selftest");
      deadline = sceKernelGetProcessTimeWide() + 60000000ULL;

      while (pick < 0)
      {
         if (netplay_lobby_poll() == NETPLAY_EVENT_LOBBY_LOST || sceKernelGetProcessTimeWide() > deadline)
            return fail("no room");

         n = netplay_rooms(rooms, NETPLAY_MAX_ROOMS);
         for (int i = 0; i < n; i++)
            if (rooms[i].players == 1)
               pick = i;
         usleep(16000);
      }

      netplay_join_room(&rooms[pick], rom_crc);
      deadline = sceKernelGetProcessTimeWide() + 30000000ULL;

      while ((ev = netplay_lobby_poll()) != NETPLAY_EVENT_CONNECTED)
      {
         if ((ev != NETPLAY_EVENT_NONE && ev != NETPLAY_EVENT_JOINED) ||
               sceKernelGetProcessTimeWide() > deadline)
            return fail(netplay_event_name(ev));
         usleep(16000);
      }
   }

   printf("%s: paired, %s\n", argv[1], netplay_is_relayed() ? "relay" : "direct");

   // the fake "booted machine", same on both sides
   state = malloc(STATE_SIZE);
   for (uint32_t i = 0; i < STATE_SIZE; i++)
      state[i] = (uint8_t)(i * 7 + (i >> 9));
   netplay_digest(state, STATE_SIZE, digest);
   free(state);

   delay = netplay_suggested_delay();
   ev = host ? netplay_sync_host(STATE_SIZE, digest, &delay, idle)
             : netplay_sync_guest(STATE_SIZE, digest, idle);
   if (ev != NETPLAY_EVENT_NONE)
      return fail(netplay_event_name(ev));

   netplay_start(delay);
   start = sceKernelGetProcessTimeWide();
   deadline = start;

   for (f = 0; f < frames; f++)
   {
      uint16_t in[2];

      netplay_push_input(input_for(f, host ? 0 : 1));

      while ((ev = netplay_poll()) == NETPLAY_EVENT_NONE && !netplay_frame_ready())
         sceKernelDelayThread(1000);
      if (ev != NETPLAY_EVENT_NONE)
         break;

      in[0] = netplay_input(0);
      in[1] = netplay_input(1);
      crc = crc32_update(crc, in, sizeof(in));
      netplay_advance();

      if (busy)
         burn(busy);

      // vsync: a frame that runs late waits for the next tick, it never
      // gets to catch up, same as vglSwapBuffers on the vita
      deadline = start + ((sceKernelGetProcessTimeWide() - start) / FRAME_US + 1) * FRAME_US;
      while (sceKernelGetProcessTimeWide() < deadline)
         usleep(500);
   }

   printf("result: frames %u crc %08X in %.1f s (%s)\n", f, crc,
         (sceKernelGetProcessTimeWide() - start) / 1e6, netplay_event_name(ev));

   // keep answering for a moment so the other side gets its last frames
   // before our quit lands
   if (ev == NETPLAY_EVENT_NONE)
   {
      uint64_t linger = sceKernelGetProcessTimeWide() + 3000000ULL;

      while (sceKernelGetProcessTimeWide() < linger && netplay_poll() == NETPLAY_EVENT_NONE)
      {
         if (netplay_frame_ready())
         {
            netplay_push_input(0);
            netplay_advance();
         }
         usleep(16000);
      }
   }

   netplay_close();
   netplay_shutdown();
   return 0;
}
