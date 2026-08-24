/* dual core cps2/mame launcher for ps vita. statically links the
 * fbalpha2012_cps2 and mame2000 libretro cores and drives them
 * directly, no retroarch involved. core symbols were prefixed via
 * objcopy as fba_retro_* and mame_retro_* to avoid collisions. */

#include <psp2/ctrl.h>
#include <psp2/touch.h>
#include <psp2/audioout.h>
#include <psp2/power.h>
#include <psp2/gxm.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/io/stat.h>
#include <psp2/io/fcntl.h>
#include <vita2d.h>
#include <stdarg.h>
#include <stdio.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "libretro.h"

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-but-set-variable"
#pragma GCC diagnostic ignored "-Wunused-variable"
#define STB_VORBIS_NO_PUSHDATA_API
#include "deps/stb_vorbis.c"
#pragma GCC diagnostic pop

// extract dir from path, in place. deliberately non-static: this exact
// symbol name/signature is what libretro-common's compat layer expects,
// and both statically linked cores (fba_retro_*/mame_retro_*) were built
// against that layer without an internal copy of it, since their own
// zip/parent romset resolution calls this to find sibling files next to
// whatever path we handed load_game(). since the buffer they call it with
// is entirely their own (we never see or size it from here), the scan
// below is bounded rather than trusting strrchr's usual "input is
// definitely null-terminated" assumption. a buffer that was ever one byte
// short of termination on their side would otherwise let this walk off
// the end of it into whatever memory happened to follow, which is exactly
// the kind of thing that reads as "random" and shifts around between
// rebuilds
#define PATH_BASEDIR_SCAN_MAX 4096

void path_basedir(char *path)
{
   size_t len;
   int i;

   if (!path)
      return;

   len = strnlen(path, PATH_BASEDIR_SCAN_MAX);
   if (len == 0)
      return;

   for (i = (int)len - 1; i >= 0; i--)
   {
      if (path[i] == '/' || path[i] == '\\')
      {
         path[i + 1] = '\0';
         return;
      }
   }

   path[0] = '.';
   path[1] = '\0';
}

// same forced-signature reasoning as path_basedir above. always a no-op:
// create_data_dirs() below makes every directory this app needs up front,
// so nothing on our side ever actually calls into this to create one
bool path_mkdir(const char *dir) { (void)dir; return false; }

// defined further down alongside the rest of the save-dir/rom-dir file
// helpers, forward declared here since rom variant resolution needs it
// well before that point in the file
static bool file_exists(const char *path);

#define SCREEN_W 960
#define SCREEN_H 544

#define SAVE_DIR        "ux0:data/NaohAC/saves"
#define SYSTEM_DIR      "ux0:data/NaohAC/system"
#define ROM_DIR         "ux0:data/NaohAC/roms/"
#define CONFIG_DIR      "ux0:data/NaohAC/configs"
#define NVRAM_ASSET_DIR "app0:assets/nvram/"

// fbalpha2012_cps2's own eeprom .nv read/write (EEPROMInit/EEPROMExit in
// eeprom.cpp) never reliably lands on the vita filesystem during play,
// even with chdir/path workarounds, so that approach was abandoned.
// difficulty/freeplay settings and skipping the boot logos are handled
// entirely through the manual quick-save slot (.qsv, see
// quick_save_do/quick_load_do). seed_default_nvram() below only drops
// bundled default .nv files into save_dir on first boot, so a fresh
// install still starts on freeplay instead of "insert coin", without
// depending on the core writing anything back.

typedef enum {
   CORE_KIND_FBA = 0,
   CORE_KIND_MAME
} core_kind_t;

typedef enum {
   REGION_USA = 0,
   REGION_JAPAN,
   REGION_EUROPE
} rom_region_t;
#define REGION_COUNT 3

// candidate list size per region, always NULL-terminated. most regions
// use exactly one filename, but a region can carry a real checked-in-order
// fallback (e.g. msh's american slot: mshu.zip, then the alt mshh.zip)
#define ROM_VARIANT_MAX 2

typedef struct {
   const char *base_id;                          // asset/config key, decoupled from whichever .zip actually loads
   const char *label;
   core_kind_t core;
   const char *american_files[ROM_VARIANT_MAX];  // checked in order, NULL-terminated
   const char *japanese_files[ROM_VARIANT_MAX];
   const char *european_files[ROM_VARIANT_MAX];
} known_game_t;

// each region is its own independently selectable slot (see the version
// row in run_menu): picking "American Version" always checks
// american_files, it no longer silently falls through to whatever's
// sitting in the european slot just because that's checked first. within
// a single region, entries are still checked in order as a same-region
// fallback only, e.g. msh's american slot tries the mshu.zip clone
// first, then the alt mshh.zip release. japanese slot is still a single
// exact file, no fallback.
static const known_game_t known_games[] = {
   { "msh",      "MARVEL SUPER HEROES",                    CORE_KIND_FBA,
      { "mshu.zip",      "mshh.zip" }, { "mshj.zip",      NULL }, { "msh.zip",      NULL } },
   { "mshvsf",   "MARVEL SUPER HEROES VS. STREET FIGHTER", CORE_KIND_FBA,
      { "mshvsfu.zip",   NULL       }, { "mshvsfj.zip",   NULL }, { "mshvsf.zip",   NULL } },
   { "mvsc",     "MARVEL VS. CAPCOM",                      CORE_KIND_FBA,
      { "mvscu.zip",     NULL       }, { "mvscj.zip",     NULL }, { "mvsc.zip",     NULL } },
   { "xmcota",   "X-MEN: CHILDREN OF THE ATOM",            CORE_KIND_FBA,
      { "xmcotau.zip",   NULL       }, { "xmcotaj.zip",   NULL }, { "xmcota.zip",   NULL } },
   { "xmvsf",    "X-MEN VS. STREET FIGHTER",               CORE_KIND_FBA,
      { "xmvsfu.zip",    NULL       }, { "xmvsfj.zip",    NULL }, { "xmvsf.zip",    NULL } },
   // mame2000 is frozen at mame 0.37b5, from back when driver short names
   // were still capped at 8 characters. that build's cps1.c only knows
   // these clones as punishru/punishrj, the punisheru/punisherj names
   // are a later mame rename that this core was never rebuilt against.
   // load_game matches purely on the zip's basename against its compiled
   // driver table, so a zip literally named punisheru.zip gets rejected
   // on the spot no matter what rom data is actually inside it. the
   // fix is just using the filenames this exact core still expects,
   // rather than inventing anything on the loader side
   { "punisher", "THE PUNISHER",                           CORE_KIND_MAME,
      { "punishru.zip",  NULL       }, { "punishrj.zip",  NULL }, { "punisher.zip", NULL } },
};
#define KNOWN_COUNT (sizeof(known_games) / sizeof(known_games[0]))

typedef struct {
   const char *label;
   core_kind_t core;
   unsigned known_idx;
} game_entry_t;

static game_entry_t games[KNOWN_COUNT];
static unsigned game_count = 0;

// every known game always shows up in the menu now, regardless of whether
// its rom is actually on disk yet. a missing file is only ever caught
// right before boot (see resolve_rom_filename / the missing rom popup),
// never by hiding the game from the list
static void init_game_list(void)
{
   for (unsigned i = 0; i < KNOWN_COUNT; i++)
   {
      games[i].label     = known_games[i].label;
      games[i].core      = known_games[i].core;
      games[i].known_idx = i;
   }

   game_count = KNOWN_COUNT;
}

#define AUDIO_PORT_GRAIN 512

typedef struct {
   void (*set_environment)(retro_environment_t);
   void (*set_video_refresh)(retro_video_refresh_t);
   void (*set_audio_sample)(retro_audio_sample_t);
   void (*set_audio_sample_batch)(retro_audio_sample_batch_t);
   void (*set_input_poll)(retro_input_poll_t);
   void (*set_input_state)(retro_input_state_t);
   void (*init)(void);
   void (*deinit)(void);
   void (*reset)(void);
   bool (*load_game)(const struct retro_game_info *);
   void (*unload_game)(void);
   void (*get_system_av_info)(struct retro_system_av_info *);
   void (*run)(void);
   size_t (*serialize_size)(void);
   bool (*serialize)(void *, size_t);
   bool (*unserialize)(const void *, size_t);
} retro_core_api_t;

static retro_core_api_t core_api;

extern void fba_retro_set_environment(retro_environment_t);
extern void fba_retro_set_video_refresh(retro_video_refresh_t);
extern void fba_retro_set_audio_sample(retro_audio_sample_t);
extern void fba_retro_set_audio_sample_batch(retro_audio_sample_batch_t);
extern void fba_retro_set_input_poll(retro_input_poll_t);
extern void fba_retro_set_input_state(retro_input_state_t);
extern void fba_retro_init(void);
extern void fba_retro_deinit(void);
extern void fba_retro_reset(void);
extern bool fba_retro_load_game(const struct retro_game_info *);
extern void fba_retro_unload_game(void);
extern void fba_retro_get_system_av_info(struct retro_system_av_info *);
extern void fba_retro_run(void);
extern size_t fba_retro_serialize_size(void);
extern bool fba_retro_serialize(void *, size_t);
extern bool fba_retro_unserialize(const void *, size_t);

extern void mame_retro_set_environment(retro_environment_t);
extern void mame_retro_set_video_refresh(retro_video_refresh_t);
extern void mame_retro_set_audio_sample(retro_audio_sample_t);
extern void mame_retro_set_audio_sample_batch(retro_audio_sample_batch_t);
extern void mame_retro_set_input_poll(retro_input_poll_t);
extern void mame_retro_set_input_state(retro_input_state_t);
extern void mame_retro_init(void);
extern void mame_retro_deinit(void);
extern void mame_retro_reset(void);
extern bool mame_retro_load_game(const struct retro_game_info *);
extern void mame_retro_unload_game(void);
extern void mame_retro_get_system_av_info(struct retro_system_av_info *);
extern void mame_retro_run(void);
extern size_t mame_retro_serialize_size(void);
extern bool mame_retro_serialize(void *, size_t);
extern bool mame_retro_unserialize(const void *, size_t);

// each core's function set lives in its own static table, built once at
// compile time. bind_core_api below just copies whichever one matches
// instead of restating all sixteen fields twice down an if/else
static const retro_core_api_t fba_core_api = {
   .set_environment        = fba_retro_set_environment,
   .set_video_refresh      = fba_retro_set_video_refresh,
   .set_audio_sample       = fba_retro_set_audio_sample,
   .set_audio_sample_batch = fba_retro_set_audio_sample_batch,
   .set_input_poll         = fba_retro_set_input_poll,
   .set_input_state        = fba_retro_set_input_state,
   .init                   = fba_retro_init,
   .deinit                 = fba_retro_deinit,
   .reset                  = fba_retro_reset,
   .load_game              = fba_retro_load_game,
   .unload_game            = fba_retro_unload_game,
   .get_system_av_info     = fba_retro_get_system_av_info,
   .run                    = fba_retro_run,
   .serialize_size         = fba_retro_serialize_size,
   .serialize              = fba_retro_serialize,
   .unserialize            = fba_retro_unserialize,
};

static const retro_core_api_t mame_core_api = {
   .set_environment        = mame_retro_set_environment,
   .set_video_refresh      = mame_retro_set_video_refresh,
   .set_audio_sample       = mame_retro_set_audio_sample,
   .set_audio_sample_batch = mame_retro_set_audio_sample_batch,
   .set_input_poll         = mame_retro_set_input_poll,
   .set_input_state        = mame_retro_set_input_state,
   .init                   = mame_retro_init,
   .deinit                 = mame_retro_deinit,
   .reset                  = mame_retro_reset,
   .load_game              = mame_retro_load_game,
   .unload_game            = mame_retro_unload_game,
   .get_system_av_info     = mame_retro_get_system_av_info,
   .run                    = mame_retro_run,
   .serialize_size         = mame_retro_serialize_size,
   .serialize              = mame_retro_serialize,
   .unserialize            = mame_retro_unserialize,
};

// bind libretro api based on selected core
static void bind_core_api(core_kind_t core)
{
   core_api = (core == CORE_KIND_MAME) ? mame_core_api : fba_core_api;
}

#define WALLPAPER_COUNT 11 // off + 10 overlay images (ux0:data/NaohAC/overlays/1..10.png)

static vita2d_texture *frame_tex = NULL;
static int current_overlay = 0; // persisted per-game, see game_config_t below
static vita2d_texture *overlay_tex = NULL;
static unsigned tex_w = 0;
static unsigned tex_h = 0;

static const void *fb_data  = NULL;
static unsigned     fb_w    = 0;
static unsigned     fb_h    = 0;
static size_t        fb_pitch = 0;

static SceCtrlData pad;
static SceTouchData touch_front; // front panel only, used for the coin-insert hack below
static int audio_port = -1;
static int core_port_type = 0;
static int core_port_rate = 0;
static int16_t audio_stage[AUDIO_PORT_GRAIN * 2];
static unsigned audio_stage_fill = 0;
static vita2d_font *font = NULL;
static vita2d_texture *loading_bg = NULL;
static bool game_loaded = false;
static unsigned current_game_idx = 0;

// which libretro joypad port (0 = p1, 1 = p2) the vita's single physical
// pad currently feeds, see input_state_cb below. latched from
// game_player_port[] right before the core loads (see main()) and never
// touched again mid-game
static unsigned active_player_port = 0;

// consecutive frames the physical start button has been continuously
// held, tracked once per emulated frame in input_poll_cb (right after
// update_pad() refreshes pad, so it always reflects this frame's true
// hold length). the only thing that reads this is the port-0 start
// bleed carve-out in input_state_cb below
static unsigned start_hold_frames = 0;

// how long start has to be continuously held before input_state_cb lets
// it bleed onto port 0 in p2 mode. short enough that a deliberate hold
// for fba2012's own "hold start" diagnostic combo (which needs port 0's
// start held for 60+ straight core frames on top of this, see
// input_state_cb) still clears comfortably, long enough that a normal
// tap to start a p2 game never reaches port 0 at all
#define PORT0_START_BLEED_HOLD_FRAMES 20

// true only while warm_up_core() is burning throwaway frames right after
// load. input_state_cb masks every button while this is set, so whatever
// is still physically held from picking the game in our own menu (start,
// most likely) never reaches the core. fbalpha2012 checks its diagnostic
// hold combo on every single frame from the moment the diagnostic_input
// core option is parsed, warmup frames included, so a start press that
// bleeds through here can silently trip and latch the combo before real
// gameplay even starts, and there is no way to unlatch it from our side
// afterward
static bool core_warming_up = false;

// tells environment_cb to answer the next RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE
// with true exactly once, right after warm_up_core() finishes. not load
// bearing for correctness (check_variables() only touches the diag_input
// pointer and its hold delay, never the actual frame counter), kept purely
// so the first real read of fbneo-diagnostic-input happens against clean
// post warmup input instead of whatever was still held at launch
static bool needs_var_update = false;

#define FONT_BASE_PX   16

#define SFX_RATE       44100
#define SFX_GRAIN      512

static int16_t   *sfx_choose_pcm    = NULL;
static unsigned    sfx_choose_frames = 0;
static int16_t   *sfx_select_pcm    = NULL;
static unsigned    sfx_select_frames = 0;
static int16_t   *sfx_back_pcm      = NULL;
static unsigned    sfx_back_frames  = 0;
static int16_t   *sfx_areyousure_pcm    = NULL;
static unsigned    sfx_areyousure_frames = 0;
static int16_t   *sfx_gamechange_pcm    = NULL;
static unsigned    sfx_gamechange_frames = 0;
static int16_t   *sfx_titlescreen_pcm    = NULL;
static unsigned    sfx_titlescreen_frames = 0;
static int16_t   *sfx_pressx_pcm    = NULL;
static unsigned    sfx_pressx_frames = 0;

static SceUID              sfx_thread_id = -1;
static volatile bool       sfx_thread_run = false;
static SceUID              sfx_sema = -1;
static volatile bool       sfx_abort = false;
static volatile const int16_t *sfx_pending_pcm    = NULL;
static volatile unsigned       sfx_pending_frames = 0;

static int16_t *load_ogg_full(const char *path, unsigned *out_frames)
{
   stb_vorbis *v = stb_vorbis_open_filename(path, NULL, NULL);
   unsigned total;
   int16_t *buf;

   *out_frames = 0;
   if (!v)
      return NULL;

   total = stb_vorbis_stream_length_in_samples(v);
   buf = (int16_t *)malloc((size_t)total * 2 * sizeof(int16_t));
   if (!buf)
   {
      stb_vorbis_close(v);
      return NULL;
   }

   *out_frames = (unsigned)stb_vorbis_get_samples_short_interleaved(v, 2, buf, (int)(total * 2));
   stb_vorbis_close(v);
   return buf;
}

static int sfx_thread_func(SceSize args, void *argp)
{
   int16_t grain_buf[SFX_GRAIN * 2];
   int vol[2] = { SCE_AUDIO_VOLUME_0DB, SCE_AUDIO_VOLUME_0DB };

   (void)args;
   (void)argp;

   while (sfx_thread_run)
   {
      sceKernelWaitSema(sfx_sema, 1, NULL);
      if (!sfx_thread_run)
         break;

      sfx_abort = false;
      const int16_t *pcm = (const int16_t *)sfx_pending_pcm;
      unsigned frames = sfx_pending_frames;
      unsigned pos = 0;

      // route ui sounds to bgm port if core uses voice
      int port_to_use = SCE_AUDIO_OUT_PORT_TYPE_VOICE;
      if (game_loaded && core_port_type == SCE_AUDIO_OUT_PORT_TYPE_VOICE)
         port_to_use = SCE_AUDIO_OUT_PORT_TYPE_BGM;

      int port = sceAudioOutOpenPort(port_to_use,
            SFX_GRAIN, SFX_RATE, SCE_AUDIO_OUT_MODE_STEREO);

      if (port < 0)
         port = sceAudioOutOpenPort(SCE_AUDIO_OUT_PORT_TYPE_VOICE,
               SFX_GRAIN, SFX_RATE, SCE_AUDIO_OUT_MODE_STEREO);

      if (port < 0)
         continue;

      sceAudioOutSetVolume(port, SCE_AUDIO_VOLUME_FLAG_L_CH | SCE_AUDIO_VOLUME_FLAG_R_CH, vol);

      while (pos < frames && !sfx_abort && sfx_thread_run)
      {
         unsigned take = (frames - pos < SFX_GRAIN) ? (frames - pos) : SFX_GRAIN;

         memcpy(grain_buf, pcm + pos * 2, take * 2 * sizeof(int16_t));
         if (take < SFX_GRAIN)
            memset(grain_buf + take * 2, 0, (SFX_GRAIN - take) * 2 * sizeof(int16_t));

         sceAudioOutOutput(port, grain_buf);
         pos += take;
      }

      sceAudioOutReleasePort(port);
   }

   return 0;
}

static void sfx_play(const int16_t *pcm, unsigned frames)
{
   if (!pcm || !frames || sfx_thread_id < 0)
      return;

   sfx_abort = true;
   sfx_pending_pcm    = pcm;
   sfx_pending_frames = frames;

   SceKernelSemaInfo info;
   info.size = sizeof(SceKernelSemaInfo);
   if (sceKernelGetSemaInfo(sfx_sema, &info) == 0)
   {
      if (info.currentCount == 0)
         sceKernelSignalSema(sfx_sema, 1);
   }
}

// dedicated one-shot thread for the per-game announcer stinger (see
// play_game_announcer_async below). kept separate from the ui_sfx thread
// above since callers here need to know when playback has actually
// finished rather than just firing it and moving on. announcer_playing
// flips back to false from inside the thread itself, right after it
// frees its own pcm buffer, so a caller can poll it while still drawing
// every frame instead of blocking the render loop
static SceUID          announcer_thread_id = -1;
static volatile bool   announcer_playing   = false;

typedef struct {
   int16_t  *pcm;
   unsigned  frames;
} announcer_args_t;

static int announcer_thread_func(SceSize args, void *argp)
{
   announcer_args_t a = *(announcer_args_t *)argp;
   int16_t grain_buf[SFX_GRAIN * 2];
   int vol[2] = { SCE_AUDIO_VOLUME_0DB, SCE_AUDIO_VOLUME_0DB };
   unsigned pos = 0;
   int port;

   (void)args;

   int port_to_use = SCE_AUDIO_OUT_PORT_TYPE_VOICE;
   if (game_loaded && core_port_type == SCE_AUDIO_OUT_PORT_TYPE_VOICE)
      port_to_use = SCE_AUDIO_OUT_PORT_TYPE_BGM;

   port = sceAudioOutOpenPort(port_to_use, SFX_GRAIN, SFX_RATE, SCE_AUDIO_OUT_MODE_STEREO);
   if (port < 0)
      port = sceAudioOutOpenPort(SCE_AUDIO_OUT_PORT_TYPE_VOICE, SFX_GRAIN, SFX_RATE, SCE_AUDIO_OUT_MODE_STEREO);

   if (port >= 0)
   {
      sceAudioOutSetVolume(port, SCE_AUDIO_VOLUME_FLAG_L_CH | SCE_AUDIO_VOLUME_FLAG_R_CH, vol);

      while (pos < a.frames)
      {
         unsigned take = (a.frames - pos < SFX_GRAIN) ? (a.frames - pos) : SFX_GRAIN;

         memcpy(grain_buf, a.pcm + pos * 2, take * 2 * sizeof(int16_t));
         if (take < SFX_GRAIN)
            memset(grain_buf + take * 2, 0, (SFX_GRAIN - take) * 2 * sizeof(int16_t));

         // sceAudioOutOutput blocks per grain, but only this dedicated
         // thread ever waits on it, never the render loop
         sceAudioOutOutput(port, grain_buf);
         pos += take;
      }

      // one last silent grain, so the tail actually drains before the
      // port closes instead of getting cut by release
      memset(grain_buf, 0, sizeof(grain_buf));
      sceAudioOutOutput(port, grain_buf);

      sceAudioOutReleasePort(port);
   }

   free(a.pcm);
   announcer_playing = false;
   return 0;
}

// loads a single game's announcer stinger from
// assets/audio/gameselection/<base_id>.ogg and plays it on its own
// thread instead of the calling thread, so whichever screen triggered
// this (see run_offline_play) keeps drawing every frame instead of
// freezing while it plays out. loaded on demand rather than preloaded
// alongside the fixed ui sfx set below, since only one of these ever
// plays per boot-to-game transition. run_offline_play polls
// announcer_playing while it draws the now loading screen, and joins
// this thread once that flag goes false
static void play_game_announcer_async(unsigned known_idx)
{
   char path[160];
   announcer_args_t args;
   int16_t *pcm;
   unsigned frames;

   if (known_idx >= KNOWN_COUNT)
      return;

   snprintf(path, sizeof(path), "app0:assets/audio/gameselection/%s.ogg",
         known_games[known_idx].base_id);

   pcm = load_ogg_full(path, &frames);
   if (!pcm)
      return;

   args.pcm    = pcm;
   args.frames = frames;

   announcer_playing = true;

   announcer_thread_id = sceKernelCreateThread("game_announcer", announcer_thread_func,
         0x10000100, 0x10000, 0, 0, NULL);

   if (announcer_thread_id < 0)
   {
      announcer_playing = false;
      free(pcm);
      return;
   }

   sceKernelStartThread(announcer_thread_id, sizeof(args), &args);
}

static void init_ui_sfx(void)
{
   sfx_choose_pcm = load_ogg_full("app0:assets/audio/menus/choose.ogg", &sfx_choose_frames);
   sfx_select_pcm = load_ogg_full("app0:assets/audio/menus/select.ogg", &sfx_select_frames);
   sfx_back_pcm       = load_ogg_full("app0:assets/audio/menus/back.ogg", &sfx_back_frames);
   sfx_areyousure_pcm = load_ogg_full("app0:assets/audio/menus/areyousure.ogg", &sfx_areyousure_frames);
   sfx_gamechange_pcm = load_ogg_full("app0:assets/audio/gameselection/gamechange.ogg", &sfx_gamechange_frames);
   sfx_titlescreen_pcm = load_ogg_full("app0:assets/audio/titlescreen/titlescreensound.ogg", &sfx_titlescreen_frames);
   sfx_pressx_pcm      = load_ogg_full("app0:assets/audio/titlescreen/whenpressxtostart.ogg", &sfx_pressx_frames);
   sfx_sema = sceKernelCreateSema("sfx_sema", 0, 0, 1, NULL);

   sfx_thread_run = true;
   sfx_thread_id = sceKernelCreateThread("ui_sfx", sfx_thread_func,
         0x10000100, 0x10000, 0, 0, NULL);

   if (sfx_thread_id < 0)
   {
      sfx_thread_run = false;
      sfx_thread_id = -1;
      return;
   }

   sceKernelStartThread(sfx_thread_id, 0, NULL);
}

static void free_ui_sfx(void)
{
   if (sfx_thread_id >= 0)
   {
      sfx_thread_run = false;
      sceKernelSignalSema(sfx_sema, 1);
      sceKernelWaitThreadEnd(sfx_thread_id, NULL, NULL);
      sceKernelDeleteThread(sfx_thread_id);
      sfx_thread_id = -1;
   }

   if (sfx_sema >= 0)
   {
      sceKernelDeleteSema(sfx_sema);
      sfx_sema = -1;
   }

   free(sfx_choose_pcm);
   free(sfx_select_pcm);
   free(sfx_back_pcm);
   free(sfx_areyousure_pcm);
   free(sfx_gamechange_pcm);
   free(sfx_titlescreen_pcm);
   free(sfx_pressx_pcm);
   sfx_choose_pcm      = NULL;
   sfx_select_pcm      = NULL;
   sfx_back_pcm        = NULL;
   sfx_areyousure_pcm  = NULL;
   sfx_gamechange_pcm  = NULL;
   sfx_titlescreen_pcm = NULL;
   sfx_pressx_pcm      = NULL;
}

#define MENU_MUSIC_PATH  "app0:assets/audio/menus/mainmenuu.ogg"
#define MENU_MUSIC_RATE  44100
#define MENU_MUSIC_GRAIN 1024

static SceUID music_thread_id = -1;
static volatile bool music_thread_run = false;
static int music_audio_port = -1;

// main menu bgm volume, 0-10 (0 mutes). session-only, always boots back
// at max -- nothing in the task calls for persisting it to disk. lives
// here rather than down by the rest of the main menu code (see
// run_main_menu) because music_thread_func below needs it the moment its
// port opens, well before that point in the file
#define MAINMENU_BGM_VOLUME_MAX 10
static int mainmenu_bgm_volume = MAINMENU_BGM_VOLUME_MAX;

