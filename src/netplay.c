// online play without anyone having to run (or pay for) a server.
//
// rooms and matchmaking live on a free public mqtt broker, reached over tls
// with the broker certificate pinned. every room is a retained message
// signed by its host, every vita has its own inbox topic, and that's how a
// guest knocks on a host's door. the knock and the answer are sealed for
// the one vita they're meant for, everything after that (punches, inputs,
// the sync check) is sealed with a key only the two players share.
//
// once paired, both vitas ask a public stun server what their address looks
// like from outside and fire udp at each other until their routers let it
// through. if that never works out, or either player asked to keep their ip
// to themselves, the same sealed packets ride the broker instead.
//
// nothing but pad input crosses during a match. both sides boot the rom
// from factory nvram and compare a hash of the resulting machine, no
// savestate ever gets sent or loaded

#include <stddef.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <psp2/apputil.h>
#include <psp2/system_param.h>
#include <psp2/sysmodule.h>
#include <psp2/net/net.h>
#include <psp2/net/netctl.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/kernel/cpu.h>

#include "deps/monocypher/monocypher.h"
#include "mqtt.h"
#include "rng.h"
#include "netplay.h"

#define NET_MEM_SIZE       (1024 * 1024)
#define NETPLAY_PORT       41795
#define NET_LOG_PATH       "ux0:data/NaohAC/system/netplay_log.txt"

#define TOPIC_ROOT         "naohac/v2"
#define ROOMS_FILTER       TOPIC_ROOT "/rooms/+"
#define LOBBY_KEEPALIVE_S  30
#define LOBBY_THREAD_STACK (256 * 1024)

#define STUN_HOST          "stun.l.google.com"
#define STUN_PORT          19302
#define STUN_TRIES         8

#define PKT_MAGIC          0x484F414Eu
#define PKT_VERSION        6
#define KEY_SIZE           NETPLAY_KEY_SIZE
#define SALT_SIZE          NETPLAY_SALT_SIZE
#define NONCE_SIZE         24
#define MAC_SIZE           16
#define SIG_SIZE           64
#define ID_BYTES           12

#define SEALED_MAX         1200
#define RELAY_QUEUE        64
#define RELAY_OUT_QUEUE    16
#define RELAY_THREAD_STACK (128 * 1024)
#define REPLAY_WINDOW      256
#define SEQ_AHEAD_MAX      65536

#define INPUT_RING         128
#define INPUT_HISTORY      32
#define MAX_DELAY          12

#define LINK_TIMEOUT_US    10000000ULL
#define SYNC_TIMEOUT_US    30000000ULL
#define JOIN_TIMEOUT_US    8000000ULL
#define JOIN_RETRY_US      2000000ULL
#define PUNCH_WAIT_US      3000000ULL
#define PUNCH_EAGER_US     15000000ULL
#define PUNCH_RETRY_US     200000ULL
#define PUNCH_LAZY_US      1000000ULL
#define STUN_RETRY_US      500000ULL
#define INFO_RETRY_US      100000ULL
#define READY_RESEND_US    50000ULL
#define INPUT_RESEND_US    16000ULL
#define RELAY_RESEND_US    50000ULL
#define STATS_EVERY_US     5000000ULL
#define ROOM_REFRESH_US    30000000ULL
#define RTT_SANE_MS        1000
#define SLOW_FRAME_US      25000ULL

// rate limits on anything an outsider can make us do work for. legit
// traffic never gets near these
#define UDP_PER_SECOND     6000.0f
#define UDP_BURST          512.0f
#define JOINS_PER_SECOND   4.0f
#define JOIN_BURST         8.0f
#define ROOM_CHECKS_PER_S  30.0f
#define ROOM_CHECK_BURST   30.0f

#define DIR_HOST           1
#define DIR_GUEST          2

// tried in order, everyone has to land on the same one to see each
// other's rooms so the second is only there for when the first is down.
// mosquitto goes first because the relay leans on it: emqx's public broker
// quietly drops everything past ~10 messages a second per client
static const struct {
   const char *host;
   uint16_t    port;
} brokers[] = {
   { "test.mosquitto.org", 8883 },
   { "broker.emqx.io",     8883 },
};
#define BROKER_COUNT (sizeof(brokers) / sizeof(brokers[0]))

// outer packet types, readable by anyone who sees them
enum {
   PKT_JOIN = 1,
   PKT_ACCEPT,
   PKT_REJECT,
   PKT_SEALED
};

// what goes inside a sealed packet, readable by the two players only
enum {
   MSG_PUNCH = 1,
   MSG_PUNCH_ACK,
   MSG_STATE_INFO,
   MSG_READY,
   MSG_MISMATCH,
   MSG_INPUT,
   MSG_QUIT
};

// both ends are always a vita, so nothing past the addresses bothers with
// byte swapping
typedef struct __attribute__((packed)) {
   uint32_t magic;
   uint8_t  version;
   uint8_t  type;
} pkt_head_t;

// join, accept and reject cross the public broker. the body is sealed
// (x25519 + xchacha20-poly1305) for the one vita it's meant for, and
// everything up to the mac rides along as associated data
typedef struct __attribute__((packed)) {
   pkt_head_t head;
   uint8_t    sender_key[KEY_SIZE];
   uint8_t    nonce[NONCE_SIZE];
   uint8_t    mac[MAC_SIZE];
} hello_head_t;

// the guest's knock. room_nonce has to match the room as it stands right
// now, so a join recorded off the broker can't be played back later.
// addresses and ports are in network order, 0 when not shared
typedef struct __attribute__((packed)) {
   char     from[NETPLAY_ID_LEN];
   char     name[NETPLAY_NAME_LEN];
   char     rom[NETPLAY_ROM_LEN];
   uint32_t rom_crc;
   uint8_t  room_nonce[SALT_SIZE];
   uint8_t  salt[SALT_SIZE];
   uint32_t stamp;
   uint8_t  relay_only;
   uint32_t public_addr;
   uint16_t public_port;
} join_body_t;

// the host's answer, echoing the guest's salt so it can't be replayed
// against a later join
typedef struct __attribute__((packed)) {
   uint8_t  guest_salt[SALT_SIZE];
   uint8_t  host_salt[SALT_SIZE];
   uint32_t session;
   uint32_t stamp;
   uint8_t  relay_only;
   uint32_t public_addr;
   uint16_t public_port;
   // used to carry the host's home address. never sent or read any more
   // (a guest can claim any public ip it likes), only kept so the layout
   // still matches v6
   uint32_t unused_addr;
   uint16_t unused_port;
} accept_body_t;

typedef struct __attribute__((packed)) {
   uint8_t guest_salt[SALT_SIZE];
   uint8_t reason;
} reject_body_t;

// everything between the two players once they're paired, over udp or
// the relay alike. the nonce is never sent: it's the sender's direction
// plus seq, unique for the key, and seq doubles as replay protection
typedef struct __attribute__((packed)) {
   pkt_head_t head;
   uint32_t   session;
   uint64_t   seq;
   uint8_t    mac[MAC_SIZE];
} sealed_head_t;

#define SEALED_BODY_MAX (SEALED_MAX - sizeof(sealed_head_t))

typedef struct __attribute__((packed)) {
   uint8_t  type;
   uint32_t stamp;
} msg_punch_t;

typedef struct __attribute__((packed)) {
   uint8_t  type;
   uint32_t size;
   uint8_t  digest[NETPLAY_DIGEST_SIZE];
} msg_state_t;

typedef struct __attribute__((packed)) {
   uint8_t type;
   uint8_t delay;
} msg_ready_t;

// every input packet repeats the last few frames, so a dropped packet
// just gets covered by the next one instead of needing a resend request
typedef struct __attribute__((packed)) {
   uint8_t  type;
   uint32_t frame;
   uint8_t  count;
   uint16_t bits[INPUT_HISTORY];
} msg_input_t;

// a room as it sits on the broker, signed by the host. the room id is a
// hash of sign_key, so nobody else can sign for that id
typedef struct __attribute__((packed)) {
   uint8_t  version;
   uint8_t  players;
   uint8_t  relay_only;
   uint32_t seq;
   uint8_t  room_nonce[SALT_SIZE];
   char     game[NETPLAY_GAME_LEN];
   char     name[NETPLAY_NAME_LEN];
   char     rom[NETPLAY_ROM_LEN];
   uint32_t rom_crc;
   uint8_t  sign_key[KEY_SIZE];
   uint8_t  box_key[KEY_SIZE];
} room_body_t;

typedef struct __attribute__((packed)) {
   room_body_t body;
   uint8_t     sig[SIG_SIZE];
} room_msg_t;

typedef enum {
   LINK_IDLE = 0,
   LINK_HOSTING,
   LINK_JOINING,
   LINK_PUNCHING,
   LINK_CONNECTED
} link_state_t;

typedef struct {
   float    tokens;
   float    rate;
   float    burst;
   uint64_t last_us;
} bucket_t;

static uint8_t           net_memory[NET_MEM_SIZE];
static bool              net_up = false;
static bool              net_inited_here = false;
static bool              apputil_up = false;
static int               init_error = 0;
static int               udp = -1;
static netplay_options_t opts;

static uint32_t local_addr;
static uint16_t local_port;
static uint32_t public_addr;
static uint16_t public_port;
static char     local_name[NETPLAY_NAME_LEN];
static char     my_id[NETPLAY_ID_LEN];
static char     inbox_topic[64];
static char     room_topic[64];

static uint8_t  sign_secret[64];
static uint8_t  sign_public[KEY_SIZE];
static uint8_t  box_secret[KEY_SIZE];
static uint8_t  box_public[KEY_SIZE];

static volatile netplay_lobby_t lobby_state = NETPLAY_LOBBY_FAILED;
static volatile bool            lobby_abort = false;
static volatile int             lobby_resolver = -1;
static SceUID                   lobby_thread = -1;
static char                     lobby_error[96] = "";

static volatile uint32_t stun_addr;
static bool              stun_known;
static uint8_t           stun_tid[12];
static unsigned          stun_tries;
static uint64_t          last_stun_us;

static char           browse_game[NETPLAY_GAME_LEN];
static netplay_room_t rooms[NETPLAY_MAX_ROOMS];
static int            room_count;

static bool     room_open;
static uint32_t room_seq;
static uint8_t  room_nonce[SALT_SIZE];
static uint64_t last_room_us;
static char     my_game[NETPLAY_GAME_LEN];
static char     my_rom[NETPLAY_ROM_LEN];
static uint32_t my_rom_crc;

static link_state_t     link_state = LINK_IDLE;
static bool             is_host;
static uint32_t         session;
static netplay_event_t  pending_event;
static netplay_reject_t reject_reason;
static unsigned         last_rtt_ms;

