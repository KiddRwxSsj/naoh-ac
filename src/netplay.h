#ifndef NETPLAY_H
#define NETPLAY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define NETPLAY_ID_LEN      32
#define NETPLAY_GAME_LEN    16
#define NETPLAY_NAME_LEN    24
#define NETPLAY_ROM_LEN     24
#define NETPLAY_MAX_ROOMS   16
#define NETPLAY_KEY_SIZE    32
#define NETPLAY_SALT_SIZE   16
#define NETPLAY_DIGEST_SIZE 32

typedef struct {
   bool use_psn_name;   // false: a random nickname for this session
   bool hide_ip;        // relay only, our address never leaves the vita
} netplay_options_t;

typedef struct {
   char     id[NETPLAY_ID_LEN];
   char     name[NETPLAY_NAME_LEN];
   char     rom[NETPLAY_ROM_LEN];
   uint32_t rom_crc;
   unsigned players;
   bool     relay_only;
   uint32_t seq;
   uint8_t  box_key[NETPLAY_KEY_SIZE];
   uint8_t  room_nonce[NETPLAY_SALT_SIZE];
} netplay_room_t;

typedef enum {
   NETPLAY_INIT_OK = 0,
   NETPLAY_INIT_NO_WIFI,
   NETPLAY_INIT_FAILED
} netplay_init_t;

typedef enum {
   NETPLAY_LOBBY_CONNECTING = 0,
   NETPLAY_LOBBY_ONLINE,
   NETPLAY_LOBBY_FAILED
} netplay_lobby_t;

typedef enum {
   NETPLAY_EVENT_NONE = 0,
   NETPLAY_EVENT_JOINED,
   NETPLAY_EVENT_CONNECTED,
   NETPLAY_EVENT_REJECTED,
   NETPLAY_EVENT_PEER_LEFT,
   NETPLAY_EVENT_TIMEOUT,
   NETPLAY_EVENT_LOBBY_LOST,
   NETPLAY_EVENT_ERROR
} netplay_event_t;

typedef enum {
   NETPLAY_REJECT_NONE = 0,
   NETPLAY_REJECT_FULL,
   NETPLAY_REJECT_ROM,
   NETPLAY_REJECT_STALE
} netplay_reject_t;

// ux0:data/NaohAC/system/netplay_log.txt, wiped once per app launch
void netplay_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
const char *netplay_event_name(netplay_event_t ev);

netplay_init_t netplay_init(const netplay_options_t *opt);
int netplay_init_error(void);
void netplay_shutdown(void);
netplay_lobby_t netplay_lobby_state(void);
const char *netplay_local_name(void);
bool netplay_hiding_ip(void);
const char *netplay_lobby_error(void);

void netplay_browse(const char *game);
int netplay_rooms(netplay_room_t *out, int max);
void netplay_create_room(const char *game, const char *rom, uint32_t rom_crc);
void netplay_join_room(const netplay_room_t *room, uint32_t rom_crc);
netplay_event_t netplay_lobby_poll(void);
netplay_reject_t netplay_reject_reason(void);
const char *netplay_peer_name(void);
bool netplay_is_relayed(void);
unsigned netplay_suggested_delay(void);
void netplay_close(void);

// both sides boot the same rom from factory nvram and only compare a hash
// of the result, no savestate ever crosses the network
void netplay_digest(const void *data, size_t len, uint8_t out[NETPLAY_DIGEST_SIZE]);
netplay_event_t netplay_sync_host(uint32_t size, const uint8_t digest[NETPLAY_DIGEST_SIZE],
      unsigned *delay, void (*idle)(void));
netplay_event_t netplay_sync_guest(uint32_t size, const uint8_t digest[NETPLAY_DIGEST_SIZE],
      void (*idle)(void));

void netplay_start(unsigned delay);
netplay_event_t netplay_poll(void);
void netplay_push_input(uint16_t bits);
bool netplay_frame_ready(void);
uint16_t netplay_input(unsigned port);
void netplay_advance(void);

#endif
