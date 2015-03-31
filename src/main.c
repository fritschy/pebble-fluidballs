/* fluidballs, Copyright (c) 2000 by Peter Birtles <peter@bqdesign.com.au>
 *
 * Permission to use, copy, modify, distribute, and sell this software and its
 * documentation for any purpose is hereby granted without fee, provided that
 * the above copyright notice appear in all copies and that both that
 * copyright notice and this permission notice appear in supporting
 * documentation.  No representations are made about the suitability of this
 * software for any purpose.  It is provided "as is" without express or
 * implied warranty.
 *
 * Ported to X11 and xscreensaver by jwz, 27-Feb-2002.
 *
 * http://astronomy.swin.edu.au/~pbourke/modelling/fluid/
 *
 * Some physics improvements by Steven Barker <steve@blckknght.org>
 *
 * Port for pebble by Marcus Fritzsch
 */

/* Future ideas:
 * Specifying a distribution in the ball sizes (with a gamma curve, possibly).
 * Brownian motion, for that extra touch of realism.
 *
 * It would be nice to detect when there are more balls than fit in
 * the window, and scale the number of balls back.  Useful for the
 * xscreensaver-demo preview, which is often too tight by default.
 */

#include <pebble.h>

#if 1
#undef APP_LOG
#define APP_LOG(...)
#define START_TIME_MEASURE()
#define END_TIME_MEASURE()
#else
static unsigned int get_time(void)
{
   time_t s;
   uint16_t ms;
   time_ms(&s, &ms);
   return (s & 0xfffff) * 1000 + ms;
}

#define START_TIME_MEASURE() unsigned tm_0 = get_time()
#define END_TIME_MEASURE()                                                  \
   do                                                                       \
   {                                                                        \
      unsigned tm_1 = get_time();                                           \
      APP_LOG(APP_LOG_LEVEL_DEBUG, "%s: took %dms", __func__, tm_1 - tm_0); \
   } while (0)
#endif

#define M_PI 3.14159265358979323846f

#define NUMBALLS 30
#define GRAV (9.81f / 30.f)  // 1/30 of 1g

#define Q 10 // works quite well with 10 bits
#define F (1 << Q)
#define M (F - 1)
#define i2f(i) ((f32)((i)*F))
#define f2i(f) ((f32)((f) / F))
#define f2f(f) ((f) / (float)F)
typedef int32_t f32;

static float sqrtf(float f)
{
   float v = f * 0.5f;
#define IT() v = (v + f / v) * 0.5f
   IT();
   IT();
   IT();
   IT();
   IT();  // 5 is not enough for a nice animation
   IT();
   IT();
   IT();  // 8 looks good
   IT();
   IT();  // 10 looks even better
#undef IT
   return v;
}

static f32 sqrtx(f32 f)
{
   f32 v = f / 2;
#define IT() v = (v + i2f(f) / v) / 2
   IT();
   IT();
   IT();
   IT();
   IT();  // 5 is not enough for a nice animation
   IT();
//   IT();
//   IT();  // 8 looks good
//   IT();
//   IT();  // 10 looks even better
#undef IT
   return v;
}

static float rsqrtf(float number)
{
   float x2;
   const float threehalfs = 1.5F;

   union
   {
      uint32_t i;
      float y;
   } u;

   x2 = number * 0.5F;
   u.y = number;
   u.i = 0x5f3759df - (u.i >> 1);                // smart one!
   u.y = u.y * (threehalfs - (x2 * u.y * u.y));  // 1st iteration
   u.y = u.y * (threehalfs - (x2 * u.y * u.y));  // 2nd iteration

   return u.y;
}

extern inline unsigned int GameRand(void)
{
   static unsigned int low = 16180, high = 31415;
   high = (high << 16) + (high >> 16);
   high += low;
   low += high;
   return high;
}

extern inline float frand(float m) { return m * GameRand() / (float)-1u; }

extern inline float xrand(f32 m) { return m * (GameRand() & M); }

enum Gravity {
   GRAV_SENSOR,
   GRAV_SHOW
};