static char     peer_id[NETPLAY_ID_LEN];
static char     peer_inbox[64];
static char     peer_name[NETPLAY_NAME_LEN];
static uint8_t  peer_key[KEY_SIZE];
static bool     peer_relay_only;
static uint8_t  target_nonce[SALT_SIZE];
static uint8_t  guest_salt[SALT_SIZE];
static uint8_t  host_salt[SALT_SIZE];
static uint32_t join_stamp;
static uint64_t join_start_us;
static uint64_t last_join_us;

static uint8_t  link_key[KEY_SIZE];
static uint64_t tx_seq;
static uint64_t rx_top;
static uint64_t rx_window[REPLAY_WINDOW / 64];

static uint32_t peer_public_addr;
static uint16_t peer_public_port;
static uint32_t peer_learned_addr;
static uint16_t peer_learned_port;
static bool     direct;
static uint32_t peer_addr;
static uint16_t peer_port;
static uint64_t peer_seen_us;
static uint64_t punch_start_us;
static uint64_t last_punch_us;
static bool     punch_slowed;

// incoming relay frames. during a match the relay thread fills it and
// the game thread empties it, so each index only has one writer: tail
// belongs to whoever reads the broker, head to the game thread
static uint8_t           relay_queue[RELAY_QUEUE][SEALED_MAX];
static uint16_t          relay_len[RELAY_QUEUE];
static volatile unsigned relay_head;
static volatile unsigned relay_tail;

// outgoing relay frames during a match, same deal the other way round
static uint8_t           relay_out[RELAY_OUT_QUEUE][SEALED_MAX];
static uint16_t          relay_out_len[RELAY_OUT_QUEUE];
static volatile unsigned relay_out_head;
static volatile unsigned relay_out_tail;

static SceUID        relay_thread = -1;
static uint32_t      relay_session;
static volatile bool relay_quit;
static volatile bool relay_lost;

static bucket_t udp_bucket;
static bucket_t join_bucket;
static bucket_t room_bucket;

static struct {
   unsigned udp_tx, udp_rx;
   unsigned relay_tx, relay_rx, relay_dropped, relay_send_failed;
   unsigned punch_tx, punch_rx;
   unsigned junk, forged, replayed;
   unsigned joins_dropped, rooms_rejected;
   unsigned waits, wait_ms, worst_wait_ms, slow_frames;
   unsigned relay_worst_ms;
} stats;
static uint64_t last_stats_us;

static uint64_t last_push_us;
static uint64_t wait_since_us;
static uint64_t frame_wait_us;

static uint16_t local_ring[INPUT_RING];
static uint16_t remote_ring[INPUT_RING];
static uint32_t local_next;
static uint32_t remote_next;
static uint32_t cur_frame;
static unsigned match_delay;
static uint64_t last_input_us;
static uint64_t last_ready_us;

static uint8_t  rx_buf[SEALED_MAX];
static uint32_t rx_addr;
static uint16_t rx_port;

static uint64_t now_us(void)
{
   return sceKernelGetProcessTimeWide();
}

// how long ago `since` was. the clock can get read before a callback
// stamps something a hair later, so this never goes negative (unsigned
// wraparound there used to read as "silent for 500,000 years")
static uint64_t elapsed(uint64_t since, uint64_t now)
{
   return (now > since) ? now - since : 0;
}

// starts fresh on the first line of each app launch, same as
// core_log.txt, then every line gets seconds since that first line
void netplay_log(const char *fmt, ...)
{
   static bool started = false;
   static uint64_t epoch_us;
   uint64_t t = now_us();
   va_list ap;
   FILE *f;

   f = fopen(NET_LOG_PATH, started ? "a" : "w");
   if (!f)
      return;

   if (!started)
   {
      started  = true;
      epoch_us = t;
   }

   fprintf(f, "[%8.3f] ", (double)(t - epoch_us) / 1000000.0);

   va_start(ap, fmt);
   vfprintf(f, fmt, ap);
   va_end(ap);

   fputc('\n', f);
   fclose(f);
}

static bool is_private_addr(uint32_t addr)
{
   const uint8_t *a = (const uint8_t *)&addr;

   return a[0] == 10 || a[0] == 127 ||
         (a[0] == 192 && a[1] == 168) ||
         (a[0] == 172 && (a[1] & 0xF0) == 16) ||
         (a[0] == 100 && (a[1] & 0xC0) == 64) ||
         (a[0] == 169 && a[1] == 254);
}

// rotating buffers so a single log line can print a couple of these.
// a player's address, public or home network, only ever gets its first
// half written down: plenty to tell two connections apart, useless for
// finding anyone. only our own servers are printed whole
static const char *format_addr(uint32_t addr, uint16_t port, bool mask)
{
   static char buf[4][32];
   static unsigned slot;
   const uint8_t *a = (const uint8_t *)&addr;
   const uint8_t *p = (const uint8_t *)&port;
   char *out = buf[slot++ % 4];

   if (mask)
      snprintf(out, sizeof(buf[0]), "%u.%u.x.x:%u (%s)", a[0], a[1], (p[0] << 8) | p[1],
            is_private_addr(addr) ? "home" : "internet");
   else
      snprintf(out, sizeof(buf[0]), "%u.%u.%u.%u:%u", a[0], a[1], a[2], a[3], (p[0] << 8) | p[1]);
   return out;
}

static const char *addr_str(uint32_t addr, uint16_t port)
{
   return format_addr(addr, port, true);
}

static const char *server_addr_str(uint32_t addr, uint16_t port)
{
   return format_addr(addr, port, false);
}

static void hex_prefix(char *out, const uint8_t *data, size_t bytes)
{
   for (size_t i = 0; i < bytes; i++)
      snprintf(out + i * 2, 3, "%02x", data[i]);
}

// ids go straight into broker topic names, so anything that isn't the
// 24 hex digits we derive ourselves gets thrown out. a '#' or '+' in a
// topic we publish to would get us kicked off the broker
static bool valid_id(const char *id)
{
   size_t n = strnlen(id, NETPLAY_ID_LEN);

   if (n != ID_BYTES * 2)
      return false;

   for (size_t i = 0; i < n; i++)
   {
      if (!((id[i] >= '0' && id[i] <= '9') || (id[i] >= 'a' && id[i] <= 'f')))
         return false;
   }

   return true;
}

// an id is the first 96 bits of a hash of the signing key, so a room can
// only ever be signed for by whoever holds that key
static void id_from_key(char out[NETPLAY_ID_LEN], const uint8_t key[KEY_SIZE])
{
   uint8_t hash[ID_BYTES];

   crypto_blake2b(hash, sizeof(hash), key, KEY_SIZE);
   memset(out, 0, NETPLAY_ID_LEN);
   hex_prefix(out, hash, sizeof(hash));
}

// names come off a public broker and end up on screen, keep them to
// plain printable ascii
static void make_printable(char *s, size_t n)
{
   for (size_t i = 0; i < n && s[i]; i++)
   {
      if ((unsigned char)s[i] < 0x20 || (unsigned char)s[i] > 0x7E)
         s[i] = '?';
   }
}

static void copy_field(char *dst, const char *src, size_t n)
{
   memcpy(dst, src, n);
   dst[n - 1] = '\0';
}

const char *netplay_event_name(netplay_event_t ev)
{
   switch (ev)
   {
      case NETPLAY_EVENT_NONE:       return "none";
      case NETPLAY_EVENT_JOINED:     return "joined";
      case NETPLAY_EVENT_CONNECTED:  return "connected";
      case NETPLAY_EVENT_REJECTED:   return "rejected";
      case NETPLAY_EVENT_PEER_LEFT:  return "peer left";
      case NETPLAY_EVENT_TIMEOUT:    return "timeout";
      case NETPLAY_EVENT_LOBBY_LOST: return "lobby lost";
      case NETPLAY_EVENT_ERROR:      return "error";
   }
   return "?";
}

static void bucket_init(bucket_t *b, float rate, float burst)
{
   b->tokens  = burst;
   b->rate    = rate;
   b->burst   = burst;
   b->last_us = now_us();
}

static bool bucket_take(bucket_t *b)
{
   uint64_t now = now_us();

   b->tokens += (float)elapsed(b->last_us, now) * b->rate / 1000000.0f;
   if (b->tokens > b->burst)
      b->tokens = b->burst;
   b->last_us = now;

   if (b->tokens < 1.0f)
      return false;

   b->tokens -= 1.0f;
   return true;
}

// dotted quad straight into network order, no help from the stack needed
static bool parse_ipv4(const char *s, uint32_t *out)
{
   uint8_t b[4];

   for (int i = 0; i < 4; i++)
   {
      unsigned v = 0;
      int digits = 0;

      while (*s >= '0' && *s <= '9')
      {
         v = v * 10 + (unsigned)(*s++ - '0');
         if (v > 255 || ++digits > 3)
            return false;
      }

      if (!digits)
         return false;

      b[i] = (uint8_t)v;

      if (i < 3 && *s++ != '.')
         return false;
   }

   if (*s)
      return false;

   memcpy(out, b, 4);
   return true;
}

static bool resolve(const char *host, uint32_t *out)
{
   SceNetInAddr addr;
   int rid, ret;

   rid = sceNetResolverCreate("naohac_dns", NULL, 0);
   if (rid < 0)
   {
      netplay_log("dns: resolver create 0x%08X", rid);
      return false;
   }

   lobby_resolver = rid;
   ret = sceNetResolverStartNtoa(rid, host, &addr, 0, 0, 0);
   lobby_resolver = -1;
   sceNetResolverDestroy(rid);

   if (ret < 0)
   {
      netplay_log("dns: %s failed 0x%08X", host, ret);
      return false;
   }

   *out = addr.s_addr;
   return true;
}

// x25519 with their public key, run through blake2b along with a label,
// both public keys (lowest first, so each side hashes the same thing) and
// whatever else the caller wants bound in. refuses outright if their key
// was a low-order point: the shared secret comes out all zeroes then, and
// anyone could work out the key
static bool dh_key(uint8_t key[KEY_SIZE], const char *label, const uint8_t their[KEY_SIZE],
      const uint8_t *extra, size_t extra_len)
{
   static const uint8_t zero[KEY_SIZE];
   uint8_t shared[KEY_SIZE];
   crypto_blake2b_ctx ctx;
   bool mine_first = memcmp(box_public, their, KEY_SIZE) < 0;

   crypto_x25519(shared, box_secret, their);
   if (crypto_verify32(shared, zero) == 0)
   {
      crypto_wipe(shared, sizeof(shared));
      return false;
   }

   crypto_blake2b_init(&ctx, KEY_SIZE);
   crypto_blake2b_update(&ctx, (const uint8_t *)label, strlen(label));
   crypto_blake2b_update(&ctx, shared, KEY_SIZE);
   crypto_blake2b_update(&ctx, mine_first ? box_public : their, KEY_SIZE);
   crypto_blake2b_update(&ctx, mine_first ? their : box_public, KEY_SIZE);
   if (extra_len)
      crypto_blake2b_update(&ctx, extra, extra_len);
   crypto_blake2b_final(&ctx, key);

   crypto_wipe(shared, sizeof(shared));
   return true;
}

