/* C interface of the GPU library (gpu/): shadPS4's Liverpool/Vulkan video core,
 * GnmDriver, VideoOut and kernel event queues, adapted to the native loader. */
#ifndef WIPEOUTGPU_H
#define WIPEOUTGPU_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct {
    const char *title;          /* window title */
    const char *serial;         /* CUSA id, names the pipeline cache */
    const char *user_dir;       /* pipeline cache/logs directory */
    uint32_t sdk_version;       /* from the eboot's procparam */
    uint32_t psf_attributes;    /* param.sfo ATTRIBUTE */
    int32_t width, height;      /* initial window size */
} WipeoutGpuConfig;
/* Registers kernel event queues (needed with or without graphics). */
void wipeoutgpu_register_kernel(void);
/* Creates window, Vulkan device, presenter and GPU command processor. */
int wipeoutgpu_init(const WipeoutGpuConfig *config);
/* Function for an imported NID ("NID#lib#mod"), or 0 when the GPU library does not provide it. */
uintptr_t wipeoutgpu_resolve(const char *scoped_nid);
/* Called first by the loader's SIGSEGV handler: 1 when a GPU page-tracking fault was handled. */
int wipeoutgpu_handle_fault(void *ucontext, void *address);
/* WIPEOUT_CPU_PROFILE (Windows): the threads it samples and the counters it reports per frame.
 * The game thread that flips frames, the GPU command thread and the frames shown so far: */
void wipeoutgpu_frame_threads(unsigned long *game_thread, unsigned long *gpu_thread, uint64_t *frames);
/* The draw recording thread, the TSC cycles the GPU command thread waited for it, and the
 * Vulkan recording thread: */
void wipeoutgpu_draw_rec_thread(unsigned long *thread, uint64_t *drain_cycles, unsigned long *vk_recorder);
/* Draws, dispatches and graphics submissions so far: */
void wipeoutgpu_draw_counts(uint64_t *draws, uint64_t *dispatches, uint64_t *submissions);
/* WIPEOUT_WRITE_LOG=1: prints the logged GPU-side writes to guest memory near the fault. */
void wipeoutgpu_dump_guest_writes(void *ucontext);
/* Keyboard text entry through the game window (IME dialog). begin returns 0 when
 * no window exists; poll returns 0 typing, 1 confirmed, 2 cancelled (UTF-8 text). */
int wipeoutgpu_text_input_begin(const char *initial_utf8, const char *prompt_utf8);
int wipeoutgpu_text_input_poll(char *out_utf8, uint64_t size);
/* A list for the player to choose from, drawn over the game (save list). images: a PNG path per
 * item drawn beside it (NULL array or entry: none). begin: 0 when no window exists.
 * poll: -1 while open, -2 cancelled, else the chosen index. */
int wipeoutgpu_choice_begin(const char *title, const char *const *items, const char *const *images, int count, int focus);
int wipeoutgpu_choice_poll(void);
/* 1 while the in-game settings menu is open: the game's pad input is held neutral. */
int wipeoutgpu_overlay_captures_input(void);
/* Controller vibration strength set in the menu, 0 (off) to 100 (the game's own). */
int wipeoutgpu_vibration_percent(void);
/* Stick deadzone set in the menu or the launcher, in percent of the stick's reach (0: off). */
int wipeoutgpu_stick_deadzone_percent(void);
/* The video decoder handed the game a picture: a cutscene is on screen (the presenter draws
 * black bars outside its 16:9 picture when the render resolution is not 16:9). */
void wipeoutgpu_movie_picture(void);
/* Saves the game's next frame to the screenshots folder, as F12 does. */
void wipeoutgpu_request_screenshot(void);
/* A short message in the lower right corner for a few seconds (the custom soundtrack's track). */
void wipeoutgpu_notice(const char *utf8);
/* wipeout.ini "music" and "music_shuffle": the player's songs play, in random order (runtime_music.c). */
int wipeoutgpu_music_on(void);
int wipeoutgpu_music_shuffle(void);
/* wipeout.ini "music_continue": a race goes on with the song that was playing (#93). */
int wipeoutgpu_music_continue(void);
/* wipeout.ini "music_volume": their loudness in percent (100: about the game's own music). */
int wipeoutgpu_music_volume(void);
/* wipeout.ini "game_music_volume": the game's own music in percent, 0 to 300 (#97). */
int wipeoutgpu_game_music_volume(void);
/* Mouse movement in px that counts as one step of a stick prompt, set in the menu. */
/* The pause key (touchpad right) was pressed: it lets go of the mouse lock, or takes it back. */
void wipeoutgpu_mouse_pause_key(void);
/* L2 aim with the mouse, set in the menu: 0 relative (follows movement), 1 absolute. */
int wipeoutgpu_mouse_aim_style(void);
/* L2 aim sensitivity in percent (10 to 200), set in the menu. */
int wipeoutgpu_mouse_aim_sensitivity(void);
/* Number of symbols registered by the vendored libraries (diagnostics). */
unsigned wipeoutgpu_symbol_count(void);
#ifdef __cplusplus
}
#endif
#endif
