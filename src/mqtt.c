// just enough mqtt 3.1.1 to talk to a public broker: connect with a last
// will, qos 0 publish/subscribe, keepalive pings. everything rides inside
// tls (mbedtls), checked against the two pinned cas in lobby_ca.h plus the
// broker's hostname. no plaintext fallback, if tls can't be set up the
// lobby just isn't available

#include <stdio.h>
#include <string.h>
#include <psp2/net/net.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>

#include "mbedtls/ssl.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/entropy.h"
#include "mbedtls/x509_crt.h"
#include "mbedtls/net_sockets.h"
#include "mbedtls/platform_time.h"
#include "psa/crypto.h"

#include "lobby_ca.h"
#include "mqtt.h"

#define MQTT_RX_SIZE      (32 * 1024)
#define MQTT_TX_SIZE      (4 * 1024)
#define MQTT_HEAD_ROOM    5
#define CONNECT_WAIT_US   10000000ULL
#define WRITE_WAIT_US     5000000ULL

#define MQTT_CONNECT      0x10
#define MQTT_CONNACK      0x20
#define MQTT_PUBLISH      0x30
#define MQTT_SUBSCRIBE    0x82
#define MQTT_UNSUBSCRIBE  0xA2
#define MQTT_PINGREQ      0xC0
#define MQTT_DISCONNECT   0xE0

static volatile int sock = -1;
static uint8_t  rx[MQTT_RX_SIZE];
static unsigned rx_len;
static uint8_t  tx[MQTT_TX_SIZE];
static uint16_t packet_id = 1;
static uint64_t keepalive_us;
static uint64_t last_tx_us;
static char     last_error[96] = "none";

static bool                     tls_ready = false;
static bool                     tls_live = false;
static mbedtls_entropy_context  entropy;
static mbedtls_ctr_drbg_context drbg;
static mbedtls_x509_crt         cas;
static mbedtls_ssl_config       conf;
static mbedtls_ssl_context      ssl;

static void set_error(const char *what, int code)
{
   snprintf(last_error, sizeof(last_error), "%s (-0x%04X)", what, (unsigned)-code);
}

// mbedtls has no clock of its own on the vita (MBEDTLS_PLATFORM_MS_TIME_ALT),
// it only uses this for elapsed times so the process clock is fine
mbedtls_ms_time_t mbedtls_ms_time(void)
{
   return (mbedtls_ms_time_t)(sceKernelGetProcessTimeWide() / 1000ULL);
}

static int bio_send(void *ctx, const unsigned char *buf, size_t len)
{
   int n;

   (void)ctx;

   // never blocks: a broker that stops reading would otherwise park the
   // caller in here for as long as tcp keeps retrying
   n = sceNetSend(sock, buf, len, SCE_NET_MSG_DONTWAIT);
   if (n > 0)
      return n;
   if (n == (int)SCE_NET_ERROR_EAGAIN)
      return MBEDTLS_ERR_SSL_WANT_WRITE;
   return MBEDTLS_ERR_NET_SEND_FAILED;
}

static int bio_recv(void *ctx, unsigned char *buf, size_t len)
{
   int n;

   (void)ctx;

   n = sceNetRecv(sock, buf, len, SCE_NET_MSG_DONTWAIT);
   if (n >= 0)
      return n;
   if (n == (int)SCE_NET_ERROR_EAGAIN)
      return MBEDTLS_ERR_SSL_WANT_READ;
   return MBEDTLS_ERR_NET_RECV_FAILED;
}

// once per app run: psa, the drbg and the pinned cas
static bool tls_setup(void)
{
   int ret;

   if (tls_ready)
      return true;

   if (psa_crypto_init() != PSA_SUCCESS)
   {
      set_error("crypto init failed", 0);
      return false;
   }

   mbedtls_entropy_init(&entropy);
   mbedtls_ctr_drbg_init(&drbg);
   mbedtls_x509_crt_init(&cas);

   ret = mbedtls_ctr_drbg_seed(&drbg, mbedtls_entropy_func, &entropy,
         (const unsigned char *)"naohac lobby", 12);
   if (ret != 0)
   {
      set_error("rng seed failed", ret);
      return false;
   }

   ret = mbedtls_x509_crt_parse(&cas, (const unsigned char *)lobby_ca_pem, sizeof(lobby_ca_pem));
   if (ret != 0)
   {
      set_error("bad pinned ca", ret);
      return false;
   }

   tls_ready = true;
   return true;
}