static void udp_send(uint32_t addr, uint16_t port, const void *pkt, unsigned len)
{
   SceNetSockaddrIn to;

   if (udp < 0)
      return;

   memset(&to, 0, sizeof(to));
   to.sin_len         = sizeof(to);
   to.sin_family      = SCE_NET_AF_INET;
   to.sin_port        = port;
   to.sin_addr.s_addr = addr;

   sceNetSendto(udp, pkt, len, 0, (SceNetSockaddr *)&to, sizeof(to));
}

static void make_nonce(uint8_t nonce[NONCE_SIZE], uint8_t dir, uint64_t seq)
{
   memset(nonce, 0, NONCE_SIZE);
   nonce[0] = dir;
   memcpy(nonce + 1, &seq, sizeof(seq));
}

static uint8_t my_dir(void)
{
   return is_host ? DIR_HOST : DIR_GUEST;
}

static uint8_t peer_dir(void)
{
   return is_host ? DIR_GUEST : DIR_HOST;
}

// 0 when it can't be sealed (no session yet, or too big)
static unsigned seal_frame(uint8_t frame[SEALED_MAX], const void *msg, unsigned len)
{
   sealed_head_t *h = (sealed_head_t *)frame;
   uint8_t nonce[NONCE_SIZE];

   if (!session || len == 0 || len > SEALED_BODY_MAX)
      return 0;

   h->head.magic   = PKT_MAGIC;
   h->head.version = PKT_VERSION;
   h->head.type    = PKT_SEALED;
   h->session      = session;
   h->seq          = tx_seq++;

   make_nonce(nonce, my_dir(), h->seq);
   crypto_aead_lock(frame + sizeof(*h), h->mac, link_key, nonce,
         frame, offsetof(sealed_head_t, mac), (const uint8_t *)msg, len);

   return sizeof(*h) + len;
}

static bool replay_fresh(uint64_t seq)
{
   uint64_t age;

   if (seq >= rx_top)
      return true;

   age = rx_top - 1 - seq;
   if (age >= REPLAY_WINDOW)
      return false;

   return !((rx_window[age / 64] >> (age % 64)) & 1);
}

// bit n of the window is seq (rx_top - 1 - n)
static void replay_mark(uint64_t seq)
{
   uint64_t age;

   if (seq >= rx_top)
   {
      uint64_t shift = seq - rx_top + 1;

      if (shift >= REPLAY_WINDOW)
         memset(rx_window, 0, sizeof(rx_window));
      else
      {
         unsigned words = (unsigned)(shift / 64);
         unsigned bits  = (unsigned)(shift % 64);

         for (int i = REPLAY_WINDOW / 64 - 1; i >= 0; i--)
         {
            int src = i - (int)words;
            uint64_t v = 0;

            if (src >= 0)
            {
               v = rx_window[src] << bits;
               if (bits && src > 0)
                  v |= rx_window[src - 1] >> (64 - bits);
            }

            rx_window[i] = v;
         }
      }

      rx_top = seq + 1;
   }

   age = rx_top - 1 - seq;
   rx_window[age / 64] |= (uint64_t)1 << (age % 64);
}

// checks and opens one sealed frame into out, returns the body length or
// -1. the cheap header checks go first so junk never costs a decrypt
static int open_frame(const uint8_t *frame, unsigned len, uint8_t *out)
{
   const sealed_head_t *h = (const sealed_head_t *)frame;
   uint8_t nonce[NONCE_SIZE];
   uint64_t seq;

   if (len <= sizeof(*h) || len > SEALED_MAX || h->head.magic != PKT_MAGIC ||
         h->head.version != PKT_VERSION || h->head.type != PKT_SEALED ||
         !session || h->session != session)
   {
      stats.junk++;
      return -1;
   }

   seq = h->seq;
   if (!replay_fresh(seq))
   {
      stats.replayed++;
      return -1;
   }

   // the opponent counts up one per packet, so anything miles past the
   // newest one we've seen is somebody guessing. turned away before it
   // costs a decrypt
   if (seq - rx_top > SEQ_AHEAD_MAX && seq >= rx_top)
   {
      stats.junk++;
      return -1;
   }

   make_nonce(nonce, peer_dir(), seq);
   if (crypto_aead_unlock(out, h->mac, link_key, nonce, frame, offsetof(sealed_head_t, mac),
            frame + sizeof(*h), len - sizeof(*h)) != 0)
   {
      if (stats.forged++ == 0)
         netplay_log("link: dropped a packet that wasn't sealed by our opponent");
      return -1;
   }

   replay_mark(seq);
   peer_seen_us = now_us();
   return (int)(len - sizeof(*h));
}

static void send_sealed_udp(uint32_t addr, uint16_t port, const void *msg, unsigned len)
{
   uint8_t frame[SEALED_MAX];
   unsigned n = seal_frame(frame, msg, len);

   if (n)
      udp_send(addr, port, frame, n);
}

// straight to the opponent if a hole got punched, otherwise through their
// broker inbox. sealed either way
static void peer_send(const void *msg, unsigned len)
{
   uint8_t frame[SEALED_MAX];
   unsigned n = seal_frame(frame, msg, len);

   if (!n)
      return;

   if (direct)
   {
      udp_send(peer_addr, peer_port, frame, n);
      stats.udp_tx++;
      return;
   }

   // mid-match the relay thread does the actual sending, a tls write
   // stuck on a slow broker can't hold up the game
   if (relay_thread >= 0)
   {
      unsigned tail = relay_out_tail;

      if (tail - __atomic_load_n(&relay_out_head, __ATOMIC_ACQUIRE) >= RELAY_OUT_QUEUE)
      {
         stats.relay_send_failed++;
         return;
      }

      memcpy(relay_out[tail % RELAY_OUT_QUEUE], frame, n);
      relay_out_len[tail % RELAY_OUT_QUEUE] = (uint16_t)n;
      __atomic_store_n(&relay_out_tail, tail + 1, __ATOMIC_RELEASE);
      return;
   }

   if (mqtt_is_open() && mqtt_publish(peer_inbox, frame, n, false))
   {
      __atomic_fetch_add(&stats.relay_tx, 1, __ATOMIC_RELAXED);
      return;
   }

   if (stats.relay_send_failed++ == 0)
      netplay_log("relay: send failed, lobby says: %s", mqtt_error());
}

static void send_msg(uint8_t type)
{
   peer_send(&type, 1);
}

static void stun_tick(uint64_t now)
{
   uint8_t req[20] = { 0x00, 0x01, 0x00, 0x00, 0x21, 0x12, 0xA4, 0x42 };

   if (opts.hide_ip || udp < 0 || stun_known || !stun_addr || elapsed(last_stun_us, now) < STUN_RETRY_US)
      return;

   if (stun_tries >= STUN_TRIES)
   {
      if (stun_tries++ == STUN_TRIES)
         netplay_log("stun: no answer after %u tries, only the local route will be offered", STUN_TRIES);
      return;
   }

   memcpy(req + 8, stun_tid, sizeof(stun_tid));
   udp_send(stun_addr, sceNetHtons(STUN_PORT), req, sizeof(req));

   stun_tries++;
   last_stun_us = now;
}

// binding response, rfc 5389. xor-mapped-address has the port xored with
// the top half of the magic cookie and the address with all of it; old
// servers only send the plain mapped-address, so take either
static bool stun_take(const uint8_t *p, int n)
{
   static const uint8_t cookie[4] = { 0x21, 0x12, 0xA4, 0x42 };
   int pos = 20;
   int end;

   if (n < 20 || p[0] != 0x01 || p[1] != 0x01 || memcmp(p + 4, cookie, 4) != 0 ||
         memcmp(p + 8, stun_tid, sizeof(stun_tid)) != 0)
      return false;

   end = 20 + ((p[2] << 8) | p[3]);
   if (end > n)
      end = n;

   while (pos + 4 <= end)
   {
      unsigned type = (p[pos] << 8) | p[pos + 1];
      unsigned len  = (p[pos + 2] << 8) | p[pos + 3];
      const uint8_t *v = p + pos + 4;

      if ((type == 0x0020 || type == 0x0001) && len >= 8 && pos + 4 + 8 <= end && v[1] == 0x01)
      {
         uint8_t port[2] = { v[2], v[3] };
         uint8_t ip[4]   = { v[4], v[5], v[6], v[7] };

         if (type == 0x0020)
         {
            for (int i = 0; i < 2; i++)
               port[i] ^= cookie[i];
            for (int i = 0; i < 4; i++)
               ip[i] ^= cookie[i];
         }

         memcpy(&public_port, port, 2);
         memcpy(&public_addr, ip, 4);
         stun_known = true;

         netplay_log("stun: public %s", addr_str(public_addr, public_port));
         return true;
      }

      pos += 4 + ((len + 3) & ~3u);
   }

   return true;
}

// -1 once the socket is drained or we've used up our budget for now, 0
// for anything that isn't for us. the budget keeps a flood of junk from
// eating the frame: whatever it doesn't get to, the kernel drops
static int udp_recv(void)
{
   SceNetSockaddrIn src;
   unsigned int src_len = sizeof(src);
   int n;

   if (udp < 0 || !bucket_take(&udp_bucket))
      return -1;

   n = sceNetRecvfrom(udp, rx_buf, sizeof(rx_buf), 0, (SceNetSockaddr *)&src, &src_len);
   if (n < 0)
   {
      udp_bucket.tokens += 1.0f;
      return -1;
   }

   rx_addr = src.sin_addr.s_addr;
   rx_port = src.sin_port;

   if (rx_addr == stun_addr && stun_take(rx_buf, n))
      return 0;

   return n;
}

static bool can_punch(void)
{
   return !opts.hide_ip && !peer_relay_only && udp >= 0 &&
         (peer_public_addr || peer_learned_addr);
}

static void punch_tick(uint64_t now)
{
   msg_punch_t p;
   bool eager = elapsed(punch_start_us, now) < PUNCH_EAGER_US;

   // hammer it for the first few seconds, then keep knocking once a second
   // for as long as the match runs. a vita that was busy loading the game
   // can't answer, and a route that opens up late still beats the relay
   if (direct || !session || !can_punch() ||
         elapsed(last_punch_us, now) < (eager ? PUNCH_RETRY_US : PUNCH_LAZY_US))
      return;

   if (!eager && !punch_slowed)
   {
      punch_slowed = true;
      netplay_log("punch: no direct route after %u tries, slowing down to once a second", stats.punch_tx);
   }

   p.type  = MSG_PUNCH;
   p.stamp = sceKernelGetProcessTimeLow();

   // public address, plus whatever route their own punch showed us
   if (peer_public_addr)
      send_sealed_udp(peer_public_addr, peer_public_port, &p, sizeof(p));
   if (peer_learned_addr)
      send_sealed_udp(peer_learned_addr, peer_learned_port, &p, sizeof(p));

   stats.punch_tx++;
   last_punch_us = now;
}

