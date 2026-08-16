/* Dual core CPS2/MAME launcher for PS Vita. Statically links the
 * fbalpha2012_cps2 and mame2000 libretro cores and drives them
 * directly, no RetroArch involved. Core symbols were prefixed via
 * objcopy as fba_retro_* and mame_retro_* to avoid collisions. */

#include <psp2/ctrl.h>
#include <psp2/audioout.h>
#include <psp2/power.h>
#include <psp2/gxm.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/io/stat.h>
#include <vita2d.h>
#include <dirent.h>
#include <stdarg.h>
#include <stdio.h>
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

// extract dir from path
void path_basedir(char *path)
{
   char *last_slash;

   if (!path || !*path)
      return;

   last_slash = strrchr(path, '/');
   if (!last_slash)
      last_slash = strrchr(path, '\\');

   if (!last_slash)
   {
      path[0] = '.';
      path[1] = '\0';
      return;
   }

   last_slash[1] = '\0';
}

bool path_mkdir(const char *dir) { (void)dir; return false; }

#define SCREEN_W 960
#define SCREEN_H 544

#define SAVE_DIR   "ux0:data/NaohAC/saves"
#define SYSTEM_DIR "ux0:data/NaohAC/system"
#define ROM_DIR    "ux0:data/NaohAC/roms/"

typedef enum {
   CORE_KIND_FBA = 0,
   CORE_KIND_MAME
} core_kind_t;

typedef struct {
   const char *file;
   const char *label;
   core_kind_t core;
} known_game_t;

static const known_game_t known_games[] = {
   { "msh.zip",      "MARVEL SUPER HEROES",                    CORE_KIND_FBA  },
   { "mshvsf.zip",   "MARVEL SUPER HEROES VS. STREET FIGHTER", CORE_KIND_FBA  },
   { "mvsc.zip",     "MARVEL VS. CAPCOM",                      CORE_KIND_FBA  },
   { "xmcota.zip",   "X-MEN: CHILDREN OF THE ATOM",            CORE_KIND_FBA  },
   { "xmvsf.zip",    "X-MEN VS. STREET FIGHTER",               CORE_KIND_FBA  },
   { "punisher.zip", "THE PUNISHER",                           CORE_KIND_MAME },
};
#define KNOWN_COUNT (sizeof(known_games) / sizeof(known_games[0]))

typedef struct {
   const char *label;
   char path[160];
   core_kind_t core;
   unsigned known_idx;
} game_entry_t;

static game_entry_t games[KNOWN_COUNT];
static unsigned game_count = 0;

// match files in rom dir with known games
static void scan_roms(void)
{
   DIR *dir = opendir(ROM_DIR);
   struct dirent *ent;

   game_count = 0;

   if (!dir)
      return;

   while ((ent = readdir(dir)) != NULL)
   {
      for (unsigned i = 0; i < KNOWN_COUNT; i++)
      {
         if (strcmp(ent->d_name, known_games[i].file) != 0)
            continue;

         games[game_count].label     = known_games[i].label;
         games[game_count].core      = known_games[i].core;
         games[game_count].known_idx = i;
         snprintf(games[game_count].path, sizeof(games[game_count].path),
               "%s%s", ROM_DIR, ent->d_name);
         game_count++;
         break;
      }
   }

   closedir(dir);
}

#define AUDIO_SAMPLERATE 32000 
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
   bool (*load_game)(const struct retro_game_info *);
   void (*unload_game)(void);
   void (*get_system_av_info)(struct retro_system_av_info *);
   void (*run)(void);
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
extern bool fba_retro_load_game(const struct retro_game_info *);
extern void fba_retro_unload_game(void);
extern void fba_retro_get_system_av_info(struct retro_system_av_info *);
extern void fba_retro_run(void);

extern void mame_retro_set_environment(retro_environment_t);
extern void mame_retro_set_video_refresh(retro_video_refresh_t);
extern void mame_retro_set_audio_sample(retro_audio_sample_t);
extern void mame_retro_set_audio_sample_batch(retro_audio_sample_batch_t);
extern void mame_retro_set_input_poll(retro_input_poll_t);
extern void mame_retro_set_input_state(retro_input_state_t);
extern void mame_retro_init(void);
extern void mame_retro_deinit(void);
extern bool mame_retro_load_game(const struct retro_game_info *);
extern void mame_retro_unload_game(void);
extern void mame_retro_get_system_av_info(struct retro_system_av_info *);
extern void mame_retro_run(void);

