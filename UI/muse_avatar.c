#include "muse_avatar.h"

#include <stdint.h>

#include "lcdspi.h"

/*
 * The imported Muse renderer has no header of its own, so its API and the pose
 * it takes are declared here.  See avatars/muse_pixel.c, which is not in git.
 *
 *   uint32_t muse_pixel_accent(int mode);   (unused: the chat has its own colours)
 *   void muse_pixel_render(const void *pose);
 *   void muse_pixel_scale(uint16_t *dst, int stride_px, int x0, int x1, int y0, int y1);
 */
#ifdef COYOTE_MUSE_AVATAR
typedef struct {
    int mode;        /* 0..6, see muse_avatar_mode_t */
    int mouth;       /* 0 closed .. 3 wide open */
    int blink;       /* 0 eyes open, 1 eyes shut */
    uint32_t tick;   /* free-running counter for the overlays */
} muse_avatar_pose_t;

void muse_pixel_render(const void *pose);
void muse_pixel_scale(uint16_t *dst, int stride_px, int x0, int x1, int y0, int y1);

/* The renderer keeps the 64x64 grid; this picks the whole of it out one frame
 * at a time, so the frame is rendered once and then blitted row by row. */
static uint16_t avatar_frame[MUSE_AVATAR_SIZE * MUSE_AVATAR_SIZE];
static uint32_t avatar_frames;
#endif

bool muse_avatar_available(void) {
#ifdef COYOTE_MUSE_AVATAR
    return true;
#else
    return false;
#endif
}

#ifdef COYOTE_MUSE_AVATAR
static void avatar_pose(muse_avatar_pose_t *pose, muse_avatar_mode_t mode) {
    pose->mode = (int)mode;
    /* Speaking opens and closes the mouth; the other moods keep it shut. */
    pose->mouth = mode == MUSE_AVATAR_SPEAKING ? (int)((avatar_frames / 2u) % 4u) : 0;
    /* A blink every couple of seconds.  The error and off faces hold theirs. */
    pose->blink = (mode != MUSE_AVATAR_ERROR && mode != MUSE_AVATAR_OFF &&
                   (avatar_frames % 24u) < 2u) ? 1 : 0;
    pose->tick = avatar_frames++;
}
#endif

void muse_avatar_draw(int x, int y, muse_avatar_mode_t mode) {
#ifdef COYOTE_MUSE_AVATAR
    muse_avatar_pose_t pose;

    avatar_pose(&pose, mode);
    muse_pixel_render(&pose);
    muse_pixel_scale(avatar_frame, (int)MUSE_AVATAR_SIZE, 0, (int)MUSE_AVATAR_SIZE,
                     0, (int)MUSE_AVATAR_SIZE);
    draw_rgb565_spi(x, y, (int)MUSE_AVATAR_SIZE, (int)MUSE_AVATAR_SIZE, avatar_frame,
                    (int)MUSE_AVATAR_SIZE);
#else
    (void)x;
    (void)y;
    (void)mode;
#endif
}