// pushes mainmenu_bgm_volume out to the live menu-music port. safe to
// call any time, including before the music thread's port has actually
// opened yet (music_audio_port sits at -1 until it has, see
// music_thread_func just below, which also calls this itself once its
// port opens)
static void apply_mainmenu_bgm_volume(void)
{
   if (music_audio_port < 0)
      return;

   int level = (int)((float)SCE_AUDIO_VOLUME_0DB *
         ((float)mainmenu_bgm_volume / (float)MAINMENU_BGM_VOLUME_MAX) + 0.5f);
   int vol[2] = { level, level };

   sceAudioOutSetVolume(music_audio_port,
         SCE_AUDIO_VOLUME_FLAG_L_CH | SCE_AUDIO_VOLUME_FLAG_R_CH, vol);
}

static int music_thread_func(SceSize args, void *argp)
{
   (void)args;
   (void)argp;

   stb_vorbis *v = stb_vorbis_open_filename(MENU_MUSIC_PATH, NULL, NULL);
   if (!v)
      return 0;

   // retry if sfx is holding the audio port
   int retries = 20;
   while (retries-- > 0 && music_thread_run)
   {
      music_audio_port = sceAudioOutOpenPort(SCE_AUDIO_OUT_PORT_TYPE_BGM,
            MENU_MUSIC_GRAIN, MENU_MUSIC_RATE, SCE_AUDIO_OUT_MODE_STEREO);
      if (music_audio_port >= 0)
         break;
      sceKernelDelayThread(50000);
   }

   if (music_audio_port < 0)
   {
      stb_vorbis_close(v);
      return 0;
   }

   // pulls in whatever level the main menu bgm slider is currently on
   // (see run_main_menu/mainmenu_options_submenu) instead of always
   // reopening at a hardcoded 0db
   apply_mainmenu_bgm_volume();

   int16_t buf[MENU_MUSIC_GRAIN * 2];

   while (music_thread_run)
   {
      int got = stb_vorbis_get_samples_short_interleaved(v, 2, buf, MENU_MUSIC_GRAIN * 2);

      if (got <= 0)
      {
         stb_vorbis_seek_start(v);
         continue;
      }

      if (got < MENU_MUSIC_GRAIN)
         memset(buf + got * 2, 0, (MENU_MUSIC_GRAIN - got) * 2 * sizeof(int16_t));

      if (music_thread_run)
         sceAudioOutOutput(music_audio_port, buf);
   }

   sceAudioOutReleasePort(music_audio_port);
   music_audio_port = -1;
   stb_vorbis_close(v);

   return 0;
}

static void start_menu_music(void)
{
   if (music_thread_id >= 0)
      return;

   music_thread_run = true;
   music_thread_id = sceKernelCreateThread("menu_music", music_thread_func,
         0x10000100, 0x10000, 0, 0, NULL);

   if (music_thread_id < 0)
   {
      music_thread_run = false;
      music_thread_id = -1;
      return;
   }

   sceKernelStartThread(music_thread_id, 0, NULL);
}

static void stop_menu_music(void)
{
   if (music_thread_id < 0)
      return;

   music_thread_run = false;
   sceKernelWaitThreadEnd(music_thread_id, NULL, NULL);
   sceKernelDeleteThread(music_thread_id);
   music_thread_id = -1;
}

// defined further down alongside the rest of the crt/vignette baking code,
// forward declared here since load_menu_assets/free_menu_assets want to
// touch them before that point in the file
static void load_vignette_pulse_tex(void);
static void load_spotlight_tex(void);
static void free_filter_tex(void);

// baked once in load_vignette_pulse_tex, see the crt/vignette baking code
// further down for how it's built and used
static vita2d_texture *vignette_pulse_tex = NULL;

// baked once in load_spotlight_tex, a soft circular white glow scaled and
// tinted at draw time over whichever cover sits in the front-center slot.
// see draw_carousel_spotlight further down
#define SPOTLIGHT_TEX_SIZE 256
static vita2d_texture *spotlight_tex = NULL;

static vita2d_texture *menu_backdrop_tex = NULL; // full-screen bg, app0:assets/backgrounds/bg_menu.png
static vita2d_texture *cover_tex[KNOWN_COUNT];

// title screen chrome, shown once at boot before the main menu (see
// run_title_screen further down)
static vita2d_texture *title_bg_tex     = NULL; // app0:assets/backgrounds/title_bg.png, 1080x1440
static vita2d_texture *title_logo_tex   = NULL; // app0:assets/screens/title/logo.png, 622x331
static vita2d_texture *title_banner_tex = NULL; // app0:assets/screens/title/presstostart.png, 480x52

// main menu chrome, shown between the title screen and the game
// carousel (see run_main_menu further down)
static vita2d_texture *mainmenu_bg_tex = NULL; // app0:assets/backgrounds/mainmenu.png

// baked pause menu chrome, see app0:assets/ui/panels/pause_*.png. the
// photo backdrop (pause_photo_tex) lives under backgrounds/ instead, it's
// shared with the carousel/main menu bottom bars and info panel, not
// pause-specific despite the filename
static vita2d_texture *pause_frame_tex   = NULL;
static vita2d_texture *pause_ribbon_tex  = NULL;
static vita2d_texture *pause_hilite_tex  = NULL;
static vita2d_texture *pause_photo_tex   = NULL; // app0:assets/backgrounds/pausemenubg.png

// select-game menu chrome, see app0:assets/ui/panels/menu_*.png and
// app0:assets/ui/icons/btn_*.png
static vita2d_texture *menu_header_tex = NULL;
static vita2d_texture *info_frame_tex  = NULL;
static vita2d_texture *btn_tri_tex     = NULL;
static vita2d_texture *btn_sq_tex      = NULL;
static vita2d_texture *btn_cross_tex   = NULL;
static vita2d_texture *btn_circ_tex    = NULL;

// controller settings overlay chrome, see
// app0:assets/ui/panels/settings_banner.png. same 480x52 size as the
// pause menu's red ribbon (pause_ribbon_tex) so it drops into the same
// header slot without any extra scaling math
static vita2d_texture *settings_banner_tex = NULL;

// modal confirmation dialog chrome, see
// app0:assets/ui/panels/warningbg.png. drawn stretched to whatever w/h
// show_prompt asks for (see draw_prompt_box), same stand-in-stretch
// convention draw_panel_box already uses for its own frame art
static vita2d_texture *prompt_bg_tex = NULL;

// staggered card layout: slots 0-2 back row (small), 3-5 front row (big)
static const int slot_x[KNOWN_COUNT] = { 239, 405, 571, 194, 390, 586 };
static const int slot_y[KNOWN_COUNT] = {  80,  80,  80, 174, 174, 174 };
static const int slot_w[KNOWN_COUNT] = { 150, 150, 150, 180, 180, 180 };
static const int slot_h[KNOWN_COUNT] = { 122, 122, 122, 152, 152, 152 };

// index into slot_x/y/w/h for the physical front-and-center card position.
// the spotlight (see draw_carousel_spotlight) is baked/anchored here, a
// fixed screen position, not to any particular game
#define FRONT_SLOT_IDX 4

// per-game region selection, actually drives which physical rom variant
// gets loaded (see resolve_rom_filename below). cover art and the
// controller/visual config stay pinned to base_id regardless of region,
// so switching a game's region never orphans either one
static const char *region_labels[REGION_COUNT] = { "English Version", "Japanese Version", "European Version" };
static unsigned char game_region[KNOWN_COUNT];

// per-game player-side selection, which libretro controller port (arcade
// cabinet slot) the vita's single physical pad feeds into for this game.
// p1 is the traditional single-player side; p2 lets a game be played from
// the right-hand cabinet position instead (right-side character in a vs.
// fighter, right player slot on a two-player-only rom, etc). a menu/runtime
// choice like region, never persisted to the per-game .cfg file, always
// starts back on p1 on a fresh launch
#define PLAYER_PORT_COUNT 2
static const char *player_port_labels[PLAYER_PORT_COUNT] = { "P1", "P2" };
static unsigned char game_player_port[KNOWN_COUNT];

/* circular track: physical slot order back l/m/r then front r/m/l (mirrors js mockup) */
static const int track_order[KNOWN_COUNT] = { 0, 1, 2, 5, 4, 3 };
#define FRONT_TRACK_IDX 4        // track_order index that lands in slot 4 (center-front)
#define ANIM_US         420000   // carousel shift duration, us (420ms, same as mockup)

static int      anim_active   = 0;
static int      anim_dir      = 0;   // -1 left, +1 right
static int      anim_from_sel = 0;
static SceInt64 anim_start_us = 0;

typedef struct { unsigned i; float x, y, w, h; } slot_draw_t;

typedef enum { MENU_FOCUS_GAMES, MENU_FOCUS_VERSION, MENU_FOCUS_PLAYER } menu_focus_t;

// frees a texture and clears the pointer, no-op if already NULL. shared
// by every one-off texture cleanup below so the free+null pair only has
// to be written once
static void free_tex_ptr(vita2d_texture **tex)
{
   if (*tex)
   {
      vita2d_free_texture(*tex);
      *tex = NULL;
   }
}

static void load_menu_assets(void)
{
   char path[160];

   menu_backdrop_tex = vita2d_load_PNG_file("app0:assets/backgrounds/bg_menu.png");

   title_bg_tex     = vita2d_load_PNG_file("app0:assets/backgrounds/title_bg.png");
   title_logo_tex   = vita2d_load_PNG_file("app0:assets/screens/title/logo.png");
   title_banner_tex = vita2d_load_PNG_file("app0:assets/screens/title/presstostart.png");

   mainmenu_bg_tex = vita2d_load_PNG_file("app0:assets/backgrounds/mainmenu.png");

   pause_frame_tex  = vita2d_load_PNG_file("app0:assets/ui/panels/pause_frame.png");
   pause_ribbon_tex = vita2d_load_PNG_file("app0:assets/ui/panels/pause_ribbon.png");
   pause_hilite_tex = vita2d_load_PNG_file("app0:assets/ui/panels/pause_highlight.png");
   pause_photo_tex  = vita2d_load_PNG_file("app0:assets/backgrounds/pausemenubg.png");

   menu_header_tex = vita2d_load_PNG_file("app0:assets/ui/panels/menu_header.png");
   info_frame_tex  = vita2d_load_PNG_file("app0:assets/ui/panels/menu_infopanel_frame.png");
   btn_tri_tex     = vita2d_load_PNG_file("app0:assets/ui/icons/btn_triangle.png");
   btn_sq_tex      = vita2d_load_PNG_file("app0:assets/ui/icons/btn_square.png");
   btn_cross_tex   = vita2d_load_PNG_file("app0:assets/ui/icons/btn_cross.png");
   btn_circ_tex    = vita2d_load_PNG_file("app0:assets/ui/icons/btn_circle.png");

   settings_banner_tex = vita2d_load_PNG_file("app0:assets/ui/panels/settings_banner.png");
   prompt_bg_tex        = vita2d_load_PNG_file("app0:assets/ui/panels/warningbg.png");

   // cache cover textures in ram, keyed off base_id so european/american/
   // japanese variants of the same game always share one cover
   for (unsigned i = 0; i < KNOWN_COUNT; i++)
   {
      snprintf(path, sizeof(path), "ux0:data/NaohAC/covers/%s.png", known_games[i].base_id);
      cover_tex[i] = vita2d_load_PNG_file(path);
   }

   load_vignette_pulse_tex();
   load_spotlight_tex();
}

static void free_menu_assets(void)
{
   for (unsigned i = 0; i < KNOWN_COUNT; i++)
      free_tex_ptr(&cover_tex[i]);

   free_tex_ptr(&menu_backdrop_tex);

   free_tex_ptr(&title_bg_tex);
   free_tex_ptr(&title_logo_tex);
   free_tex_ptr(&title_banner_tex);
   free_tex_ptr(&mainmenu_bg_tex);

   free_tex_ptr(&pause_frame_tex);
   free_tex_ptr(&pause_ribbon_tex);
   free_tex_ptr(&pause_hilite_tex);
   free_tex_ptr(&pause_photo_tex);

   free_tex_ptr(&menu_header_tex);
   free_tex_ptr(&info_frame_tex);
   free_tex_ptr(&btn_tri_tex);
   free_tex_ptr(&btn_sq_tex);
   free_tex_ptr(&btn_cross_tex);
   free_tex_ptr(&btn_circ_tex);

   free_tex_ptr(&settings_banner_tex);
   free_tex_ptr(&prompt_bg_tex);

   free_tex_ptr(&vignette_pulse_tex);
   free_tex_ptr(&spotlight_tex);
   free_filter_tex();
}

#define ASPECT_FULLSCREEN 0
#define ASPECT_4_3        1
#define ASPECT_5_4        2
#define ASPECT_COUNT      3

// persisted per-game, see game_config_t below
static int aspect_mode = ASPECT_FULLSCREEN;

// display filter modes. heavier variants are just bigger numbers on the
// same effect as their base version, kept grouped so cycling feels sane
#define DISPLAY_FILTER_OFF           0
#define DISPLAY_FILTER_SCANLINES     1
#define DISPLAY_FILTER_SCANLINES_HVY 2
#define DISPLAY_FILTER_GRID          3
#define DISPLAY_FILTER_GRID_HVY      4
#define DISPLAY_FILTER_CURVED        5
#define DISPLAY_FILTER_LIGHT_SCAN    6
#define DISPLAY_FILTER_RGB_MASK      7
#define DISPLAY_FILTER_INTERLACE     8
#define DISPLAY_FILTER_COUNT         9

// persisted per-game now, see game_config_t. only ever drawn while a core
// is actually running
static int display_filter = DISPLAY_FILTER_OFF;

static const char *display_filter_labels[DISPLAY_FILTER_COUNT] = {
   "Off", "Scanlines", "Scanlines+", "Grid", "Grid+",
   "Curved", "Light Scanlines", "RGB Mask", "Interlace"
};

// oled burn-in mitigation for the wallpaper/overlay border. purely a
// time-driven animation, no per-frame state to track
#define WALLPAPER_FX_OFF        0
#define WALLPAPER_FX_KENBURNS   1
#define WALLPAPER_FX_BREATHING  2
#define WALLPAPER_FX_COLORDRIFT 3
#define WALLPAPER_FX_VIGNETTE   4
#define WALLPAPER_FX_COUNT      5

// persisted per-game, same reasoning as display_filter above
static int wallpaper_fx = WALLPAPER_FX_OFF;

static const char *wallpaper_fx_labels[WALLPAPER_FX_COUNT] = {
   "Off", "Ken Burns", "Breathing", "Color Drift", "Vignette Pulse"
};

// user's base opacity for the wallpaper, 1-10 (10 = 100% visible). any
// wallpaper_fx that animates its own alpha (breathing) scales relative to
// this instead of a hardcoded 255, so dialing the base down dims the fx too
#define WALLPAPER_OPACITY_MAX 10
static int wallpaper_opacity = WALLPAPER_OPACITY_MAX;

#define PAUSE_SCREEN_MAIN    0
#define PAUSE_SCREEN_OPTIONS 1

#define PAUSE_RESUME          0
#define PAUSE_CTRL_SETTINGS   1
#define PAUSE_OPTIONS         2
#define PAUSE_LOAD_QSAVE      3
#define PAUSE_SAVE_QSAVE      4
#define PAUSE_RESTART         5
#define PAUSE_EXIT            6
#define PAUSE_ITEM_COUNT      7

#define OPT_WALLPAPER         0
#define OPT_WALLPAPER_OPACITY 1
#define OPT_WALLPAPER_FX      2
#define OPT_FILTER            3
#define OPT_SIZE              4
#define OPT_ITEM_COUNT        5

typedef struct {
   const char *label;
   const char *description; // bottom-bar left text while highlighted
} pause_item_t;

static const pause_item_t pause_items[PAUSE_ITEM_COUNT] = {
   { "Resume",                   "Return to the game." },
   { "Controller Settings",      "Remap buttons for this game." },
   { "Display & Sound Settings", "Change wallpaper, filter, and screen size." },
   { "Load Quick Save",          "Load your saved progress for this game." },
   { "Quick Save",               "Save your progress for this game." },
   { "Restart Game",             "Restart the current game from the beginning." },
   { "Quit",                     "Exit to the game selection screen." },
};

// where the core's own log lines get written to, since plain vprintf
// output has nowhere visible to go once the app is running standalone
// on vita. this is the only way to actually see why a core refuses to
// load a given rom (bad crc, missing shared file, etc)
#define CORE_LOG_PATH SYSTEM_DIR "/core_log.txt"

// truncates the log at boot so each session starts clean instead of
// growing forever. called once from main() before anything else runs
static void reset_core_log(void)
{
   FILE *lf = fopen(CORE_LOG_PATH, "w");
   if (lf)
      fclose(lf);
}

static void core_log(enum retro_log_level level, const char *fmt, ...)
{
   va_list ap;
   FILE *lf;

   (void)level;

   lf = fopen(CORE_LOG_PATH, "a");
   if (lf)
   {
      va_start(ap, fmt);
      vfprintf(lf, fmt, ap);
      va_end(ap);
      fclose(lf);
   }

   va_start(ap, fmt);
   vprintf(fmt, ap);
   va_end(ap);
}

// libretro environment callbacks
static bool environment_cb(unsigned cmd, void *data)
{
   switch (cmd)
   {
      case RETRO_ENVIRONMENT_GET_LOG_INTERFACE:
         ((struct retro_log_callback *)data)->log = core_log;
         return true;

      case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY:
         // fba2012 inserts its own separator when it builds
         // <save_dir>/<drvname>.nv (see eeprom.cpp) and .fs (see
         // libretro.cpp), so giving it a dir that already ends in "/"
         // double-slashes that path and the eeprom write silently
         // fails. mame2000 does the exact same thing internally when it
         // resolves a clone's parent driver (<system_dir>/<drvname>) -
         // this was left on the trailing-slash form for mame2000
         // "unverified" and that was the bug: punisher (the parent, no
         // clone-of to resolve) never exercises that path so it always
         // loaded fine, punisheru/punisherj (clones) hit it on every
         // load and got rejected outright on the double slash. both
         // cores get the no-trailing-slash form now
         *(const char **)data = SAVE_DIR;
         return true;

      case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
         // same rule, same fix as save dir above
         *(const char **)data = SYSTEM_DIR;
         return true;

      case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT:
         return *(enum retro_pixel_format *)data == RETRO_PIXEL_FORMAT_RGB565;

      case RETRO_ENVIRONMENT_SET_VARIABLES:
         // fbalpha2012's set_environment() sends its option list here and
         // never looks at the return value, so this only exists to be a
         // well behaved frontend. answering it changes nothing about
         // whether the diagnostic option below actually gets read
         return true;

      case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE:
         // check_variables() only reassigns the diag_input pointer and
         // its hold delay here, it never touches the frame counter that
         // does the actual holding, so firing this every frame is
         // harmless. needs_var_update still only goes true once, right
         // after warm_up_core() finishes, purely to keep the very first
         // read of fbneo-diagnostic-input away from warmup's
         // phantom input rather than because repeats would break anything
         *(bool *)data = needs_var_update;
         needs_var_update = false;
         return true;

      case RETRO_ENVIRONMENT_GET_VARIABLE:
      {
         struct retro_variable *var = (struct retro_variable *)data;

         if (!var || !var->key)
            return false;

         if (!strcmp(var->key, "mame2000_sample_rate") ||
               !strcmp(var->key, "mame2000-sample_rate"))
         {
            var->value = "48000";
            return true;
         }

         // fbalpha2012's actual check_variables() (retro_common.cpp,
         // shared with fbneo) reads "fbneo-diagnostic-input", not the
         // CORE_OPTION_NAME prefixed key libretro.cpp defines but never
         // uses. matching a couple of older spellings too in case a
         // build predates that merge. hold start tells the core to
         // watch start itself and pop the menu after its own internal
         // hold timer, whichever key it asked under
         if (!strcmp(var->key, "fbneo-diagnostic-input") ||
               !strcmp(var->key, "fbalpha2012_diagnostic_input") ||
               !strcmp(var->key, "fba-diagnostic-input"))
         {
            var->value = "Hold Start";
            return true;
         }

         return false;
      }

      default:
         return false;
   }
}

static void video_refresh_cb(const void *data, unsigned width, unsigned height, size_t pitch)
{
   if (!data)
      return;

   fb_data  = data;
   fb_w     = width;
   fb_h     = height;
   fb_pitch = pitch;
}

static void audio_sample_cb(int16_t left, int16_t right)
{
   (void)left;
   (void)right;
}

static size_t audio_sample_batch_cb(const int16_t *data, size_t frames)
{
   size_t pos = 0;

   while (pos < frames)
   {
      unsigned space = AUDIO_PORT_GRAIN - audio_stage_fill;
      unsigned take  = (frames - pos < space) ? (unsigned)(frames - pos) : space;

      memcpy(audio_stage + audio_stage_fill * 2, data + pos * 2, take * 2 * sizeof(int16_t));
      audio_stage_fill += take;
      pos += take;

      if (audio_stage_fill == AUDIO_PORT_GRAIN)
      {
         if (audio_port >= 0)
            sceAudioOutOutput(audio_port, audio_stage);
         audio_stage_fill = 0;
      }
   }

   return frames;
}

// analog range is 0-255, center ~128. press and release sit at different
// thresholds on purpose: a stick resting right on the edge of a single
// shared threshold reads as a new press every time it wobbles a pixel
// either side of it, which is what was firing the choose/gamechange sfx
// twice off one nudge. crossing the outer band latches the bit, and it
// only lets go once the stick actually comes back past the inner one
#define STICK_PRESS_LO   80
#define STICK_PRESS_HI   170
#define STICK_RELEASE_LO 96
#define STICK_RELEASE_HI 154

static bool stick_left  = false;
static bool stick_right = false;
static bool stick_up    = false;
static bool stick_down  = false;

// read input and map left analog stick to d-pad
static void update_pad(void)
{
   sceCtrlPeekBufferPositive(0, &pad, 1);

   stick_left  = stick_left  ? (pad.lx < STICK_RELEASE_LO) : (pad.lx < STICK_PRESS_LO);
   stick_right = stick_right ? (pad.lx > STICK_RELEASE_HI) : (pad.lx > STICK_PRESS_HI);
   stick_up    = stick_up    ? (pad.ly < STICK_RELEASE_LO) : (pad.ly < STICK_PRESS_LO);
   stick_down  = stick_down  ? (pad.ly > STICK_RELEASE_HI) : (pad.ly > STICK_PRESS_HI);

   if (stick_left)  pad.buttons |= SCE_CTRL_LEFT;
   if (stick_right) pad.buttons |= SCE_CTRL_RIGHT;
   if (stick_up)    pad.buttons |= SCE_CTRL_UP;
   if (stick_down)  pad.buttons |= SCE_CTRL_DOWN;
}

// front panel only, the coin-insert hack below is the only thing that
// cares about touch, nothing else in the app reads this
static void update_touch(void)
{
   sceTouchPeek(SCE_TOUCH_PORT_FRONT, &touch_front, 1);
}

// edge-detects buttons newly pressed this frame against last frame's
// state. shared by every full-screen menu loop (main menu, pause menu,
// controller settings, popups) so each one isn't restating the same
// poll/xor/mask sequence for itself
static unsigned poll_pressed(SceCtrlData *prev)
{
   update_pad();

   unsigned pressed = (pad.buttons ^ prev->buttons) & pad.buttons;
   *prev = pad;
   return pressed;
}

// waits out whatever's still physically held, then clears prev so the
// caller's next poll_pressed() doesn't see a stale edge. every menu loop
// below calls this once on entry, and again after returning from a popup
// or submenu, so a button still held on the way in or out never bleeds
// through as a spurious press
static void flush_pad(SceCtrlData *prev)
{
   do
   {
      update_pad();
   } while (pad.buttons != 0);

   memset(prev, 0, sizeof(*prev));
}

static void input_poll_cb(void)
{
   update_pad();
   update_touch();

   // see PORT0_START_BLEED_HOLD_FRAMES / input_state_cb: this has to stay
   // in lockstep with the core's own per-frame input snapshot, so it's
   // updated here rather than off pad state read anywhere else
   if (pad.buttons & SCE_CTRL_START)
      start_hold_frames++;
   else
      start_hold_frames = 0;
}

// -1 isn't a real retropad joypad id (they're all 0 and up), safe to use
// as "this button does nothing" without colliding with a real action
#define REMAP_UNASSIGNED (-1)

// live per-game button mapping. cross/circle/square/triangle/l1/r1 each
// point at one of the retropad actions below, editable from the main
// menu's controller settings screen and persisted in game_config_t.
// defaults here get overwritten by apply_default_mapping as soon as a
// rom is picked, this is just the fallback before that ever runs
static int map_cross    = REMAP_UNASSIGNED;
static int map_circle   = REMAP_UNASSIGNED;
static int map_square   = REMAP_UNASSIGNED;
static int map_triangle = REMAP_UNASSIGNED;
static int map_l1       = REMAP_UNASSIGNED;
static int map_r1       = REMAP_UNASSIGNED;

// what a physical button can be pointed at, in cycling order for the
// controller settings screen. the list depends on which core the
// highlighted game uses: mame (punisher) only needs punch/jump, the fba
// fighters get the full 6-button spread
typedef struct { int id; const char *label; } remap_target_t;

// punisher's only a 2-button beat 'em up, punch and jump, nothing else
static const remap_target_t remap_targets_mame[] = {
   { RETRO_DEVICE_ID_JOYPAD_B, "Punch" },
   { RETRO_DEVICE_ID_JOYPAD_A, "Jump"  },
   { REMAP_UNASSIGNED,         "Unassigned" },
};
#define REMAP_TARGETS_MAME_COUNT (sizeof(remap_targets_mame) / sizeof(remap_targets_mame[0]))

// cps1/cps2/cps3 fighters, standard 6-button spread, each id unique so
// all 6 face/shoulder buttons can hold a distinct action at once
static const remap_target_t remap_targets_fba[] = {
   { RETRO_DEVICE_ID_JOYPAD_X, "Light Punch"  },
   { RETRO_DEVICE_ID_JOYPAD_Y, "Medium Punch" },
   { RETRO_DEVICE_ID_JOYPAD_L, "Heavy Punch"  },
   { RETRO_DEVICE_ID_JOYPAD_B, "Light Kick"   },
   { RETRO_DEVICE_ID_JOYPAD_A, "Medium Kick"  },
   { RETRO_DEVICE_ID_JOYPAD_R, "Heavy Kick"   },
   { REMAP_UNASSIGNED,         "Unassigned"   },
};
#define REMAP_TARGETS_FBA_COUNT (sizeof(remap_targets_fba) / sizeof(remap_targets_fba[0]))