// bind libretro api based on selected core
static void bind_core_api(core_kind_t core)
{
   if (core == CORE_KIND_MAME)
   {
      core_api.set_environment        = mame_retro_set_environment;
      core_api.set_video_refresh      = mame_retro_set_video_refresh;
      core_api.set_audio_sample       = mame_retro_set_audio_sample;
      core_api.set_audio_sample_batch = mame_retro_set_audio_sample_batch;
      core_api.set_input_poll         = mame_retro_set_input_poll;
      core_api.set_input_state        = mame_retro_set_input_state;
      core_api.init                   = mame_retro_init;
      core_api.deinit                 = mame_retro_deinit;
      core_api.load_game              = mame_retro_load_game;
      core_api.unload_game            = mame_retro_unload_game;
      core_api.get_system_av_info     = mame_retro_get_system_av_info;
      core_api.run                    = mame_retro_run;
   }
   else
   {
      core_api.set_environment        = fba_retro_set_environment;
      core_api.set_video_refresh      = fba_retro_set_video_refresh;
      core_api.set_audio_sample       = fba_retro_set_audio_sample;
      core_api.set_audio_sample_batch = fba_retro_set_audio_sample_batch;
      core_api.set_input_poll         = fba_retro_set_input_poll;
      core_api.set_input_state        = fba_retro_set_input_state;
      core_api.init                   = fba_retro_init;
      core_api.deinit                 = fba_retro_deinit;
      core_api.load_game              = fba_retro_load_game;
      core_api.unload_game            = fba_retro_unload_game;
      core_api.get_system_av_info     = fba_retro_get_system_av_info;
      core_api.run                    = fba_retro_run;
   }
}

static vita2d_texture *frame_tex = NULL;
static int current_overlay = 0;
static vita2d_texture *overlay_tex = NULL;
static unsigned tex_w = 0;
static unsigned tex_h = 0;

static const void *fb_data  = NULL;
static unsigned     fb_w    = 0;
static unsigned     fb_h    = 0;
static size_t        fb_pitch = 0;

static SceCtrlData pad;
static int audio_port = -1;
static int core_port_type = 0;
static int core_port_rate = 0;
static int16_t audio_stage[AUDIO_PORT_GRAIN * 2];
static unsigned audio_stage_fill = 0;
static vita2d_font *font = NULL;
static vita2d_texture *loading_bg = NULL;
static bool game_loaded = false;

#define FONT_BASE_PX   16
#define HILITE_W_PAD   40
#define HILITE_H       34
#define HILITE_Y_OFF   24
#define ASPECT_VALUE_GAP 24

#define SFX_RATE       44100
#define SFX_GRAIN      512

static int16_t   *sfx_choose_pcm    = NULL;
static unsigned    sfx_choose_frames = 0;
static int16_t   *sfx_select_pcm    = NULL;
static unsigned    sfx_select_frames = 0;

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

static void init_ui_sfx(void)
{
   sfx_choose_pcm = load_ogg_full("app0:assets/choose.ogg", &sfx_choose_frames);
   sfx_select_pcm = load_ogg_full("app0:assets/select.ogg", &sfx_select_frames);
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
   sfx_choose_pcm = NULL;
   sfx_select_pcm = NULL;
}

#define MENU_MUSIC_PATH  "app0:assets/mainmenu.ogg"
#define MENU_MUSIC_RATE  44100
#define MENU_MUSIC_GRAIN 1024

static SceUID music_thread_id = -1;
static volatile bool music_thread_run = false;
static int music_audio_port = -1;

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

   int vol[2] = { SCE_AUDIO_VOLUME_0DB, SCE_AUDIO_VOLUME_0DB };
   sceAudioOutSetVolume(music_audio_port,
         SCE_AUDIO_VOLUME_FLAG_L_CH | SCE_AUDIO_VOLUME_FLAG_R_CH, vol);

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

static vita2d_texture *background_tex = NULL;
static vita2d_texture *pause_bg_tex = NULL;
static vita2d_texture *cover_tex[KNOWN_COUNT];

