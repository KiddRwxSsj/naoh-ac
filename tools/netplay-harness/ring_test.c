// pc check for the gameplay audio ring in main.c. ring_test.sh pulls the
// real code out of main.c into ring_extract.h; this file fakes the sema
// and the audio port around it and runs a producer (the game thread) and
// the consumer thread against each other.
//
// every stereo frame carries a 32-bit sequence number (low half left, high
// half right). the consumer checks it only ever sees whole frames, in
// order, never twice; gaps are fine, that's the ring dropping on overflow.
//
//   ring_test [realtime|stress] [seconds]

#include <pthread.h>
#include <semaphore.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

typedef int SceUID;
typedef unsigned int SceSize;

static sem_t  sema;
static bool   realtime = true;
static int    audio_port = 0;

static int sceKernelSignalSema(SceUID id, int n)
{
   (void)id;
   while (n--)
      sem_post(&sema);
   return 0;
}

static int sceKernelWaitSema(SceUID id, int n, void *timeout)
{
   (void)id; (void)n; (void)timeout;
   return sem_wait(&sema);
}

static uint64_t seen_frames, seen_grains, gaps, errors;
static uint32_t last_seq = UINT32_MAX;

// the hardware: check the grain, then take ~10.7 ms to play it
static int sceAudioOutOutput(int port, const void *buf)
{
   const int16_t *p = buf;

   (void)port;
   for (unsigned i = 0; i < 512; i++)
   {
      uint32_t seq = (uint16_t)p[i * 2] | ((uint32_t)(uint16_t)p[i * 2 + 1] << 16);

      if (last_seq != UINT32_MAX)
      {
         if (seq <= last_seq)
         {
            if (errors++ < 5)
               fprintf(stderr, "out of order or repeated: %u after %u\n", seq, last_seq);
         }
         else if (seq != last_seq + 1)
            gaps++;
      }
      last_seq = seq;
   }

   seen_frames += 512;
   seen_grains++;
   if (realtime)
      usleep(10667);
   return 0;
}

#include "ring_extract.h"

static void *consumer(void *p)
{
   (void)p;
   core_audio_thread_func(0, NULL);
   return NULL;
}

static uint64_t now_us(void)
{
   struct timespec ts;

   clock_gettime(CLOCK_MONOTONIC, &ts);
   return (uint64_t)ts.tv_sec * 1000000ULL + ts.tv_nsec / 1000;
}

int main(int argc, char **argv)
{
   unsigned seconds = (argc > 2) ? (unsigned)atoi(argv[2]) : 6;
   int16_t batch[2048 * 2];
   uint32_t seq = 0;
   uint64_t produced = 0, end;
   pthread_t t;
   unsigned round = 0;

   realtime = !(argc > 1 && !strcmp(argv[1], "stress"));
   setvbuf(stdout, NULL, _IOLBF, 0);

   sem_init(&sema, 0, 0);
   core_audio_ring_write = 0;
   core_audio_ring_read  = 0;
   core_audio_sema       = 1;
   core_audio_thread_run = true;
   pthread_create(&t, NULL, consumer, NULL);

   end = now_us() + seconds * 1000000ULL;
   while (now_us() < end)
   {
      // mostly ~800 frames a "frame" like a core at 48 khz, with bursts
      // that overflow the ring and gaps that starve it
      unsigned n = 800 + (round * 37) % 64;

      if (round % 97 == 0)
         n = 2048;
      if (!realtime)
         n = 1 + (round * 131) % 2048;

      for (unsigned i = 0; i < n; i++, seq++)
      {
         batch[i * 2]     = (int16_t)(seq & 0xFFFF);
         batch[i * 2 + 1] = (int16_t)(seq >> 16);
      }

      audio_sample_batch_cb(batch, n);
      produced += n;
      round++;

      if (realtime)
         usleep((round % 211 == 0) ? 120000 : 16600);
   }

   // same as core_audio_thread_stop()
   __atomic_store_n(&core_audio_thread_run, false, __ATOMIC_RELEASE);
   sceKernelSignalSema(core_audio_sema, 1);
   pthread_join(t, NULL);

   printf("%s: produced %llu frames, played %llu (%llu grains), gaps %llu, errors %llu\n",
         realtime ? "realtime" : "stress", (unsigned long long)produced,
         (unsigned long long)seen_frames, (unsigned long long)seen_grains,
         (unsigned long long)gaps, (unsigned long long)errors);
   return errors ? 1 : 0;
}