static const remap_target_t *remap_table_for(core_kind_t core, unsigned *count)
{
   if (core == CORE_KIND_MAME)
   {
      *count = REMAP_TARGETS_MAME_COUNT;
      return remap_targets_mame;
   }
   *count = REMAP_TARGETS_FBA_COUNT;
   return remap_targets_fba;
}

static bool remap_target_valid(core_kind_t core, int id)
{
   unsigned count;
   const remap_target_t *table = remap_table_for(core, &count);
   for (unsigned i = 0; i < count; i++)
      if (table[i].id == id)
         return true;
   return false;
}

static const char *remap_target_label(core_kind_t core, int id)
{
   unsigned count;
   const remap_target_t *table = remap_table_for(core, &count);
   for (unsigned i = 0; i < count; i++)
      if (table[i].id == id)
         return table[i].label;
   return "?";
}

// steps a mapped action forward/backward through the list, wrapping
// around either direction: forward is cross or the right arrow, backward
// is the left arrow. always walks whichever table matches the game's
// core, so it never reads past that table's own length
static int remap_target_cycle(core_kind_t core, int id)
{
   unsigned count;
   const remap_target_t *table = remap_table_for(core, &count);
   for (unsigned i = 0; i < count; i++)
      if (table[i].id == id)
         return table[(i + 1) % count].id;
   return table[0].id;
}

static int remap_target_cycle_prev(core_kind_t core, int id)
{
   unsigned count;
   const remap_target_t *table = remap_table_for(core, &count);
   for (unsigned i = 0; i < count; i++)
      if (table[i].id == id)
         return table[(i + count - 1) % count].id;
   return table[0].id;
}

// true if whichever physical button the user pointed at this action is
// currently held. set_row_mapping (controller_settings_menu) keeps the
// map_* globals a 1:1 assignment - at most one of them ever holds a
// given real action at once - but this just reads whatever's actually
// in them, it doesn't depend on that being true
static bool action_pressed(int action_id)
{
   if (map_cross    == action_id && (pad.buttons & SCE_CTRL_CROSS))    return true;
   if (map_circle   == action_id && (pad.buttons & SCE_CTRL_CIRCLE))   return true;
   if (map_square   == action_id && (pad.buttons & SCE_CTRL_SQUARE))   return true;
   if (map_triangle == action_id && (pad.buttons & SCE_CTRL_TRIANGLE)) return true;
   if (map_l1       == action_id && (pad.buttons & SCE_CTRL_LTRIGGER)) return true;
   if (map_r1       == action_id && (pad.buttons & SCE_CTRL_RTRIGGER)) return true;
   return false;
}

// map vita input to libretro joypad. d-pad, start, and coin are always
// hardcoded here and never touched by the remap menu, only the face
// buttons and l1/r1 go through the user's custom mapping.
//
// the vita only ever has one physical pad, so rather than wiring up
// set_controller_port_device (arcade cores here don't use it: both
// mame2000 and fba2012 just read fixed joypad ports directly, confirmed
// against fba2012_neogeo's own libretro.cpp - there's no connected/
// disconnected device state anywhere in its input path, just per-port,
// per-button reads), the p1/p2 choice from the main menu is applied
// right here: whichever single port active_player_port names is the
// only one that ever sees real input, every other port stays silent.
//
// that silence *is* the disconnect for these cores: arcade hardware
// (and mame/fba's emulation of it) never had a notion of an unplugged
// stick to begin with, only coin/start state the game's own driver code
// tracks. a port that never once sees a coin, a start, or a button
// press never gets claimed by the driver in the first place, so it
// can't spawn a character or hold the game open waiting on it - unlike
// a real "P1 vs P2" credit split, where both sides get properly coined
// and started, this is what actually keeps this a true single-player
// side switch. the one place that used to undermine this is the start
// carve-out right below - see PORT0_START_BLEED_HOLD_FRAMES
static int16_t input_state_cb(unsigned port, unsigned device, unsigned index, unsigned id)
{
   (void)index;

   // warm_up_core() burns throwaway frames right after load with
   // whatever's still physically held from picking the game in our own
   // menu (start, most likely). masking everything here keeps that from
   // reaching the core at all, so it can't trip anything, diagnostic
   // hold combo included, before real gameplay has even started
   if (core_warming_up)
      return 0;

   if (device != RETRO_DEVICE_JOYPAD)
      return 0;

   // fbalpha2012's diagnostic hold combo polls start on a hardcoded
   // port 0 no matter which port the game itself is actually fed on
   // (see environment_cb's fbneo-diagnostic-input handling, which always
   // answers "hold start" for whichever diagnostic-input key a given fba
   // build asks for). confirmed against fba2012_neogeo's own libretro.cpp:
   // the combo checker always reads input_cb(0, RETRO_DEVICE_JOYPAD, 0,
   // diag_input[...]) regardless of which port is actually playing, and
   // "hold start" needs that port-0 read to stay pressed for more than
   // 60 straight core frames (diag_input_hold_frame_delay) before the
   // combo actually fires. in p2 mode our physical start only otherwise
   // answers on active_player_port, so without some carve-out here the
   // core's port 0 check would always read start as unpressed and the
   // hold could never fire at all.
   //
   // this used to just forward start to port 0 unconditionally, which
   // was the actual cause of the p1/p2 bug: a plain tap to start a p2
   // game reads as pressed on port 0 for those same frames, and that's
   // plenty for the game's own driver logic (not fba's diagnostic
   // checker, the arcade ROM code itself) to register a genuine p1
   // start/credit event and open a real two-player game, exactly like
   // pressing both start buttons on a real cabinet would. holding start
   // for PORT0_START_BLEED_HOLD_FRAMES before ever letting it reach
   // port 0 fixes that: an ordinary tap-to-start (a handful of frames)
   // never reaches port 0 at all now, so p1 never gets coined or
   // started and stays genuinely disconnected, while a deliberate hold
   // still clears this gate quickly enough that fba2012's own longer
   // 60-frame timer still has time to fire afterward
   if (port == 0 && id == RETRO_DEVICE_ID_JOYPAD_START && active_player_port != 0
         && start_hold_frames > PORT0_START_BLEED_HOLD_FRAMES)
      return (pad.buttons & SCE_CTRL_START) ? 1 : 0;

   // fba2012/mame2000's own dip switch/test menu is drawn and read
   // entirely inside the core, and every driver checked reads its
   // navigation off a hardcoded port 0, same as the diagnostic combo
   // itself does for start just above. trying to track exactly when
   // that menu is open from out here was too fragile, so in p2 mode
   // the d-pad and every standard action button just mirror onto
   // port 0 unconditionally instead, menu open or not - which exact
   // button a given driver reads for confirm/toggle inside its own
   // dipswitch screen isn't consistent across cores, so all of them
   // go through rather than guessing one. port 0 never sees a coin
   // or a real start in p2 mode (see the start carve-out above), so
   // the driver never claims it for actual gameplay - a stray
   // direction or button on an uncredited, unclaimed port is a no-op
   // during real play, and a working cursor/confirm the moment the
   // menu is actually up
   if (port == 0 && active_player_port != 0)
   {
      switch (id)
      {
         case RETRO_DEVICE_ID_JOYPAD_UP:    return (pad.buttons & SCE_CTRL_UP)    ? 1 : 0;
         case RETRO_DEVICE_ID_JOYPAD_DOWN:  return (pad.buttons & SCE_CTRL_DOWN)  ? 1 : 0;
         case RETRO_DEVICE_ID_JOYPAD_LEFT:  return (pad.buttons & SCE_CTRL_LEFT)  ? 1 : 0;
         case RETRO_DEVICE_ID_JOYPAD_RIGHT: return (pad.buttons & SCE_CTRL_RIGHT) ? 1 : 0;
         case RETRO_DEVICE_ID_JOYPAD_A: return action_pressed(RETRO_DEVICE_ID_JOYPAD_A) ? 1 : 0;
         case RETRO_DEVICE_ID_JOYPAD_B: return action_pressed(RETRO_DEVICE_ID_JOYPAD_B) ? 1 : 0;
         case RETRO_DEVICE_ID_JOYPAD_X: return action_pressed(RETRO_DEVICE_ID_JOYPAD_X) ? 1 : 0;
         case RETRO_DEVICE_ID_JOYPAD_Y: return action_pressed(RETRO_DEVICE_ID_JOYPAD_Y) ? 1 : 0;
         case RETRO_DEVICE_ID_JOYPAD_L: return action_pressed(RETRO_DEVICE_ID_JOYPAD_L) ? 1 : 0;
         case RETRO_DEVICE_ID_JOYPAD_R: return action_pressed(RETRO_DEVICE_ID_JOYPAD_R) ? 1 : 0;
         default: break;
      }
   }

   if (port != active_player_port)
      return 0;

   switch (id)
   {
      case RETRO_DEVICE_ID_JOYPAD_UP:    return (pad.buttons & SCE_CTRL_UP)    ? 1 : 0;
      case RETRO_DEVICE_ID_JOYPAD_DOWN:  return (pad.buttons & SCE_CTRL_DOWN)  ? 1 : 0;
      case RETRO_DEVICE_ID_JOYPAD_LEFT:  return (pad.buttons & SCE_CTRL_LEFT)  ? 1 : 0;
      case RETRO_DEVICE_ID_JOYPAD_RIGHT: return (pad.buttons & SCE_CTRL_RIGHT) ? 1 : 0;
      case RETRO_DEVICE_ID_JOYPAD_START: return (pad.buttons & SCE_CTRL_START) ? 1 : 0;

      // coin now comes from a tap anywhere on the front touchscreen instead
      // of a physical button. physical select stays reserved for the pause
      // menu (see run_core) and never reaches the core while playing, so
      // there's no conflict between the two
      case RETRO_DEVICE_ID_JOYPAD_SELECT:
         return (touch_front.reportNum > 0) ? 1 : 0;

      case RETRO_DEVICE_ID_JOYPAD_A: return action_pressed(RETRO_DEVICE_ID_JOYPAD_A) ? 1 : 0;
      case RETRO_DEVICE_ID_JOYPAD_B: return action_pressed(RETRO_DEVICE_ID_JOYPAD_B) ? 1 : 0;
      case RETRO_DEVICE_ID_JOYPAD_X: return action_pressed(RETRO_DEVICE_ID_JOYPAD_X) ? 1 : 0;
      case RETRO_DEVICE_ID_JOYPAD_Y: return action_pressed(RETRO_DEVICE_ID_JOYPAD_Y) ? 1 : 0;
      case RETRO_DEVICE_ID_JOYPAD_L: return action_pressed(RETRO_DEVICE_ID_JOYPAD_L) ? 1 : 0;
      case RETRO_DEVICE_ID_JOYPAD_R: return action_pressed(RETRO_DEVICE_ID_JOYPAD_R) ? 1 : 0;

      // l2/r2 mirror whatever's currently pointed at l/r. nothing in the
      // current punch/kick/unassigned menu ever assigns l or r, so these
      // are dead in practice right now, left in harmlessly in case a
      // future build brings back more than 2 action tiers
      case RETRO_DEVICE_ID_JOYPAD_L2: return action_pressed(RETRO_DEVICE_ID_JOYPAD_L) ? 1 : 0;
      case RETRO_DEVICE_ID_JOYPAD_R2: return action_pressed(RETRO_DEVICE_ID_JOYPAD_R) ? 1 : 0;
   }

   return 0;
}

// compute scaled destination rect. w/h (source geometry) only matter for
// the aspect-preserving modes below, fullscreen ignores them entirely now
static void compute_dest_rect(unsigned w, unsigned h, float *dx, float *dy, float *dw, float *dh)
{
   if (aspect_mode == ASPECT_4_3 || aspect_mode == ASPECT_5_4)
   {
      // 4:3 and 5:4 only differ in target ratio, same letterbox math either way
      float target_aspect = (aspect_mode == ASPECT_4_3) ? (4.0f / 3.0f) : (5.0f / 4.0f);
      float avail_h = SCREEN_H;
      float avail_w = avail_h * target_aspect;

      if (avail_w > SCREEN_W)
      {
         avail_w = SCREEN_W;
         avail_h = avail_w / target_aspect;
      }

      *dw = avail_w;
      *dh = avail_h;
   }
   else
   {
      // true fullscreen: slam the texture across the entire 960x544
      // panel, aspect ratio be damned. no letterbox/pillarbox math, no
      // source w/h involved at all, so there's no way for black borders
      // to sneak back in here
      (void)w;
      (void)h;

      *dw = SCREEN_W;
      *dh = SCREEN_H;
   }

   *dx = (SCREEN_W - *dw) / 2.0f;
   *dy = (SCREEN_H - *dh) / 2.0f;
}

// crt filter / vignette baking
//
// all of these used to be drawn as a pile of individual vita2d_draw_rectangle
// calls every single frame, up to 700+ of them for the heavier filters
// (grid+, rgb mask), plus another 250-400 for the vignette pulse fx on top
// of that. each rectangle is a separate draw submission, and that per-call
// overhead (not fill rate) is what was almost certainly blowing the frame
// budget and starving the audio thread, especially with a crt filter and a
// wallpaper fx both going at once. everything below gets rasterized into a
// plain rgba texture once, either the first time it's needed or whenever
// the filter mode or viewport size actually changes, and after that it's
// a single draw call a frame no matter how busy the pattern looks.
// interlace keeps two baked fields and just flips which one it draws

// straight-alpha "over" compositing of one packed rgba8 pixel onto another.
// used while baking so overlapping bands/lines darken the same way
// sequential blended draws would have on real hardware
static inline unsigned blend_over(unsigned dst, unsigned src)
{
   int sa = (int)((src >> 24) & 0xFF);

   if (sa <= 0)
      return dst;
   if (sa >= 255)
      return src;

   int sr = (int)(src & 0xFF), sg = (int)((src >> 8) & 0xFF), sb = (int)((src >> 16) & 0xFF);
   int dr = (int)(dst & 0xFF), dg = (int)((dst >> 8) & 0xFF), db = (int)((dst >> 16) & 0xFF), da = (int)((dst >> 24) & 0xFF);
   int oa = sa + da * (255 - sa) / 255;
   int or_ = 0, og = 0, ob = 0;

   if (oa > 0)
   {
      or_ = (sr * sa + dr * da * (255 - sa) / 255) / oa;
      og  = (sg * sa + dg * da * (255 - sa) / 255) / oa;
      ob  = (sb * sa + db * da * (255 - sa) / 255) / oa;
   }

   return RGBA8((unsigned)or_, (unsigned)og, (unsigned)ob, (unsigned)oa);
}

static inline void blend_px(uint32_t *px, unsigned stride_px, int w, int h, int x, int y, unsigned color)
{
   if ((unsigned)x >= (unsigned)w || (unsigned)y >= (unsigned)h)
      return;

   px[y * stride_px + x] = blend_over(px[y * stride_px + x], color);
}

// paints every 'step'th row solid black at the given alpha. shared by the
// display filter cases below that are nothing but a plain horizontal line
// pattern, only the spacing and darkness differ between them. writes
// straight into the still-transparent buffer, nothing to blend against yet
static void fill_horiz_lines(uint32_t *px, unsigned stride_px, int w, int h, int step, unsigned alpha)
{
   for (int y = 0; y < h; y += step)
      for (int x = 0; x < w; x++)
         px[y * stride_px + x] = RGBA8(0, 0, 0, alpha);
}

// same pattern, blended over whatever the buffer already holds. for
// passes that land on top of a color fill instead of empty space
static void fill_horiz_lines_blend(uint32_t *px, unsigned stride_px, int w, int h, int step, unsigned alpha)
{
   for (int y = 0; y < h; y += step)
      for (int x = 0; x < w; x++)
         blend_px(px, stride_px, w, h, x, y, RGBA8(0, 0, 0, alpha));
}

// vertical counterpart, always blended since it always runs after a
// horizontal pass has already filled the buffer
static void fill_vert_lines_blend(uint32_t *px, unsigned stride_px, int w, int h, int step, unsigned alpha)
{
   for (int x = 0; x < w; x += step)
      for (int y = 0; y < h; y++)
         blend_px(px, stride_px, w, h, x, y, RGBA8(0, 0, 0, alpha));
}

// darkens a w x h buffer's edges by the given fraction of its size / peak
// alpha, shared by the curved display filter and the vignette pulse fx so
// the heavy curved variant is just bigger numbers on the same helper
static void bake_vignette(uint32_t *px, unsigned stride_px, int w, int h,
      float frac_y, float frac_x, int peak_y, int peak_x)
{
   int vig_y = (int)(h * frac_y);
   int vig_x = (int)(w * frac_x);

   for (int i = 0; i < vig_y; i++)
   {
      unsigned c = RGBA8(0, 0, 0, (unsigned)((vig_y - i) * peak_y / vig_y));

      for (int x = 0; x < w; x++)
      {
         blend_px(px, stride_px, w, h, x, i, c);
         blend_px(px, stride_px, w, h, x, h - 1 - i, c);
      }
   }

   for (int i = 0; i < vig_x; i++)
   {
      unsigned c = RGBA8(0, 0, 0, (unsigned)((vig_x - i) * peak_x / vig_x));

      for (int y = 0; y < h; y++)
      {
         blend_px(px, stride_px, w, h, i, y, c);
         blend_px(px, stride_px, w, h, w - 1 - i, y, c);
      }
   }
}

// allocates a w x h rgba8 texture, cleared to transparent, ready for a
// bake_* helper to fill in before it's drawn
static vita2d_texture *alloc_bake_tex(int w, int h, uint32_t **out_px, unsigned *out_stride_px)
{
   vita2d_texture *tex = vita2d_create_empty_texture_format(w, h, SCE_GXM_TEXTURE_FORMAT_U8U8U8U8_ABGR);

   if (!tex)
      return NULL;

   *out_px        = (uint32_t *)vita2d_texture_get_datap(tex);
   *out_stride_px = vita2d_texture_get_stride(tex) / 4;
   memset(*out_px, 0, (size_t)(*out_stride_px) * (unsigned)h * 4);

   return tex;
}

// rasterizes one display_filter pattern at w x h. 'field' only matters for
// interlace: 0 darkens rows 0-1/4-5/..., 1 darkens rows 2-3/6-7/...
static vita2d_texture *bake_display_filter(int mode, int w, int h, int field)
{
   uint32_t *px;
   unsigned stride_px;
   vita2d_texture *tex = alloc_bake_tex(w, h, &px, &stride_px);

   if (!tex)
      return NULL;

   switch (mode)
   {
      case DISPLAY_FILTER_SCANLINES:
         fill_horiz_lines(px, stride_px, w, h, 2, 90);
         break;

      case DISPLAY_FILTER_SCANLINES_HVY:
         // thicker lines, closer together, much darker gaps
         for (int y = 0; y < h; y += 3)
            for (int yy = y; yy < y + 2 && yy < h; yy++)
               for (int x = 0; x < w; x++)
                  px[yy * stride_px + x] = RGBA8(0, 0, 0, 190);
         break;

      case DISPLAY_FILTER_GRID:
         fill_horiz_lines(px, stride_px, w, h, 2, 90);
         fill_vert_lines_blend(px, stride_px, w, h, 3, 60);
         break;

      case DISPLAY_FILTER_GRID_HVY:
         // denser mesh, darker on both axes
         fill_horiz_lines(px, stride_px, w, h, 2, 150);
         fill_vert_lines_blend(px, stride_px, w, h, 2, 110);
         break;

      case DISPLAY_FILTER_CURVED:
         fill_horiz_lines(px, stride_px, w, h, 2, 70);
         bake_vignette(px, stride_px, w, h, 0.12f, 0.08f, 180, 150);
         break;

      case DISPLAY_FILTER_LIGHT_SCAN:
         // faint lines only, no vignette, keeps the image bright instead
         // of the usual crt-shader darkening
         fill_horiz_lines(px, stride_px, w, h, 2, 40);
         break;

      case DISPLAY_FILTER_RGB_MASK:
      {
         // aperture-grille style tinted stripes, faked with alternating rgb
         // columns since there's no per-subpixel shader to lean on here
         static const unsigned mask_colors[3] = {
            RGBA8(255, 60, 60, 70), RGBA8(60, 255, 60, 70), RGBA8(60, 60, 255, 70)
         };
         int col = 0;

         for (int x = 0; x < w; x += 2)
         {
            for (int xx = x; xx < x + 2 && xx < w; xx++)
               for (int y = 0; y < h; y++)
                  px[y * stride_px + xx] = mask_colors[col];
            col = (col + 1) % 3;
         }

         fill_horiz_lines_blend(px, stride_px, w, h, 2, 60);
         break;
      }

      case DISPLAY_FILTER_INTERLACE:
         for (int y = field * 2; y < h; y += 4)
            for (int yy = y; yy < y + 2 && yy < h; yy++)
               for (int x = 0; x < w; x++)
                  px[yy * stride_px + x] = RGBA8(0, 0, 0, 200);
         break;

      default:
         break;
   }

   return tex;
}

// live cache: the baked texture(s) for whichever filter is on, plus the
// mode/size they were baked for. a rebuild only fires when either one
// actually changes, a settings cycle in the pause menu or a new game
// with different viewport geometry, never once a frame
static vita2d_texture *filter_tex        = NULL;
static vita2d_texture *filter_tex_field2 = NULL; // interlace's second field
static int filter_tex_mode = -1;
static int filter_tex_w    = -1;
static int filter_tex_h    = -1;

static void free_filter_tex(void)
{
   // the gpu can still be mid-draw on whichever texture last got submitted
   // (interlace flips between the two every frame, see draw_display_filter),
   // so both frees need to wait, not just the first one. freeing field2
   // without waiting here was the actual crash: cycling off interlace into
   // another filter could yank it out from under an in-flight draw
   if (filter_tex || filter_tex_field2)
      vita2d_wait_rendering_done();

   if (filter_tex)
   {
      vita2d_free_texture(filter_tex);
      filter_tex = NULL;
   }

   if (filter_tex_field2)
   {
      vita2d_free_texture(filter_tex_field2);
      filter_tex_field2 = NULL;
   }

   filter_tex_mode = -1;
   filter_tex_w    = -1;
   filter_tex_h    = -1;
}

// clipped to the game viewport so it doesn't bleed onto the 4:3/5:4
// overlay bezel, same as the old per-rect version did
static void draw_display_filter(float dx, float dy, float dw, float dh)
{
   if (display_filter == DISPLAY_FILTER_OFF)
      return;

   int w = (int)(dw + 0.5f);
   int h = (int)(dh + 0.5f);

   if (w < 1) w = 1;
   if (h < 1) h = 1;

   if (display_filter != filter_tex_mode || w != filter_tex_w || h != filter_tex_h)
   {
      free_filter_tex();

      if (display_filter == DISPLAY_FILTER_INTERLACE)
      {
         filter_tex        = bake_display_filter(display_filter, w, h, 0);
         filter_tex_field2 = bake_display_filter(display_filter, w, h, 1);
      }
      else
         filter_tex = bake_display_filter(display_filter, w, h, 0);

      filter_tex_mode = display_filter;
      filter_tex_w    = w;
      filter_tex_h    = h;
   }

   vita2d_texture *tex = filter_tex;

   if (display_filter == DISPLAY_FILTER_INTERLACE && filter_tex_field2)
   {
      // flips roughly 15x/sec, same cadence as the old live version
      SceInt64 t = sceKernelGetProcessTimeWide();
      if ((t / 66666LL) % 2)
         tex = filter_tex_field2;
   }

   if (tex)
      vita2d_draw_texture(tex, dx, dy);
}

// baked once at load_menu_assets time, peak 255 on both axes (see the
// forward declaration and vignette_pulse_tex up near menu_backdrop_tex).
// the live pulse (50-160) is applied at draw time as a tint alpha multiply
// instead of re-rasterizing every frame, see draw_wallpaper_overlay
static void load_vignette_pulse_tex(void)
{
   uint32_t *px;
   unsigned stride_px;

   vignette_pulse_tex = alloc_bake_tex(SCREEN_W, SCREEN_H, &px, &stride_px);
   if (vignette_pulse_tex)
      bake_vignette(px, stride_px, SCREEN_W, SCREEN_H, 0.22f, 0.16f, 255, 255);
}

// rasterizes a soft circular white glow into a square buffer: solid-ish
// core fading smoothly to fully transparent at the edge. baked once at
// full strength (peak alpha below), the live draw just tints/scales this
// same texture over the front-center slot instead of re-rasterizing a
// gradient every frame (see draw_carousel_spotlight)
static void bake_spotlight(uint32_t *px, unsigned stride_px, int w, int h)
{
   float cx     = w * 0.5f;
   float cy     = h * 0.5f;
   float radius = w * 0.5f;

   for (int y = 0; y < h; y++)
   {
      float dy = (y + 0.5f) - cy;

      for (int x = 0; x < w; x++)
      {
         float dx   = (x + 0.5f) - cx;
         float dist = sqrtf(dx * dx + dy * dy) / radius;

         if (dist >= 1.0f)
            continue; // leave fully transparent past the circle

         // smoothstep falloff reads as a soft-edged glow instead of the
         // hard-edged disc a plain linear ramp would give
         float t     = 1.0f - dist;
         float alpha = t * t * (3.0f - 2.0f * t);

         px[y * stride_px + x] = RGBA8(255, 255, 255, (unsigned)(alpha * 235.0f + 0.5f));
      }
   }
}

// baked once at load_menu_assets time, same one-shot-bake-then-tint
// approach as load_vignette_pulse_tex above
static void load_spotlight_tex(void)
{
   uint32_t *px;
   unsigned stride_px;

   spotlight_tex = alloc_bake_tex(SPOTLIGHT_TEX_SIZE, SPOTLIGHT_TEX_SIZE, &px, &stride_px);
   if (spotlight_tex)
      bake_spotlight(px, stride_px, SPOTLIGHT_TEX_SIZE, SPOTLIGHT_TEX_SIZE);
}