static struct
{
   GRect bounds;
   Window *window;
   Animation *anim;
   f32 accx;                       /* horizontal acceleration (wind) */
   f32 accy;                       /* vertical acceleration (gravity) */
   f32 vx[NUMBALLS], vy[NUMBALLS]; /* current ball velocities */
   f32 px[NUMBALLS], py[NUMBALLS]; /* current ball positions */
   f32 r[NUMBALLS];                /* ball radiuses */
   f32 m[NUMBALLS];                /* ball mass, precalculated */
   f32 e;                          /* coeficient of elasticity */
   enum Gravity grav;
} s_state;

static void fluidballs_init(void)
{
   s_state.accx = 0;
   s_state.accy = i2f(GRAV);
   s_state.e = i2f(0.97);
   s_state.grav = GRAV_SHOW;

   float max_radius = 10;

   for (int i = 0; i < NUMBALLS; i++)
   {
      float r = (frand(max_radius * 0.65f) + max_radius * 0.35f) / sqrtf((float)NUMBALLS / 50.f);
      s_state.r[i] = i2f(r);
      s_state.px[i] = i2f(frand(s_state.bounds.size.w - 2.f * r) + r);
      s_state.py[i] = i2f(frand(s_state.bounds.size.h - 2.f * r) + r);
      s_state.vx[i] = 0;  // frand(5) - 2.5;
      s_state.vy[i] = 0;  // frand(5) - 2.5;
      s_state.m[i] = i2f(r * r * r * M_PI * 4.f / 3.f);

      APP_LOG(APP_LOG_LEVEL_DEBUG,
              "created ball %d: p=(%d, %d), v=(%d, %d), r=%d, m=%d", i,
              (int)f2i(s_state.px[i]), (int)f2i(s_state.py[i]), (int)f2i(s_state.vx[i]),
              (int)f2i(s_state.vy[i]), (int)f2i(s_state.r[i]), (int)f2i(s_state.m[i]));
   }
}

/* Implements the laws of physics: move balls to their new positions.
 */