static void take_punch(const msg_punch_t *p)
{
   msg_punch_t ack;

   if (stats.punch_rx++ == 0)
      netplay_log("punch: first punch from %s, answering", addr_str(rx_addr, rx_port));

   // a route nobody told us about (their home address when the router
   // hairpins). it's sealed, so it really is them, and it obviously works
   // from their end, so start knocking back on it too
   if (!direct && !peer_learned_addr &&
         !(rx_addr == peer_public_addr && rx_port == peer_public_port))
   {
      peer_learned_addr = rx_addr;
      peer_learned_port = rx_port;
      last_punch_us     = 0;
      netplay_log("punch: learned a route to %s from their punch", addr_str(rx_addr, rx_port));
   }

   ack.type  = MSG_PUNCH_ACK;
   ack.stamp = p->stamp;
   send_sealed_udp(rx_addr, rx_port, &ack, sizeof(ack));
}

static void take_punch_ack(const msg_punch_t *p)
{
   unsigned rtt;

   if (direct)
      return;

   // an answer to a punch sent before the other side got busy loading
   // comes back seconds late, that's no round trip time
   rtt = (sceKernelGetProcessTimeLow() - p->stamp) / 1000;
   if (rtt < RTT_SANE_MS && (!last_rtt_ms || rtt < last_rtt_ms))
      last_rtt_ms = rtt;

   direct    = true;
   peer_addr = rx_addr;
   peer_port = rx_port;
   netplay_log("link: direct route to %s after %.1f s, rtt %u ms (using %u)",
         addr_str(peer_addr, peer_port),
         (double)elapsed(punch_start_us, now_us()) / 1000000.0, rtt, last_rtt_ms);
}

// next message from the opponent off either path, opened into msg.
// punches are handled right here and never make it up to the caller, and
// only count when they came over udp
static int peer_recv(uint8_t *msg)
{
   int n;

   while ((n = udp_recv()) >= 0)
   {
      int m;

      if (n == 0)
         continue;

      m = open_frame(rx_buf, n, msg);
      if (m <= 0)
         continue;

      stats.udp_rx++;

      if (msg[0] == MSG_PUNCH && m >= (int)sizeof(msg_punch_t))
      {
         take_punch((const msg_punch_t *)msg);
         continue;
      }

      if (msg[0] == MSG_PUNCH_ACK && m >= (int)sizeof(msg_punch_t))
      {
         take_punch_ack((const msg_punch_t *)msg);
         continue;
      }

      return m;
   }

   while (relay_head != __atomic_load_n(&relay_tail, __ATOMIC_ACQUIRE))
   {
      unsigned idx = relay_head % RELAY_QUEUE;
      int m;

      m = open_frame(relay_queue[idx], relay_len[idx], msg);
      __atomic_store_n(&relay_head, relay_head + 1, __ATOMIC_RELEASE);

      if (m <= 0 || msg[0] == MSG_PUNCH || msg[0] == MSG_PUNCH_ACK)
         continue;

      stats.relay_rx++;
      return m;
   }

   return -1;
}

static void send_hello(const char *topic, uint8_t type, const uint8_t their[KEY_SIZE],
      const void *body, unsigned body_len)
{
   uint8_t buf[sizeof(hello_head_t) + sizeof(join_body_t)];
   hello_head_t *h = (hello_head_t *)buf;
   uint8_t key[KEY_SIZE];

   if (body_len > sizeof(join_body_t))
      return;

   if (!dh_key(key, "naohac hello", their, NULL, 0))
   {
      netplay_log("hello: refusing a degenerate key");
      return;
   }

   if (!rng_bytes(h->nonce, sizeof(h->nonce)))
   {
      netplay_log("hello: hardware rng failed, not sending");
      crypto_wipe(key, sizeof(key));
      return;
   }

   h->head.magic   = PKT_MAGIC;
   h->head.version = PKT_VERSION;
   h->head.type    = type;
   memcpy(h->sender_key, box_public, KEY_SIZE);

   crypto_aead_lock(buf + sizeof(*h), h->mac, key, h->nonce,
         buf, offsetof(hello_head_t, mac), (const uint8_t *)body, body_len);
   crypto_wipe(key, sizeof(key));

   mqtt_publish(topic, buf, sizeof(*h) + body_len, false);
}

// false if it wasn't sealed for us, was tampered with, or came from a
// degenerate key
static bool open_hello(const uint8_t *payload, void *body, unsigned body_len)
{
   const hello_head_t *h = (const hello_head_t *)payload;
   uint8_t key[KEY_SIZE];
   int ret;

   if (!dh_key(key, "naohac hello", h->sender_key, NULL, 0))
      return false;

   ret = crypto_aead_unlock((uint8_t *)body, h->mac, key, h->nonce,
         payload, offsetof(hello_head_t, mac), payload + sizeof(*h), body_len);
   crypto_wipe(key, sizeof(key));

   return ret == 0;
}

// the session key both players seal with from here on. both salts and
// the session id go in, so every match gets a key of its own even between
// the same two vitas
static bool derive_link_key(void)
{
   uint8_t extra[SALT_SIZE * 2 + sizeof(session)];

   memcpy(extra, guest_salt, SALT_SIZE);
   memcpy(extra + SALT_SIZE, host_salt, SALT_SIZE);
   memcpy(extra + SALT_SIZE * 2, &session, sizeof(session));

   tx_seq = 0;
   rx_top = 0;
   memset(rx_window, 0, sizeof(rx_window));

   return dh_key(link_key, "naohac link", peer_key, extra, sizeof(extra));
}

static bool new_room_nonce(void)
{
   if (rng_bytes(room_nonce, sizeof(room_nonce)))
      return true;

   netplay_log("room: hardware rng failed");
   return false;
}

static void publish_room(unsigned players)
{
   room_msg_t m;

   memset(&m, 0, sizeof(m));
   m.body.version    = PKT_VERSION;
   m.body.players    = (uint8_t)players;
   m.body.relay_only = opts.hide_ip;
   m.body.seq        = ++room_seq;
   memcpy(m.body.room_nonce, room_nonce, SALT_SIZE);
   copy_field(m.body.game, my_game, sizeof(m.body.game));
   copy_field(m.body.name, local_name, sizeof(m.body.name));
   copy_field(m.body.rom, my_rom, sizeof(m.body.rom));
   m.body.rom_crc = my_rom_crc;
   memcpy(m.body.sign_key, sign_public, KEY_SIZE);
   memcpy(m.body.box_key, box_public, KEY_SIZE);

   crypto_eddsa_sign(m.sig, sign_secret, (const uint8_t *)&m.body, sizeof(m.body));

   mqtt_publish(room_topic, &m, sizeof(m), true);
   last_room_us = now_us();
}

// an empty retained publish is how mqtt deletes a retained message
static void clear_room(void)
{
   if (!room_open)
      return;

   mqtt_publish(room_topic, "", 0, true);
   room_open = false;
   netplay_log("room: closed");
}

static void reset_link(void)
{
   link_state        = LINK_IDLE;
   session           = 0;
   direct            = false;
   relay_head        = __atomic_load_n(&relay_tail, __ATOMIC_ACQUIRE);
   peer_id[0]        = '\0';
   peer_relay_only   = false;
   peer_public_addr  = 0;
   peer_learned_addr = 0;
   crypto_wipe(link_key, sizeof(link_key));
}

static void take_peer(void)
{
   snprintf(peer_inbox, sizeof(peer_inbox), TOPIC_ROOT "/peer/%s", peer_id);

   direct            = false;
   relay_head        = __atomic_load_n(&relay_tail, __ATOMIC_ACQUIRE);
   peer_learned_addr = 0;
   link_state        = LINK_PUNCHING;
   punch_start_us    = now_us();
   last_punch_us     = 0;
   peer_seen_us      = punch_start_us;
   last_stats_us     = punch_start_us;
   punch_slowed      = false;
   memset(&stats, 0, sizeof(stats));

   if (opts.hide_ip || peer_relay_only)
      netplay_log("punch: session %08x, %s asked for relay only, no addresses exchanged",
            (unsigned)session, opts.hide_ip ? "we" : "they");
   else
      netplay_log("punch: session %08x, opponent public %s", (unsigned)session,
            peer_public_addr ? addr_str(peer_public_addr, peer_public_port) : "(unknown)");
}

static bool share_addresses(void)
{
   return !opts.hide_ip && !peer_relay_only && stun_known;
}

static void send_accept(void)
{
   accept_body_t b;

   memset(&b, 0, sizeof(b));
   memcpy(b.guest_salt, guest_salt, SALT_SIZE);
   memcpy(b.host_salt, host_salt, SALT_SIZE);
   b.session    = session;
   b.stamp      = join_stamp;
   b.relay_only = opts.hide_ip;

   // public address only. the home address never leaves the vita
   if (share_addresses())
   {
      b.public_addr = public_addr;
      b.public_port = public_port;
   }

   send_hello(peer_inbox, PKT_ACCEPT, peer_key, &b, sizeof(b));
}

static void send_reject(const char *topic, const uint8_t their[KEY_SIZE], const uint8_t salt[SALT_SIZE],
      netplay_reject_t reason)
{
   reject_body_t b;

   memcpy(b.guest_salt, salt, SALT_SIZE);
   b.reason = (uint8_t)reason;
   send_hello(topic, PKT_REJECT, their, &b, sizeof(b));
}

static void send_join(void)
{
   join_body_t b;

   memset(&b, 0, sizeof(b));
   copy_field(b.from, my_id, sizeof(b.from));
   copy_field(b.name, local_name, sizeof(b.name));
   copy_field(b.rom, my_rom, sizeof(b.rom));
   b.rom_crc = my_rom_crc;
   memcpy(b.room_nonce, target_nonce, SALT_SIZE);
   memcpy(b.salt, guest_salt, SALT_SIZE);
   b.stamp      = join_stamp;
   b.relay_only = opts.hide_ip;

   if (share_addresses())
   {
      b.public_addr = public_addr;
      b.public_port = public_port;
   }

   send_hello(peer_inbox, PKT_JOIN, peer_key, &b, sizeof(b));
}