// draws the wallpaper/overlay border, optionally animated per wallpaper_fx.
// shared by the live gameplay render and the options-screen preview so both
// stay in sync. all effects are plain sine waves off the system clock
static void draw_wallpaper_overlay(void)
{
   if (!overlay_tex)
      return;

   float ow = (float)vita2d_texture_get_width(overlay_tex);
   float oh = (float)vita2d_texture_get_height(overlay_tex);
   float base_sx = SCREEN_W / ow;
   float base_sy = SCREEN_H / oh;

   // user's base opacity (1-10 -> 10%-100%). every fx below scales its own
   // alpha off of this instead of a hardcoded 255
   float base_alpha = wallpaper_opacity * (255.0f / WALLPAPER_OPACITY_MAX);

   SceInt64 t = sceKernelGetProcessTimeWide();
   float secs = (float)t / 1000000.0f;
   float scale = 1.0f, x = 0.0f, y = 0.0f;
   float alpha = base_alpha, r = 1.0f, g = 1.0f, b = 1.0f;

   switch (wallpaper_fx)
   {
      case WALLPAPER_FX_KENBURNS:
         // 20s round trip, 1.0 -> 1.05 -> 1.0, zoom centered on screen
         scale = 1.025f + 0.025f * sinf(secs * (2.0f * (float)M_PI / 20.0f));
         x = (SCREEN_W - SCREEN_W * scale) * 0.5f;
         y = (SCREEN_H - SCREEN_H * scale) * 0.5f;
         break;

      case WALLPAPER_FX_BREATHING:
         // 40%-100% of the user's base opacity now, 6s round trip (used to
         // swing 40-100% of a hardcoded 255 regardless of the opacity
         // slider). old range was 85-100%, way too narrow to actually
         // rest any pixels. also truncating the float straight to
         // unsigned was quietly biasing alpha low every frame, so round
         // properly at the point it gets packed into the tint
         alpha = base_alpha * (0.7f + 0.3f * sinf(secs * (2.0f * (float)M_PI / 6.0f)));
         break;

      case WALLPAPER_FX_COLORDRIFT:
      {
         // image never moves, just the rgb balance. three channels riding
         // the same sine 120 degrees apart so it eases through the color
         // wheel instead of snapping, and no channel ever fully bottoms out
         const float period  = 24.0f;
         const float floor_v = 0.80f;
         const float swing   = 0.20f;
         float phase = secs * (2.0f * (float)M_PI / period);

         r = floor_v + swing * (0.5f + 0.5f * sinf(phase));
         g = floor_v + swing * (0.5f + 0.5f * sinf(phase + 2.0f * (float)M_PI / 3.0f));
         b = floor_v + swing * (0.5f + 0.5f * sinf(phase + 4.0f * (float)M_PI / 3.0f));
         break;
      }

      case WALLPAPER_FX_OFF:
      case WALLPAPER_FX_VIGNETTE:
      default:
         // wallpaper texture itself stays put/unmodified for both of these,
         // though vignette's extra shading gets layered on top after, see below
         break;
   }

   // skip the tint draw path entirely when it'd be a no-op (full opacity,
   // no color/alpha animation active this frame). tinted and untinted
   // draws don't share the same gpu state in vita2d, so bouncing between
   // them every frame was its own quiet source of redundant state changes,
   // on top of all the rect spam above
   if (r == 1.0f && g == 1.0f && b == 1.0f && alpha >= 254.5f)
      vita2d_draw_texture_scale(overlay_tex, x, y, base_sx * scale, base_sy * scale);
   else
   {
      unsigned tint = RGBA8(
            (unsigned)(r * 255.0f + 0.5f),
            (unsigned)(g * 255.0f + 0.5f),
            (unsigned)(b * 255.0f + 0.5f),
            (unsigned)(alpha + 0.5f));

      vita2d_draw_texture_tint_scale(overlay_tex, x, y, base_sx * scale, base_sy * scale, tint);
   }

   if (wallpaper_fx == WALLPAPER_FX_VIGNETTE && vignette_pulse_tex)
   {
      // 10s round trip, shadow breathes from faint to fairly dark and
      // back. baked once at peak 255, tinted here to the live pulse value
      // instead of rebuilding every frame. the two overlapping bands in
      // each corner make this a close approximation rather than
      // pixel-exact at partial peaks, not worth a rebake every frame to fix
      float pulse = 0.5f + 0.5f * sinf(secs * (2.0f * (float)M_PI / 10.0f));
      int peak = (int)(50.0f + 110.0f * pulse);

      vita2d_draw_texture_tint(vignette_pulse_tex, 0.0f, 0.0f, RGBA8(255, 255, 255, (unsigned)peak));
   }
}

// draws the running game frame plus its wallpaper border and crt filter.
// shared by the live render loop and the paused-frame preview behind the
// pause menu so the two can never drift out of sync with each other
static void draw_gameplay_layer(float dx, float dy, float dw, float dh)
{
   // draw overlay if aspect ratio leaves borders
   if ((aspect_mode == ASPECT_4_3 || aspect_mode == ASPECT_5_4) && overlay_tex != NULL)
      draw_wallpaper_overlay();

   vita2d_draw_texture_scale(frame_tex, dx, dy, dw / (float)fb_w, dh / (float)fb_h);
   draw_display_filter(dx, dy, dw, dh);
}

static void blit_frame(void)
{
   if (!fb_data || !fb_w || !fb_h)
      return;

   if (!frame_tex || tex_w != fb_w || tex_h != fb_h)
   {
      if (frame_tex)
      {
         // same rule as free_filter_tex/reload_overlay_texture: don't free
         // a texture the gpu might still be reading from last frame's draw.
         // this path fires on every new game load (main() zeroes tex_w/h to
         // force it), so skipping the wait here is exactly what could leave
         // the loading screen up forever on a fast quit-then-load
         vita2d_wait_rendering_done();
         vita2d_free_texture(frame_tex);
         frame_tex = NULL;
      }

      frame_tex = vita2d_create_empty_texture_format(fb_w, fb_h, SCE_GXM_TEXTURE_FORMAT_R5G6B5);
      if (!frame_tex)
      {
         // out of gpu memory or similar: bail and retry next frame instead
         // of handing vita2d_texture_get_datap() below a null texture
         tex_w = 0;
         tex_h = 0;
         return;
      }

      tex_w = fb_w;
      tex_h = fb_h;
   }

   uint8_t *dst = (uint8_t *)vita2d_texture_get_datap(frame_tex);
   unsigned dst_stride = vita2d_texture_get_stride(frame_tex);
   const uint8_t *src = (const uint8_t *)fb_data;

   for (unsigned y = 0; y < fb_h; y++)
      memcpy(dst + y * dst_stride, src + y * fb_pitch, fb_w * 2);

   float dx, dy, dw, dh;
   compute_dest_rect(fb_w, fb_h, &dx, &dy, &dw, &dh);

   vita2d_start_drawing();
   vita2d_clear_screen();

   draw_gameplay_layer(dx, dy, dw, dh);

   vita2d_end_drawing();
   vita2d_swap_buffers();
}

// pause menu panel layout, 1:1 with the html/canvas mockup, centered on
// screen and sized/offset to fit this menu's own content: the ribbon
// banner (pause_ribbon.png, drawn at PAUSE_HEADER_X/Y) plus either the
// 7-row main screen (grew from 6 with the addition of controller
// settings) or the 5-row options screen, both drawn inside the same
// panel. this is its own independent geometry now -- it used to be
// shared with every other menu behind bare PANEL_*/ITEM_* macros, which
// meant a tweak aimed at one menu silently reflowed all of them. each
// menu now owns its geometry outright; only truly generic, position-free
// constants (row height, font metrics, spacing, the hilite sheet, the
// border/opacity used by every panel's box art) stay shared below.
// draw_pause_panel scales the baked frame/photo art to match at draw
// time, see draw_panel_box further down, so PAUSE_PANEL_H is the one
// constant to touch if a row ever gets added or removed again. also
// nudged up a little (Y) and given a taller PANEL_H (+40, one extra
// ITEM_SPACING_MAIN row) versus its old 6-row size, so the panel's own
// bottom edge still clears the new bottom bar at BAR_Y with room to
// spare instead of running into it
#define PAUSE_PANEL_W        500
#define PAUSE_PANEL_H        346
#define PAUSE_PANEL_X        ((SCREEN_W - PAUSE_PANEL_W) / 2)
#define PAUSE_PANEL_Y        100

#define PAUSE_HEADER_X       (PAUSE_PANEL_X + 18)
#define PAUSE_HEADER_Y       (PAUSE_PANEL_Y + 18)

#define PAUSE_ITEM_W         434
#define PAUSE_ITEM_X         (PAUSE_PANEL_X + 33)
#define PAUSE_ITEM_START_Y   (PAUSE_PANEL_Y + 90)

#define PANEL_BORDER  4

// flat fallback fill for draw_panel_box when the photo art hasn't loaded --
// same blue as the baked panel photo, at 80% opacity (204/255) so the
// translucent look holds even without the texture. shared by every menu's
// panel, this one genuinely is the same everywhere
#define PANEL_BG_COLOR RGBA8(12, 30, 56, 204)

// each menu derives its own HEADER_X/Y from its own panel macros now, only
// the shared gap between a header and whatever ribbon/banner sits on it
// stays a common constant
#define HEADER_PAD    8

// row height/font metrics/spacing are genuinely generic -- every menu's
// rows use the same look, so these stay shared rather than duplicated
// per menu
#define ITEM_H        40
#define ITEM_SPACING  46

// main pause screen only has 5 rows and room to spare in the taller panel
// now, so it keeps its own tighter spacing rather than spreading out
#define ITEM_SPACING_MAIN 36

#define HILITE_COLS      4
#define HILITE_ROWS      4
#define HILITE_FRAMES    (HILITE_COLS * HILITE_ROWS)
#define HILITE_CYCLE_US  1600000LL

// pause_highlight.png is a fixed 4x4 sheet of pre-rendered shine frames,
// each cell baked at this exact pixel size. always sample the sheet at
// this size regardless of whatever on-screen width a given menu's rows
// happen to use -- sampling at the destination width instead (as before)
// walks the source rect into neighboring cells and shows up as the
// highlight bar getting cut off/garbled on menus wider than the sheet's
// native cell
#define HILITE_CELL_W    434
#define HILITE_CELL_H    40

#define ITEM_FONT_SIZE   19
#define ITEM_TEXT_PAD    16
#define ITEM_MIN_FONT_SIZE 10

// left-aligned text with a black outline, mimics the mockup's stroked item labels
static void draw_text_outline_l(int x, int y, unsigned int size, const char *s)
{
   // 4-pass cross instead of 8-pass box, the diagonals were redundant
   // and, with anti-aliased glyphs, stacked enough overlapping alpha at
   // the edges to make the outline look thick/muddy instead of crisp
   static const int off[4][2] = {
      { 0, -1 }, { -1, 0 }, { 1, 0 }, { 0, 1 }
   };

   for (int i = 0; i < 4; i++)
      vita2d_font_draw_text(font, x + off[i][0], y + off[i][1], RGBA8(0, 0, 0, 255), size, s);

   vita2d_font_draw_text(font, x, y, RGBA8(231, 231, 231, 255), size, s);
}

// crop rect (cover fit) so a photo texture fills dst_w x dst_h with no letterboxing
static void cover_src_rect(vita2d_texture *tex, float dst_w, float dst_h,
      float *sx, float *sy, float *sw, float *sh)
{
   float tw = (float)vita2d_texture_get_width(tex);
   float th = (float)vita2d_texture_get_height(tex);
   float scale_x = dst_w / tw;
   float scale_y = dst_h / th;
   float scale = (scale_x > scale_y) ? scale_x : scale_y;

   *sw = dst_w / scale;
   *sh = dst_h / scale;
   *sx = (tw - *sw) * 0.5f;
   *sy = (th - *sh) * 0.5f;
}

// draws a box using the same photo+frame art as the pause panel, at
// whatever position/size is asked for. draw_pause_panel below is just
// this with the pause menu's own fixed geometry
static void draw_panel_box(int x, int y, int w, int h)
{
   if (pause_photo_tex)
   {
      float sx, sy, sw, sh;
      cover_src_rect(pause_photo_tex, (float)w, (float)h, &sx, &sy, &sw, &sh);
      vita2d_draw_texture_part_scale(pause_photo_tex, (float)x, (float)y,
            sx, sy, sw, sh, (float)w / sw, (float)h / sh);
   }
   else
   {
      vita2d_draw_rectangle(x, y, w, h, PANEL_BG_COLOR);
   }

   // frame art is baked at the old 500x260-ish panel size, so stretch it
   // to whatever w/h actually are instead of assuming a 1:1 pixel match.
   // a uniform stretch is a fine stand-in until there's a re-exported
   // frame asset at the new size
   if (pause_frame_tex)
   {
      float fw = (float)vita2d_texture_get_width(pause_frame_tex);
      float fh = (float)vita2d_texture_get_height(pause_frame_tex);
      float sx = (fw > 0.0f) ? (float)(w + PANEL_BORDER * 2) / fw : 1.0f;
      float sy = (fh > 0.0f) ? (float)(h + PANEL_BORDER * 2) / fh : 1.0f;

      vita2d_draw_texture_scale(pause_frame_tex, (float)(x - PANEL_BORDER),
            (float)(y - PANEL_BORDER), sx, sy);
   }
}

// full-screen cover-fit backdrop plus a dark tint over it, a no-op if the
// texture never loaded. shared by the main menu, controller settings
// screen, and the rom popups so all three stay pixel-identical instead of
// each restating the same cover-fit-and-tint block
static void draw_menu_backdrop(void)
{
   if (!menu_backdrop_tex)
      return;

   float sx, sy, sw, sh;
   cover_src_rect(menu_backdrop_tex, (float)SCREEN_W, (float)SCREEN_H, &sx, &sy, &sw, &sh);
   vita2d_draw_texture_part_scale(menu_backdrop_tex, 0.0f, 0.0f,
         sx, sy, sw, sh, (float)SCREEN_W / sw, (float)SCREEN_H / sh);
   vita2d_draw_rectangle(0, 0, SCREEN_W, SCREEN_H, RGBA8(3, 8, 18, 89));
}

// draws the panel: live photo (cover fit) + baked border/overlay frame on top
static void draw_pause_panel(void)
{
   draw_panel_box(PAUSE_PANEL_X, PAUSE_PANEL_Y, PAUSE_PANEL_W, PAUSE_PANEL_H);
}

static void draw_pause_ribbon(void)
{
   if (pause_ribbon_tex)
      vita2d_draw_texture(pause_ribbon_tex, (float)(PAUSE_HEADER_X - HEADER_PAD), (float)(PAUSE_HEADER_Y - HEADER_PAD));
}

// which two adjacent cells of the shine sheet we're between right now, and
// how far along that transition we are (0..1) -- driven by a free-running
// float clock instead of an integer frame index. every draw call
// cross-fades the two nearest baked frames on top of each other, turning
// the sheet's 16 discrete frames into a continuous sweep instead of a
// visible ~10fps step, and keeps every hilite draw call (menus, info
// panel) perfectly in phase with each other
static void hilite_frame_blend(unsigned *col_a, unsigned *row_a,
      unsigned *col_b, unsigned *row_b, float *blend)
{
   SceInt64 t_us = sceKernelGetProcessTimeWide() % HILITE_CYCLE_US;
   float frame_f = (float)t_us / (float)HILITE_CYCLE_US * (float)HILITE_FRAMES;
   unsigned frame_a = (unsigned)frame_f % HILITE_FRAMES;
   unsigned frame_b = (frame_a + 1) % HILITE_FRAMES;

   *col_a = frame_a % HILITE_COLS;
   *row_a = frame_a / HILITE_COLS;
   *col_b = frame_b % HILITE_COLS;
   *row_b = frame_b / HILITE_COLS;
   *blend = frame_f - (float)(unsigned)frame_f;
}

// draws the sprite-sheet hilite bar (with the smooth cross-fade shine
// animation above) under item i, at whatever x/w is asked for. sheet
// cells are always sampled at their native HILITE_CELL_W/H regardless of
// w, and stretched to fit via the scale args -- same stand-in stretch
// draw_panel_box already does for the frame border at a non-native size
static void draw_pause_hilite_at(int x, int item_y, int w)
{
   unsigned col_a, row_a, col_b, row_b;
   float blend;
   int y = item_y - ITEM_H / 2;
   float xs = (float)w / (float)HILITE_CELL_W;
   float ys = (float)ITEM_H / (float)HILITE_CELL_H;

   if (!pause_hilite_tex)
   {
      vita2d_draw_rectangle(x, y, w, ITEM_H, RGBA8(220, 130, 40, 200));
      return;
   }

   hilite_frame_blend(&col_a, &row_a, &col_b, &row_b, &blend);

   vita2d_draw_texture_tint_part_scale(pause_hilite_tex, (float)x, (float)y,
         (float)(col_a * HILITE_CELL_W), (float)(row_a * HILITE_CELL_H),
         (float)HILITE_CELL_W, (float)HILITE_CELL_H, xs, ys,
         RGBA8(255, 255, 255, 255));

   vita2d_draw_texture_tint_part_scale(pause_hilite_tex, (float)x, (float)y,
         (float)(col_b * HILITE_CELL_W), (float)(row_b * HILITE_CELL_H),
         (float)HILITE_CELL_W, (float)HILITE_CELL_H, xs, ys,
         RGBA8(255, 255, 255, (unsigned)(blend * 255.0f)));
}

static void draw_pause_hilite(int item_y)
{
   draw_pause_hilite_at(PAUSE_ITEM_X, item_y, PAUSE_ITEM_W);
}

// draws an item label at whatever x/w is asked for: shrinks to fit inside
// the bar, baseline centered vertically
static void draw_item_label_at(int x, int item_y, int w, const char *s)
{
   unsigned size = ITEM_FONT_SIZE;
   int max_w = w - ITEM_TEXT_PAD * 2;
   int tw = vita2d_font_text_width(font, size, s);

   if (tw > max_w && tw > 0)
   {
      size = (unsigned)((float)size * (float)max_w / (float)tw);
      if (size < ITEM_MIN_FONT_SIZE)
         size = ITEM_MIN_FONT_SIZE;
   }

   int y = item_y + (int)(size * 0.35f);
   draw_text_outline_l(x + ITEM_TEXT_PAD, y, size, s);
}

static void draw_item_label(int item_y, const char *s)
{
   draw_item_label_at(PAUSE_ITEM_X, item_y, PAUSE_ITEM_W, s);
}

// modal confirmation dialog, reused by every "are you sure"/"heads up"
// prompt in the app (main menu circle-back, pause menu quit/restart/no-
// save-found). draws over whatever's already on screen behind it -- the
// caller passes a no-arg draw_backdrop callback that redraws exactly what
// that screen normally draws each frame, since a plain c function pointer
// can't capture the caller's local selection/state the way a closure
// would. callers that need their backdrop to reflect a specific selection
// stash it in prompt_caller_sel first, see draw_pause_prompt_backdrop and
// draw_mainmenu_prompt_backdrop for the pattern
#define PROMPT_YES_NO 0
#define PROMPT_OK     1

// snapshot of whichever item was highlighted in the calling menu at the
// moment it opened a prompt, read back by that menu's own backdrop
// callback below so the redraw behind the dialog matches. only one prompt
// is ever open at a time, so one shared slot is enough
static int prompt_caller_sel;

#define PROMPT_PANEL_W        520
#define PROMPT_PANEL_H        200
#define PROMPT_PANEL_X        ((SCREEN_W - PROMPT_PANEL_W) / 2)
#define PROMPT_PANEL_Y        ((SCREEN_H - PROMPT_PANEL_H) / 2)

#define PROMPT_MSG_FONT_SIZE  20
#define PROMPT_MSG_LINE_H     26
#define PROMPT_MSG_TOP_PAD    34
#define PROMPT_MAX_LINES      4

#define PROMPT_BTN_W          170
#define PROMPT_BTN_GAP        20
#define PROMPT_BTN_BOTTOM_PAD 32

// dialog box art: a single flat asset (border baked in, unlike the pause
// panel's separate photo+frame layers) stretched to whatever w/h is
// asked for, flat fallback fill if it hasn't loaded -- same stand-in
// stretch and fallback convention draw_panel_box uses
static void draw_prompt_box(int x, int y, int w, int h)
{
   if (prompt_bg_tex)
   {
      float tw = (float)vita2d_texture_get_width(prompt_bg_tex);
      float th = (float)vita2d_texture_get_height(prompt_bg_tex);
      float sx = (tw > 0.0f) ? (float)w / tw : 1.0f;
      float sy = (th > 0.0f) ? (float)h / th : 1.0f;

      vita2d_draw_texture_scale(prompt_bg_tex, (float)x, (float)y, sx, sy);
   }
   else
   {
      vita2d_draw_rectangle(x, y, w, h, PANEL_BG_COLOR);
   }
}

// blocks until the person picks an option. type PROMPT_YES_NO shows a
// left/right yes/no pair (left/right or up/down to move, cross to pick,
// circle cancels as if "no" were picked); type PROMPT_OK shows a single
// dismiss button (cross or circle either one dismiss it). returns true
// for yes/ok, false for no/cancelled. message lines are split on '\n',
// up to PROMPT_MAX_LINES, each centered
static bool show_prompt(const char *message, int type, void (*draw_backdrop)(void))
{
   char buf[256];
   const char *lines[PROMPT_MAX_LINES];
   int line_count = 0;
   char *p;
   int sel = 0; // 0 = yes/ok, 1 = no
   SceCtrlData pad_prev;

   sfx_play(sfx_areyousure_pcm, sfx_areyousure_frames);

   snprintf(buf, sizeof(buf), "%s", message);

   p = buf;
   while (p && line_count < PROMPT_MAX_LINES)
   {
      char *nl = strchr(p, '\n');

      lines[line_count++] = p;
      if (!nl)
         break;

      *nl = '\0';
      p = nl + 1;
   }

   int cx    = PROMPT_PANEL_X + PROMPT_PANEL_W / 2;
   int msg_y = PROMPT_PANEL_Y + PROMPT_MSG_TOP_PAD;
   int btn_y = PROMPT_PANEL_Y + PROMPT_PANEL_H - PROMPT_BTN_BOTTOM_PAD;

   flush_pad(&pad_prev);

   for (;;)
   {
      unsigned pressed = poll_pressed(&pad_prev);

      if (type == PROMPT_YES_NO)
      {
         if (pressed & (SCE_CTRL_LEFT | SCE_CTRL_RIGHT | SCE_CTRL_UP | SCE_CTRL_DOWN))
         {
            sel ^= 1;
            sfx_play(sfx_choose_pcm, sfx_choose_frames);
         }
         if (pressed & SCE_CTRL_CROSS)
         {
            sfx_play(sfx_select_pcm, sfx_select_frames);
            return sel == 0;
         }
         if (pressed & SCE_CTRL_CIRCLE)
            return false;
      }
      else
      {
         if (pressed & (SCE_CTRL_CROSS | SCE_CTRL_CIRCLE))
         {
            sfx_play(sfx_select_pcm, sfx_select_frames);
            return true;
         }
      }

      vita2d_start_drawing();
      vita2d_clear_screen();

      if (draw_backdrop)
         draw_backdrop();

      // slight dim over whatever's behind, on top of that screen's own
      // dim/tint if it has one -- the dialog needs to read as clearly
      // frontmost no matter which menu opened it
      vita2d_draw_rectangle(0, 0, SCREEN_W, SCREEN_H, RGBA8(0, 0, 0, 140));

      draw_prompt_box(PROMPT_PANEL_X, PROMPT_PANEL_Y, PROMPT_PANEL_W, PROMPT_PANEL_H);

      for (int i = 0; i < line_count; i++)
      {
         int lw = vita2d_font_text_width(font, PROMPT_MSG_FONT_SIZE, lines[i]);
         draw_text_outline_l(cx - lw / 2, msg_y + i * PROMPT_MSG_LINE_H, PROMPT_MSG_FONT_SIZE, lines[i]);
      }

      if (type == PROMPT_YES_NO)
      {
         int total_w = PROMPT_BTN_W * 2 + PROMPT_BTN_GAP;
         int yes_x   = cx - total_w / 2;
         int no_x    = yes_x + PROMPT_BTN_W + PROMPT_BTN_GAP;
         int yw      = vita2d_font_text_width(font, ITEM_FONT_SIZE, "Yes");
         int nw      = vita2d_font_text_width(font, ITEM_FONT_SIZE, "No");

         draw_pause_hilite_at(sel == 0 ? yes_x : no_x, btn_y, PROMPT_BTN_W);

         draw_text_outline_l(yes_x + (PROMPT_BTN_W - yw) / 2,
               btn_y + (int)(ITEM_FONT_SIZE * 0.35f), ITEM_FONT_SIZE, "Yes");
         draw_text_outline_l(no_x + (PROMPT_BTN_W - nw) / 2,
               btn_y + (int)(ITEM_FONT_SIZE * 0.35f), ITEM_FONT_SIZE, "No");
      }
      else
      {
         int ok_x = cx - PROMPT_BTN_W / 2;
         int ow   = vita2d_font_text_width(font, ITEM_FONT_SIZE, "OK");

         draw_pause_hilite_at(ok_x, btn_y, PROMPT_BTN_W);
         draw_text_outline_l(ok_x + (PROMPT_BTN_W - ow) / 2,
               btn_y + (int)(ITEM_FONT_SIZE * 0.35f), ITEM_FONT_SIZE, "OK");
      }

      vita2d_end_drawing();
      vita2d_swap_buffers();
   }
}

// filename (no extension) of whichever exact rom variant is actually
// loaded for the current play session, set by set_current_rom_stem right
// before boot. only the quick-save slot keys off this, covers and the
// controller/visual config stay pinned to base_id instead, so a save
// state never silently gets treated as compatible across regions
static char current_rom_stem[64];

static void set_current_rom_stem(const char *filename)
{
   size_t len = strlen(filename);
   size_t base_len = (len > 4) ? len - 4 : len;

   memset(current_rom_stem, 0, sizeof(current_rom_stem));
   snprintf(current_rom_stem, sizeof(current_rom_stem), "%.*s", (int)base_len, filename);
}

// builds the quick-save path for the currently loaded game, one slot per
// exact rom variant
static void quick_save_path(char *out, size_t out_sz)
{
   snprintf(out, out_sz, "%s/%s.qsv", SAVE_DIR, current_rom_stem);
}

// appends a timestamped line to the debug log. plain append mode so a
// save-then-close-then-load sequence across two app launches shows up as
// one continuous trace instead of getting wiped between sessions
static void debug_log(const char *fmt, ...)
{
   char path[192];
   FILE *f;
   va_list ap;

   snprintf(path, sizeof(path), "%s/debug_log.txt", SAVE_DIR);
   f = fopen(path, "a");
   if (!f)
      return;

   fprintf(f, "[%lld] ", (long long)sceKernelGetProcessTimeWide());

   va_start(ap, fmt);
   vfprintf(f, fmt, ap);
   va_end(ap);

   fprintf(f, "\n");
   fclose(f);
}

// how many throwaway core_api.run() frames to execute right after
// load_game(), enough to burn past the boot logos/warnings/system
// check loop on a cold boot, and also enough that cps2's boot sequence
// is far enough along for a savestate to apply cleanly on auto-resume
#define CORE_WARMUP_FRAMES 400