static void update_balls(void)
{
   APP_LOG(APP_LOG_LEVEL_DEBUG, "update_balls");

   f32 fe = s_state.e;

   uint16_t collision_count = 0;
   START_TIME_MEASURE();

   /* For each ball, compute the influence of every other ball. */
   for (int a = 0; a < NUMBALLS - 1; a++)
   {
      f32 fpxa = s_state.px[a], fpya = s_state.py[a], fra = s_state.r[a],
            fma = s_state.m[a], fvxa = s_state.vx[a], fvya = s_state.vy[a];

      for (int b = a + 1; b < NUMBALLS; b++)
      {
         f32 fpxb = s_state.px[b], fpyb = s_state.py[b], frb = s_state.r[b];
         f32 fdx = fpxa - fpxb;
         fdx = f2i(fdx * (long long)fdx);
         f32 fdy = fpya - fpyb;
         fdy = f2i(fdy * (long long)fdy);
         f32 fd = fdx + fdy;
         f32 fdee2 = f2i(fra + frb) * (long long)(fra + frb);

         if (fd < fdee2)
         {
            f32 fmb = s_state.m[b];

            f32 fvxb = s_state.vx[b];
            f32 fvyb = s_state.vy[b];

            collision_count++;
            fd = sqrtx(fd);
            f32 frd = i2f((long long)i2f(1)) / fd;
            f32 fdd = fra + frb - fd;
            f32 fcdx = f2i((fpxb - fpxa) * (long long)frd);
            f32 fcdy = f2i((fpyb - fpya) * (long long)frd);

            /* Move each ball apart from the other by half the
             * 'collision' distance.
             */
            f32 fdpx = f2i((fdd / 2) * (long long)fcdx);
            f32 fdpy = f2i((fdd / 2) * (long long)fcdy);
            fpxa -= fdpx;
            fpya -= fdpy;
            s_state.px[b] += fdpx;
            s_state.py[b] += fdpy;

            f32 fvca =
               f2i(fvxa * (long long)fcdx) + f2i(fvya * (long long)fcdy); /* the component of each velocity */
            f32 fvcb =
               f2i(fvxb * (long long)fcdx) + f2i(fvyb * (long long)fcdy); /* along the axis of the collision */

            /* elastic collison */
            f32 fdva = (f2i((long long)fvca * (fma - fmb)) + f2i(fvcb * (long long)fmb * 2)) / f2i(fma + fmb) - fvca;
            f32 fdvb = (f2i((long long)fvcb * (fmb - fma)) + f2i(fvca * (long long)fma * 2)) / f2i(fma + fmb) - fvcb;

            fdva = f2i(fdva * (long long)fe);
            fdvb = f2i(fdvb * (long long)fe);

#if 0
            dva += (frand (50) - 25) / ma;   /* q: why are elves so chaotic? */
            dvb += (frand (50) - 25) / mb;   /* a: brownian motion. */
#endif

            fvxa += f2i(fdva * (long long)fcdx);
            fvya += f2i(fdva * (long long)fcdy);
            fvxb += f2i(fdvb * (long long)fcdx);
            fvyb += f2i(fdvb * (long long)fcdy);

            s_state.vx[b] = fvxb;
            s_state.vy[b] = fvyb;
         }

         s_state.px[a] = fpxa;
         s_state.py[a] = fpya;
         s_state.vx[a] = fvxa;
         s_state.vy[a] = fvya;
      }
   }

   /* Force all balls to be on screen.
    */
   for (int a = 0; a < NUMBALLS; a++)
   {
      f32 r = s_state.r[a];
      if (s_state.px[a] < r)
      {
         s_state.px[a] = r;
         s_state.vx[a] = f2i(-s_state.vx[a] * fe);
      }
      if (s_state.px[a] + r > i2f(s_state.bounds.size.w))
      {
         s_state.px[a] = i2f(s_state.bounds.size.w) - r;
         s_state.vx[a] = f2i(-s_state.vx[a] * fe);
      }
      if (s_state.py[a] < r)
      {
         s_state.py[a] = r;
         s_state.vy[a] = f2i(-s_state.vy[a] * fe);
      }
      if (s_state.py[a] + r > i2f(s_state.bounds.size.h))
      {
         s_state.py[a] = i2f(s_state.bounds.size.h) - r;
         s_state.vy[a] = f2i(-s_state.vy[a] * fe);
      }
   }

   /* Apply gravity to all balls.
    */
   for (int a = 0; a < NUMBALLS; a++)
   {
      s_state.vx[a] += s_state.accx;
      s_state.vy[a] += s_state.accy;
      s_state.px[a] += s_state.vx[a];
      s_state.py[a] += s_state.vy[a];
   }

   END_TIME_MEASURE();

   APP_LOG(APP_LOG_LEVEL_DEBUG, "update_balls, had %d collisions",
           collision_count);
}

/* Erases the balls at their previous positions, and draws the new ones.
 */
static void repaint_balls(Layer *layer, GContext *ctx)
{
   APP_LOG(APP_LOG_LEVEL_DEBUG, "repaint_balls");

   START_TIME_MEASURE();

   // When I'm all grown up; I'll make this nice!
   const int outline_only = 1;

#if defined(PBL_PLATFORM_BASALT)
   graphics_context_set_antialiased(ctx, false);
   graphics_context_set_stroke_width(ctx, outline_only ? 1 : 0);
#endif

   // black bg
   graphics_context_set_fill_color(ctx, GColorBlack);
   graphics_fill_rect(ctx, s_state.bounds, 0, 0);

   // white frame with rounded corners
   graphics_context_set_fill_color(ctx, GColorWhite);
   graphics_fill_rect(ctx, s_state.bounds, 3, GCornersAll);

   // black blobs
   if (outline_only) {
      graphics_context_set_stroke_color(ctx, GColorBlack);
#if defined(PBL_PLATFORM_BASALT)
      graphics_context_set_fill_color(ctx, GColorBrightGreen);
#endif
   } else {
      graphics_context_set_fill_color(ctx, GColorBlack);
   }

   for (int a = 0; a < NUMBALLS; a++)
   {
      if (outline_only) {
#if defined(PBL_PLATFORM_BASALT)
         graphics_fill_circle(
            ctx, (GPoint){.x = f2i(s_state.px[a]), .y = f2i(s_state.py[a])}, f2i(s_state.r[a]));
#endif
         graphics_draw_circle(
            ctx, (GPoint){.x = f2i(s_state.px[a]), .y = f2i(s_state.py[a])}, f2i(s_state.r[a]));
      } else {
         graphics_fill_circle(
            ctx, (GPoint){.x = f2i(s_state.px[a]), .y = f2i(s_state.py[a])}, f2i(s_state.r[a]));
      }
   }

   END_TIME_MEASURE();
}