static void handle_join(const uint8_t *payload)
{
   const hello_head_t *h = (const hello_head_t *)payload;
   join_body_t b;
   char from[NETPLAY_ID_LEN];
   char reply[64];

   if (!is_host || (link_state != LINK_HOSTING && link_state != LINK_PUNCHING && link_state != LINK_CONNECTED))
      return;

   // every join costs an x25519 before we know anything about it, so an
   // outsider only gets to make us do a handful a second
   if (!bucket_take(&join_bucket))
   {
      stats.joins_dropped++;
      return;
   }

   if (!open_hello(payload, &b, sizeof(b)))
   {
      netplay_log("room: got a join we couldn't open, ignoring it");
      return;
   }

   copy_field(from, b.from, sizeof(from));
   if (!valid_id(from))
   {
      netplay_log("room: got a join with a bogus id, ignoring it");
      return;
   }

   snprintf(reply, sizeof(reply), TOPIC_ROOT "/peer/%s", from);

   // our guest never got the accept and is asking again
   if ((link_state == LINK_PUNCHING || link_state == LINK_CONNECTED) &&
         !memcmp(b.salt, guest_salt, SALT_SIZE) && !memcmp(h->sender_key, peer_key, KEY_SIZE))
   {
      netplay_log("room: repeat join from %s, resending accept", from);
      send_accept();
      return;
   }

   if (link_state != LINK_HOSTING)
   {
      netplay_log("room: turned away %s, already busy", from);
      send_reject(reply, h->sender_key, b.salt, NETPLAY_REJECT_FULL);
      return;
   }

   // a join for the room as it was before we reopened it, or one somebody
   // recorded and is playing back
   if (memcmp(b.room_nonce, room_nonce, SALT_SIZE) != 0)
   {
      netplay_log("room: turned away %s, join was for an older room", from);
      send_reject(reply, h->sender_key, b.salt, NETPLAY_REJECT_STALE);
      return;
   }

   if (strncmp(b.rom, my_rom, NETPLAY_ROM_LEN) != 0 || b.rom_crc != my_rom_crc)
   {
      netplay_log("room: turned away %s, rom %.23s crc %08x vs ours %s crc %08x",
            from, b.rom, (unsigned)b.rom_crc, my_rom, (unsigned)my_rom_crc);
      send_reject(reply, h->sender_key, b.salt, NETPLAY_REJECT_ROM);
      return;
   }

   if (!rng_bytes(host_salt, sizeof(host_salt)) || !rng_bytes(&session, sizeof(session)))
   {
      netplay_log("room: hardware rng failed, can't take the join");
      session = 0;
      return;
   }
   if (!session)
      session = 1;

   memcpy(peer_key, h->sender_key, KEY_SIZE);
   memcpy(guest_salt, b.salt, SALT_SIZE);
   copy_field(peer_id, from, sizeof(peer_id));
   copy_field(peer_name, b.name, sizeof(peer_name));
   make_printable(peer_name, sizeof(peer_name));

   peer_relay_only  = b.relay_only != 0;
   peer_public_addr = (opts.hide_ip || peer_relay_only) ? 0 : b.public_addr;
   peer_public_port = (opts.hide_ip || peer_relay_only) ? 0 : b.public_port;
   join_stamp       = b.stamp;
   last_rtt_ms      = 0;

   if (!derive_link_key())
   {
      netplay_log("room: refusing a degenerate key from %s", from);
      reset_link();
      link_state = LINK_HOSTING;
      return;
   }

   take_peer();
   publish_room(2);
   send_accept();

   netplay_log("room: %s joined", peer_id);
   pending_event = NETPLAY_EVENT_JOINED;
}

static void handle_accept(const uint8_t *payload)
{
   const hello_head_t *h = (const hello_head_t *)payload;
   accept_body_t b;

   // only the key that was on the room we picked can answer for it
   if (is_host || link_state != LINK_JOINING || memcmp(h->sender_key, peer_key, KEY_SIZE) != 0)
      return;

   if (!open_hello(payload, &b, sizeof(b)))
   {
      netplay_log("join: got an accept we couldn't open, ignoring it");
      return;
   }

   if (memcmp(b.guest_salt, guest_salt, SALT_SIZE) != 0 || b.stamp != join_stamp || !b.session)
      return;

   session = b.session;
   memcpy(host_salt, b.host_salt, SALT_SIZE);

   peer_relay_only  = b.relay_only != 0;
   peer_public_addr = (opts.hide_ip || peer_relay_only) ? 0 : b.public_addr;
   peer_public_port = (opts.hide_ip || peer_relay_only) ? 0 : b.public_port;
   last_rtt_ms      = (sceKernelGetProcessTimeLow() - b.stamp) / 1000;

   if (!derive_link_key())
   {
      netplay_log("join: refusing a degenerate key from the host");
      reset_link();
      pending_event = NETPLAY_EVENT_ERROR;
      return;
   }

   take_peer();
   netplay_log("join: accepted by %s, broker rtt %u ms", peer_id, last_rtt_ms);
}

static void handle_reject(const uint8_t *payload)
{
   const hello_head_t *h = (const hello_head_t *)payload;
   reject_body_t b;

   if (is_host || link_state != LINK_JOINING || memcmp(h->sender_key, peer_key, KEY_SIZE) != 0)
      return;

   if (!open_hello(payload, &b, sizeof(b)) || memcmp(b.guest_salt, guest_salt, SALT_SIZE) != 0)
      return;

   reject_reason = (netplay_reject_t)b.reason;
   netplay_log("join: rejected, reason %u", (unsigned)reject_reason);
   reset_link();
   pending_event = NETPLAY_EVENT_REJECTED;
}

static int find_room(const char *id)
{
   for (int i = 0; i < room_count; i++)
   {
      if (!strcmp(rooms[i].id, id))
         return i;
   }
   return -1;
}

static void drop_room(int i)
{
   if (i >= 0 && i < room_count)
      rooms[i] = rooms[--room_count];
}

static void room_update(const char *id, const uint8_t *payload, unsigned len)
{
   const room_msg_t *m = (const room_msg_t *)payload;
   char key_id[NETPLAY_ID_LEN];
   bool for_us;
   int i;

   if (!valid_id(id) || !strcmp(id, my_id))
      return;

   i = find_room(id);

   // a deletion. a public broker can't authenticate those, so all a griefer
   // gets out of it is a room vanishing until its host puts it back up
   if (len == 0)
   {
      drop_room(i);
      return;
   }

   if (len != sizeof(room_msg_t) || m->body.version != PKT_VERSION)
      return;

   for_us = strncmp(m->body.game, browse_game, NETPLAY_GAME_LEN) == 0;
   if (!for_us && i < 0)
      return;

   // older than what we have: a replay, or just the broker catching up
   if (i >= 0 && m->body.seq <= rooms[i].seq)
      return;

   id_from_key(key_id, m->body.sign_key);
   if (strcmp(key_id, id) != 0)
   {
      if (stats.rooms_rejected++ == 0)
         netplay_log("lobby: ignored a room signed with somebody else's key");
      return;
   }

   // signature checks are the expensive bit, an outsider only gets so many
   if (!bucket_take(&room_bucket))
      return;

   if (crypto_eddsa_check(m->sig, m->body.sign_key, (const uint8_t *)&m->body, sizeof(m->body)) != 0)
   {
      if (stats.rooms_rejected++ == 0)
         netplay_log("lobby: ignored a room with a bad signature");
      return;
   }

   if (!for_us)
   {
      drop_room(i);
      return;
   }

   if (i < 0)
   {
      if (room_count == NETPLAY_MAX_ROOMS)
         return;
      i = room_count++;
   }

   copy_field(rooms[i].id, id, sizeof(rooms[i].id));
   copy_field(rooms[i].name, m->body.name, sizeof(rooms[i].name));
   copy_field(rooms[i].rom, m->body.rom, sizeof(rooms[i].rom));
   make_printable(rooms[i].name, sizeof(rooms[i].name));
   make_printable(rooms[i].rom, sizeof(rooms[i].rom));
   rooms[i].rom_crc    = m->body.rom_crc;
   rooms[i].players    = (m->body.players >= 2) ? 2 : 1;
   rooms[i].relay_only = m->body.relay_only != 0;
   rooms[i].seq        = m->body.seq;
   memcpy(rooms[i].box_key, m->body.box_key, KEY_SIZE);
   memcpy(rooms[i].room_nonce, m->body.room_nonce, SALT_SIZE);
}

// game traffic coming the long way round. it gets opened (and checked)
// when it's pulled off the queue, this only weeds out the obvious junk
static void relay_take(const uint8_t *payload, unsigned len, uint32_t want)
{
   const sealed_head_t *sh = (const sealed_head_t *)payload;
   unsigned tail = relay_tail;

   if (len <= sizeof(sealed_head_t) || len > SEALED_MAX || !want || sh->session != want)
      return;

   if (tail - __atomic_load_n(&relay_head, __ATOMIC_ACQUIRE) >= RELAY_QUEUE)
   {
      __atomic_fetch_add(&stats.relay_dropped, 1, __ATOMIC_RELAXED);
      return;
   }

   memcpy(relay_queue[tail % RELAY_QUEUE], payload, len);
   relay_len[tail % RELAY_QUEUE] = (uint16_t)len;
   __atomic_store_n(&relay_tail, tail + 1, __ATOMIC_RELEASE);
}

static void on_lobby_message(const char *topic, const uint8_t *payload, unsigned len)
{
   static const char rooms_prefix[] = TOPIC_ROOT "/rooms/";
   const pkt_head_t *h = (const pkt_head_t *)payload;

   if (!strncmp(topic, rooms_prefix, sizeof(rooms_prefix) - 1))
   {
      room_update(topic + sizeof(rooms_prefix) - 1, payload, len);
      return;
   }

   if (strcmp(topic, inbox_topic) != 0 || len < sizeof(pkt_head_t) || len > SEALED_MAX ||
         h->magic != PKT_MAGIC || h->version != PKT_VERSION)
      return;

   switch (h->type)
   {
      case PKT_SEALED:
         relay_take(payload, len, session);
         return;

      case PKT_JOIN:
         if (len == sizeof(hello_head_t) + sizeof(join_body_t))
            handle_join(payload);
         return;

      case PKT_ACCEPT:
         if (len == sizeof(hello_head_t) + sizeof(accept_body_t))
            handle_accept(payload);
         return;

      case PKT_REJECT:
         if (len == sizeof(hello_head_t) + sizeof(reject_body_t))
            handle_reject(payload);
         return;

      default:
         return;
   }
}

// the relay thread's view of the broker while a match runs: only game
// traffic for us. rooms and joins can wait, the room says 2/2 anyway and
// browsing resubscribes afterwards
static void on_relay_message(const char *topic, const uint8_t *payload, unsigned len)
{
   const pkt_head_t *h = (const pkt_head_t *)payload;

   if (strcmp(topic, inbox_topic) != 0 || len < sizeof(pkt_head_t) || len > SEALED_MAX ||
         h->magic != PKT_MAGIC || h->version != PKT_VERSION || h->type != PKT_SEALED)
      return;

   relay_take(payload, len, relay_session);
}

// the lobby thread builds the whole tls session and only then flips this
// to online, so reading it has to see everything written before the flip
static netplay_lobby_t lobby_now(void)
{
   return __atomic_load_n(&lobby_state, __ATOMIC_ACQUIRE);
}

// keeps the broker connection and stun going, false if the lobby's gone
static bool pump_lobby(void)
{
   stun_tick(now_us());

   if (lobby_now() != NETPLAY_LOBBY_ONLINE)
      return false;

   if (!mqtt_poll(on_lobby_message))
   {
      netplay_log("lobby: connection dropped: %s", mqtt_error());
      snprintf(lobby_error, sizeof(lobby_error), "%s", mqtt_error());
      lobby_state = NETPLAY_LOBBY_FAILED;
      room_open = false;
      return false;
   }

   return true;
}