// runs the core for a fixed number of frames so the boot-logo/warning
// screens are skipped on every load. called once in main(), right after
// core_api.load_game() succeeds, always with CORE_WARMUP_FRAMES, but
// the frame count stays a parameter so callers never hardcode it themselves
static void warm_up_core(int frames)
{
   debug_log("WARMUP: running %d frames", frames);

   core_warming_up = true;
   for (int i = 0; i < frames; i++)
      core_api.run();
   core_warming_up = false;
   needs_var_update = true;

   // whatever was held going into/through warmup (most likely start,
   // from picking the game in our own menu) shouldn't count toward the
   // port-0 start bleed gate - real gameplay starts counting fresh
   start_hold_frames = 0;

   debug_log("WARMUP: done");
}

// grabs a serialized state blob from the core and writes it straight to disk
static void quick_save_do(void)
{
   char path[192];
   size_t size;
   void *buf;
   FILE *f;

   if (!game_loaded || !core_api.serialize_size || !core_api.serialize)
      return;

   size = core_api.serialize_size();
   debug_log("SAVE: serialize_size=%u", (unsigned)size);
   if (!size)
      return;

   buf = malloc(size);
   if (!buf)
      return;

   bool ser_ok = core_api.serialize(buf, size);
   debug_log("SAVE: serialize() returned %s", ser_ok ? "true" : "false");

   if (ser_ok)
   {
      quick_save_path(path, sizeof(path));
      f = fopen(path, "wb");
      if (f)
      {
         size_t written = fwrite(buf, 1, size, f);
         fclose(f);
         debug_log("SAVE: wrote %u bytes to %s", (unsigned)written, path);
      }
      else
         debug_log("SAVE: fopen for write failed: %s", path);
   }

   free(buf);
}

// whether a quick-save file already exists for the currently loaded rom,
// so the pause menu can tell "load quick save" apart from "nothing to
// load" before it tries and silently no-ops
static bool quick_save_exists(void)
{
   char path[192];

   quick_save_path(path, sizeof(path));
   return file_exists(path);
}

// reads the saved blob back off disk and hands it to the core, no-op if
// there's nothing saved yet for this game
static void quick_load_do(void)
{
   char path[192];
   long fsize;
   size_t size, expected_size, read_bytes;
   void *buf;
   FILE *f;

   if (!game_loaded || !core_api.unserialize)
      return;

   quick_save_path(path, sizeof(path));
   f = fopen(path, "rb");
   if (!f)
   {
      debug_log("LOAD: no save file at %s", path);
      return;
   }

   expected_size = core_api.serialize_size ? core_api.serialize_size() : 0;
   debug_log("LOAD: expected serialize_size=%u", (unsigned)expected_size);

   fseek(f, 0, SEEK_END);
   fsize = ftell(f);
   fseek(f, 0, SEEK_SET);

   debug_log("LOAD: actual file size=%ld (%s)", fsize, path);

   if (fsize <= 0)
   {
      fclose(f);
      return;
   }

   size = (size_t)fsize;
   buf = malloc(size);
   if (!buf)
   {
      fclose(f);
      debug_log("LOAD: malloc(%u) failed", (unsigned)size);
      return;
   }

   read_bytes = fread(buf, 1, size, f);
   if (read_bytes == size)
   {
      bool unser_ok = core_api.unserialize(buf, size);
      debug_log("LOAD: unserialize() returned %s", unser_ok ? "true" : "false");
   }
   else
      debug_log("LOAD: fread got %u of %u bytes", (unsigned)read_bytes, (unsigned)size);

   fclose(f);
   free(buf);
}

// single entry point for "auto-resume": the core is already warmed up
// by main() before this runs, so if rom_stem has a quick save on disk
// this just applies it, dropping the player straight into their last
// save instead of the fresh-boot state warm_up_core() left them at.
// no-op (and clearly logged as such) if there's nothing to resume from
static void attempt_auto_resume(const char *rom_stem)
{
   char path[192];

   snprintf(path, sizeof(path), "%s/%s.qsv", SAVE_DIR, rom_stem);

   if (!file_exists(path))
   {
      debug_log("AUTO-RESUME: no quick save for \"%s\", starting fresh", rom_stem);
      return;
   }

   debug_log("AUTO-RESUME: quick save found for \"%s\" at %s", rom_stem, path);

   quick_load_do();
}

// (re)loads overlay_tex to match current_overlay, freeing whatever was
// loaded before. shared by the pause-menu wallpaper toggle and config load
// below so both stay in sync with current_overlay
static void reload_overlay_texture(void)
{
   if (overlay_tex)
   {
      vita2d_wait_rendering_done();
      vita2d_free_texture(overlay_tex);
      overlay_tex = NULL;
   }

   if (current_overlay > 0)
   {
      char overlay_path[64];
      snprintf(overlay_path, sizeof(overlay_path),
            "ux0:data/NaohAC/overlays/%d.png", current_overlay);
      overlay_tex = vita2d_load_PNG_file(overlay_path);
   }
}

// on-disk layout for the persisted display/sound settings. version lets a
// future build tell an old-format file apart from a fresh one instead of
// trusting garbage bytes. v1->v2 bumped for wallpaper_opacity (a v1 file is
// shorter than sizeof(game_config_t), so it just short-reads and falls
// through to defaults). v2->v3 bumped because ASPECT_ORIGINAL's removal
// renumbered the aspect_mode enum, so a v2 file's saved aspect_mode is
// still in-bounds under the new numbering, just meaning something else now
// (old 4:3=2 would silently read back as 5:4), so this one needs a hard
// reject rather than relying on a size mismatch. v3->v4 bumped for the 6
// new button-mapping fields, purely additive, but kept the same
// hard-reject convention as the others rather than leaning on the size
// check alone
#define GAME_CONFIG_VERSION 4

typedef struct {
   int version;
   int wallpaper;
   int wallpaper_opacity;
   int wallpaper_fx;
   int display_filter;
   int aspect_mode;
   int map_cross;
   int map_circle;
   int map_square;
   int map_triangle;
   int map_l1;
   int map_r1;
} game_config_t;

// keyed off base_id rather than the exact rom filename, so switching a
// game's region never orphans its saved controller/visual settings.
// mvsc.zip and mvscj.zip both land on the same "mvsc.cfg". takes an
// explicit game index rather than always assuming current_game_idx since
// the controller settings screen edits a rom's mapping straight from the
// main menu, before it's actually loaded
static void game_config_path_for(unsigned idx, char *out, size_t out_sz)
{
   snprintf(out, out_sz, "%s/%s.cfg", CONFIG_DIR, known_games[games[idx].known_idx].base_id);
}

static void game_config_path(char *out, size_t out_sz)
{
   game_config_path_for(current_game_idx, out, out_sz);
}

// stock arcade layouts, applied whenever a rom has no saved mapping yet
// (or the user hits "reset to defaults" in the controller settings screen)
static void apply_default_mapping(core_kind_t core)
{
   if (core == CORE_KIND_FBA)
   {
      // stock cps2 6-button spread, one action per button
      map_square   = RETRO_DEVICE_ID_JOYPAD_X; // light punch
      map_triangle = RETRO_DEVICE_ID_JOYPAD_Y; // medium punch
      map_r1       = RETRO_DEVICE_ID_JOYPAD_L; // heavy punch
      map_cross    = RETRO_DEVICE_ID_JOYPAD_B; // light kick
      map_circle   = RETRO_DEVICE_ID_JOYPAD_A; // medium kick
      map_l1       = RETRO_DEVICE_ID_JOYPAD_R; // heavy kick
   }
   else
   {
      // punisher only needs two inputs, square doubles as attack, cross
      // as jump, everything else sits idle. mame2000 assigns its own
      // button1/button2 to retropad b/a here, not y/x, so square must map
      // to b and cross to a or the core reads them as the wrong action
      map_square   = RETRO_DEVICE_ID_JOYPAD_B; // punch, acts as attack
      map_cross    = RETRO_DEVICE_ID_JOYPAD_A; // jump
      map_triangle = REMAP_UNASSIGNED;
      map_circle   = REMAP_UNASSIGNED;
      map_l1       = REMAP_UNASSIGNED;
      map_r1       = REMAP_UNASSIGNED;
   }
}

// loads just rom idx's saved button mapping into the live map_* globals,
// falling back to its core's stock arcade layout if there's no cfg yet
// (or it predates this feature). used both right before gameplay starts
// and by the controller settings screen when it's opened from the main
// menu for a game that isn't loaded at all yet
static void load_button_mapping(unsigned idx)
{
   char path[192];
   game_config_t cfg;
   FILE *f;

   apply_default_mapping(games[idx].core);

   game_config_path_for(idx, path, sizeof(path));

   f = fopen(path, "rb");
   if (!f)
      return;

   if (fread(&cfg, sizeof(cfg), 1, f) == 1 && cfg.version == GAME_CONFIG_VERSION)
   {
      core_kind_t core = games[idx].core;
      if (remap_target_valid(core, cfg.map_cross))    map_cross    = cfg.map_cross;
      if (remap_target_valid(core, cfg.map_circle))   map_circle   = cfg.map_circle;
      if (remap_target_valid(core, cfg.map_square))   map_square   = cfg.map_square;
      if (remap_target_valid(core, cfg.map_triangle)) map_triangle = cfg.map_triangle;
      if (remap_target_valid(core, cfg.map_l1))       map_l1       = cfg.map_l1;
      if (remap_target_valid(core, cfg.map_r1))       map_r1       = cfg.map_r1;
   }

   fclose(f);
}

// writes just the mapping fields back out for rom idx, preserving whatever
// visual settings are already on disk for it. the main menu has no idea
// what that rom's live wallpaper/filter state should be (those globals
// might still be holding whatever the last-played game left behind), so
// this reads the existing file first rather than stomping it. a rom
// that's never been configured at all just gets fresh defaults
static void save_button_mapping(unsigned idx)
{
   char path[192];
   game_config_t cfg;
   FILE *f;
   bool have_existing = false;

   game_config_path_for(idx, path, sizeof(path));

   f = fopen(path, "rb");
   if (f)
   {
      have_existing = (fread(&cfg, sizeof(cfg), 1, f) == 1 && cfg.version == GAME_CONFIG_VERSION);
      fclose(f);
   }

   if (!have_existing)
   {
      cfg.wallpaper         = 0;
      cfg.wallpaper_opacity = WALLPAPER_OPACITY_MAX;
      cfg.wallpaper_fx      = WALLPAPER_FX_OFF;
      cfg.display_filter    = DISPLAY_FILTER_OFF;
      cfg.aspect_mode       = ASPECT_FULLSCREEN;
   }

   cfg.version      = GAME_CONFIG_VERSION;
   cfg.map_cross    = map_cross;
   cfg.map_circle   = map_circle;
   cfg.map_square   = map_square;
   cfg.map_triangle = map_triangle;
   cfg.map_l1       = map_l1;
   cfg.map_r1       = map_r1;

   f = fopen(path, "wb");
   if (!f)
      return;

   fwrite(&cfg, sizeof(cfg), 1, f);
   fclose(f);
}

// pulls this game's saved wallpaper/effects/filter/size into the live
// globals, called right after a game finishes loading. no file yet (first
// time this rom has been played) just leaves the current in-memory
// settings alone, same for a corrupt or old-version file
static void load_game_config(void)
{
   char path[192];
   game_config_t cfg;
   FILE *f;

   game_config_path(path, sizeof(path));

   f = fopen(path, "rb");
   if (f)
   {
      if (fread(&cfg, sizeof(cfg), 1, f) == 1 && cfg.version == GAME_CONFIG_VERSION)
      {
         if (cfg.wallpaper >= 0 && cfg.wallpaper < WALLPAPER_COUNT)
            current_overlay = cfg.wallpaper;
         if (cfg.wallpaper_opacity >= 1 && cfg.wallpaper_opacity <= WALLPAPER_OPACITY_MAX)
            wallpaper_opacity = cfg.wallpaper_opacity;
         if (cfg.wallpaper_fx >= 0 && cfg.wallpaper_fx < WALLPAPER_FX_COUNT)
            wallpaper_fx = cfg.wallpaper_fx;
         if (cfg.display_filter >= 0 && cfg.display_filter < DISPLAY_FILTER_COUNT)
            display_filter = cfg.display_filter;
         if (cfg.aspect_mode >= 0 && cfg.aspect_mode < ASPECT_COUNT)
            aspect_mode = cfg.aspect_mode;
      }

      fclose(f);
   }

   // button mapping has its own loader (with its own core-specific
   // defaults) since the controller settings screen needs to call it too,
   // straight from the main menu before this game is even loaded
   load_button_mapping(current_game_idx);

   // wallpaper index above may have just changed, get the actual texture
   // loaded up to match before the first frame draws. the filter bake is
   // freed here too, same reasoning -- this runs before run_core ever
   // opens a scene, so it's the safe, outside-the-scene place to drop
   // whatever the previous game session left cached. otherwise a filter
   // baked at the last game's viewport size sails through into this
   // game's first blit_frame() still tagged valid, and the mismatch only
   // gets caught (and freed) from inside that frame's own scene
   reload_overlay_texture();
   free_filter_tex();
}

// writes the live settings out for whichever game is currently loaded,
// display/sound options plus whatever button mapping is currently active
// for it. called right after any of them changes in the options screen
static void save_game_config(void)
{
   char path[192];
   game_config_t cfg;
   FILE *f;

   if (!game_loaded)
      return;

   cfg.version           = GAME_CONFIG_VERSION;
   cfg.wallpaper         = current_overlay;
   cfg.wallpaper_opacity = wallpaper_opacity;
   cfg.wallpaper_fx      = wallpaper_fx;
   cfg.display_filter    = display_filter;
   cfg.aspect_mode       = aspect_mode;
   cfg.map_cross         = map_cross;
   cfg.map_circle        = map_circle;
   cfg.map_square        = map_square;
   cfg.map_triangle      = map_triangle;
   cfg.map_l1            = map_l1;
   cfg.map_r1            = map_r1;

   game_config_path(path, sizeof(path));

   f = fopen(path, "wb");
   if (!f)
      return;

   fwrite(&cfg, sizeof(cfg), 1, f);
   fclose(f);
}

// controller settings screen and the pause menu's own bottom bar are
// defined further down (the bottom bar needs BAR_Y/ICON_SIZE/
// draw_ps_prompt, and controller settings reuses the game-carousel draw
// code, neither of which exist yet at this point in the file) -- forward
// declared here since pause_menu wants to call both before that point
static void controller_settings_menu(unsigned game_idx, void (*draw_backdrop)(void));
static void draw_pause_bottom_bar(int sel);

// paused game frame plus the dim tint over it, factored out of pause_menu
// so a prompt or the controller settings screen opened from the pause
// menu can redraw the exact same thing as their own backdrop
static void draw_paused_gameplay_dimmed(void)
{
   if (frame_tex && fb_w && fb_h)
   {
      float dx, dy, dw, dh;
      compute_dest_rect(fb_w, fb_h, &dx, &dy, &dw, &dh);

      draw_gameplay_layer(dx, dy, dw, dh);
   }

   // dim the paused game frame behind the panel
   vita2d_draw_rectangle(0, 0, SCREEN_W, SCREEN_H, RGBA8(0, 0, 0, 180));
}

// the pause menu's main-screen row list (panel + ribbon are drawn
// separately by the caller), factored out so both the normal per-frame
// draw and the prompt-backdrop callback below can share it instead of
// keeping two copies of the same loop in sync
static void draw_pause_main_list(int sel)
{
   int item_y = PAUSE_ITEM_START_Y;

   for (int i = 0; i < PAUSE_ITEM_COUNT; i++)
   {
      if (i == sel)
         draw_pause_hilite(item_y);

      draw_item_label(item_y, pause_items[i].label);
      item_y += ITEM_SPACING_MAIN;
   }
}

// backdrop callback for a prompt opened from the pause menu's main
// screen (quit/restart/no-save-found are all main-screen items, so this
// never needs to handle the options screen). prompt_caller_sel is
// stashed by pause_menu right before it calls show_prompt
static void draw_pause_prompt_backdrop(void)
{
   draw_paused_gameplay_dimmed();
   draw_pause_panel();
   draw_pause_ribbon();
   draw_pause_main_list(prompt_caller_sel);
   draw_pause_bottom_bar(prompt_caller_sel);
}

// backdrop callback for controller settings when opened from the pause
// menu instead of the game carousel: just the paused frame, dimmed --
// controller_settings_menu draws its own panel/rows on top of whatever
// backdrop it's handed, same as it always has
static void draw_pause_ctrlcfg_backdrop(void)
{
   draw_paused_gameplay_dimmed();
}

static bool pause_menu(void)
{
   int screen = PAUSE_SCREEN_MAIN;
   int sel = PAUSE_RESUME;
   int opt_sel = OPT_WALLPAPER;
   SceCtrlData pad_prev;

   flush_pad(&pad_prev);

   for (;;)
   {
      unsigned pressed = poll_pressed(&pad_prev);

      if (screen == PAUSE_SCREEN_MAIN)
      {
         if (pressed & SCE_CTRL_UP)
            sel = (sel == 0) ? PAUSE_ITEM_COUNT - 1 : sel - 1;
         if (pressed & SCE_CTRL_DOWN)
            sel = (sel + 1) % PAUSE_ITEM_COUNT;
         if (pressed & (SCE_CTRL_UP | SCE_CTRL_DOWN))
            sfx_play(sfx_choose_pcm, sfx_choose_frames);

         if (pressed & SCE_CTRL_CROSS)
         {
            sfx_play(sfx_select_pcm, sfx_select_frames);
            if (sel == PAUSE_RESUME)
               return false;
            if (sel == PAUSE_CTRL_SETTINGS)
            {
               controller_settings_menu(current_game_idx, draw_pause_ctrlcfg_backdrop);
               flush_pad(&pad_prev);
            }
            if (sel == PAUSE_OPTIONS)
            {
               screen = PAUSE_SCREEN_OPTIONS;
               opt_sel = OPT_WALLPAPER;
            }
            if (sel == PAUSE_LOAD_QSAVE)
            {
               if (quick_save_exists())
               {
                  quick_load_do();
                  return false;
               }

               prompt_caller_sel = sel;
               show_prompt("No quick save data found for this game.",
                     PROMPT_OK, draw_pause_prompt_backdrop);
               flush_pad(&pad_prev);
            }
            if (sel == PAUSE_SAVE_QSAVE)
            {
               quick_save_do();
               return false;
            }
            if (sel == PAUSE_RESTART)
            {
               prompt_caller_sel = sel;
               if (show_prompt("Restart the game?\nAll unsaved progress will be lost.",
                     PROMPT_YES_NO, draw_pause_prompt_backdrop))
               {
                  // retro_reset reboots the loaded rom in place, core stays
                  // initialized/loaded, no menu round-trip needed
                  if (core_api.reset)
                     core_api.reset();
                  return false;
               }
               flush_pad(&pad_prev);
            }
            if (sel == PAUSE_EXIT)
            {
               prompt_caller_sel = sel;
               if (show_prompt("Exit the game?", PROMPT_YES_NO, draw_pause_prompt_backdrop))
                  return true;
               flush_pad(&pad_prev);
            }
         }

         // same "back" affordance the bottom bar's o prompt advertises --
         // backs out to the game, same as picking resume
         if (pressed & SCE_CTRL_CIRCLE)
         {
            sfx_play(sfx_back_pcm, sfx_back_frames);
            return false;
         }
      }
      else
      {
         if (pressed & SCE_CTRL_UP)
            opt_sel = (opt_sel == 0) ? OPT_ITEM_COUNT - 1 : opt_sel - 1;
         if (pressed & SCE_CTRL_DOWN)
            opt_sel = (opt_sel + 1) % OPT_ITEM_COUNT;
         if (pressed & (SCE_CTRL_UP | SCE_CTRL_DOWN))
            sfx_play(sfx_choose_pcm, sfx_choose_frames);

         if (pressed & SCE_CTRL_CROSS)
         {
            sfx_play(sfx_select_pcm, sfx_select_frames);
            if (opt_sel == OPT_WALLPAPER)
            {
               current_overlay = (current_overlay + 1) % WALLPAPER_COUNT;
               reload_overlay_texture();
            }
            else if (opt_sel == OPT_WALLPAPER_OPACITY)
            {
               wallpaper_opacity++;
               if (wallpaper_opacity > WALLPAPER_OPACITY_MAX)
                  wallpaper_opacity = 1;
            }
            else if (opt_sel == OPT_WALLPAPER_FX)
               wallpaper_fx = (wallpaper_fx + 1) % WALLPAPER_FX_COUNT;
            else if (opt_sel == OPT_FILTER)
            {
               display_filter = (display_filter + 1) % DISPLAY_FILTER_COUNT;

               // free the stale bake right here, before this loop's own
               // vita2d_start_drawing() below opens a scene -- draw_display_filter
               // would otherwise catch the mode mismatch itself on the very next
               // draw and free it from inside that scene instead, and
               // free_filter_tex's wait_rendering_done() call is not valid mid-scene
               // (see free_filter_tex). this is the exact same rule
               // reload_overlay_texture already follows for the wallpaper toggle
               // just above
               free_filter_tex();
            }
            else if (opt_sel == OPT_SIZE)
            {
               aspect_mode = (aspect_mode + 1) % ASPECT_COUNT;

               // aspect mode feeds compute_dest_rect, which changes the w/h
               // draw_display_filter bakes against, so this needs the same
               // eager, outside-the-scene free as the filter toggle above
               free_filter_tex();
            }

            // any of the four settings above just changed, persist all of
            // them for this game right away rather than trying to track
            // which one moved
            save_game_config();
         }

         if (pressed & SCE_CTRL_CIRCLE)
         {
            sfx_play(sfx_back_pcm, sfx_back_frames);
            screen = PAUSE_SCREEN_MAIN;
         }
      }

      vita2d_start_drawing();
      vita2d_clear_screen();

      draw_paused_gameplay_dimmed();

      draw_pause_panel();
      draw_pause_ribbon();

      if (screen == PAUSE_SCREEN_MAIN)
      {
         draw_pause_main_list(sel);
         draw_pause_bottom_bar(sel);
      }
      else
      {
         char line[80];
         int item_y = PAUSE_ITEM_START_Y;

         for (int i = 0; i < OPT_ITEM_COUNT; i++)
         {
            if (i == OPT_WALLPAPER)
            {
               if (current_overlay == 0)
                  snprintf(line, sizeof(line), "Wallpaper: Off");
               else
                  snprintf(line, sizeof(line), "Wallpaper: Type %c",
                        (char)('A' + current_overlay - 1));
            }
            else if (i == OPT_WALLPAPER_OPACITY)
            {
               // wallpaper_opacity is always 1-10, percent is just that
               // times 10, no division/rounding involved. named here as
               // its own variable rather than inlined so it's obvious at
               // a glance what's landing in the format string
               int opacity_pct = wallpaper_opacity * 10;
               snprintf(line, sizeof(line), "Wallpaper Opacity: %d%%", opacity_pct);
            }
            else if (i == OPT_WALLPAPER_FX)
               snprintf(line, sizeof(line), "Wallpaper Effects (recommended for Vita1K): %s",
                     wallpaper_fx_labels[wallpaper_fx]);
            else if (i == OPT_FILTER)
               snprintf(line, sizeof(line), "Display Filter: %s", display_filter_labels[display_filter]);
            else
            {
               const char *val_str;

               switch (aspect_mode)
               {
                  case ASPECT_FULLSCREEN: val_str = "FULLSCREEN"; break;
                  case ASPECT_4_3:        val_str = "4:3";        break;
                  default:                val_str = "5:4";        break;
               }

               snprintf(line, sizeof(line), "Display Size: %s", val_str);
            }

            if (i == opt_sel)
               draw_pause_hilite(item_y);

            draw_item_label(item_y, line);
            item_y += ITEM_SPACING;
         }
      }

      vita2d_end_drawing();
      vita2d_swap_buffers();
   }
}

// select-game menu layout, 1:1-ish with the html/canvas mockup
#define GHDR_X        20
#define GHDR_Y        22
#define GHDR_PAD      8
#define GHDR_TRIM_H   3
#define GHDR_TRIM_GAP 3

#define INFO_X        40
#define INFO_Y        342
#define INFO_W        880
#define INFO_H        96
#define INFO_BORDER   4
#define INFO_ROW_H    (INFO_H / 3)

#define BAR_Y         462
#define BAR_H         34
#define ICON_SIZE     28

#define MARQUEE_GAP    60      // px between tiled items/copies
#define MARQUEE_SPEED  40.0f   // px/sec, scrolls left
#define MARQUEE_FONT_SIZE 22   // bumped up to better match the 28px icons

static float marquee_offset = 0.0f;   // global x-offset driving the bottom bar scroll

// stretches the pause-menu hilite sprite to fit an arbitrary box (info
// rows), same native-cell sampling + cross-fade as draw_pause_hilite_at
static void draw_hilite_stretched(int x, int y, int w, int h)
{
   unsigned col_a, row_a, col_b, row_b;
   float blend;
   float xs = (float)w / (float)HILITE_CELL_W;
   float ys = (float)h / (float)HILITE_CELL_H;

   if (!pause_hilite_tex)
      return;

   hilite_frame_blend(&col_a, &row_a, &col_b, &row_b, &blend);

   vita2d_draw_texture_tint_part_scale(pause_hilite_tex, (float)x, (float)y,
         (float)(col_a * HILITE_CELL_W), (float)(row_a * HILITE_CELL_H),
         (float)HILITE_CELL_W, (float)HILITE_CELL_H, xs, ys,
         RGBA8(255, 255, 255, 255));

   vita2d_draw_texture_tint_part_scale(pause_hilite_tex, (float)x, (float)y,
         (float)(col_b * HILITE_CELL_W), (float)(row_b * HILITE_CELL_H),
         (float)HILITE_CELL_W, (float)HILITE_CELL_H, xs, ys,
         RGBA8(255, 255, 255, (unsigned)(blend * 255.0f)));
}

static void draw_menu_header(void)
{
   if (menu_header_tex)
   {
      float hdr_x = (float)(GHDR_X - GHDR_PAD);
      float hdr_y = (float)(GHDR_Y - GHDR_PAD);
      float hdr_w = (float)vita2d_texture_get_width(menu_header_tex);

      vita2d_draw_texture(menu_header_tex, hdr_x, hdr_y);

      // thin gold trim strip, floating just above the red header bar
      vita2d_draw_rectangle(hdr_x, hdr_y - (float)(GHDR_TRIM_GAP + GHDR_TRIM_H), hdr_w, (float)GHDR_TRIM_H,
            RGBA8(212, 175, 55, 255));
   }
}

static float f_lerp(float a, float b, float t) { return a + (b - a) * t; }

static float ease_in_out_cubic(float x)
{
   return x < 0.5f ? 4.0f * x * x * x : 1.0f - powf(-2.0f * x + 2.0f, 3.0f) / 2.0f;
}