#define GRID_COLS      3
#define GRID_ROWS      2
#define COVER_W        216
#define COVER_H        150
#define ROW0_STAGGER_X 45

static const int grid_col_cx[GRID_COLS] = { 200, 480, 760 };
static const int grid_row_cy[GRID_ROWS] = { 190, 372 };

static void load_menu_assets(void)
{
   char path[160];

   background_tex = vita2d_load_PNG_file("app0:assets/background.png");
   pause_bg_tex   = vita2d_load_PNG_file("app0:assets/pause_bg.png");

   // cache cover textures in ram
   for (unsigned i = 0; i < KNOWN_COUNT; i++)
   {
      size_t len = strlen(known_games[i].file);
      size_t base_len = (len > 4) ? len - 4 : len;
      snprintf(path, sizeof(path), "ux0:data/NaohAC/covers/%.*s.png",
            (int)base_len, known_games[i].file);
      cover_tex[i] = vita2d_load_PNG_file(path);
   }
}

static void free_menu_assets(void)
{
   for (unsigned i = 0; i < KNOWN_COUNT; i++)
   {
      if (cover_tex[i])
      {
         vita2d_free_texture(cover_tex[i]);
         cover_tex[i] = NULL;
      }
   }

   if (background_tex)
   {
      vita2d_free_texture(background_tex);
      background_tex = NULL;
   }

   if (pause_bg_tex)
   {
      vita2d_free_texture(pause_bg_tex);
      pause_bg_tex = NULL;
   }
}

#define ASPECT_FULLSCREEN 0
#define ASPECT_4_3        1
#define ASPECT_5_4        2

static int aspect_mode = ASPECT_FULLSCREEN;

#define PAUSE_SCREEN_MAIN    0
#define PAUSE_SCREEN_OPTIONS 1

#define PAUSE_RESUME     0
#define PAUSE_OPTIONS    1
#define PAUSE_EXIT       2
#define PAUSE_ITEM_COUNT 3

#define OPT_ASPECT     0
#define OPT_OVERLAY    1
#define OPT_ITEM_COUNT 2

static const char *pause_labels[PAUSE_ITEM_COUNT] = {
   "RESUME",
   "OPTIONS",
   "EXIT GAME"
};

static const char *opt_labels[OPT_ITEM_COUNT] = {
   "ASPECT RATIO",
   "OVERLAY"
};