// owns the broker connection for the length of a match: sends what
// peer_send queued, reads the inbox, answers keepalives. it runs on a
// core of its own, so tls and a slow broker cost the game nothing. the
// game thread doesn't touch mqtt again until relay_stop() has joined it
static int relay_thread_func(SceSize args, void *argp)
{
   (void)args;
   (void)argp;

   for (;;)
   {
      // quit is read before the queue, so whatever got queued before the
      // game thread asked us to stop still goes out
      bool quit = __atomic_load_n(&relay_quit, __ATOMIC_ACQUIRE);
      unsigned tail = __atomic_load_n(&relay_out_tail, __ATOMIC_ACQUIRE);

      while (relay_out_head != tail)
      {
         unsigned idx = relay_out_head % RELAY_OUT_QUEUE;
         uint64_t start = now_us();
         unsigned ms;

         if (!mqtt_publish(peer_inbox, relay_out[idx], relay_out_len[idx], false))
         {
            __atomic_store_n(&relay_lost, true, __ATOMIC_RELEASE);
            return 0;
         }

         ms = (unsigned)(elapsed(start, now_us()) / 1000);
         if (ms > __atomic_load_n(&stats.relay_worst_ms, __ATOMIC_RELAXED))
            __atomic_store_n(&stats.relay_worst_ms, ms, __ATOMIC_RELAXED);
         __atomic_fetch_add(&stats.relay_tx, 1, __ATOMIC_RELAXED);
         __atomic_store_n(&relay_out_head, relay_out_head + 1, __ATOMIC_RELEASE);
      }

      if (quit)
         return 0;

      if (!mqtt_poll(on_relay_message))
      {
         __atomic_store_n(&relay_lost, true, __ATOMIC_RELEASE);
         return 0;
      }

      sceKernelDelayThread(1000);
   }
}

static void relay_start(void)
{
   int main_mask = sceKernelGetThreadCpuAffinityMask(sceKernelGetThreadId());
   int mask = SCE_KERNEL_CPU_MASK_USER_2;

   if (relay_thread >= 0 || lobby_now() != NETPLAY_LOBBY_ONLINE || !mqtt_is_open())
      return;

   // any core but the game's
   if (main_mask == SCE_KERNEL_CPU_MASK_USER_2)
      mask = SCE_KERNEL_CPU_MASK_USER_1;

   relay_out_head = 0;
   relay_out_tail = 0;
   relay_session  = session;
   relay_quit     = false;
   relay_lost     = false;

   relay_thread = sceKernelCreateThread("naohac_relay", relay_thread_func, 0x10000100,
         RELAY_THREAD_STACK, 0, mask, NULL);
   if (relay_thread >= 0 && sceKernelStartThread(relay_thread, 0, NULL) < 0)
   {
      sceKernelDeleteThread(relay_thread);
      relay_thread = -1;
   }

   if (relay_thread < 0)
      netplay_log("relay: no thread for the broker, it stays on the game thread");
   else
      netplay_log("relay: broker moved to its own thread (game thread mask 0x%X, relay 0x%X)",
            (unsigned)main_mask, (unsigned)mask);
}

static void relay_stop(void)
{
   if (relay_thread < 0)
      return;

   __atomic_store_n(&relay_quit, true, __ATOMIC_RELEASE);
   sceKernelWaitThreadEnd(relay_thread, NULL, NULL);
   sceKernelDeleteThread(relay_thread);
   relay_thread = -1;

   if (relay_lost)
   {
      netplay_log("lobby: connection dropped: %s", mqtt_error());
      snprintf(lobby_error, sizeof(lobby_error), "%s", mqtt_error());
      lobby_state = NETPLAY_LOBBY_FAILED;
      room_open = false;
   }
}

static int lobby_thread_func(SceSize args, void *argp)
{
   char client_id[48];
   uint32_t addr;

   (void)args;
   (void)argp;

   // nobody asks google where we are when the ip is meant to stay hidden
   if (!opts.hide_ip && resolve(STUN_HOST, &addr))
   {
      stun_addr = addr;
      netplay_log("dns: %s -> %s", STUN_HOST, server_addr_str(addr, sceNetHtons(STUN_PORT)));
   }

   snprintf(client_id, sizeof(client_id), "naohac-%s", my_id);

   for (unsigned i = 0; i < BROKER_COUNT && !lobby_abort; i++)
   {
      if (!resolve(brokers[i].host, &addr) || lobby_abort)
         continue;

      netplay_log("dns: %s -> %s", brokers[i].host, server_addr_str(addr, sceNetHtons(brokers[i].port)));

      if (mqtt_open(brokers[i].host, addr, brokers[i].port, client_id, room_topic, LOBBY_KEEPALIVE_S))
      {
         if (mqtt_subscribe(inbox_topic) && mqtt_subscribe(ROOMS_FILTER))
         {
            netplay_log("lobby: connected to %s over tls", brokers[i].host);
            __atomic_store_n(&lobby_state, NETPLAY_LOBBY_ONLINE, __ATOMIC_RELEASE);
            return 0;
         }

         mqtt_close();
      }

      netplay_log("lobby: couldn't use %s: %s", brokers[i].host, mqtt_error());
      snprintf(lobby_error, sizeof(lobby_error), "%s", mqtt_error());
   }

   __atomic_store_n(&lobby_state, NETPLAY_LOBBY_FAILED, __ATOMIC_RELEASE);
   return 0;
}

static bool pick_nickname(void)
{
   static const char *const adjectives[16] = {
      "Swift", "Brave", "Silent", "Mighty", "Lucky", "Crimson", "Golden", "Shadow",
      "Turbo", "Iron", "Wild", "Cosmic", "Rapid", "Atomic", "Electric", "Frozen"
   };
   static const char *const animals[16] = {
      "Tiger", "Falcon", "Dragon", "Wolf", "Phoenix", "Cobra", "Panther", "Hawk",
      "Viper", "Rhino", "Shark", "Raven", "Lynx", "Bison", "Mantis", "Kraken"
   };
   uint8_t r[3];

   if (!rng_bytes(r, sizeof(r)))
      return false;

   snprintf(local_name, sizeof(local_name), "%s %s %02u",
         adjectives[r[0] & 15], animals[r[1] & 15], (unsigned)(r[2] % 100));
   return true;
}

static void load_psn_name(void)
{
   if (!apputil_up)
   {
      SceAppUtilInitParam init;
      SceAppUtilBootParam boot;

      memset(&init, 0, sizeof(init));
      memset(&boot, 0, sizeof(boot));
      apputil_up = sceAppUtilInit(&init, &boot) >= 0;
   }

   memset(local_name, 0, sizeof(local_name));

   if (!apputil_up ||
         sceAppUtilSystemParamGetString(SCE_SYSTEM_PARAM_ID_USERNAME,
            (SceChar8 *)local_name, sizeof(local_name)) < 0 || !local_name[0])
      snprintf(local_name, sizeof(local_name), "PS Vita");

   make_printable(local_name, sizeof(local_name));
}

static bool open_udp(void)
{
   SceNetSockaddrIn addr;
   unsigned int addr_len = sizeof(addr);
   int on = 1;
   int rcvbuf = 256 * 1024;

   udp = sceNetSocket("naohac_netplay", SCE_NET_AF_INET, SCE_NET_SOCK_DGRAM, SCE_NET_IPPROTO_UDP);
   if (udp < 0)
   {
      init_error = udp;
      netplay_log("init: udp socket 0x%08X", udp);
      return false;
   }

   sceNetSetsockopt(udp, SCE_NET_SOL_SOCKET, SCE_NET_SO_NBIO, &on, sizeof(on));
   sceNetSetsockopt(udp, SCE_NET_SOL_SOCKET, SCE_NET_SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));

   memset(&addr, 0, sizeof(addr));
   addr.sin_len         = sizeof(addr);
   addr.sin_family      = SCE_NET_AF_INET;
   addr.sin_port        = sceNetHtons(NETPLAY_PORT);
   addr.sin_addr.s_addr = SCE_NET_INADDR_ANY;

   // any free port is fine if ours is taken, nobody types it in anymore
   if (sceNetBind(udp, (SceNetSockaddr *)&addr, sizeof(addr)) < 0)
   {
      addr.sin_port = 0;
      if (sceNetBind(udp, (SceNetSockaddr *)&addr, sizeof(addr)) < 0)
      {
         init_error = -1;
         netplay_log("init: bind failed");
         sceNetSocketClose(udp);
         udp = -1;
         return false;
      }
   }

   local_port = addr.sin_port;
   if (sceNetGetsockname(udp, (SceNetSockaddr *)&addr, &addr_len) >= 0)
      local_port = addr.sin_port;

   return true;
}

// fresh identity every time online play opens, nothing is ever stored
static bool make_keys(void)
{
   uint8_t seed[KEY_SIZE];

   if (!rng_bytes(seed, sizeof(seed)) || !rng_bytes(box_secret, sizeof(box_secret)) ||
         !rng_bytes(stun_tid, sizeof(stun_tid)))
   {
      crypto_wipe(seed, sizeof(seed));
      return false;
   }

   crypto_eddsa_key_pair(sign_secret, sign_public, seed);
   crypto_x25519_public_key(box_public, box_secret);
   id_from_key(my_id, sign_public);
   return true;
}

static void wipe_keys(void)
{
   crypto_wipe(sign_secret, sizeof(sign_secret));
   crypto_wipe(box_secret, sizeof(box_secret));
   crypto_wipe(link_key, sizeof(link_key));
}