// interpolated rect for a fractional track position, wraps around the 6 slots
static void track_rect_at(float pos, float *ox, float *oy, float *ow, float *oh)
{
   float tl = fmodf(pos, (float)KNOWN_COUNT);
   int base, a, b;
   float frac;

   if (tl < 0.0f)
      tl += (float)KNOWN_COUNT;

   base = (int)tl;
   frac = tl - (float)base;
   a = track_order[base];
   b = track_order[(base + 1) % KNOWN_COUNT];

   *ox = f_lerp((float)slot_x[a], (float)slot_x[b], frac);
   *oy = f_lerp((float)slot_y[a], (float)slot_y[b], frac);
   *ow = f_lerp((float)slot_w[a], (float)slot_w[b], frac);
   *oh = f_lerp((float)slot_h[a], (float)slot_h[b], frac);
}

// starts a shift, blocked while one is already running (same guard as the js version)
static void rotate_carousel(int dir, int *sel)
{
   anim_from_sel = *sel;
   *sel = (*sel + dir + (int)game_count) % (int)game_count;
   anim_dir = dir;
   anim_active = 1;
   anim_start_us = sceKernelGetProcessTimeWide();
}

// dim tint applied to every cover except the one under the spotlight,
// roughly -40% brightness (0.6x per channel) so the lit cover reads as
// sharply singled out rather than just "a bit brighter than the rest"
static const unsigned SLOT_DIM_TINT = RGBA8(153, 153, 153, 255);

// draws one staggered game card. 'is_lit' means this cover is the one
// currently passing through the front-center slot under the fixed
// spotlight (see draw_carousel_spotlight) -- it draws at full brightness,
// everything else gets tinted down so the light actually has contrast to
// pop out of
static void draw_game_slot(unsigned i, float x, float y, float w, float h, bool is_lit)
{
   vita2d_texture *tex = cover_tex[games[i].known_idx];
   if (tex)
   {
      float tw = (float)vita2d_texture_get_width(tex);
      float th = (float)vita2d_texture_get_height(tex);

      if (is_lit)
         vita2d_draw_texture_scale(tex, x, y, w / tw, h / th);
      else
         vita2d_draw_texture_tint_scale(tex, x, y, w / tw, h / th, SLOT_DIM_TINT);
   }
   else
   {
      vita2d_draw_rectangle(x, y, w, h, RGBA8(60, 60, 60, 255));
      vita2d_font_draw_text(font, (int)(x + 8), (int)(y + h / 2.0f),
            RGBA8(220, 220, 220, 255), (unsigned int)(FONT_BASE_PX * 0.7f),
            known_games[games[i].known_idx].base_id);
   }
}

// soft white glow baked/centered on the fixed screen coordinates of the
// front-center physical slot (slot 4 in slot_x/y/w/h) -- a stationary
// stage light that stays put while games rotate through it, rather than
// something that tracks any particular card. sized a bit larger than the
// box on both axes so it spills slightly past the art's edges instead of
// stopping dead at the border. one scaled texture blit, no per-frame
// rasterization
static void draw_carousel_spotlight(void)
{
   if (!spotlight_tex)
      return;

   float x = (float)slot_x[FRONT_SLOT_IDX];
   float y = (float)slot_y[FRONT_SLOT_IDX];
   float w = (float)slot_w[FRONT_SLOT_IDX];
   float h = (float)slot_h[FRONT_SLOT_IDX];

   const float spill = 1.18f; // ~18% larger than the box on each axis
   float sw = w * spill;
   float sh = h * spill;
   float sx = x + w * 0.5f - sw * 0.5f;
   float sy = y + h * 0.5f - sh * 0.5f;
   float tex_size = (float)SPOTLIGHT_TEX_SIZE;

   vita2d_draw_texture_tint_scale(spotlight_tex, sx, sy,
         sw / tex_size, sh / tex_size, RGBA8(255, 255, 255, 130));
}

// one info-panel row: label left, value centered, hilite bar + arrows if selectable and focused
static void draw_info_row(int row, const char *label, const char *value, bool sel)
{
   int ry  = INFO_Y + row * INFO_ROW_H;
   int mid = ry + INFO_ROW_H / 2 + (int)(16 * 0.35f);

   if (sel)
      draw_hilite_stretched(INFO_X + 16, ry + 4, INFO_W - 32, INFO_ROW_H - 8);

   draw_text_outline_l(INFO_X + 24, mid, 16, label);

   int vw = vita2d_font_text_width(font, 16, value);
   int vx = INFO_X + INFO_W / 2 - vw / 2;

   if (sel)
   {
      // small arrows hint the value is toggleable with left/right
      vita2d_font_draw_text(font, vx - 24, mid, RGBA8(58, 42, 16, 255), 16, "<");
      vita2d_font_draw_text(font, vx + vw + 10, mid, RGBA8(58, 42, 16, 255), 16, ">");
      vita2d_font_draw_text(font, vx, mid, RGBA8(26, 26, 26, 255), 16, value);
   }
   else
   {
      draw_text_outline_l(vx, mid, 16, value);
   }
}

static void draw_info_panel(int sel, menu_focus_t focus)
{
   if (pause_photo_tex)
   {
      float sx, sy, sw, sh;
      cover_src_rect(pause_photo_tex, (float)INFO_W, (float)INFO_H, &sx, &sy, &sw, &sh);
      vita2d_draw_texture_part_scale(pause_photo_tex, (float)INFO_X, (float)INFO_Y,
            sx, sy, sw, sh, (float)INFO_W / sw, (float)INFO_H / sh);
   }
   if (info_frame_tex)
      vita2d_draw_texture(info_frame_tex, (float)(INFO_X - INFO_BORDER), (float)(INFO_Y - INFO_BORDER));

   draw_info_row(0, "Game", games[sel].label, focus == MENU_FOCUS_GAMES);
   draw_info_row(1, "Game Version", region_labels[game_region[sel]], focus == MENU_FOCUS_VERSION);
   draw_info_row(2, "Player", player_port_labels[game_player_port[sel]], focus == MENU_FOCUS_PLAYER);
}

static void draw_ps_prompt(vita2d_texture *tex, int x, int cy, const char *label)
{
   if (tex)
   {
      float s = (float)ICON_SIZE / (float)vita2d_texture_get_width(tex);
      vita2d_draw_texture_scale(tex, (float)x, (float)(cy - ICON_SIZE / 2), s, s);
   }
   vita2d_font_draw_text(font, x + ICON_SIZE + 8, cy + (int)(MARQUEE_FONT_SIZE * 0.35f),
         RGBA8(220, 220, 220, 255), MARQUEE_FONT_SIZE, label);
}

// bottom bar shown while the pause menu's main screen is open (not the
// options screen, which has never had one of its own). highlighted
// item's description (see pause_items) on the left, static "confirm/
// back" button prompts right-aligned on the right -- same bar art and
// draw_ps_prompt convention as the main menu's own bottom bar
// (draw_mainmenu_bottom_bar), forward declared above pause_menu since it
// needs BAR_Y/ICON_SIZE/draw_ps_prompt, none of which exist yet up there
#define PAUSE_BAR_SIDE_PAD   20
#define PAUSE_BAR_PROMPT_GAP 28

static void draw_pause_bottom_bar(int sel)
{
   if (pause_photo_tex)
   {
      float sx, sy, sw, sh;
      cover_src_rect(pause_photo_tex, (float)SCREEN_W, (float)BAR_H, &sx, &sy, &sw, &sh);
      vita2d_draw_texture_part_scale(pause_photo_tex, 0.0f, (float)BAR_Y,
            sx, sy, sw, sh, (float)SCREEN_W / sw, (float)BAR_H / sh);
   }
   else
   {
      vita2d_draw_rectangle(0, BAR_Y, SCREEN_W, BAR_H, RGBA8(10, 26, 52, 255));
   }

   int cy = BAR_Y + BAR_H / 2;

   if (pause_items[sel].description)
      vita2d_font_draw_text(font, PAUSE_BAR_SIDE_PAD, cy + (int)(MARQUEE_FONT_SIZE * 0.35f),
            RGBA8(170, 184, 210, 255), MARQUEE_FONT_SIZE, pause_items[sel].description);

   int confirm_label_w = vita2d_font_text_width(font, MARQUEE_FONT_SIZE, "Confirm");
   int back_label_w    = vita2d_font_text_width(font, MARQUEE_FONT_SIZE, "Back");
   int confirm_w = ICON_SIZE + 8 + confirm_label_w;
   int back_w    = ICON_SIZE + 8 + back_label_w;

   int back_x    = SCREEN_W - PAUSE_BAR_SIDE_PAD - back_w;
   int confirm_x = back_x - PAUSE_BAR_PROMPT_GAP - confirm_w;

   draw_ps_prompt(btn_cross_tex, confirm_x, cy, "Confirm");
   draw_ps_prompt(btn_circ_tex, back_x, cy, "Back");
}

// one marquee item: icon + label, or a plain text-only entry (icon == NULL)
typedef struct { vita2d_texture **icon; const char *label; } marquee_item_t;

static const marquee_item_t marquee_items[] = {
   { &btn_tri_tex,   "Game Settings" },
   { &btn_sq_tex,    "Controller Settings" },
   { &btn_cross_tex, "Start Game" },
   { &btn_circ_tex,  "Back" },
   { NULL,           "Choose the game you want to play" },
};
#define MARQUEE_ITEM_COUNT (sizeof(marquee_items) / sizeof(marquee_items[0]))

static float marquee_item_width(const marquee_item_t *it)
{
   int lw = vita2d_font_text_width(font, MARQUEE_FONT_SIZE, it->label);
   float pre = it->icon ? (float)ICON_SIZE + 8.0f : 0.0f;
   return pre + (float)lw;
}

static float marquee_strip_width(void)
{
   float w = 0.0f;
   for (unsigned i = 0; i < MARQUEE_ITEM_COUNT; i++)
      w += marquee_item_width(&marquee_items[i]) + MARQUEE_GAP;
   return w;
}

// draws one copy of the full item list starting at x, returns x + strip width
static float marquee_draw_strip(float x, int cy)
{
   for (unsigned i = 0; i < MARQUEE_ITEM_COUNT; i++)
   {
      const marquee_item_t *it = &marquee_items[i];

      if (it->icon)
         draw_ps_prompt(*it->icon, (int)x, cy, it->label);
      else
         vita2d_font_draw_text(font, (int)x, cy + (int)(MARQUEE_FONT_SIZE * 0.35f),
               RGBA8(170, 184, 210, 255), MARQUEE_FONT_SIZE, it->label);

      x += marquee_item_width(it) + MARQUEE_GAP;
   }
   return x;
}

static void draw_bottom_bar(void)
{
   if (pause_photo_tex)
   {
      float sx, sy, sw, sh;
      cover_src_rect(pause_photo_tex, (float)SCREEN_W, (float)BAR_H, &sx, &sy, &sw, &sh);
      vita2d_draw_texture_part_scale(pause_photo_tex, 0.0f, (float)BAR_Y,
            sx, sy, sw, sh, (float)SCREEN_W / sw, (float)BAR_H / sh);
   }
   else
   {
      vita2d_draw_rectangle(0, BAR_Y, SCREEN_W, BAR_H, RGBA8(10, 26, 52, 255));
   }
   int cy = BAR_Y + BAR_H / 2;

   float strip_w = marquee_strip_width();
   float off = fmodf(marquee_offset, strip_w);
   if (off < 0.0f)
      off += strip_w;

   float x = -off;
   while (x < (float)SCREEN_W)
      x = marquee_draw_strip(x, cy);
}

// draws the full main-menu scene: backdrop, header, either the empty-roms
// message or the game carousel + spotlight + info panel. pulled out of
// run_menu so the controller settings overlay can paint the exact same
// background underneath its own panel instead of clearing to black first.
// info_sel picks which game the info panel/focus highlight shows, while
// base_sel and shift are the carousel's own position inputs (run_menu
// passes anim_from_sel/eased-shift mid-rotation, everyone else just passes
// info_sel and 0.0f for a static frame)
static void draw_main_menu_scene(int info_sel, int base_sel, float shift, menu_focus_t focus)
{
   draw_menu_backdrop();
   draw_menu_header();

   if (game_count == 0)
   {
      vita2d_font_draw_text(font, 80, 300, RGBA8(200, 80, 80, 255), FONT_BASE_PX,
            "NO ROMS FOUND IN " ROM_DIR);
      return;
   }

   slot_draw_t sd[KNOWN_COUNT];
   unsigned ns = 0;

   for (unsigned i = 0; i < game_count; i++)
   {
      float pos = (float)(FRONT_TRACK_IDX + (int)i - base_sel) + shift;
      float x, y, w, h;
      track_rect_at(pos, &x, &y, &w, &h);
      sd[ns].i = i; sd[ns].x = x; sd[ns].y = y; sd[ns].w = w; sd[ns].h = h;
      ns++;
   }

   // back-to-front draw order, smaller y first (insertion sort, ns is tiny)
   for (unsigned a = 1; a < ns; a++)
   {
      slot_draw_t key = sd[a];
      int b = (int)a - 1;
      while (b >= 0 && sd[b].y > key.y)
      {
         sd[b + 1] = sd[b];
         b--;
      }
      sd[b + 1] = key;
   }

   // whichever card's computed rect is currently nearest the fixed
   // front-center slot is the one under the (equally fixed) spotlight
   // -- geometric, not by game index, so the lit card tracks whatever
   // is actually passing through that screen position mid-shift
   float front_cx = (float)slot_x[FRONT_SLOT_IDX] + (float)slot_w[FRONT_SLOT_IDX] * 0.5f;
   float front_cy = (float)slot_y[FRONT_SLOT_IDX] + (float)slot_h[FRONT_SLOT_IDX] * 0.5f;
   unsigned lit_k = 0;
   float best_dist2 = -1.0f;

   for (unsigned k = 0; k < ns; k++)
   {
      float ccx = sd[k].x + sd[k].w * 0.5f;
      float ccy = sd[k].y + sd[k].h * 0.5f;
      float ddx = ccx - front_cx;
      float ddy = ccy - front_cy;
      float dist2 = ddx * ddx + ddy * ddy;

      if (best_dist2 < 0.0f || dist2 < best_dist2)
      {
         best_dist2 = dist2;
         lit_k = k;
      }
   }

   for (unsigned k = 0; k < ns; k++)
      draw_game_slot(sd[k].i, sd[k].x, sd[k].y, sd[k].w, sd[k].h, k == lit_k);

   // spotlight goes on last, above every card, always at the same
   // fixed screen position
   draw_carousel_spotlight();

   draw_info_panel(info_sel, focus);
}

// controller settings screen layout, reuses the pause panel's box art
// (draw_panel_box) but is otherwise fully independent from the pause
// menu's own geometry -- own width/x/y and own item x/w, sized for this
// screen's actual content (the settings_banner_tex header plus 6 mapping
// rows and a reset row, 7 total). CTRLCFG_PANEL_H is taller than the
// pause menu's panel because it has more rows to wrap; that's the whole
// reason this menu has never shared geometry with the pause menu, so it
// keeps its own set of macros rather than aliasing anyone else's
#define CTRLCFG_PANEL_W          500
#define CTRLCFG_PANEL_H          360
#define CTRLCFG_PANEL_X          ((SCREEN_W - CTRLCFG_PANEL_W) / 2)
#define CTRLCFG_PANEL_Y          ((SCREEN_H - CTRLCFG_PANEL_H) / 2)
#define CTRLCFG_HEADER_X         (CTRLCFG_PANEL_X + 18)
#define CTRLCFG_HEADER_Y         (CTRLCFG_PANEL_Y + 18)
#define CTRLCFG_ITEM_W           434
#define CTRLCFG_ITEM_X           (CTRLCFG_PANEL_X + 33)
#define CTRLCFG_ROW_START_Y      (CTRLCFG_PANEL_Y + 80)
#define CTRLCFG_ROW_SPACING      44
#define CTRLCFG_ICON_SIZE        28
#define CTRLCFG_ROW_COUNT        6
#define CTRLCFG_ITEM_COUNT       (CTRLCFG_ROW_COUNT + 1) // +1 for the reset row

// l1/r1 have no button glyph, this nudges their fallback text right so it
// lines up with where the icon glyphs' actual pixels start instead of
// sitting flush against the panel's inner edge
#define CTRLCFG_TRIGGER_LABEL_NUDGE 6

// fixed x offset (from CTRLCFG_ITEM_X) where every row's value starts, regardless
// of how wide that row's icon/label is. keeps long values from drifting
// past the panel edge on the text-only l1/r1 rows
#define CTRLCFG_VALUE_COL 130

typedef struct {
   const char *label;      // fallback text for l1/r1, which have no button glyph
   vita2d_texture **icon;  // NULL for l1/r1
   int *map;               // points at one of the map_* globals above
} ctrlcfg_row_t;

// one row per remappable physical button, in on-screen order. the icon
// textures are the same btn_*_tex assets the main menu's bottom bar
// already loads, no need to load separate copies for this screen
static const ctrlcfg_row_t ctrlcfg_rows[CTRLCFG_ROW_COUNT] = {
   { "Cross",    &btn_cross_tex, &map_cross    },
   { "Circle",   &btn_circ_tex,  &map_circle   },
   { "Square",   &btn_sq_tex,    &map_square   },
   { "Triangle", &btn_tri_tex,   &map_triangle },
   { "L1",       NULL,           &map_l1       },
   { "R1",       NULL,           &map_r1       },
};

static void draw_ctrlcfg_row(int row_y, const ctrlcfg_row_t *row, bool sel, core_kind_t core)
{
   char value[24];

   if (sel)
      draw_pause_hilite_at(CTRLCFG_ITEM_X, row_y, CTRLCFG_ITEM_W);

   if (row->icon && *row->icon)
   {
      float s = (float)CTRLCFG_ICON_SIZE / (float)vita2d_texture_get_width(*row->icon);
      vita2d_draw_texture_scale(*row->icon, (float)(CTRLCFG_ITEM_X + ITEM_TEXT_PAD),
            (float)(row_y - CTRLCFG_ICON_SIZE / 2), s, s);
   }
   else
   {
      draw_text_outline_l(CTRLCFG_ITEM_X + ITEM_TEXT_PAD + CTRLCFG_TRIGGER_LABEL_NUDGE,
            row_y + (int)(ITEM_FONT_SIZE * 0.35f), ITEM_FONT_SIZE, row->label);
   }

   // arrows only show on the focused row, they're a d-pad cycling hint,
   // not meaningful on a row that isn't selected
   if (sel)
      snprintf(value, sizeof(value), "< %s >", remap_target_label(core, *row->map));
   else
      snprintf(value, sizeof(value), "%s", remap_target_label(core, *row->map));

   draw_text_outline_l(CTRLCFG_ITEM_X + CTRLCFG_VALUE_COL, row_y + (int)(ITEM_FONT_SIZE * 0.35f),
         ITEM_FONT_SIZE, value);
}

static void draw_ctrlcfg_reset_row(int row_y, bool sel)
{
   if (sel)
      draw_pause_hilite_at(CTRLCFG_ITEM_X, row_y, CTRLCFG_ITEM_W);

   draw_text_outline_l(CTRLCFG_ITEM_X + ITEM_TEXT_PAD, row_y + (int)(ITEM_FONT_SIZE * 0.35f),
         ITEM_FONT_SIZE, "Reset to Defaults");
}

// red banner across the top of the controller settings panel, same spot
// and same no-scaling draw as the pause menu's ribbon (draw_pause_ribbon)
// so a 480x52 asset drops straight into the header without any stretching
static void draw_ctrlcfg_banner(void)
{
   if (settings_banner_tex)
      vita2d_draw_texture(settings_banner_tex,
            (float)(CTRLCFG_HEADER_X - HEADER_PAD), (float)(CTRLCFG_HEADER_Y - HEADER_PAD));
}

// bottom bar shown while the controller settings overlay is open. same bar
// art as the main menu's marquee (draw_bottom_bar) but static instead of
// scrolling: fixed instructions on the left, circle-to-go-back on the
// right, since there's nothing else worth cycling through on this screen
#define CTRLCFG_BAR_SIDE_PAD 20
#define CTRLCFG_BAR_HINT     "Change your controller's button layout."

static void draw_ctrlcfg_bottom_bar(void)
{
   if (pause_photo_tex)
   {
      float sx, sy, sw, sh;
      cover_src_rect(pause_photo_tex, (float)SCREEN_W, (float)BAR_H, &sx, &sy, &sw, &sh);
      vita2d_draw_texture_part_scale(pause_photo_tex, 0.0f, (float)BAR_Y,
            sx, sy, sw, sh, (float)SCREEN_W / sw, (float)BAR_H / sh);
   }
   else
   {
      vita2d_draw_rectangle(0, BAR_Y, SCREEN_W, BAR_H, RGBA8(10, 26, 52, 255));
   }

   int cy = BAR_Y + BAR_H / 2;

   vita2d_font_draw_text(font, CTRLCFG_BAR_SIDE_PAD, cy + (int)(MARQUEE_FONT_SIZE * 0.35f),
         RGBA8(170, 184, 210, 255), MARQUEE_FONT_SIZE, CTRLCFG_BAR_HINT);

   // right-aligned circle+"back" prompt, same icon-plus-label layout
   // draw_ps_prompt already uses for every other bottom-bar entry
   int back_label_w = vita2d_font_text_width(font, MARQUEE_FONT_SIZE, "Back");
   int back_total_w = ICON_SIZE + 8 + back_label_w;
   int back_x = SCREEN_W - CTRLCFG_BAR_SIDE_PAD - back_total_w;

   draw_ps_prompt(btn_circ_tex, back_x, cy, "Back");
}

// applies a newly cycled action to one ctrlcfg row, auto-swapping with
// whichever other row already held that action so two physical buttons
// never end up doing the same thing at once. unassigned is exempt -
// several rows sitting on "unassigned" at the same time is normal (the
// mame table only ever uses 2 of the 6 rows), not a conflict, so it
// never triggers a swap
static void set_row_mapping(unsigned game_idx, int row, int new_action)
{
   int old_action = *ctrlcfg_rows[row].map;

   if (new_action != REMAP_UNASSIGNED)
   {
      for (int i = 0; i < CTRLCFG_ROW_COUNT; i++)
      {
         if (i == row)
            continue;

         if (*ctrlcfg_rows[i].map == new_action)
         {
            *ctrlcfg_rows[i].map = old_action;
            break;
         }
      }
   }

   *ctrlcfg_rows[row].map = new_action;
   save_button_mapping(game_idx);
}

// controller settings screen, reached from the main menu with square. edits
// the highlighted game's mapping in place (the map_* globals) and writes
// it straight to that rom's .cfg on every change, same save-on-change
// pattern the pause menu's options screen uses. menu navigation (up/down,
// circle to leave) stays hardcoded same as everywhere else, only the
// values being edited are user-defined here
// draw_backdrop redraws whatever's behind this screen: the game carousel
// when opened from the rom list (see the wrapper just above its own call
// site further down), or the paused game frame when opened from the
// pause menu's own controller settings row (draw_pause_ctrlcfg_backdrop)
static void controller_settings_menu(unsigned game_idx, void (*draw_backdrop)(void))
{
   int sel = 0;
   SceCtrlData pad_prev;

   load_button_mapping(game_idx);

   flush_pad(&pad_prev);

   for (;;)
   {
      unsigned pressed = poll_pressed(&pad_prev);

      if (pressed & SCE_CTRL_UP)
         sel = (sel == 0) ? CTRLCFG_ITEM_COUNT - 1 : sel - 1;
      if (pressed & SCE_CTRL_DOWN)
         sel = (sel + 1) % CTRLCFG_ITEM_COUNT;
      if (pressed & (SCE_CTRL_UP | SCE_CTRL_DOWN))
         sfx_play(sfx_choose_pcm, sfx_choose_frames);

      if (sel < CTRLCFG_ROW_COUNT)
      {
         // a mapping row: left steps back, right/cross step forward.
         // whichever other row already had the newly picked action gets
         // swapped onto this row's old one, so two buttons never end up
         // sharing a single action (see set_row_mapping). left/right are
         // a d-pad value adjustment, so they play choose.ogg; cross plays
         // select.ogg since it's the confirm-style button doing the same
         // step
         if (pressed & SCE_CTRL_LEFT)
         {
            sfx_play(sfx_choose_pcm, sfx_choose_frames);
            set_row_mapping(game_idx, sel,
                  remap_target_cycle_prev(games[game_idx].core, *ctrlcfg_rows[sel].map));
         }
         else if (pressed & SCE_CTRL_RIGHT)
         {
            sfx_play(sfx_choose_pcm, sfx_choose_frames);
            set_row_mapping(game_idx, sel,
                  remap_target_cycle(games[game_idx].core, *ctrlcfg_rows[sel].map));
         }
         else if (pressed & SCE_CTRL_CROSS)
         {
            sfx_play(sfx_select_pcm, sfx_select_frames);
            set_row_mapping(game_idx, sel,
                  remap_target_cycle(games[game_idx].core, *ctrlcfg_rows[sel].map));
         }
      }
      else if (pressed & SCE_CTRL_CROSS)
      {
         // reset row, snap straight back to this rom's stock arcade
         // layout and save immediately, same as any other change here
         sfx_play(sfx_select_pcm, sfx_select_frames);
         apply_default_mapping(games[game_idx].core);
         save_button_mapping(game_idx);
      }

      if (pressed & SCE_CTRL_CIRCLE)
      {
         sfx_play(sfx_back_pcm, sfx_back_frames);
         return;
      }

      vita2d_start_drawing();
      vita2d_clear_screen();

      // keep whatever screen opened this one rendering underneath as a
      // static frame -- this screen is an overlay on top of it, not a
      // replacement for it
      if (draw_backdrop)
         draw_backdrop();

      // same box art as the pause panel, just its own taller size, with
      // the red banner from settings_banner_tex across the top instead of
      // plain corner text
      draw_panel_box(CTRLCFG_PANEL_X, CTRLCFG_PANEL_Y, CTRLCFG_PANEL_W, CTRLCFG_PANEL_H);
      draw_ctrlcfg_banner();

      int row_y = CTRLCFG_ROW_START_Y;
      for (int i = 0; i < CTRLCFG_ROW_COUNT; i++)
      {
         draw_ctrlcfg_row(row_y, &ctrlcfg_rows[i], i == sel, games[game_idx].core);
         row_y += CTRLCFG_ROW_SPACING;
      }
      draw_ctrlcfg_reset_row(row_y, sel == CTRLCFG_ROW_COUNT);

      // static bottom bar override, replaces the scrolling main-menu
      // marquee while this screen owns the frame
      draw_ctrlcfg_bottom_bar();

      vita2d_end_drawing();
      vita2d_swap_buffers();
   }
}

