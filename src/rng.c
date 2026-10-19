#include <stdint.h>
#include <psp2/kernel/rng.h>

#include "rng.h"

// small pieces per call, the kernel caps how much it hands out at once
#define RNG_CHUNK 32

bool rng_bytes(void *out, size_t len)
{
   uint8_t *p = (uint8_t *)out;

   while (len)
   {
      size_t n = (len < RNG_CHUNK) ? len : RNG_CHUNK;

      if (sceKernelGetRandomNumber(p, n) < 0)
         return false;

      p   += n;
      len -= n;
   }

   return true;
}

// mbedtls entropy source (MBEDTLS_ENTROPY_HARDWARE_ALT), feeds both the tls
// drbg and psa. failing here makes the tls handshake fail, which is the point
int mbedtls_hardware_poll(void *data, unsigned char *output, size_t len, size_t *olen)
{
   (void)data;

   if (!rng_bytes(output, len))
   {
      *olen = 0;
      return -1;
   }

   *olen = len;
   return 0;
}