netplay_init_t netplay_init(const netplay_options_t *opt)
{
   SceNetInitParam param;
   SceNetCtlInfo info;
   int ret;

   if (net_up)
      return NETPLAY_INIT_OK;

   opts = *opt;
   init_error = 0;
   lobby_error[0] = '\0';
   netplay_log("--- online play --- (%s name, %s)", opts.use_psn_name ? "psn" : "random",
         opts.hide_ip ? "ip hidden, relay only" : "direct when possible");

   if (!make_keys() || !(opts.use_psn_name || pick_nickname()))
   {
      netplay_log("init: hardware rng failed, online play stays off");
      wipe_keys();
      return NETPLAY_INIT_FAILED;
   }

   if (opts.use_psn_name)
      load_psn_name();

   ret = sceSysmoduleLoadModule(SCE_SYSMODULE_NET);
   if (ret < 0)
      netplay_log("init: net module 0x%08X", ret);

   param.memory = net_memory;
   param.size   = sizeof(net_memory);
   param.flags  = 0;

   // fine if something else in the process brought the stack up first
   ret = sceNetInit(&param);
   net_inited_here = ret >= 0;
   if (ret < 0 && sceNetShowNetstat() < 0)
   {
      init_error = ret;
      netplay_log("init: sceNetInit 0x%08X", ret);
      wipe_keys();
      return NETPLAY_INIT_FAILED;
   }

   // same deal, only an error here if it was already running
   ret = sceNetCtlInit();
   if (ret < 0)
      netplay_log("init: sceNetCtlInit 0x%08X", ret);

   memset(&info, 0, sizeof(info));
   ret = sceNetCtlInetGetInfo(SCE_NETCTL_INFO_GET_IP_ADDRESS, &info);
   if (ret < 0 || !parse_ipv4(info.ip_address, &local_addr) || local_addr == 0)
   {
      init_error = ret;
      netplay_log("init: no ip address (0x%08X)", ret);
      sceNetCtlTerm();
      if (net_inited_here)
         sceNetTerm();
      wipe_keys();
      return NETPLAY_INIT_NO_WIFI;
   }

   netplay_log("init: local ip %s", format_addr(local_addr, 0, true));

   // relay-only never needs a udp socket at all
   udp = -1;
   if (!opts.hide_ip && !open_udp())
   {
      sceNetCtlTerm();
      if (net_inited_here)
         sceNetTerm();
      wipe_keys();
      return NETPLAY_INIT_FAILED;
   }

   snprintf(inbox_topic, sizeof(inbox_topic), TOPIC_ROOT "/peer/%s", my_id);
   snprintf(room_topic, sizeof(room_topic), TOPIC_ROOT "/rooms/%s", my_id);

   bucket_init(&udp_bucket, UDP_PER_SECOND, UDP_BURST);
   bucket_init(&join_bucket, JOINS_PER_SECOND, JOIN_BURST);
   bucket_init(&room_bucket, ROOM_CHECKS_PER_S, ROOM_CHECK_BURST);

   stun_addr      = 0;
   stun_known     = false;
   stun_tries     = 0;
   last_stun_us   = 0;
   room_count     = 0;
   room_open      = false;
   room_seq       = 0;
   browse_game[0] = '\0';
   is_host        = false;
   reset_link();

   if (udp >= 0)
      netplay_log("init: id %s, udp port %u", my_id, (unsigned)sceNetNtohs(local_port));
   else
      netplay_log("init: id %s, no udp", my_id);

   lobby_state = NETPLAY_LOBBY_CONNECTING;
   lobby_abort = false;
   lobby_thread = sceKernelCreateThread("naohac_lobby", lobby_thread_func, 0x10000100,
         LOBBY_THREAD_STACK, 0, 0, NULL);
   if (lobby_thread < 0 || sceKernelStartThread(lobby_thread, 0, NULL) < 0)
      lobby_state = NETPLAY_LOBBY_FAILED;

   net_up = true;
   return NETPLAY_INIT_OK;
}

int netplay_init_error(void)
{
   return init_error;
}

const char *netplay_lobby_error(void)
{
   return lobby_error;
}

const char *netplay_local_name(void)
{
   return local_name;
}

bool netplay_hiding_ip(void)
{
   return opts.hide_ip;
}

void netplay_shutdown(void)
{
   if (!net_up)
      return;

   netplay_log("shutdown: leaving online play");
   netplay_close();

   if (lobby_thread >= 0)
   {
      int rid = lobby_resolver;

      lobby_abort = true;
      if (rid >= 0)
         sceNetResolverAbort(rid, 0);
      if (lobby_now() == NETPLAY_LOBBY_CONNECTING)
         mqtt_abort();

      sceKernelWaitThreadEnd(lobby_thread, NULL, NULL);
      sceKernelDeleteThread(lobby_thread);
      lobby_thread = -1;
   }

   mqtt_close();
   lobby_state = NETPLAY_LOBBY_FAILED;

   if (udp >= 0)
      sceNetSocketClose(udp);
   udp = -1;

   sceNetCtlTerm();
   if (net_inited_here)
      sceNetTerm();

   wipe_keys();
   net_up = false;
}

netplay_lobby_t netplay_lobby_state(void)
{
   if (lobby_now() == NETPLAY_LOBBY_ONLINE)
      pump_lobby();
   return lobby_now();
}

void netplay_browse(const char *game)
{
   memset(browse_game, 0, sizeof(browse_game));
   snprintf(browse_game, sizeof(browse_game), "%s", game);
   room_count = 0;
   netplay_log("lobby: browsing rooms for %s", browse_game);

   // retained rooms only get delivered on subscribe, so resubscribing is
   // how we get the full list again for a different game
   if (lobby_now() == NETPLAY_LOBBY_ONLINE)
   {
      mqtt_unsubscribe(ROOMS_FILTER);
      mqtt_subscribe(ROOMS_FILTER);
   }
}

int netplay_rooms(netplay_room_t *out, int max)
{
   if (max > room_count)
      max = room_count;

   memcpy(out, rooms, sizeof(netplay_room_t) * max);
   return max;
}

void netplay_close(void)
{
   relay_stop();

   if (session && (link_state == LINK_PUNCHING || link_state == LINK_CONNECTED))
   {
      netplay_log("link: closing, telling the opponent (udp tx %u rx %u, relay tx %u rx %u)",
            stats.udp_tx, stats.udp_rx, stats.relay_tx, stats.relay_rx);

      for (int i = 0; i < 3; i++)
         send_msg(MSG_QUIT);
   }

   clear_room();
   reset_link();
   pending_event = NETPLAY_EVENT_NONE;
   reject_reason = NETPLAY_REJECT_NONE;
}

void netplay_create_room(const char *game, const char *rom, uint32_t rom_crc)
{
   netplay_close();

   memset(my_game, 0, sizeof(my_game));
   memset(my_rom, 0, sizeof(my_rom));
   snprintf(my_game, sizeof(my_game), "%s", game);
   snprintf(my_rom, sizeof(my_rom), "%s", rom);
   my_rom_crc = rom_crc;
   is_host    = true;

   if (!new_room_nonce())
   {
      pending_event = NETPLAY_EVENT_ERROR;
      return;
   }

   link_state = LINK_HOSTING;
   publish_room(1);
   room_open = true;

   netplay_log("room: opened for %s with %s (crc %08x)", my_game, my_rom, (unsigned)my_rom_crc);
}

void netplay_join_room(const netplay_room_t *room, uint32_t rom_crc)
{
   netplay_close();

   is_host = false;

   if (!valid_id(room->id) || !rng_bytes(guest_salt, sizeof(guest_salt)))
   {
      netplay_log("join: can't join that room");
      pending_event = NETPLAY_EVENT_ERROR;
      return;
   }

   memset(my_rom, 0, sizeof(my_rom));
   snprintf(my_rom, sizeof(my_rom), "%s", room->rom);
   my_rom_crc = rom_crc;

   copy_field(peer_id, room->id, sizeof(peer_id));
   copy_field(peer_name, room->name, sizeof(peer_name));
   snprintf(peer_inbox, sizeof(peer_inbox), TOPIC_ROOT "/peer/%s", peer_id);
   memcpy(peer_key, room->box_key, KEY_SIZE);
   memcpy(target_nonce, room->room_nonce, SALT_SIZE);
   peer_relay_only = room->relay_only;

   link_state    = LINK_JOINING;
   join_start_us = now_us();
   last_join_us  = 0;
   join_stamp    = sceKernelGetProcessTimeLow();

   netplay_log("join: knocking on %s with %s crc %08x", peer_id, my_rom, (unsigned)my_rom_crc);
}

netplay_event_t netplay_lobby_poll(void)
{
   uint8_t msg[SEALED_MAX];
   netplay_event_t ev;
   uint64_t now;
   int n;

   if (!pump_lobby())
      return NETPLAY_EVENT_LOBBY_LOST;

   now = now_us();

   while ((n = peer_recv(msg)) > 0)
   {
      if (msg[0] != MSG_QUIT)
         continue;

      netplay_log("link: opponent left before the match started");
      reset_link();

      // guest bailed before the match got going, put the room back up as a
      // fresh one so the old join can't be replayed against it
      if (is_host && room_open && new_room_nonce())
      {
         link_state = LINK_HOSTING;
         publish_room(1);
      }
      return NETPLAY_EVENT_PEER_LEFT;
   }

   ev = pending_event;
   pending_event = NETPLAY_EVENT_NONE;
   if (ev != NETPLAY_EVENT_NONE)
      return ev;

   // anyone on a public broker can wipe a retained message, so a waiting
   // room quietly puts itself back up every so often
   if (link_state == LINK_HOSTING && room_open && elapsed(last_room_us, now) >= ROOM_REFRESH_US)
      publish_room(1);

   if (link_state == LINK_JOINING)
   {
      if (elapsed(join_start_us, now) >= JOIN_TIMEOUT_US)
      {
         char topic[64];

         netplay_log("join: no answer from %s in %u s, clearing the stale room",
               peer_id, (unsigned)(JOIN_TIMEOUT_US / 1000000ULL));

         // nobody home, almost certainly a room left behind by a vita that
         // dropped off. take it down so the next person doesn't trip on it
         snprintf(topic, sizeof(topic), TOPIC_ROOT "/rooms/%s", peer_id);
         mqtt_publish(topic, "", 0, true);

         reset_link();
         return NETPLAY_EVENT_TIMEOUT;
      }

      if (elapsed(last_join_us, now) >= JOIN_RETRY_US)
      {
         send_join();
         last_join_us = now;
      }
   }

   if (link_state == LINK_PUNCHING)
   {
      punch_tick(now);

      if (direct || !can_punch() || elapsed(punch_start_us, now) >= PUNCH_WAIT_US)
      {
         if (!direct)
            netplay_log("link: starting on the relay (%u punches out, %u in)%s",
                  stats.punch_tx, stats.punch_rx, can_punch() ? ", still trying direct in the background" : "");

         link_state = LINK_CONNECTED;
         return NETPLAY_EVENT_CONNECTED;
      }
   }

   return NETPLAY_EVENT_NONE;
}

netplay_reject_t netplay_reject_reason(void)
{
   return reject_reason;
}

const char *netplay_peer_name(void)
{
   return peer_name;
}

bool netplay_is_relayed(void)
{
   return !direct;
}

unsigned netplay_suggested_delay(void)
{
   // one-way trip rounded up to whole frames, plus one so a frame's input
   // is usually already sitting there by the time it's needed
   unsigned delay = (last_rtt_ms / 2 + 16) / 17 + 1;

   if (delay < 1)
      delay = 1;
   if (delay > MAX_DELAY)
      delay = MAX_DELAY;
   return delay;
}

void netplay_digest(const void *data, size_t len, uint8_t out[NETPLAY_DIGEST_SIZE])
{
   crypto_blake2b(out, NETPLAY_DIGEST_SIZE, (const uint8_t *)data, len);
}