// snapshot of whichever game row square was pressed on in the carousel,
// read back by the wrapper below so controller_settings_menu's backdrop
// callback (a plain function pointer, no closures) can redraw the exact
// same carousel frame it always has
static unsigned ctrlcfg_carousel_backdrop_idx;

static void draw_ctrlcfg_carousel_backdrop(void)
{
   // focus is pinned to the games row here since this screen doesn't
   // track whichever row was actually focused in run_menu at the moment
   // square was pressed
   draw_main_menu_scene(ctrlcfg_carousel_backdrop_idx, ctrlcfg_carousel_backdrop_idx,
         0.0f, MENU_FOCUS_GAMES);
}

// exact rom filename (e.g. "mvscu.zip") resolved by resolve_rom_filename
// for whichever game run_menu just launched. main() reads this right
// after run_menu returns to build the actual boot path
static char pending_rom_file[64];

// single source of truth for "which candidate list does this region use",
// shared by resolve_rom_filename and seed_default_nvram so the two can
// never drift apart (see seed_default_nvram's comment for what drifting
// once cost)
static const char *const *region_file_list(unsigned known_idx, rom_region_t region)
{
   switch (region)
   {
      case REGION_USA:
         return known_games[known_idx].american_files;
      case REGION_JAPAN:
         return known_games[known_idx].japanese_files;
      case REGION_EUROPE:
      default:
         return known_games[known_idx].european_files;
   }
}

// looks up which exact rom filename should be used for known game idx at
// its currently selected region, walking the same-region fallback order
// and returning the first candidate actually present on disk. writes just
// the filename, never a full path, callers that need the full path
// prefix it with ROM_DIR themselves. logs every path it checks so an
// on-device "missing rom" report can be traced back to the exact string
// that was tested, instead of guessed at afterwards
static bool resolve_rom_filename(unsigned idx, char *out, size_t out_sz)
{
   unsigned known_idx;
   const char *const *list;

   if (!out || out_sz == 0 || idx >= game_count)
      return false;

   known_idx = games[idx].known_idx;
   if (known_idx >= KNOWN_COUNT)
      return false;

   list = region_file_list(known_idx, (rom_region_t)game_region[idx]);

   memset(out, 0, out_sz);

   for (unsigned i = 0; i < ROM_VARIANT_MAX; i++)
   {
      char full[192];
      int n;

      if (!list[i])
         break;

      memset(full, 0, sizeof(full));
      n = snprintf(full, sizeof(full), "%s%s", ROM_DIR, list[i]);
      if (n < 0 || (size_t)n >= sizeof(full))
         continue;

      if (file_exists(full))
      {
         core_log(RETRO_LOG_INFO,
               "naohac: resolved \"%s\" (region %u) -> %s\n",
               known_games[known_idx].base_id, (unsigned)game_region[idx], full);

         n = snprintf(out, out_sz, "%s", list[i]);
         if (n < 0 || (size_t)n >= out_sz)
         {
            memset(out, 0, out_sz);
            return false;
         }

         out[out_sz - 1] = '\0';
         return true;
      }

      core_log(RETRO_LOG_INFO,
            "naohac: candidate not on disk, skipping: %s\n", full);
   }

   core_log(RETRO_LOG_ERROR,
         "naohac: no candidate found for \"%s\" region %u\n",
         known_games[known_idx].base_id, (unsigned)game_region[idx]);

   return false;
}

// popup panel dimensions, reuses draw_panel_box, the exact same box art
// the controller settings screen draws its panel with, just sized for a
// two-line message plus a dismiss prompt instead of a row list
#define ROMPOPUP_PANEL_W       CTRLCFG_PANEL_W
#define ROMPOPUP_PANEL_H       200
#define ROMPOPUP_PANEL_X       CTRLCFG_PANEL_X
#define ROMPOPUP_PANEL_Y       ((SCREEN_H - ROMPOPUP_PANEL_H) / 2)
#define ROMPOPUP_MSG_FONT_SIZE 20
#define ROMPOPUP_MSG_LINE_H    28
#define ROMPOPUP_OK_FONT_SIZE  MARQUEE_FONT_SIZE
#define ROMPOPUP_OK_ICON_GAP   8   // same icon-to-label gap as the bottom marquee bar
#define ROMPOPUP_OK_BOTTOM_PAD 34

// blocks until dismissed with cross. drawn over the main menu backdrop
// using the exact same panel/box style as the controller settings screen
// (draw_panel_box) rather than a one-off look of its own. shared by every
// two-line popup below (missing rom, core load rejection, ...) so they
// all stay pixel-identical instead of drifting apart over time
static void show_two_line_popup(const char *line1, const char *line2)
{
   static const char *ok_label = "OK";

   int cx    = ROMPOPUP_PANEL_X + ROMPOPUP_PANEL_W / 2;
   int msg_y = ROMPOPUP_PANEL_Y + ROMPOPUP_PANEL_H / 2 - ROMPOPUP_MSG_LINE_H / 2;
   int ok_y  = ROMPOPUP_PANEL_Y + ROMPOPUP_PANEL_H - ROMPOPUP_OK_BOTTOM_PAD;

   int w1 = vita2d_font_text_width(font, ROMPOPUP_MSG_FONT_SIZE, line1);
   int w2 = vita2d_font_text_width(font, ROMPOPUP_MSG_FONT_SIZE, line2);

   // real cross-button icon in place of the old "[X]" text, same
   // icon+gap convention draw_ps_prompt uses for the bottom marquee bar
   int ok_icon_w = btn_cross_tex ? (ICON_SIZE + ROMPOPUP_OK_ICON_GAP) : 0;
   int ok_label_w = vita2d_font_text_width(font, ROMPOPUP_OK_FONT_SIZE, ok_label);
   int wo = ok_icon_w + ok_label_w;
   int ok_x = cx - wo / 2;

   // draw_text_outline_l takes a baseline y; the icon instead wants a
   // vertical center, roughly one size*0.35 above that same baseline,
   // same convention draw_item_label uses going the other direction
   int ok_icon_cy = ok_y - (int)(ROMPOPUP_OK_FONT_SIZE * 0.35f);

   SceCtrlData pad_prev;

   flush_pad(&pad_prev);

   for (;;)
   {
      unsigned pressed = poll_pressed(&pad_prev);

      if (pressed & SCE_CTRL_CROSS)
         return;

      vita2d_start_drawing();
      vita2d_clear_screen();

      draw_menu_backdrop();

      draw_panel_box(ROMPOPUP_PANEL_X, ROMPOPUP_PANEL_Y, ROMPOPUP_PANEL_W, ROMPOPUP_PANEL_H);

      draw_text_outline_l(cx - w1 / 2, msg_y, ROMPOPUP_MSG_FONT_SIZE, line1);
      draw_text_outline_l(cx - w2 / 2, msg_y + ROMPOPUP_MSG_LINE_H, ROMPOPUP_MSG_FONT_SIZE, line2);

      if (btn_cross_tex)
      {
         float s = (float)ICON_SIZE / (float)vita2d_texture_get_width(btn_cross_tex);
         vita2d_draw_texture_scale(btn_cross_tex, (float)ok_x, (float)(ok_icon_cy - ICON_SIZE / 2), s, s);
      }
      draw_text_outline_l(ok_x + ok_icon_w, ok_y, ROMPOPUP_OK_FONT_SIZE, ok_label);

      vita2d_end_drawing();
      vita2d_swap_buffers();
   }
}

static void missing_rom_popup(void)
{
   show_two_line_popup("Missing ROM file in directory:", ROM_DIR);
}

// shown when core_api.load_game() rejects the rom outright. the frontend
// never alters the user's rom folder to work around this, it just
// reports the rejection and drops the player back at the menu.
//
// the exact cause differs sharply by core, so the second line is picked
// per core rather than sharing one generic guess:
//
// fba2012_cps2 opens whatever archive path it's handed directly, so a
// rejection there really does mean a bad dump, wrong split/merged
// layout, or a genuinely missing parent archive (see
// ensure_parent_placeholder for the last one).
//
// mame2000 instead identifies the game by matching the rom's filename
// (minus ".zip") against its own compiled-in driver table *before* it
// ever opens the archive. a rejection on a file resolve_rom_filename
// already confirmed exists on disk (check core_log.txt for its
// "resolved ... ->" line) most likely means this exact driver name
// simply isn't compiled into this particular build of mame_retro -
// commonly the case for clone sets on a size-trimmed vita build that
// only kept one driver per game. no frontend-side path logic can work
// around that; it needs the core rebuilt with that driver re-enabled.
static void load_fail_popup(core_kind_t core)
{
   if (core == CORE_KIND_MAME)
      show_two_line_popup("Error: Core rejected ROM.",
            "Driver unsupported by this core build.");
   else
      show_two_line_popup("Error: Core rejected ROM.",
            "Incompatible ROMset or missing parent.");
}

static int run_menu(void)
{
   int sel = 0;
   menu_focus_t focus = MENU_FOCUS_GAMES;
   SceCtrlData pad_prev;

   flush_pad(&pad_prev);

   for (;;)
   {
      unsigned pressed = poll_pressed(&pad_prev);

      if (game_count > 0)
      {
         // up/down steps focus through the card grid, version row, and
         // player row in that order, one row per press, clamped at both
         // ends rather than wrapping around
         if (pressed & SCE_CTRL_UP)
         {
            if (focus == MENU_FOCUS_PLAYER)
               focus = MENU_FOCUS_VERSION;
            else
               focus = MENU_FOCUS_GAMES;
            sfx_play(sfx_choose_pcm, sfx_choose_frames);
         }
         if (pressed & SCE_CTRL_DOWN)
         {
            if (focus == MENU_FOCUS_GAMES)
               focus = MENU_FOCUS_VERSION;
            else
               focus = MENU_FOCUS_PLAYER;
            sfx_play(sfx_choose_pcm, sfx_choose_frames);
         }

         if (focus == MENU_FOCUS_GAMES)
         {
            // scrolling between games in the carousel gets its own
            // dedicated stinger instead of the generic choose.ogg blip
            if (!anim_active && (pressed & SCE_CTRL_LEFT))
            {
               rotate_carousel(1, &sel);
               sfx_play(sfx_gamechange_pcm, sfx_gamechange_frames);
            }
            if (!anim_active && (pressed & SCE_CTRL_RIGHT))
            {
               rotate_carousel(-1, &sel);
               sfx_play(sfx_gamechange_pcm, sfx_gamechange_frames);
            }
         }
         else if (focus == MENU_FOCUS_VERSION && (pressed & (SCE_CTRL_LEFT | SCE_CTRL_RIGHT)))
         {
            // cycles which region this game boots as, resolved fresh
            // against disk the moment the player tries to start, see
            // resolve_rom_filename below. each region is a real distinct
            // choice now, not a euro-with-silent-usa-fallback toggle, so
            // an american rom sitting next to a european one is actually
            // reachable from this row
            int dir = (pressed & SCE_CTRL_RIGHT) ? 1 : -1;
            game_region[sel] = (unsigned char)((game_region[sel] + dir + REGION_COUNT) % REGION_COUNT);
            sfx_play(sfx_choose_pcm, sfx_choose_frames);
         }
         else if (focus == MENU_FOCUS_PLAYER && (pressed & (SCE_CTRL_LEFT | SCE_CTRL_RIGHT)))
         {
            // flips which controller port (cabinet side) this game boots
            // into, only p1/p2 exist so either direction just toggles it.
            // latched into active_player_port right before the core loads
            // (see main()), which is what input_state_cb actually routes
            // the vita's single physical pad against
            game_player_port[sel] = (unsigned char)(game_player_port[sel] ^ 1);
            sfx_play(sfx_choose_pcm, sfx_choose_frames);
         }

         if (pressed & (SCE_CTRL_CROSS | SCE_CTRL_START))
         {
            if (resolve_rom_filename(sel, pending_rom_file, sizeof(pending_rom_file)))
            {
               // the game's own announcer stinger no longer plays (or
               // blocks) here -- run_offline_play kicks it off
               // asynchronously once it switches to the now loading
               // screen, see play_game_announcer_async
               return sel;
            }

            sfx_play(sfx_select_pcm, sfx_select_frames);
            missing_rom_popup();

            // drop whatever was still held on the way out so it doesn't
            // bleed into the menu as a spurious press next frame
            flush_pad(&pad_prev);
            continue;
         }

         if (pressed & SCE_CTRL_SQUARE)
         {
            sfx_play(sfx_select_pcm, sfx_select_frames);
            ctrlcfg_carousel_backdrop_idx = sel;
            controller_settings_menu(sel, draw_ctrlcfg_carousel_backdrop);

            // drop whatever was still held on the way out so it doesn't
            // bleed into the menu as a spurious press next frame
            flush_pad(&pad_prev);
            continue;
         }
      }

      // circle backs out to the main menu screen, regardless of whether
      // any roms were found -- checked outside the game_count > 0 gate
      // above so the empty-roms screen isn't a dead end
      if (pressed & SCE_CTRL_CIRCLE)
      {
         sfx_play(sfx_back_pcm, sfx_back_frames);
         return -1;
      }

      // advance the carousel shift for this frame
      float shift = 0.0f;
      int base_sel = sel;
      if (anim_active)
      {
         SceInt64 now = sceKernelGetProcessTimeWide();
         float p = (float)(now - anim_start_us) / (float)ANIM_US;
         if (p >= 1.0f)
         {
            anim_active = 0;
         }
         else
         {
            float eased = ease_in_out_cubic(p);
            shift = -(float)anim_dir * eased;
            base_sel = anim_from_sel;
         }
      }

      // advance the bottom-bar ticker offset for this frame
      marquee_offset = (float)sceKernelGetProcessTimeWide() / 1000000.0f * MARQUEE_SPEED;

      vita2d_start_drawing();
      vita2d_clear_screen();

      draw_main_menu_scene(sel, base_sel, shift, focus);

      draw_bottom_bar();

      vita2d_end_drawing();
      vita2d_swap_buffers();
   }
}

static void core_audio_port_open(void)
{
   int vol[2] = { SCE_AUDIO_VOLUME_0DB, SCE_AUDIO_VOLUME_0DB };

   audio_port = sceAudioOutOpenPort(core_port_type, AUDIO_PORT_GRAIN,
         core_port_rate, SCE_AUDIO_OUT_MODE_STEREO);

   if (audio_port >= 0)
      sceAudioOutSetVolume(audio_port, SCE_AUDIO_VOLUME_FLAG_L_CH | SCE_AUDIO_VOLUME_FLAG_R_CH, vol);
}

static void core_audio_port_close(void)
{
   if (audio_port >= 0)
   {
      sceAudioOutReleasePort(audio_port);
      audio_port = -1;
   }
}

static void run_core(void)
{
   SceCtrlData pad_prev;
   memset(&pad_prev, 0, sizeof(pad_prev));

   for (;;)
   {
      unsigned pressed = poll_pressed(&pad_prev);

      // select alone opens the pause menu now, free play means we don't
      // need select free for coin insert anymore
      if (pressed & SCE_CTRL_SELECT)
      {
         core_audio_port_close();

         bool exit_core = pause_menu();

         // quitting: don't reopen a port that's just going to get closed
         // again a few lines down in the teardown path. that open-then-
         // immediately-close pair was pure churn on the hardware port at
         // exactly the moment the next game's load needs it fully released
         if (exit_core)
            break;

         core_audio_port_open();
         audio_stage_fill = 0;

         // flush here, not a bare memset like the entry above -- whichever
         // button just closed the pause menu (circle, or cross on resume)
         // can still be physically held for a frame or two, and without
         // waiting it out first that same press reads as freshly pressed
         // the instant we're back, which was bleeding into gameplay as a
         // real button hold. the entry memset stays a memset on purpose:
         // that one has to let a deliberately-held movement button apply
         // the instant gameplay starts, not block waiting on it
         flush_pad(&pad_prev);
         continue;
      }

      core_api.run();
      blit_frame();
   }

   // unload_game()/deinit() run back to back in main(), right after this
   // function returns
   core_audio_port_close();
}

static int snap_sample_rate(double rate)
{
   static const int valid[3] = { 32000, 44100, 48000 };
   int best = valid[0];
   double best_diff = 1e18;

   for (int i = 0; i < 3; i++)
   {
      double diff = rate - valid[i];
      if (diff < 0)
         diff = -diff;
      if (diff < best_diff)
      {
         best_diff = diff;
         best = valid[i];
      }
   }

   return best;
}

static void create_data_dirs(void)
{
   sceIoMkdir("ux0:data/NaohAC", 0777);
   sceIoMkdir("ux0:data/NaohAC/roms", 0777);
   sceIoMkdir("ux0:data/NaohAC/covers", 0777);
   sceIoMkdir("ux0:data/NaohAC/overlays", 0777);
   sceIoMkdir("ux0:data/NaohAC/saves", 0777);
   sceIoMkdir("ux0:data/NaohAC/system", 0777);
   sceIoMkdir(CONFIG_DIR, 0777);
}

static bool file_exists(const char *path)
{
   SceIoStat st;

   if (!path || !path[0])
      return false;

   return sceIoGetstat(path, &st) >= 0;
}

// copies src to dst byte for byte. true only if the whole file wrote
// cleanly, a partial dst is removed on failure so a later boot doesn't
// mistake it for an already-seeded file and skip re-seeding forever
static bool copy_file(const char *src, const char *dst)
{
   char buf[4096];
   size_t n;
   bool ok = true;
   FILE *in  = fopen(src, "rb");
   FILE *out;

   if (!in)
      return false;

   out = fopen(dst, "wb");
   if (!out)
   {
      fclose(in);
      return false;
   }

   while ((n = fread(buf, 1, sizeof(buf), in)) > 0)
   {
      if (fwrite(buf, 1, n, out) != n)
      {
         ok = false;
         break;
      }
   }

   fclose(in);
   fclose(out);

   if (!ok)
      remove(dst);

   return ok;
}

// drops bundled default .nv files into save_dir on first boot only, any
// file already present (a real save, or one seeded on a previous boot)
// is left untouched. one .nv per exact rom variant a game could actually
// resolve to (see resolve_rom_filename), derived straight off the same
// region_file_list helper resolve_rom_filename uses instead of a
// hand-maintained parallel list. a separate literal list is exactly how
// mshu.nv previously went missing and msh's usa file ended up seeded
// under the wrong name. mshh.zip is now itself a real, official american
// candidate (see known_games), so it gets its own correct mshh.nv seeded
// here automatically, same as every other candidate. same
// single-source-of-truth approach bakmain.c's seed_nvram_files() used.
//
// fbalpha2012_cps2's own eeprom .nv read/write never reliably lands on
// the vita filesystem during play (see the save directory case in
// environment_cb), this is what actually seeds freeplay, not the core
// writing anything back. mame2000 punisher is left on defaults, no
// seeding for that core.
static void seed_default_nvram(void)
{
   for (unsigned i = 0; i < KNOWN_COUNT; i++)
   {
      if (known_games[i].core != CORE_KIND_FBA)
         continue;

      for (unsigned r = 0; r < REGION_COUNT; r++)
      {
         const char *const *list = region_file_list(i, (rom_region_t)r);

         for (unsigned v = 0; v < ROM_VARIANT_MAX; v++)
         {
            char base[160];
            char src[192];
            char dst[192];
            size_t len, base_len;

            if (!list[v])
               break;

            len = strlen(list[v]);
            base_len = (len > 4) ? len - 4 : len;

            snprintf(base, sizeof(base), "%.*s", (int)base_len, list[v]);
            snprintf(src, sizeof(src), "%s%s.nv", NVRAM_ASSET_DIR, base);
            snprintf(dst, sizeof(dst), "%s/%s.nv", SAVE_DIR, base);

            if (file_exists(dst))
               continue;

            copy_file(src, dst);
         }
      }
   }
}

// renames src to dst if src exists and dst doesn't already. always safe
// to call again on every single boot, a clean install or an already
// migrated one both just fall straight through the first check and do
// nothing. never overwrites an existing dst, so a player who somehow
// already has both names sitting around keeps whichever one the new
// code path actually uses and the stray old one is simply left alone
// rather than risking clobbering a real save
static void migrate_file_if_present(const char *src, const char *dst)
{
   if (!file_exists(src))
      return;

   if (file_exists(dst))
   {
      core_log(RETRO_LOG_INFO,
            "naohac: migration skipped, \"%s\" already exists next to \"%s\"\n",
            dst, src);
      return;
   }

   if (sceIoRename(src, dst) < 0)
   {
      core_log(RETRO_LOG_ERROR,
            "naohac: migration failed renaming \"%s\" to \"%s\"\n",
            src, dst);
      return;
   }

   core_log(RETRO_LOG_INFO, "naohac: migrated \"%s\" to \"%s\"\n", src, dst);
}

// mame2000's punisher driver only ever knew its usa/japan clones as the
// old 8-character short names punishru/punishrj (see the known_games
// comment above), never the 9-character punisheru/punisherj modern mame
// uses. earlier revisions of this frontend, or roms downloaded under the
// modern names, can leave a player's rom dir on the name mame2000 will
// never load. this table is what actually drives the one-time rename,
// each row is a stem pair, no extension, so the same pair can be reused
// against every kind of file that stem shows up in
typedef struct {
   const char *old_stem;
   const char *new_stem;
} rom_stem_migration_t;

static const rom_stem_migration_t rom_stem_migrations[] = {
   { "punisheru", "punishru" },
   { "punisherj", "punishrj" },
};
#define ROM_STEM_MIGRATION_COUNT \
   (sizeof(rom_stem_migrations) / sizeof(rom_stem_migrations[0]))

// runs once at the very top of main(), before the rom dir is ever read
// for real. quick and a no-op on every boot after the first, since each
// rename only fires when the old name is actually still there
static void migrate_legacy_rom_names(void)
{
   for (unsigned i = 0; i < ROM_STEM_MIGRATION_COUNT; i++)
   {
      const char *old_stem = rom_stem_migrations[i].old_stem;
      const char *new_stem = rom_stem_migrations[i].new_stem;
      char src[192];
      char dst[192];

      // the rom itself, this is the one that actually stops the game
      // from loading at all
      snprintf(src, sizeof(src), "%s%s.zip", ROM_DIR, old_stem);
      snprintf(dst, sizeof(dst), "%s%s.zip", ROM_DIR, new_stem);
      migrate_file_if_present(src, dst);

      // quick-save slots are keyed off the exact rom stem, not the base
      // game id, see quick_save_path/current_rom_stem, so they move in
      // lockstep with the rom file itself or a player's save silently
      // stops being found the moment the rom above gets renamed
      snprintf(src, sizeof(src), "%s/%s.qsv", SAVE_DIR, old_stem);
      snprintf(dst, sizeof(dst), "%s/%s.qsv", SAVE_DIR, new_stem);
      migrate_file_if_present(src, dst);

      // mame2000's own nvram file, same rom-stem keying as the quick
      // save above. only ever matters for a player who was on a build
      // where this somehow got far enough to load and write one
      snprintf(src, sizeof(src), "%s/%s.nv", SAVE_DIR, old_stem);
      snprintf(dst, sizeof(dst), "%s/%s.nv", SAVE_DIR, new_stem);
      migrate_file_if_present(src, dst);

      // included for completeness, but this one is structurally a
      // no-op today: game_config_path_for keys .cfg off base_id
      // ("punisher"), not the region's rom stem, so every region has
      // always shared one punisher.cfg and a file named punisheru.cfg
      // or punisherj.cfg could never have been written by this app in
      // the first place. left in so a future refactor that does start
      // keying .cfg per stem doesn't silently reopen this same bug
      snprintf(src, sizeof(src), "%s/%s.cfg", CONFIG_DIR, old_stem);
      snprintf(dst, sizeof(dst), "%s/%s.cfg", CONFIG_DIR, new_stem);
      migrate_file_if_present(src, dst);
   }
}

// main menu: sits between the title screen and the game carousel. cross
// on "offline play" drops into the existing carousel (run_menu, driven
// from run_offline_play below), circle from inside that carousel comes
// back here. background is a plain fullscreen stretch of mainmenu.png,
// not a cover-fit crop like the carousel backdrop, since the art is
// already composed for the vita's exact 960x544 output
typedef enum {
   MAINMENU_OFFLINE_PLAY = 0,
   MAINMENU_ONLINE_PLAY,     // disabled, purely aesthetic
   MAINMENU_MUSEUM,          // disabled, purely aesthetic
   MAINMENU_FIGHTER_AWARDS,  // disabled, purely aesthetic
   MAINMENU_OPTIONS,
   MAINMENU_QUIT,
   MAINMENU_ITEM_COUNT
} mainmenu_item_id_t;

typedef struct {
   const char *label;
   const char *description; // bottom-bar left text while highlighted, NULL if never selectable
   bool enabled;             // false = grayed out, skipped over by up/down navigation
} mainmenu_item_t;

static const mainmenu_item_t mainmenu_items[MAINMENU_ITEM_COUNT] = {
   { "Offline Play",   "Play offline.",                     true  },
   { "Online Play",    NULL,                                false },
   { "Museum",          NULL,                                false },
   { "Fighter Awards", NULL,                                false },
   { "Options",         "Change various in-game settings.", true  },
   { "Quit",             "Exit the application.",             true  },
};

// main menu's own panel geometry -- fully independent from the pause menu
// and controller settings screen's own macros above, same as those two
// are independent from each other. wider than either of them and its own
// height/offset, sized for this screen's 6 items and no header banner
#define MAINMENU_PANEL_W        620
#define MAINMENU_PANEL_H        268
#define MAINMENU_PANEL_X        ((SCREEN_W - MAINMENU_PANEL_W) / 2)
#define MAINMENU_PANEL_Y        90

#define MAINMENU_ITEM_W         554
#define MAINMENU_ITEM_X         (MAINMENU_PANEL_X + 33)
#define MAINMENU_ITEM_START_Y   (MAINMENU_PANEL_Y + 44)

#define MAINMENU_DISABLED_COLOR  RGBA8(140, 140, 140, 160)

// mainmenu_bgm_volume and apply_mainmenu_bgm_volume() live up near
// music_thread_func now, not here -- music_thread_func calls the setter
// directly from its own port-open path, well before this point in the file

static void draw_mainmenu_backdrop(void)
{
   if (!mainmenu_bg_tex)
      return;

   float sx = (float)SCREEN_W / (float)vita2d_texture_get_width(mainmenu_bg_tex);
   float sy = (float)SCREEN_H / (float)vita2d_texture_get_height(mainmenu_bg_tex);

   vita2d_draw_texture_scale(mainmenu_bg_tex, 0.0f, 0.0f, sx, sy);
}