static void window_load(Window *window)
{
   layer_set_update_proc(window_get_root_layer(window), repaint_balls);
   animation_schedule(s_state.anim);
   APP_LOG(APP_LOG_LEVEL_DEBUG, "window %p loaded", window);
}

static void update_gravity(void)
{
   static int u = 0;

   if (s_state.grav == GRAV_SENSOR) {
// untested
      AccelData adata;
      int e;
      if ((e = accel_service_peek(&adata)) < 0) {
         APP_LOG(APP_LOG_LEVEL_DEBUG, "Could not get accel data: %d", e);
         return;
      }
      s_state.accx = i2f(adata.x) >> 15; // / (30.f * 1024.f));
      s_state.accy = i2f(-adata.y) >> 15; // / (30.f * 1024.f));

      u = 0;
   } else {
      u++;

      const int frames = 40;
      static int sign = 1;

      switch (u / frames)
      {
      case 0:
         s_state.accx = 0;
         s_state.accy = i2f(GRAV);
         break;
      case 3+5:
         s_state.accx = sign * i2f(GRAV);
         s_state.accy = 0;
         break;
      case 4+5:
         s_state.accx = 0;
         break;
      }

      // 6, let no grav last 2x as long
      if (u >= frames * 25)
      {
         u = 0;
         sign = -sign;
      }
   }
}

static void window_unload(Window *window)
{
   layer_set_update_proc(window_get_root_layer(window), NULL);
   animation_unschedule_all();
   APP_LOG(APP_LOG_LEVEL_DEBUG, "window %p unloaded", window);
}

static void anim_setup(Animation *anim) {}

static void anim_update(Animation *anim, const uint32_t d)
{
   layer_mark_dirty(window_get_root_layer(s_state.window));
   update_gravity();
   update_balls();
}

static void anim_teardown(Animation *anim) {}

static AnimationImplementation anim_impl = {
   .setup = anim_setup, .update = anim_update, .teardown = anim_teardown};

static void down_single_click_handler(ClickRecognizerRef recognizer, void *context) {
   s_state.grav ^= 1;
}

static void config_provider(Window *window) {
   window_single_click_subscribe(BUTTON_ID_DOWN, down_single_click_handler);
}

static void init(void)
{
   accel_data_service_subscribe(0, NULL);
   s_state.window = window_create();
   window_set_fullscreen(s_state.window, true);
   s_state.bounds = (GRect){.size = (GSize){.w = 144, .h = 168}};
   s_state.anim = animation_create();
   fluidballs_init();
   animation_set_duration(s_state.anim, ANIMATION_DURATION_INFINITE);
   animation_set_implementation(s_state.anim, &anim_impl);
   window_set_window_handlers(s_state.window,
                              (WindowHandlers){
                                 .load = window_load, .unload = window_unload,
                              });
   window_stack_push(s_state.window, false);
   window_set_click_config_provider(s_state.window, (ClickConfigProvider) config_provider);
}

static void deinit(void) {
   accel_data_service_unsubscribe();
   window_destroy(s_state.window);
}

int main(void)
{
   init();

   APP_LOG(APP_LOG_LEVEL_DEBUG, "Done initializing, pushed window: %p",
           s_state.window);

   app_event_loop();
   deinit();
}