netplay_event_t netplay_sync_host(uint32_t size, const uint8_t digest[NETPLAY_DIGEST_SIZE],
      unsigned *delay, void (*idle)(void))
{
   uint8_t msg[SEALED_MAX];
   msg_state_t info;
   uint64_t start_us = now_us();
   uint64_t last_info_us = 0;
   char hex[17];

   info.type = MSG_STATE_INFO;
   info.size = size;
   memcpy(info.digest, digest, NETPLAY_DIGEST_SIZE);

   hex_prefix(hex, digest, 8);
   netplay_log("sync: our machine is %u bytes, hash %s..., over %s", (unsigned)size, hex,
         direct ? "udp" : "the relay");

   // the other side spent the last few seconds booting the core too, give
   // it a lot more slack here than once the match is actually running
   peer_seen_us = start_us;

   for (;;)
   {
      uint64_t now;
      int n;

      pump_lobby();
      now = now_us();
      punch_tick(now);

      while ((n = peer_recv(msg)) > 0)
      {
         if (msg[0] == MSG_READY && n >= (int)sizeof(msg_ready_t))
         {
            unsigned d = ((const msg_ready_t *)msg)->delay;

            *delay = (d < 1) ? 1 : (d > MAX_DELAY) ? MAX_DELAY : d;
            netplay_log("sync: guest matches us, ready after %u ms, input delay %u, over %s",
                  (unsigned)(elapsed(start_us, now) / 1000), *delay, direct ? "udp" : "the relay");
            return NETPLAY_EVENT_NONE;
         }

         if (msg[0] == MSG_MISMATCH)
         {
            netplay_log("sync: guest booted into a different machine than ours, calling it off");
            reset_link();
            return NETPLAY_EVENT_ERROR;
         }

         if (msg[0] == MSG_QUIT)
         {
            netplay_log("sync: guest left");
            reset_link();
            return NETPLAY_EVENT_PEER_LEFT;
         }
      }

      if (elapsed(peer_seen_us, now) >= SYNC_TIMEOUT_US)
      {
         netplay_log("sync: nothing from the guest for %u s, giving up",
               (unsigned)(SYNC_TIMEOUT_US / 1000000ULL));
         reset_link();
         return NETPLAY_EVENT_TIMEOUT;
      }

      if (elapsed(last_info_us, now) >= INFO_RETRY_US)
      {
         peer_send(&info, sizeof(info));
         last_info_us = now;
      }

      if (idle)
         idle();
   }
}

netplay_event_t netplay_sync_guest(uint32_t size, const uint8_t digest[NETPLAY_DIGEST_SIZE],
      void (*idle)(void))
{
   uint8_t msg[SEALED_MAX];
   uint64_t start_us = now_us();
   char ours[17], theirs[17];

   hex_prefix(ours, digest, 8);
   netplay_log("sync: our machine is %u bytes, hash %s..., waiting for the host's", (unsigned)size, ours);

   peer_seen_us = start_us;

   for (;;)
   {
      uint64_t now;
      int n;

      pump_lobby();
      now = now_us();
      punch_tick(now);

      while ((n = peer_recv(msg)) > 0)
      {
         if (msg[0] == MSG_STATE_INFO && n >= (int)sizeof(msg_state_t))
         {
            const msg_state_t *info = (const msg_state_t *)msg;

            hex_prefix(theirs, info->digest, 8);

            if (info->size == size && !memcmp(info->digest, digest, NETPLAY_DIGEST_SIZE))
            {
               netplay_log("sync: host's machine matches ours after %u ms, over %s",
                     (unsigned)(elapsed(start_us, now) / 1000), direct ? "udp" : "the relay");
               return NETPLAY_EVENT_NONE;
            }

            netplay_log("sync: host's machine is %u bytes %s..., ours %u bytes %s..., calling it off",
                  (unsigned)info->size, theirs, (unsigned)size, ours);
            for (int i = 0; i < 3; i++)
               send_msg(MSG_MISMATCH);
            reset_link();
            return NETPLAY_EVENT_ERROR;
         }

         if (msg[0] == MSG_QUIT)
         {
            netplay_log("sync: host left");
            reset_link();
            return NETPLAY_EVENT_PEER_LEFT;
         }
      }

      if (elapsed(peer_seen_us, now) >= SYNC_TIMEOUT_US)
      {
         netplay_log("sync: nothing from the host for %u s, giving up",
               (unsigned)(SYNC_TIMEOUT_US / 1000000ULL));
         reset_link();
         return NETPLAY_EVENT_TIMEOUT;
      }

      if (idle)
         idle();
   }
}

static void send_ready(void)
{
   msg_ready_t p;

   p.type  = MSG_READY;
   p.delay = (uint8_t)match_delay;
   peer_send(&p, sizeof(p));
   last_ready_us = now_us();
}

static void send_inputs(void)
{
   msg_input_t p;
   uint32_t count, first;

   if (local_next == 0)
      return;

   count = (local_next < INPUT_HISTORY) ? local_next : INPUT_HISTORY;
   first = local_next - count;

   p.type  = MSG_INPUT;
   p.frame = local_next - 1;
   p.count = (uint8_t)count;

   for (uint32_t i = 0; i < count; i++)
      p.bits[i] = local_ring[(first + i) % INPUT_RING];

   peer_send(&p, offsetof(msg_input_t, bits) + count * sizeof(uint16_t));
   last_input_us = now_us();
}

void netplay_start(unsigned delay)
{
   memset(local_ring, 0, sizeof(local_ring));
   memset(remote_ring, 0, sizeof(remote_ring));

   // our first `delay` frames are blank, that's the head start the other
   // side gets before it ever has to wait on us
   match_delay   = delay;
   local_next    = delay;
   remote_next   = 0;
   cur_frame     = 0;
   last_input_us = 0;
   last_push_us  = 0;
   wait_since_us = 0;
   frame_wait_us = 0;
   peer_seen_us  = now_us();
   last_stats_us = peer_seen_us;

   netplay_log("match: started as %s, input delay %u, over %s", is_host ? "host (p1)" : "guest (p2)",
         delay, direct ? "udp" : "the relay");

   relay_start();

   if (!is_host)
      send_ready();
}

static void take_inputs(const msg_input_t *p, int len)
{
   uint32_t last = p->frame;
   uint32_t first;

   if (p->count == 0 || p->count > INPUT_HISTORY ||
         len < (int)(offsetof(msg_input_t, bits) + p->count * sizeof(uint16_t)))
      return;

   first = last + 1 - p->count;

   // only useful if it picks up exactly where our contiguous run ends
   if (first > remote_next || last < remote_next || last - cur_frame >= INPUT_RING)
      return;

   for (uint32_t f = remote_next; f <= last; f++)
      remote_ring[f % INPUT_RING] = p->bits[f - first];

   remote_next = last + 1;
}

static void log_match_stats(const char *why)
{
   netplay_log("match: %s - frame %u, have their input up to %u, ours up to %u, over %s, "
         "udp tx %u rx %u, relay tx %u rx %u dropped %u unsent %u worst send %u ms, "
         "junk %u forged %u replayed %u, last heard %u ms ago, "
         "waited on them %u times %u ms (worst %u ms), slow frames %u",
         why, (unsigned)cur_frame, (unsigned)remote_next, (unsigned)local_next,
         direct ? "udp" : "the relay", stats.udp_tx, stats.udp_rx,
         __atomic_load_n(&stats.relay_tx, __ATOMIC_RELAXED), stats.relay_rx,
         __atomic_load_n(&stats.relay_dropped, __ATOMIC_RELAXED), stats.relay_send_failed,
         __atomic_load_n(&stats.relay_worst_ms, __ATOMIC_RELAXED),
         stats.junk, stats.forged, stats.replayed,
         (unsigned)(elapsed(peer_seen_us, now_us()) / 1000),
         stats.waits, stats.wait_ms, stats.worst_wait_ms, stats.slow_frames);
}

netplay_event_t netplay_poll(void)
{
   uint8_t msg[SEALED_MAX];
   uint64_t now;
   int n;

   // a dead broker only matters if it was carrying the game for us
   if (relay_thread >= 0)
      stun_tick(now_us());

   if ((relay_thread >= 0 ? __atomic_load_n(&relay_lost, __ATOMIC_ACQUIRE) : !pump_lobby()) && !direct)
   {
      log_match_stats("lobby connection died while relaying");
      reset_link();
      return NETPLAY_EVENT_TIMEOUT;
   }

   now = now_us();
   punch_tick(now);

   while ((n = peer_recv(msg)) > 0)
   {
      if (msg[0] == MSG_INPUT)
         take_inputs((const msg_input_t *)msg, n);
      else if (msg[0] == MSG_QUIT)
      {
         log_match_stats("opponent left");
         reset_link();
         return NETPLAY_EVENT_PEER_LEFT;
      }
   }

   // only sealed packets from the opponent count as hearing from them, so
   // junk can't keep a dead match looking alive
   if (elapsed(peer_seen_us, now) >= LINK_TIMEOUT_US)
   {
      log_match_stats("opponent went silent");
      reset_link();
      return NETPLAY_EVENT_TIMEOUT;
   }

   if (elapsed(last_stats_us, now) >= STATS_EVERY_US)
   {
      log_match_stats("still going");
      last_stats_us = now;
   }

   // keep saying ready until the host's first inputs show up
   if (!is_host && remote_next == 0 && elapsed(last_ready_us, now) >= READY_RESEND_US)
      send_ready();

   if (elapsed(last_input_us, now) >= (direct ? INPUT_RESEND_US : RELAY_RESEND_US))
      send_inputs();

   return NETPLAY_EVENT_NONE;
}

void netplay_push_input(uint16_t bits)
{
   uint64_t now = now_us();

   // a frame that took too long with the waiting taken out is this vita
   // falling behind on its own, not the network
   if (last_push_us && elapsed(last_push_us, now) - frame_wait_us >= SLOW_FRAME_US)
      stats.slow_frames++;
   last_push_us  = now;
   frame_wait_us = 0;

   local_ring[local_next % INPUT_RING] = bits;
   local_next++;
   send_inputs();
}

bool netplay_frame_ready(void)
{
   bool ready = remote_next > cur_frame;

   if (!ready && !wait_since_us)
      wait_since_us = now_us();
   else if (ready && wait_since_us)
   {
      uint64_t waited = elapsed(wait_since_us, now_us());
      unsigned ms = (unsigned)(waited / 1000);

      frame_wait_us += waited;
      stats.waits++;
      stats.wait_ms += ms;
      if (ms > stats.worst_wait_ms)
         stats.worst_wait_ms = ms;
      wait_since_us = 0;
   }

   return ready;
}

uint16_t netplay_input(unsigned port)
{
   uint16_t mine   = local_ring[cur_frame % INPUT_RING];
   uint16_t theirs = remote_ring[cur_frame % INPUT_RING];

   // host always sits on the p1 side of the cabinet, guest on p2
   if (port == 0)
      return is_host ? mine : theirs;
   if (port == 1)
      return is_host ? theirs : mine;
   return 0;
}

void netplay_advance(void)
{
   cur_frame++;
}