static void tls_teardown(void)
{
   if (!tls_live)
      return;

   mbedtls_ssl_free(&ssl);
   mbedtls_ssl_config_free(&conf);
   tls_live = false;
}

static void close_socket(void);

// keeps going until it's all through, the link is dead or the broker
// hasn't taken a byte in WRITE_WAIT_US. a half-sent record can't be
// picked up again, so any failure closes the connection
static bool tls_write_all(const uint8_t *buf, unsigned len)
{
   uint64_t start = sceKernelGetProcessTimeWide();

   while (len)
   {
      int n = mbedtls_ssl_write(&ssl, buf, len);

      if (n == MBEDTLS_ERR_SSL_WANT_WRITE || n == MBEDTLS_ERR_SSL_WANT_READ)
      {
         if (sceKernelGetProcessTimeWide() - start >= WRITE_WAIT_US)
         {
            set_error("broker stopped taking data", 0);
            close_socket();
            return false;
         }

         sceKernelDelayThread(1000);
         continue;
      }

      if (n <= 0)
      {
         set_error("send failed", n);
         close_socket();
         return false;
      }

      buf += n;
      len -= n;
   }

   last_tx_us = sceKernelGetProcessTimeWide();
   return true;
}

static void describe_cert_failure(uint32_t flags)
{
   if (flags & (MBEDTLS_X509_BADCERT_FUTURE | MBEDTLS_X509_BADCERT_EXPIRED))
      snprintf(last_error, sizeof(last_error),
            "broker certificate dates don't fit, is the vita's clock right? (flags 0x%X)", (unsigned)flags);
   else if (flags & MBEDTLS_X509_BADCERT_CN_MISMATCH)
      snprintf(last_error, sizeof(last_error), "broker certificate is for another host (flags 0x%X)", (unsigned)flags);
   else
      snprintf(last_error, sizeof(last_error), "broker certificate not trusted (flags 0x%X)", (unsigned)flags);
}

static unsigned put_u16(uint8_t *p, unsigned v)
{
   p[0] = (uint8_t)(v >> 8);
   p[1] = (uint8_t)v;
   return 2;
}

static unsigned put_str(uint8_t *p, const char *s)
{
   unsigned len = strlen(s);

   put_u16(p, len);
   memcpy(p + 2, s, len);
   return len + 2;
}

// the body is built at tx + MQTT_HEAD_ROOM, the fixed header gets slotted
// in right in front of it so the whole thing goes out in one record
static bool send_packet(uint8_t first, unsigned body_len)
{
   uint8_t head[MQTT_HEAD_ROOM];
   unsigned head_len = 1;
   unsigned len = body_len;
   uint8_t *start;

   if (sock < 0 || !tls_live)
      return false;

   head[0] = first;
   do
   {
      uint8_t b = len & 0x7F;
      len >>= 7;
      if (len)
         b |= 0x80;
      head[head_len++] = b;
   } while (len);

   start = tx + MQTT_HEAD_ROOM - head_len;
   memcpy(start, head, head_len);
   return tls_write_all(start, head_len + body_len);
}

// remaining length is a 1-4 byte varint. 0 = not all here yet, -1 = junk
static int read_len(const uint8_t *p, unsigned avail, unsigned *value)
{
   unsigned mult = 1;
   unsigned v = 0;

   for (unsigned i = 0; i < 4; i++)
   {
      if (i >= avail)
         return 0;

      v += (p[i] & 0x7F) * mult;
      if (!(p[i] & 0x80))
      {
         *value = v;
         return (int)i + 1;
      }

      mult *= 128;
   }

   return -1;
}

static void close_socket(void)
{
   int s = sock;

   tls_teardown();
   sock = -1;
   rx_len = 0;

   if (s >= 0)
      sceNetSocketClose(s);
}