static void core_log(enum retro_log_level level, const char *fmt, ...)
{
   va_list ap;
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
         *(const char **)data = SAVE_DIR;
         return true;

      case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
         *(const char **)data = SYSTEM_DIR;
         return true;

      case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT:
         return *(enum retro_pixel_format *)data == RETRO_PIXEL_FORMAT_RGB565;

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

// read input and map left analog stick to d-pad
static void update_pad(void)
{
   sceCtrlPeekBufferPositive(0, &pad, 1);
   if (pad.lx < 80)  pad.buttons |= SCE_CTRL_LEFT;
   if (pad.lx > 170) pad.buttons |= SCE_CTRL_RIGHT;
   if (pad.ly < 80)  pad.buttons |= SCE_CTRL_UP;
   if (pad.ly > 170) pad.buttons |= SCE_CTRL_DOWN;
}

static void input_poll_cb(void)
{
   update_pad();
}

// map vita pad to libretro joypad
static int16_t input_state_cb(unsigned port, unsigned device, unsigned index, unsigned id)
{
   static const struct { unsigned id; unsigned mask; } map[] = {
      { RETRO_DEVICE_ID_JOYPAD_UP,     SCE_CTRL_UP },
      { RETRO_DEVICE_ID_JOYPAD_DOWN,   SCE_CTRL_DOWN },
      { RETRO_DEVICE_ID_JOYPAD_LEFT,   SCE_CTRL_LEFT },
      { RETRO_DEVICE_ID_JOYPAD_RIGHT,  SCE_CTRL_RIGHT },
      { RETRO_DEVICE_ID_JOYPAD_A,      SCE_CTRL_CIRCLE },
      { RETRO_DEVICE_ID_JOYPAD_B,      SCE_CTRL_CROSS },
      { RETRO_DEVICE_ID_JOYPAD_X,      SCE_CTRL_TRIANGLE },
      { RETRO_DEVICE_ID_JOYPAD_Y,      SCE_CTRL_SQUARE },
      { RETRO_DEVICE_ID_JOYPAD_L,      SCE_CTRL_L1 },
      { RETRO_DEVICE_ID_JOYPAD_R,      SCE_CTRL_R1 },
      { RETRO_DEVICE_ID_JOYPAD_START,  SCE_CTRL_START },
      { RETRO_DEVICE_ID_JOYPAD_SELECT, SCE_CTRL_SELECT },
   };

   (void)index;

   if (port != 0)
      return 0;

   if (device != RETRO_DEVICE_JOYPAD)
      return 0;

   for (unsigned i = 0; i < sizeof(map) / sizeof(map[0]); i++)
      if (map[i].id == id)
         return (pad.buttons & map[i].mask) ? 1 : 0;

   return 0;
}

// compute scaled destination rect
static void compute_dest_rect(unsigned w, unsigned h, float *dx, float *dy, float *dw, float *dh)
{
   if (aspect_mode == ASPECT_4_3)
   {
      float target_aspect = 4.0f / 3.0f;
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
   else if (aspect_mode == ASPECT_5_4)
   {
      float target_aspect = 5.0f / 4.0f;
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
      float src_aspect = (float)w / (float)h;
      float screen_aspect = (float)SCREEN_W / (float)SCREEN_H;

      if (src_aspect > screen_aspect)
      {
         *dw = SCREEN_W;
         *dh = SCREEN_W / src_aspect;
      }
      else
      {
         *dh = SCREEN_H;
         *dw = SCREEN_H * src_aspect;
      }
   }

   *dx = (SCREEN_W - *dw) / 2.0f;
   *dy = (SCREEN_H - *dh) / 2.0f;
}

static void blit_frame(void)
{
   if (!fb_data || !fb_w || !fb_h)
      return;

   if (!frame_tex || tex_w != fb_w || tex_h != fb_h)
   {
      if (frame_tex)
         vita2d_free_texture(frame_tex);

      frame_tex = vita2d_create_empty_texture_format(fb_w, fb_h, SCE_GXM_TEXTURE_FORMAT_R5G6B5);
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
   
   // draw overlay if aspect ratio leaves borders
   if ((aspect_mode == ASPECT_4_3 || aspect_mode == ASPECT_5_4) && overlay_tex != NULL)
   {
      float ow = (float)vita2d_texture_get_width(overlay_tex);
      float oh = (float)vita2d_texture_get_height(overlay_tex);
      vita2d_draw_texture_scale(overlay_tex, 0.0f, 0.0f, SCREEN_W / ow, SCREEN_H / oh);
   }

   vita2d_draw_texture_scale(frame_tex, dx, dy, dw / (float)fb_w, dh / (float)fb_h);

   vita2d_end_drawing();
   vita2d_swap_buffers();
}

#define PAUSE_BG_FALLBACK_W  400
#define PAUSE_BG_FALLBACK_H  400
#define PAUSE_BG_LOGO_OFFSET 90
#define PAUSE_BG_BOTTOM_MARGIN 40
#define ITEM_SPACING_MAIN 56
#define ITEM_SPACING_OPT  60

static float font_text_w(float scale, const char *s)
{
   return (float)vita2d_font_text_width(font, (unsigned int)(FONT_BASE_PX * scale), s);
}

static void draw_text_c(int cx, int y, unsigned color, float scale, const char *s)
{
   vita2d_font_draw_text(font, cx - (int)(font_text_w(scale, s) / 2.0f), y, color,
         (unsigned int)(FONT_BASE_PX * scale), s);
}

static void draw_hilite_rect(int cx, int y, float scale, const char *s, int x_min, int x_max)
{
   float tw = font_text_w(scale, s);
   float x0 = cx - tw / 2.0f - HILITE_W_PAD / 2.0f;
   float x1 = cx + tw / 2.0f + HILITE_W_PAD / 2.0f;

   if (x0 < x_min) x0 = x_min;
   if (x1 > x_max) x1 = x_max;

   vita2d_draw_rectangle(x0, y - HILITE_Y_OFF, x1 - x0, HILITE_H, RGBA8(220, 0, 0, 204));
}

static bool pause_menu(void)
{
   int screen = PAUSE_SCREEN_MAIN;
   int sel = PAUSE_RESUME;
   int opt_sel = OPT_ASPECT;
   SceCtrlData pad_prev;

   do
   {
      update_pad();
   } while (pad.buttons != 0);

   memset(&pad_prev, 0, sizeof(pad_prev));

   for (;;)
   {
      update_pad();
      unsigned pressed = (pad.buttons ^ pad_prev.buttons) & pad.buttons;
      pad_prev = pad;

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
            if (sel == PAUSE_OPTIONS)
            {
               screen = PAUSE_SCREEN_OPTIONS;
               opt_sel = OPT_ASPECT;
            }
            if (sel == PAUSE_EXIT)
               return true;
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
            if (opt_sel == OPT_ASPECT)
               aspect_mode = (aspect_mode + 1) % 3;
            else if (opt_sel == OPT_OVERLAY)
            {
               current_overlay = (current_overlay + 1) % 11;

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
         }

         if (pressed & SCE_CTRL_CIRCLE)
            screen = PAUSE_SCREEN_MAIN;
      }

      vita2d_start_drawing();
      vita2d_clear_screen();

      if (frame_tex && fb_w && fb_h)
      {
         float dx, dy, dw, dh;
         compute_dest_rect(fb_w, fb_h, &dx, &dy, &dw, &dh);

         if ((aspect_mode == ASPECT_4_3 || aspect_mode == ASPECT_5_4) && overlay_tex != NULL)
         {
            float ow = (float)vita2d_texture_get_width(overlay_tex);
            float oh = (float)vita2d_texture_get_height(overlay_tex);
            vita2d_draw_texture_scale(overlay_tex, 0.0f, 0.0f, SCREEN_W / ow, SCREEN_H / oh);
         }

         vita2d_draw_texture_scale(frame_tex, dx, dy, dw / (float)fb_w, dh / (float)fb_h);
      }

      // dim background for pause menu
      vita2d_draw_rectangle(0, 0, SCREEN_W, SCREEN_H, RGBA8(0, 0, 0, 180));

      int bg_w = pause_bg_tex ? (int)vita2d_texture_get_width(pause_bg_tex)  : PAUSE_BG_FALLBACK_W;
      int bg_h = pause_bg_tex ? (int)vita2d_texture_get_height(pause_bg_tex) : PAUSE_BG_FALLBACK_H;
      int bg_x = (SCREEN_W - bg_w) / 2;
      int bg_y = (SCREEN_H - bg_h) / 2;

      if (pause_bg_tex)
         vita2d_draw_texture(pause_bg_tex, bg_x, bg_y);

      int cx = bg_x + bg_w / 2;
      int x_min = bg_x + 20;
      int x_max = bg_x + bg_w - 20;
      int content_top    = bg_y + PAUSE_BG_LOGO_OFFSET;
      int content_bottom = bg_y + bg_h - PAUSE_BG_BOTTOM_MARGIN;
      int content_cy      = (content_top + content_bottom) / 2;

      if (screen == PAUSE_SCREEN_MAIN)
      {
         int block_h = (PAUSE_ITEM_COUNT - 1) * ITEM_SPACING_MAIN;
         int item_y  = content_cy - block_h / 2;

         for (int i = 0; i < PAUSE_ITEM_COUNT; i++)
         {
            if (i == sel)
               draw_hilite_rect(cx, item_y, 1.0f, pause_labels[i], x_min, x_max);

            draw_text_c(cx, item_y, RGBA8(0, 0, 0, 255), 1.0f, pause_labels[i]);
            item_y += ITEM_SPACING_MAIN;
         }
      }
      else
      {
         int block_h = (OPT_ITEM_COUNT - 1) * ITEM_SPACING_OPT + ASPECT_VALUE_GAP;
         int item_y  = content_cy - block_h / 2;

         for (int i = 0; i < OPT_ITEM_COUNT; i++)
         {
            if (i == OPT_ASPECT)
            {
               const char *val_str = aspect_mode == ASPECT_FULLSCREEN ? "FULLSCREEN" :
                     (aspect_mode == ASPECT_4_3 ? "4:3" : "5:4");

               if (i == opt_sel)
               {
                  float w1 = font_text_w(1.0f, opt_labels[i]);
                  float w2 = font_text_w(1.0f, val_str);
                  float max_w = w1 > w2 ? w1 : w2;
                  float x0 = cx - max_w / 2.0f - HILITE_W_PAD / 2.0f;
                  float x1 = cx + max_w / 2.0f + HILITE_W_PAD / 2.0f;

                  if (x0 < x_min) x0 = x_min;
                  if (x1 > x_max) x1 = x_max;

                  vita2d_draw_rectangle(x0, item_y - HILITE_Y_OFF, x1 - x0,
                        HILITE_H + ASPECT_VALUE_GAP, RGBA8(220, 0, 0, 204));
               }

               draw_text_c(cx, item_y, RGBA8(0, 0, 0, 255), 1.0f, opt_labels[i]);
               draw_text_c(cx, item_y + ASPECT_VALUE_GAP, RGBA8(0, 0, 0, 255), 1.0f, val_str);
               item_y += ITEM_SPACING_OPT + ASPECT_VALUE_GAP;
            }
            else
            {
               char ovl_str[32];

               if (current_overlay == 0)
                  snprintf(ovl_str, sizeof(ovl_str), "OVERLAY: OFF");
               else
                  snprintf(ovl_str, sizeof(ovl_str), "OVERLAY: %d", current_overlay);

               if (i == opt_sel)
                  draw_hilite_rect(cx, item_y, 1.0f, ovl_str, x_min, x_max);

               draw_text_c(cx, item_y, RGBA8(0, 0, 0, 255), 1.0f, ovl_str);
               item_y += ITEM_SPACING_OPT;
            }
         }
      }

      vita2d_end_drawing();
      vita2d_swap_buffers();
   }
}

static int run_menu(void)
{
   int sel = 0;
   SceCtrlData pad_prev;

   do
   {
      update_pad();
   } while (pad.buttons != 0);

   memset(&pad_prev, 0, sizeof(pad_prev));

   for (;;)
   {
      update_pad();
      unsigned pressed = (pad.buttons ^ pad_prev.buttons) & pad.buttons;
      pad_prev = pad;

      if (game_count > 0)
      {
         unsigned rows_used = (game_count + GRID_COLS - 1) / GRID_COLS;
         unsigned row = (unsigned)sel / GRID_COLS;
         unsigned col = (unsigned)sel % GRID_COLS;

         if (pressed & SCE_CTRL_LEFT)
            col = (col == 0) ? GRID_COLS - 1 : col - 1;
         if (pressed & SCE_CTRL_RIGHT)
            col = (col + 1) % GRID_COLS;
         if (pressed & SCE_CTRL_UP)
            row = (row == 0) ? rows_used - 1 : row - 1;
         if (pressed & SCE_CTRL_DOWN)
            row = (row + 1) % rows_used;
         
         if (pressed & (SCE_CTRL_LEFT | SCE_CTRL_RIGHT | SCE_CTRL_UP | SCE_CTRL_DOWN))
            sfx_play(sfx_choose_pcm, sfx_choose_frames);

         unsigned new_sel = row * GRID_COLS + col;
         if (new_sel >= game_count)
            new_sel = game_count - 1;
         sel = (int)new_sel;

         if (pressed & (SCE_CTRL_CROSS | SCE_CTRL_START))
         {
            // exit menu loop on selection
            sfx_play(sfx_select_pcm, sfx_select_frames);
            return sel;
         }
      }

      vita2d_start_drawing();
      vita2d_clear_screen();

      if (background_tex)
      {
         float bw = (float)vita2d_texture_get_width(background_tex);
         float bh = (float)vita2d_texture_get_height(background_tex);
         vita2d_draw_texture_scale(background_tex, 0, 0,
               SCREEN_W / bw, SCREEN_H / bh);
      }

      if (game_count == 0)
      {
         vita2d_font_draw_text(font, 80, 300, RGBA8(200, 80, 80, 255), FONT_BASE_PX,
               "NO ROMS FOUND IN " ROM_DIR);
      }

      for (unsigned i = 0; i < game_count && i < GRID_COLS * GRID_ROWS; i++)
      {
         unsigned row = i / GRID_COLS;
         unsigned col = i % GRID_COLS;
         bool is_sel = (i == (unsigned)sel);
         float scale = is_sel ? 1.12f : 1.0f; // slightly scale up highlighted cover
         float w = COVER_W * scale;
         float h = COVER_H * scale;
         float cx = (float)grid_col_cx[col] + (row == 0 ? ROW0_STAGGER_X : 0);
         float cy = (float)grid_row_cy[row];
         float x = cx - w / 2.0f;
         float y = cy - h / 2.0f;

         if (is_sel)
            vita2d_draw_rectangle(x - 6, y - 6, w + 12, h + 12, RGBA8(255, 200, 0, 255));

         vita2d_texture *tex = cover_tex[games[i].known_idx];
         if (tex)
         {
            float tw = (float)vita2d_texture_get_width(tex);
            float th = (float)vita2d_texture_get_height(tex);
            vita2d_draw_texture_scale(tex, x, y, w / tw, h / th);
         }
         else
         {
            vita2d_draw_rectangle(x, y, w, h, RGBA8(60, 60, 60, 255));
            vita2d_font_draw_text(font, (int)(x + w / 2.0f - 55), (int)(y + h / 2.0f),
                  RGBA8(220, 220, 220, 255), (unsigned int)(FONT_BASE_PX * 0.9f),
                  known_games[games[i].known_idx].file);
         }
      }

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
      update_pad();
      unsigned pressed = (pad.buttons ^ pad_prev.buttons) & pad.buttons;
      pad_prev = pad;

      // start + select pause combo
      if ((pad.buttons & (SCE_CTRL_SELECT | SCE_CTRL_START)) == (SCE_CTRL_SELECT | SCE_CTRL_START)
            && (pressed & (SCE_CTRL_SELECT | SCE_CTRL_START)))
      {
         core_audio_port_close();

         bool exit_core = pause_menu();

         core_audio_port_open();
         audio_stage_fill = 0;

         if (exit_core)
            break;

         memset(&pad_prev, 0, sizeof(pad_prev));
         continue;
      }

      core_api.run();
      blit_frame();
   }

   core_audio_port_close();

   if (game_loaded)
   {
      core_api.unload_game();
      game_loaded = false;
   }
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
}

int main(int argc, char *argv[])
{
   (void)argc;
   (void)argv;

   // max out cpu clock
   scePowerSetArmClockFrequency(444);
   sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG);

   create_data_dirs();

   vita2d_init();
   vita2d_set_clear_color(RGBA8(0, 0, 0, 255));
   
   font = vita2d_load_font_file("app0:assets/PressStart2P-Regular.ttf");
   loading_bg = vita2d_load_PNG_file("app0:assets/loadingbackground.png");
   
   load_menu_assets();
   init_ui_sfx();

   scan_roms();

   for (;;)
   {
      start_menu_music();

      int sel = run_menu();

      stop_menu_music();

      bind_core_api(games[sel].core);

      core_api.set_environment(environment_cb);
      core_api.set_video_refresh(video_refresh_cb);
      core_api.set_audio_sample(audio_sample_cb);
      core_api.set_audio_sample_batch(audio_sample_batch_cb);
      core_api.set_input_poll(input_poll_cb);
      core_api.set_input_state(input_state_cb);

      core_api.init();

      struct retro_game_info game;
      memset(&game, 0, sizeof(game));
      game.path = games[sel].path;

      {
         // render loading screen before core init blocks thread
         vita2d_start_drawing();
         vita2d_clear_screen();

         if (loading_bg)
            vita2d_draw_texture(loading_bg, 0, 0);

         vita2d_texture *ltex = cover_tex[games[sel].known_idx];
         if (ltex)
            vita2d_draw_texture(ltex, 264, 122);

         vita2d_end_drawing();
         vita2d_swap_buffers();
      }

      if (!core_api.load_game(&game))
      {
         core_api.deinit();
         continue;
      }

      game_loaded = true;

      struct retro_system_av_info av_info;
      core_api.get_system_av_info(&av_info);

      tex_w = 0;
      tex_h = 0;

      core_port_rate = snap_sample_rate(av_info.timing.sample_rate);
      core_port_type = (core_port_rate == 44100 || core_port_rate == 48000)
            ? SCE_AUDIO_OUT_PORT_TYPE_BGM
            : SCE_AUDIO_OUT_PORT_TYPE_VOICE;

      core_audio_port_open();

      run_core();

      core_api.deinit();
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