// one row of the main menu list, drawn inside its own panel box. enabled
// rows reuse the pause menu's hilite bar + shrink-to-fit outlined label
// draws (draw_pause_hilite_at/draw_item_label_at), just at the main menu's
// own MAINMENU_ITEM_X/W instead of the pause menu's, disabled rows draw
// flat dim text with no outline pass and no hilite -- a disabled row can
// never be the selection, so it never needs either
static void draw_mainmenu_item(int item_y, const mainmenu_item_t *it, bool sel)
{
   if (!it->enabled)
   {
      vita2d_font_draw_text(font, MAINMENU_ITEM_X + ITEM_TEXT_PAD,
            item_y + (int)(ITEM_FONT_SIZE * 0.35f),
            MAINMENU_DISABLED_COLOR, ITEM_FONT_SIZE, it->label);
      return;
   }

   if (sel)
      draw_pause_hilite_at(MAINMENU_ITEM_X, item_y, MAINMENU_ITEM_W);

   draw_item_label_at(MAINMENU_ITEM_X, item_y, MAINMENU_ITEM_W, it->label);
}

static void draw_mainmenu_list(int sel)
{
   int item_y = MAINMENU_ITEM_START_Y;

   for (int i = 0; i < MAINMENU_ITEM_COUNT; i++)
   {
      draw_mainmenu_item(item_y, &mainmenu_items[i], i == sel);
      item_y += ITEM_SPACING_MAIN;
   }
}

// bottom bar: highlighted item's description (see mainmenu_items) on the
// left, static "confirm/back" button prompts right-aligned on the right.
// same bar art and draw_ps_prompt convention as every other bottom bar in
// the app (draw_bottom_bar, draw_ctrlcfg_bottom_bar)
#define MAINMENU_BAR_SIDE_PAD   20
#define MAINMENU_BAR_PROMPT_GAP 28

static void draw_mainmenu_bottom_bar(int sel)
{
   if (pause_photo_tex)
   {
      float sx, sy, sw, sh;
      cover_src_rect(pause_photo_tex, (float)SCREEN_W, (float)BAR_H, &sx, &sy, &sw, &sh);
      vita2d_draw_texture_part_scale(pause_photo_tex, 0.0f, (float)BAR_Y,
            sx, sy, sw, sh, (float)SCREEN_W / sw, (float)BAR_H / sh);
   }
   else
   {
      vita2d_draw_rectangle(0, BAR_Y, SCREEN_W, BAR_H, RGBA8(10, 26, 52, 255));
   }

   int cy = BAR_Y + BAR_H / 2;

   if (mainmenu_items[sel].description)
      vita2d_font_draw_text(font, MAINMENU_BAR_SIDE_PAD, cy + (int)(MARQUEE_FONT_SIZE * 0.35f),
            RGBA8(170, 184, 210, 255), MARQUEE_FONT_SIZE, mainmenu_items[sel].description);

   // right-aligned "x confirm" then "o back", same icon+label layout
   // draw_ps_prompt already uses everywhere else in the app
   int confirm_label_w = vita2d_font_text_width(font, MARQUEE_FONT_SIZE, "Confirm");
   int back_label_w    = vita2d_font_text_width(font, MARQUEE_FONT_SIZE, "Back");
   int confirm_w = ICON_SIZE + 8 + confirm_label_w;
   int back_w    = ICON_SIZE + 8 + back_label_w;

   int back_x    = SCREEN_W - MAINMENU_BAR_SIDE_PAD - back_w;
   int confirm_x = back_x - MAINMENU_BAR_PROMPT_GAP - confirm_w;

   draw_ps_prompt(btn_cross_tex, confirm_x, cy, "Confirm");
   draw_ps_prompt(btn_circ_tex, back_x, cy, "Back");
}

// small overlay: main menu bgm volume only, left/right steps it, circle
// leaves. drawn over a static frame of the main menu itself, same
// freeze-the-screen-behind/overlay-a-panel-on-top pattern
// controller_settings_menu uses over the carousel
#define MAINMENU_VOL_PANEL_W CTRLCFG_PANEL_W
#define MAINMENU_VOL_PANEL_H 160
#define MAINMENU_VOL_PANEL_X CTRLCFG_PANEL_X
#define MAINMENU_VOL_PANEL_Y ((SCREEN_H - MAINMENU_VOL_PANEL_H) / 2)
#define MAINMENU_VOL_ROW_Y   (MAINMENU_VOL_PANEL_Y + MAINMENU_VOL_PANEL_H / 2)

static void mainmenu_options_submenu(void)
{
   SceCtrlData pad_prev;

   flush_pad(&pad_prev);

   for (;;)
   {
      unsigned pressed = poll_pressed(&pad_prev);

      if (pressed & (SCE_CTRL_LEFT | SCE_CTRL_RIGHT))
      {
         // d-pad adjusting a live setting value, not confirming anything --
         // choose.ogg, same as any other value cycle
         sfx_play(sfx_choose_pcm, sfx_choose_frames);

         if (pressed & SCE_CTRL_RIGHT)
         {
            if (mainmenu_bgm_volume < MAINMENU_BGM_VOLUME_MAX)
               mainmenu_bgm_volume++;
         }
         else
         {
            if (mainmenu_bgm_volume > 0)
               mainmenu_bgm_volume--;
         }

         apply_mainmenu_bgm_volume();
      }

      if (pressed & SCE_CTRL_CIRCLE)
      {
         sfx_play(sfx_back_pcm, sfx_back_frames);
         return;
      }

      vita2d_start_drawing();
      vita2d_clear_screen();

      draw_mainmenu_backdrop();
      draw_panel_box(MAINMENU_PANEL_X, MAINMENU_PANEL_Y, MAINMENU_PANEL_W, MAINMENU_PANEL_H);
      draw_mainmenu_list(MAINMENU_OPTIONS);

      draw_panel_box(MAINMENU_VOL_PANEL_X, MAINMENU_VOL_PANEL_Y,
            MAINMENU_VOL_PANEL_W, MAINMENU_VOL_PANEL_H);

      {
         char line[32];
         int lw;

         snprintf(line, sizeof(line), "Main Menu BGM Volume: < %d%% >",
               mainmenu_bgm_volume * 10);

         lw = vita2d_font_text_width(font, ITEM_FONT_SIZE, line);
         draw_text_outline_l(MAINMENU_VOL_PANEL_X + (MAINMENU_VOL_PANEL_W - lw) / 2,
               MAINMENU_VOL_ROW_Y, ITEM_FONT_SIZE, line);
      }

      draw_mainmenu_bottom_bar(MAINMENU_OPTIONS);

      vita2d_end_drawing();
      vita2d_swap_buffers();
   }
}

// main menu loop. returns MAINMENU_OFFLINE_PLAY, MAINMENU_QUIT, or
// MAINMENU_ACTION_BACK_TO_TITLE, the only three outcomes the caller
// (main()) needs to act on -- options is handled right here as a self-
// contained overlay, and the three grayed rows can never be reached at all
#define MAINMENU_ACTION_BACK_TO_TITLE (-1)

// how long the fade in from black takes on entry, a bit quicker than the
// title screen's own fade-out (350ms here vs. 500ms for
// TITLE_FADEOUT_US) so the handoff doesn't linger
#define MAINMENU_FADEIN_US 350000

// backdrop callback for a prompt opened from the main menu. prompt_caller_sel
// is stashed by run_main_menu right before it calls show_prompt
static void draw_mainmenu_prompt_backdrop(void)
{
   draw_mainmenu_backdrop();
   draw_panel_box(MAINMENU_PANEL_X, MAINMENU_PANEL_Y, MAINMENU_PANEL_W, MAINMENU_PANEL_H);
   draw_mainmenu_list(prompt_caller_sel);
   draw_mainmenu_bottom_bar(prompt_caller_sel);
}

static int run_main_menu(void)
{
   int sel = MAINMENU_OFFLINE_PLAY;
   SceCtrlData pad_prev;

   // no-op if the title screen (or a previous trip through the carousel)
   // already has this running -- see start_menu_music
   start_menu_music();
   flush_pad(&pad_prev);

   // fade in from black on entry
   {
      SceInt64 fadein_start_us = sceKernelGetProcessTimeWide();

      for (;;)
      {
         SceInt64 fade_elapsed = sceKernelGetProcessTimeWide() - fadein_start_us;

         float fade_t = (float)fade_elapsed / (float)MAINMENU_FADEIN_US;
         if (fade_t > 1.0f)
            fade_t = 1.0f;

         unsigned char overlay_alpha = (unsigned char)((1.0f - fade_t) * 255.0f + 0.5f);

         vita2d_start_drawing();
         vita2d_clear_screen();

         draw_mainmenu_backdrop();
         draw_panel_box(MAINMENU_PANEL_X, MAINMENU_PANEL_Y, MAINMENU_PANEL_W, MAINMENU_PANEL_H);
         draw_mainmenu_list(sel);
         draw_mainmenu_bottom_bar(sel);
         vita2d_draw_rectangle(0, 0, SCREEN_W, SCREEN_H, RGBA8(0, 0, 0, overlay_alpha));

         vita2d_end_drawing();
         vita2d_swap_buffers();

         if (fade_t >= 1.0f)
            break;
      }
   }

   for (;;)
   {
      unsigned pressed = poll_pressed(&pad_prev);

      if (pressed & SCE_CTRL_DOWN)
      {
         do { sel = (sel + 1) % MAINMENU_ITEM_COUNT; } while (!mainmenu_items[sel].enabled);
         sfx_play(sfx_choose_pcm, sfx_choose_frames);
      }
      if (pressed & SCE_CTRL_UP)
      {
         do { sel = (sel == 0) ? MAINMENU_ITEM_COUNT - 1 : sel - 1; } while (!mainmenu_items[sel].enabled);
         sfx_play(sfx_choose_pcm, sfx_choose_frames);
      }

      if (pressed & SCE_CTRL_CROSS)
      {
         sfx_play(sfx_select_pcm, sfx_select_frames);

         if (sel == MAINMENU_OFFLINE_PLAY)
            return MAINMENU_OFFLINE_PLAY;

         if (sel == MAINMENU_OPTIONS)
         {
            mainmenu_options_submenu();
            flush_pad(&pad_prev);
            continue;
         }

         if (sel == MAINMENU_QUIT)
            return MAINMENU_QUIT;
      }

      if (pressed & SCE_CTRL_CIRCLE)
      {
         prompt_caller_sel = sel;
         if (show_prompt(
               "If you return to the title screen,\nall unsaved progress will be lost.\nContinue?",
               PROMPT_YES_NO, draw_mainmenu_prompt_backdrop))
            return MAINMENU_ACTION_BACK_TO_TITLE;

         flush_pad(&pad_prev);
         continue;
      }

      vita2d_start_drawing();
      vita2d_clear_screen();

      draw_mainmenu_backdrop();
      draw_panel_box(MAINMENU_PANEL_X, MAINMENU_PANEL_Y, MAINMENU_PANEL_W, MAINMENU_PANEL_H);
      draw_mainmenu_list(sel);
      draw_mainmenu_bottom_bar(sel);

      vita2d_end_drawing();
      vita2d_swap_buffers();
   }
}

// offline-play flow: repeatedly drops into the game carousel and, once a
// game's picked, loads/plays/tears it down, same as this frontend always
// has. only new part is bailing back out to the caller (run_main_menu,
// via main()) when run_menu itself reports a circle-back rather than an
// actual selection
static void run_offline_play(void)
{
   for (;;)
   {
      // --- menu: pick a game ---

      start_menu_music();
      int selected_game_idx = run_menu();
      stop_menu_music();

      if (selected_game_idx < 0)
         return; // circle pressed in the carousel, back out to the main menu

      current_game_idx = selected_game_idx;
      active_player_port = game_player_port[selected_game_idx];
      bind_core_api(games[selected_game_idx].core);

      core_api.set_environment(environment_cb);
      core_api.set_video_refresh(video_refresh_cb);
      core_api.set_audio_sample(audio_sample_cb);
      core_api.set_audio_sample_batch(audio_sample_batch_cb);
      core_api.set_input_poll(input_poll_cb);
      core_api.set_input_state(input_state_cb);

      core_api.init();

      struct retro_game_info game;
      char rom_path[192];
      char load_rom_file[64];
      int rom_path_len;
      bool loaded;

      memset(&game, 0, sizeof(game));
      memset(rom_path, 0, sizeof(rom_path));
      snprintf(load_rom_file, sizeof(load_rom_file), "%s", pending_rom_file);

      {
         // switch to the now loading screen immediately instead of
         // freezing on the announcer stinger -- it plays on its own
         // thread (see play_game_announcer_async) while this loop keeps
         // swapping buffers every frame, and only lets the core boot
         // once that thread reports it's actually finished
         vita2d_texture *cover_texture = cover_tex[games[selected_game_idx].known_idx];

         play_game_announcer_async(games[selected_game_idx].known_idx);

         do
         {
            vita2d_start_drawing();
            vita2d_clear_screen();

            if (loading_bg)
               vita2d_draw_texture(loading_bg, 0, 0);

            if (cover_texture)
               vita2d_draw_texture(cover_texture, 264, 122);

            vita2d_end_drawing();
            vita2d_swap_buffers();
         }
         while (announcer_playing);

         if (announcer_thread_id >= 0)
         {
            sceKernelWaitThreadEnd(announcer_thread_id, NULL, NULL);
            sceKernelDeleteThread(announcer_thread_id);
            announcer_thread_id = -1;
         }
      }

      // --- load: hand the rom to the core ---

      // almost every attempt loads on the first try. the second pass
      // exists purely as a safety net for whatever region the player had
      // selected turning out to be unsupported by this exact core build
      // (wrong driver name for that clone, a variant this core was never
      // compiled with, a corrupt dump, etc), it quietly retries once
      // against the known good european parent set instead of just
      // rejecting the player outright. the original failure is still
      // written to core_log.txt either way, this only changes what the
      // player sees, and it never runs at all if they were already on
      // the european region or if the european set isn't even on disk
      loaded = false;

      for (int attempt = 0; attempt < 2 && !loaded; attempt++)
      {
         if (attempt == 1)
         {
            unsigned known_idx = games[selected_game_idx].known_idx;
            const char *const *eu_list;
            bool found = false;

            if ((rom_region_t)game_region[selected_game_idx] == REGION_EUROPE)
               break;

            eu_list = region_file_list(known_idx, REGION_EUROPE);

            for (unsigned v = 0; v < ROM_VARIANT_MAX && eu_list[v]; v++)
            {
               char eu_full[192];

               snprintf(eu_full, sizeof(eu_full), "%s%s", ROM_DIR, eu_list[v]);

               if (file_exists(eu_full))
               {
                  snprintf(load_rom_file, sizeof(load_rom_file), "%s", eu_list[v]);
                  found = true;
                  break;
               }
            }

            if (!found)
               break;

            core_log(RETRO_LOG_INFO,
                  "naohac: \"%s\" failed to load on the selected region, "
                  "falling back to the european set \"%s\"\n",
                  games[selected_game_idx].label, load_rom_file);
         }

         rom_path_len = snprintf(rom_path, sizeof(rom_path), "%s%s",
               ROM_DIR, load_rom_file);
         if (rom_path_len < 0 || (size_t)rom_path_len >= sizeof(rom_path))
         {
            core_log(RETRO_LOG_ERROR,
                  "naohac: rom path construction overflow for \"%s\"\n",
                  load_rom_file);
            break;
         }

         rom_path[sizeof(rom_path) - 1] = '\0';
         game.path = rom_path;

         // purely diagnostic, logged before the call so it's on record even
         // if load_game itself never gets a chance to log anything. driver
         // name here is load_rom_file with ".zip" stripped, i.e. exactly
         // what mame2000 will try to match against its own compiled-in
         // driver table before it ever opens the archive. if this line is
         // followed by a bare rejection with no further core-side output in
         // core_log.txt, that's the signature of this build's mame_retro
         // simply not having that driver name compiled in at all (common on
         // size-trimmed builds that keep only one variant per game) rather
         // than anything wrong with the rom file itself
         {
            char driver_name[64];
            size_t dn_len = strlen(load_rom_file);

            if (dn_len > 4 && !strcmp(load_rom_file + dn_len - 4, ".zip"))
               dn_len -= 4;

            snprintf(driver_name, sizeof(driver_name), "%.*s", (int)dn_len, load_rom_file);

            core_log(RETRO_LOG_INFO,
                  "naohac: loading \"%s\" via %s core (driver name \"%s\")\n",
                  rom_path,
                  (games[selected_game_idx].core == CORE_KIND_MAME) ? "mame2000" : "fba2012",
                  driver_name);
         }

         if (core_api.load_game(&game))
         {
            loaded = true;
         }
         else
         {
            core_log(RETRO_LOG_ERROR,
                  "naohac: load_game failed for \"%s\" (%s)\n",
                  games[selected_game_idx].label, rom_path);
         }
      }

      if (!loaded)
      {
         core_api.deinit();
         load_fail_popup(games[selected_game_idx].core);
         continue;
      }

      set_current_rom_stem(load_rom_file);
      game_loaded = true;

      // --- warm-up: burn past boot logos/warnings on every load ---

      warm_up_core(CORE_WARMUP_FRAMES);

      // --- auto-resume: reapply the last quick save, if any ---

      attempt_auto_resume(current_rom_stem);

      struct retro_system_av_info av_info;
      core_api.get_system_av_info(&av_info);

      // pull in whatever wallpaper/effects/filter/size this rom was last
      // left on
      load_game_config();

      tex_w = 0;
      tex_h = 0;

      core_port_rate = snap_sample_rate(av_info.timing.sample_rate);
      core_port_type = (core_port_rate == 44100 || core_port_rate == 48000)
            ? SCE_AUDIO_OUT_PORT_TYPE_BGM
            : SCE_AUDIO_OUT_PORT_TYPE_VOICE;

      core_audio_port_open();
      audio_stage_fill = 0;

      // --- run: play until the player exits back to the menu ---

      run_core();

      // --- teardown: standard order for every core ---

      if (game_loaded)
      {
         core_api.unload_game();
         game_loaded = false;
      }

      core_api.deinit();
   }
}

// how long the logo takes to reach full opacity once the title screen boots
#define TITLE_LOGO_FADE_US        1200000

// one full top -> center -> bottom -> center round trip of the background
// pan, in microseconds. bumped way up from the original 10s -- that read
// as a fast drift, this is a slow, barely-there one
#define TITLE_BG_PAN_PERIOD_US    40000000

// logo is top-anchored with this much headroom rather than pinned to a
// strict half-of-544 midline -- at 622x331 the art is taller than an exact
// upper half would allow without clipping, so this keeps it sitting high
// on the screen, clear of the banner near the bottom, without cropping it
#define TITLE_LOGO_TOP_PAD        40

// gap from the screen's bottom edge to the banner's bottom edge. nudged up
// 45px from the original 48 so the banner sits higher, clear of the very
// bottom of the screen
#define TITLE_BANNER_BOTTOM_PAD   93

// how long the fade to black takes once cross is pressed, before handing
// off to the main menu's own (slightly faster) fade-in -- see
// MAINMENU_FADEIN_US
#define TITLE_FADEOUT_US          500000

// crop rect for the title backdrop: same cover-fit scale math as
// cover_src_rect, but vertically panned by u instead of always centered.
// u=0 shows the top of the image, u=1 shows the bottom, u=0.5 the middle
static void title_bg_src_rect(vita2d_texture *tex, float dst_w, float dst_h, float u,
      float *sx, float *sy, float *sw, float *sh)
{
   float tw = (float)vita2d_texture_get_width(tex);
   float th = (float)vita2d_texture_get_height(tex);
   float scale_x = dst_w / tw;
   float scale_y = dst_h / th;
   float scale = (scale_x > scale_y) ? scale_x : scale_y;

   *sw = dst_w / scale;
   *sh = dst_h / scale;
   *sx = (tw - *sw) * 0.5f;
   *sy = (th - *sh) * u;
}

// slow, continuous vertical pan: starts at the vertical center, eases up
// to the top edge first, then reverses and eases all the way down to the
// bottom edge, looping forever. a plain sine keeps both turnarounds smooth
// (speed naturally drops to zero right at the top/bottom) without needing
// a separate easing curve on top of it
static void draw_title_background(SceInt64 elapsed_us)
{
   if (!title_bg_tex)
      return;

   float phase = (float)elapsed_us / (float)TITLE_BG_PAN_PERIOD_US * 2.0f * (float)M_PI;
   float u = 0.5f - 0.5f * sinf(phase);

   float sx, sy, sw, sh;
   title_bg_src_rect(title_bg_tex, (float)SCREEN_W, (float)SCREEN_H, u, &sx, &sy, &sw, &sh);

   vita2d_draw_texture_part_scale(title_bg_tex, 0.0f, 0.0f,
         sx, sy, sw, sh, (float)SCREEN_W / sw, (float)SCREEN_H / sh);
}

// logo, centered horizontally and sitting high on the screen (see
// TITLE_LOGO_TOP_PAD), fading in from alpha 0 on a smoothstep ease so the
// motion has no hard start/stop
static void draw_title_logo(float alpha)
{
   if (!title_logo_tex)
      return;

   int lw = vita2d_texture_get_width(title_logo_tex);
   int x  = (SCREEN_W - lw) / 2;

   unsigned tint = RGBA8(255, 255, 255, (unsigned)(alpha + 0.5f));
   vita2d_draw_texture_tint(title_logo_tex, (float)x, (float)TITLE_LOGO_TOP_PAD, tint);
}

// "press (x) to start" prompt, centered near the bottom. the button
// graphic is already baked into presstostart.png itself, so this is just
// a straight draw of the banner, no separate icon overlay needed
static void draw_title_prompt(void)
{
   if (!title_banner_tex)
      return;

   int bw = vita2d_texture_get_width(title_banner_tex);
   int bh = vita2d_texture_get_height(title_banner_tex);
   int bx = (SCREEN_W - bw) / 2;
   int by = SCREEN_H - TITLE_BANNER_BOTTOM_PAD - bh;

   vita2d_draw_texture(title_banner_tex, (float)bx, (float)by);
}

// title screen, shown once at boot ahead of the game-select menu. cross
// breaks out of the loop straight into the main menu. bgm is kicked off
// right here instead of waiting for run_menu to do it -- start_menu_music
// is a no-op once the thread's already running, so the same track just
// keeps playing through the handoff instead of restarting
static void run_title_screen(void)
{
   SceCtrlData pad_prev;
   SceInt64 start_us = sceKernelGetProcessTimeWide();

   start_menu_music();
   sfx_play(sfx_titlescreen_pcm, sfx_titlescreen_frames);
   flush_pad(&pad_prev);

   for (;;)
   {
      unsigned pressed = poll_pressed(&pad_prev);

      if (pressed & SCE_CTRL_CROSS)
      {
         // fire-and-forget on the ui_sfx thread instead of blocking this
         // loop until whenpressxtostart.ogg finishes -- the fade below
         // keeps the screen moving while it plays out
         sfx_play(sfx_pressx_pcm, sfx_pressx_frames);

         // fade to black before handing off to the main menu
         SceInt64 fadeout_start_us = sceKernelGetProcessTimeWide();

         for (;;)
         {
            SceInt64 elapsed      = sceKernelGetProcessTimeWide() - start_us;
            SceInt64 fade_elapsed = sceKernelGetProcessTimeWide() - fadeout_start_us;

            float fade_t = (float)fade_elapsed / (float)TITLE_FADEOUT_US;
            if (fade_t > 1.0f)
               fade_t = 1.0f;

            unsigned char overlay_alpha = (unsigned char)(fade_t * 255.0f + 0.5f);

            vita2d_start_drawing();
            vita2d_clear_screen();

            draw_title_background(elapsed);
            draw_title_logo(255.0f);
            draw_title_prompt();
            vita2d_draw_rectangle(0, 0, SCREEN_W, SCREEN_H, RGBA8(0, 0, 0, overlay_alpha));

            vita2d_end_drawing();
            vita2d_swap_buffers();

            if (fade_t >= 1.0f)
               break;
         }

         return;
      }

      SceInt64 elapsed = sceKernelGetProcessTimeWide() - start_us;

      float fade_t = (float)elapsed / (float)TITLE_LOGO_FADE_US;
      if (fade_t > 1.0f)
         fade_t = 1.0f;

      // smoothstep, eases both into and out of the fade rather than
      // ramping linearly
      float fade_alpha = fade_t * fade_t * (3.0f - 2.0f * fade_t) * 255.0f;

      vita2d_start_drawing();
      vita2d_clear_screen();

      draw_title_background(elapsed);
      draw_title_logo(fade_alpha);
      draw_title_prompt();

      vita2d_end_drawing();
      vita2d_swap_buffers();
   }
}

int main(int argc, char *argv[])
{
   (void)argc;
   (void)argv;

   // --- init: platform, filesystem, core-agnostic assets ---

   scePowerSetArmClockFrequency(444);
   sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG);
   sceTouchSetSamplingState(SCE_TOUCH_PORT_FRONT, SCE_TOUCH_SAMPLING_STATE_START);

   create_data_dirs();
   reset_core_log();
   migrate_legacy_rom_names();
   seed_default_nvram();

   vita2d_init();
   vita2d_set_clear_color(RGBA8(0, 0, 0, 255));

   font = vita2d_load_font_file("app0:assets/fonts/Saira-Regular.ttf");
   if (!font)
      debug_log("INIT: failed to load font Saira-Regular.ttf");

   loading_bg = vita2d_load_PNG_file("app0:assets/backgrounds/loadingbackground.png");
   if (!loading_bg)
      debug_log("INIT: failed to load loadingbackground.png");

   load_menu_assets();
   init_ui_sfx();
   init_game_list();

   run_title_screen();

   // --- flow: title screen -> main menu -> offline play (carousel) ---
   //
   // run_main_menu returns for three cases now: offline play drops into
   // the existing carousel loop, quit breaks out to the same teardown
   // path this app has always used, and back-to-title (circle, confirmed
   // through show_prompt) re-runs the title screen and then drops right
   // back into the main menu once it's dismissed again. circle from
   // inside the carousel returns control here without going through any
   // of those, see run_offline_play/run_menu

   for (;;)
   {
      int action = run_main_menu();

      if (action == MAINMENU_OFFLINE_PLAY)
         run_offline_play();
      else if (action == MAINMENU_QUIT)
         break;
      else if (action == MAINMENU_ACTION_BACK_TO_TITLE)
         run_title_screen();
   }

   stop_menu_music();
   free_ui_sfx();
   free_menu_assets();

   if (font)
      vita2d_free_font(font);

   if (loading_bg)
      vita2d_free_texture(loading_bg);

   vita2d_fini();
   sceKernelExitProcess(0);
   return 0;
}