bool mqtt_open(const char *host, uint32_t addr, uint16_t port, const char *client_id,
      const char *will_topic, unsigned keepalive_s)
{
   SceNetSockaddrIn sa;
   uint8_t *p = tx + MQTT_HEAD_ROOM;
   uint64_t start;
   int one = 1;
   int s, ret;

   if (!tls_setup())
      return false;

   s = sceNetSocket("naohac_lobby", SCE_NET_AF_INET, SCE_NET_SOCK_STREAM, 0);
   if (s < 0)
   {
      set_error("socket failed", s);
      return false;
   }

   sock = s;
   rx_len = 0;
   sceNetSetsockopt(s, SCE_NET_IPPROTO_TCP, SCE_NET_TCP_NODELAY, &one, sizeof(one));

   memset(&sa, 0, sizeof(sa));
   sa.sin_len         = sizeof(sa);
   sa.sin_family      = SCE_NET_AF_INET;
   sa.sin_port        = sceNetHtons(port);
   sa.sin_addr.s_addr = addr;

   ret = sceNetConnect(s, (SceNetSockaddr *)&sa, sizeof(sa));
   if (ret < 0)
   {
      set_error("connect failed", ret);
      goto fail;
   }

   mbedtls_ssl_init(&ssl);
   mbedtls_ssl_config_init(&conf);
   tls_live = true;

   ret = mbedtls_ssl_config_defaults(&conf, MBEDTLS_SSL_IS_CLIENT,
         MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT);
   if (ret != 0)
   {
      set_error("tls config failed", ret);
      goto fail;
   }

   mbedtls_ssl_conf_authmode(&conf, MBEDTLS_SSL_VERIFY_REQUIRED);
   mbedtls_ssl_conf_ca_chain(&conf, &cas, NULL);
   mbedtls_ssl_conf_rng(&conf, mbedtls_ctr_drbg_random, &drbg);
   mbedtls_ssl_conf_min_tls_version(&conf, MBEDTLS_SSL_VERSION_TLS1_2);

   ret = mbedtls_ssl_setup(&ssl, &conf);
   if (ret == 0)
      ret = mbedtls_ssl_set_hostname(&ssl, host);
   if (ret != 0)
   {
      set_error("tls setup failed", ret);
      goto fail;
   }

   mbedtls_ssl_set_bio(&ssl, NULL, bio_send, bio_recv, NULL);

   start = sceKernelGetProcessTimeWide();
   while ((ret = mbedtls_ssl_handshake(&ssl)) != 0)
   {
      if (ret != MBEDTLS_ERR_SSL_WANT_READ && ret != MBEDTLS_ERR_SSL_WANT_WRITE)
      {
         uint32_t flags = mbedtls_ssl_get_verify_result(&ssl);

         if (ret == MBEDTLS_ERR_X509_CERT_VERIFY_FAILED && flags)
            describe_cert_failure(flags);
         else
            set_error("tls handshake failed", ret);
         goto fail;
      }

      if (sock < 0 || sceKernelGetProcessTimeWide() - start >= CONNECT_WAIT_US)
      {
         set_error("tls handshake timed out", 0);
         goto fail;
      }

      sceKernelDelayThread(5000);
   }

   // clean session, plus a retained empty will on will_topic: if we drop
   // off without saying goodbye the broker wipes our room for us
   p += put_str(p, "MQTT");
   *p++ = 4;
   *p++ = 0x02 | 0x04 | 0x20;
   p += put_u16(p, keepalive_s);
   p += put_str(p, client_id);
   p += put_str(p, will_topic);
   p += put_u16(p, 0);

   keepalive_us = (uint64_t)keepalive_s * 1000000ULL;

   if (!send_packet(MQTT_CONNECT, p - (tx + MQTT_HEAD_ROOM)))
      goto fail;

   start = sceKernelGetProcessTimeWide();
   while (rx_len < 4)
   {
      int n = mbedtls_ssl_read(&ssl, rx + rx_len, 4 - rx_len);

      if (n > 0)
      {
         rx_len += n;
         continue;
      }

      if (n != MBEDTLS_ERR_SSL_WANT_READ && n != MBEDTLS_ERR_SSL_WANT_WRITE &&
            n != MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET)
      {
         set_error("connack recv failed", n);
         goto fail;
      }

      if (sock < 0 || sceKernelGetProcessTimeWide() - start >= CONNECT_WAIT_US)
      {
         set_error("no connack", 0);
         goto fail;
      }

      sceKernelDelayThread(10000);
   }

   if (rx[0] != MQTT_CONNACK || rx[3] != 0)
   {
      set_error("connection refused", -((rx[0] << 8) | rx[3]));
      goto fail;
   }

   rx_len = 0;
   return true;

fail:
   close_socket();
   return false;
}

// safe from another thread, kicks a blocking connect or a handshake loose
void mqtt_abort(void)
{
   int s = sock;

   if (s >= 0)
      sceNetSocketAbort(s, 0);
}

void mqtt_close(void)
{
   if (sock < 0)
      return;

   if (tls_live)
   {
      tx[MQTT_HEAD_ROOM - 2] = MQTT_DISCONNECT;
      tx[MQTT_HEAD_ROOM - 1] = 0;
      if (tls_write_all(tx + MQTT_HEAD_ROOM - 2, 2))
         mbedtls_ssl_close_notify(&ssl);
   }

   close_socket();
}

bool mqtt_is_open(void)
{
   return sock >= 0 && tls_live;
}

const char *mqtt_error(void)
{
   return last_error;
}

bool mqtt_publish(const char *topic, const void *payload, unsigned len, bool retain)
{
   uint8_t *p = tx + MQTT_HEAD_ROOM;

   if (2 + strlen(topic) + len > MQTT_TX_SIZE - MQTT_HEAD_ROOM)
      return false;

   p += put_str(p, topic);
   memcpy(p, payload, len);
   p += len;

   return send_packet(MQTT_PUBLISH | (retain ? 0x01 : 0x00), p - (tx + MQTT_HEAD_ROOM));
}

static bool send_filter(uint8_t type, const char *filter, bool with_qos)
{
   uint8_t *p = tx + MQTT_HEAD_ROOM;

   p += put_u16(p, packet_id);
   packet_id = (packet_id == 0xFFFF) ? 1 : packet_id + 1;
   p += put_str(p, filter);
   if (with_qos)
      *p++ = 0;

   return send_packet(type, p - (tx + MQTT_HEAD_ROOM));
}

bool mqtt_subscribe(const char *filter)
{
   return send_filter(MQTT_SUBSCRIBE, filter, true);
}

bool mqtt_unsubscribe(const char *filter)
{
   return send_filter(MQTT_UNSUBSCRIBE, filter, false);
}

static void handle_packet(uint8_t first, const uint8_t *body, unsigned len, mqtt_message_cb cb)
{
   char topic[128];
   unsigned topic_len, pos;

   // acks and ping replies need nothing from us
   if ((first & 0xF0) != MQTT_PUBLISH || len < 2)
      return;

   topic_len = (body[0] << 8) | body[1];
   pos = 2 + topic_len;
   if (((first >> 1) & 3) != 0)
      pos += 2;

   if (pos > len || topic_len >= sizeof(topic))
      return;

   memcpy(topic, body + 2, topic_len);
   topic[topic_len] = '\0';

   cb(topic, body + pos, len - pos);
}

// false once the connection is gone
bool mqtt_poll(mqtt_message_cb cb)
{
   unsigned pos = 0;

   if (sock < 0 || !tls_live)
      return false;

   while (rx_len < sizeof(rx))
   {
      int n = mbedtls_ssl_read(&ssl, rx + rx_len, sizeof(rx) - rx_len);

      if (n > 0)
      {
         rx_len += n;
         continue;
      }

      if (n == MBEDTLS_ERR_SSL_WANT_READ || n == MBEDTLS_ERR_SSL_WANT_WRITE)
         break;

      if (n == MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET)
         continue;

      if (n == 0 || n == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY || n == MBEDTLS_ERR_SSL_CONN_EOF)
         set_error("closed by the broker", 0);
      else
         set_error("recv failed", n);

      close_socket();
      return false;
   }

   while (rx_len - pos >= 2)
   {
      unsigned body_len, total;
      int len_bytes = read_len(rx + pos + 1, rx_len - pos - 1, &body_len);

      if (len_bytes == 0)
         break;

      if (len_bytes < 0)
      {
         set_error("bad packet from the broker", 0);
         close_socket();
         return false;
      }

      total = 1 + len_bytes + body_len;
      if (total > sizeof(rx))
      {
         set_error("packet too big for us", -(int)total);
         close_socket();
         return false;
      }

      if (rx_len - pos < total)
         break;

      handle_packet(rx[pos], rx + pos + 1 + len_bytes, body_len, cb);
      pos += total;

      if (sock < 0)
         return false;
   }

   memmove(rx, rx + pos, rx_len - pos);
   rx_len -= pos;

   if (sceKernelGetProcessTimeWide() - last_tx_us >= keepalive_us / 2)
   {
      tx[MQTT_HEAD_ROOM - 2] = MQTT_PINGREQ;
      tx[MQTT_HEAD_ROOM - 1] = 0;
      if (!tls_write_all(tx + MQTT_HEAD_ROOM - 2, 2))
      {
         close_socket();
         return false;
      }
   }

   return true;
}
