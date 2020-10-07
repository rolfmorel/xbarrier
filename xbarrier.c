/*
 * xbarrier -- X pointer barrier
 *
 * Copyright (c), Rolf Morel <me@rolfmorel.me>
 * Copyright (c), Zev Weiss <zev@bewilderbeest.net>
 *
 * Sets up a single pointer barrier at specified coordinates.
 * Has multiple options for releasing the pointer, also at other locations.
 * Intended to be used by scripts, e.g. to lock the mouse to one monitor.
 *
 * Originally pointer-barriers-interactive.c by Jasper St. Pierre.
 * Subsequently it was adapted into xdpd, by Zev Weiss.
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY
 * SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR
 * IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <time.h>
#include <math.h>
#include <stdlib.h>
#include <errno.h>
#include <assert.h>
#include <string.h>
#include <stdio.h>
#include <getopt.h>
#include <unistd.h>
#include <sys/time.h>
#include <sys/param.h>
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/extensions/Xfixes.h>
#include <X11/extensions/XInput2.h>
#include <X11/extensions/Xrandr.h>

#ifdef DEBUG
#define dbg(...) (void)fprintf(stderr, __VA_ARGS__)
#else
/* Define as an empty function so arguments get used */
static inline void dbg(const char* fmt, ...) { }
#endif

#define SIGNUM(x) (((x) > 0) - ((x) < 0))

#define error(msg, ...) do { \
    fprintf(stderr, "Error: "msg"\n", ##__VA_ARGS__); \
    exit(1); \
  } while (0)

#define internal_error(msg, ...) do { \
    fprintf(stderr, "Internal error: "msg"\n", ##__VA_ARGS__); \
    abort(); \
  } while (0)

static const char* progname;

typedef struct {
  double x;
  double y;
} Vector;

static struct {
  PointerBarrier xid;
  int directions;
  Vector pos; // the first endpoint defining the line segment
  Vector disp; // when added to pos gives the second endpoint
} barrier;

/* What action is initiated on satifying a condition */
typedef enum {
  ACTION_NONE = 0,
    // -> do nothing, the default action
  ACTION_RELEASE,
    // -> let pointer pass through the barrier
  ACTION_PRINT,
    // -> print a location to stdout
  ACTION_WARP,    
    // map position within barrier to another line segment, with scaling
} ActionType;

typedef union {
  ActionType type;
  struct {
    ActionType __type; // either ACTION_WARP
    Vector pos; // the first endpoint defining the line segment
    Vector disp; // when added to pos gives the second endpoint
  } bar;
} Action;

typedef struct {
  Action action;
  double threshold;
} Condition;
static Condition min_speed = { action : { type : ACTION_NONE } };
static Condition max_speed = { action : { type : ACTION_NONE } };

static struct {
  Action action;
  double threshold;
  double dx;
  double dy;
} distance = { action : { type : ACTION_NONE } };

static struct {
  Action action;
  double threshold;
  double timestamp_last_entered;
  double timestamp_last_left;
} doubletap = { action : { type : ACTION_NONE } };

static Display* dpy;
static Window rootwin;
static int xi2_opcode;

/* Return the current seconds-since-epoch time as a double */
static inline double dnow(void)
{
  struct timeval t;
  if (gettimeofday(&t, NULL))
    internal_error("gettimeofday: %s\n", strerror(errno));
  return (double)t.tv_sec + (((double)t.tv_usec) / 1000000);
}

static void handle_barrier_leave(XIBarrierEvent* event)
{
  dbg("BarrierLeave, cursor: %.2f %.2f, delta: %.2f %.2f\n",
    event->root_x, event->root_y, event->dx, event->dy);

  distance.dx = 0;
  distance.dy = 0;

  doubletap.timestamp_last_left = dnow();
}

static void do_action(Action* action, XIBarrierEvent* event) {
  switch (action->type) {
  case ACTION_NONE:
    break;
  case ACTION_RELEASE:
    XIBarrierReleasePointer(dpy, event->deviceid, event->barrier, event->eventid);
    XFlush(dpy);
    break;
  case ACTION_PRINT:
    printf("%.0f %.0f\n", event->root_x, event->root_y);
    break;
  case ACTION_WARP:{
    Vector cursor_disp = {
      x: event->root_x - barrier.pos.x,
      y: event->root_y - barrier.pos.y,
    };

    double ratio = (cursor_disp.x + cursor_disp.y) /
                   (barrier.disp.x + barrier.disp.y);

    Vector cursor_pos = {
      x: action->bar.pos.x + ratio * action->bar.disp.x,
      y: action->bar.pos.y + ratio * action->bar.disp.y,
    };
 
    XWarpPointer(dpy, None, rootwin, 0, 0, 0, 0,
                 (int) cursor_pos.x, (int) cursor_pos.y);
    XFlush(dpy);
    break;}
  }
}

static void handle_barrier_hit(XIBarrierEvent* event)
{
  const double now = dnow();  
  const double dx = event->dx;
  const double dy = event->dy;

  dbg("BarrierHit, cursor: %.2f %.2f, delta: %.2f %.2f\n",
      event->root_x, event->root_y, event->dx, event->dy);

  // if cursor outside of barrier, project it onto barrier
    // FIXME: should really be a more elegant solution...
  if (barrier.disp.x == 0 &&
      !(barrier.pos.x <= event->root_x &&
      event->root_x <= barrier.pos.x + barrier.disp.x)) {
    dbg("outside barrier: %.2f %.2f ", event->root_x, event->root_y);

    event->root_x = barrier.pos.x;
    double high_y = MAX(barrier.pos.y, barrier.pos.y + barrier.disp.y);
    double low_y = MIN(barrier.pos.y, barrier.pos.y + barrier.disp.y);

    if (event->root_y > high_y)
      event->root_y = high_y;
    if (event->root_y < low_y)
      event->root_y = low_y;

    dbg("mapped to: %.2f %.2f\n", event->root_x, event->root_y);
  }
  if (barrier.disp.y == 0 &&
      !(barrier.pos.y <= event->root_y &&
      event->root_y <= barrier.pos.y + barrier.disp.y)) {
    dbg("outside barrier: %.2f %.2f ", event->root_x, event->root_y);

    event->root_y = barrier.pos.y;
    double high_x = MAX(barrier.pos.x, barrier.pos.x + barrier.disp.x);
    double low_x = MIN(barrier.pos.x, barrier.pos.x + barrier.disp.x);

    if (event->root_x > high_x)
      event->root_x = high_x;
    if (event->root_x < low_x)
      event->root_x = low_x;

    dbg("mapped to: %.2f %.2f\n", event->root_x, event->root_y);
    event->root_y = barrier.pos.y;
  }

  distance.dx += event->dx;
  distance.dy += event->dy;
  {
    const double dis_dx = distance.dx;
    const double dis_dy = distance.dy;

    // FIXME: should we be using L1 distance instead?
    double euclidian = sqrt(dis_dx * dis_dx + dis_dy * dis_dy);

    if (euclidian > distance.threshold)
      do_action(&distance.action, event);
  }

  double speed = sqrt(dx * dx + dy * dy);
  if (speed > min_speed.threshold)
    do_action(&min_speed.action, event);
  if (speed < max_speed.threshold)
    do_action(&max_speed.action, event);

  if (doubletap.timestamp_last_entered < doubletap.timestamp_last_left &&
      (now - doubletap.timestamp_last_entered) <= doubletap.threshold)
    do_action(&doubletap.action, event);

  doubletap.timestamp_last_entered = now;
}

/* Check for necessary extensions */
static void check_extensions(void)
{
  int major, minor, opcode, evt, err;

  if (!XQueryExtension(dpy, "XFIXES", &opcode, &evt, &err))
    error("XFixes extension not found");

  if (!XFixesQueryVersion(dpy, &major, &minor) || (major * 10 + minor) < 50)
    error("XFixes too old (have %d.%d, need 5.0+)", major, minor);

  if (!XQueryExtension(dpy, "XInputExtension", &xi2_opcode, &evt, &err))
    error("XInput extension not found");

  major = 2;
  minor = 3;

  if (XIQueryVersion(dpy, &major, &minor) != Success || ((major * 10) + minor) < 22)
    error("XInput too old (have %d.%d, need 2.2+)", major, minor);
}

static void usage(FILE* out, int full)
{
  fprintf(out, "Usage: %s X1 Y1 X2 Y2 DIRECTION [..] [ -h | -d DISTANCE [ACTION] | -s MAX_SPEED [ACTION] | -S MIN_SPEED [ACTION] | -m SECONDS [ACTION] ]\n", progname);
  if (!full)
    return;
  fprintf(out, "\n");
  fprintf(out, "Arguments:\n");
  fprintf(out, "  X1 Y1 X2 Y2 are the coordinates of a pixel wide pointer barrier\n");
  fprintf(out, "  DIRECTION is one of -x, +x, -y, +y, each indicating a direction in which the pointer is not obstructed\n");
  fprintf(out, "\n");
  fprintf(out, "Flags:\n");
  fprintf(out, "  -h %-12s print this usage message\n", "");
  fprintf(out, "  -d %-12s perform ACTION after DISTANCE pixels of (suppressed) pointer travel\n", "DISTANCE");
  fprintf(out, "  -s %-12s perform ACTION when cursor speed (against barrier) is below MAX_SPEED\n", "MAX_SPEED");
  fprintf(out, "  -S %-12s perform ACTION when cursor speed (against barrier) exceeds MIN_SPEED\n", "MIN_SPEED");
  fprintf(out, "  -t %-12s perform ACTION on two taps against barrier within SECONDS seconds\n", "SECONDS");
  fprintf(out, "\n");
  fprintf(out, "  ACTION is one of release, print, or warp X3 Y3 X4 Y4\n");
  fprintf(out, "         release lets the cursor pass through the barrier\n");
  fprintf(out, "         print only prints an event to stdout\n");
  fprintf(out, "         warp X3 Y3 X4 Y4 teleports the cursor to this second bar, with a linear scaling between the two bars\n");
}

static int parse_condition(int cur_arg, int argc, char** argv, Condition* condition) {
  condition->action.type = ACTION_RELEASE;

  char* end;
  condition->threshold = strtod(argv[cur_arg], &end);

  if (*end || condition->threshold < 0.0)
    error("Invalid threshold '%s' (must be numeric and non-negative)",
          argv[cur_arg]);
  if (++cur_arg >= argc) return cur_arg;

  if (strcmp(argv[cur_arg], "release") == 0) cur_arg++;
  else if (strcmp(argv[cur_arg], "print") == 0) {
    cur_arg++;
    condition->action.type = ACTION_PRINT;
  } else if (strcmp(argv[cur_arg], "warp") == 0) {
    cur_arg++;
    condition->action.type = ACTION_WARP;

    Vector* pos = &condition->action.bar.pos;
    Vector* disp = &condition->action.bar.disp;

    pos->x = strtod(argv[cur_arg++], &end);
    if (!*end) pos->y = strtod(argv[cur_arg++], &end);
    if (!*end) disp->x = strtod(argv[cur_arg++], &end) - pos->x;
    if (!*end) disp->y = strtod(argv[cur_arg++], &end) - pos->y;

    if (*end)
      error("Invalid coordinates (must be numeric and non-negative)");
  }

  return cur_arg;
}

static void set_options(int argc, char** argv)
{
  char* end;
  int cur_arg = 1;

  while (cur_arg < argc) {
    if (strcmp(argv[cur_arg], "-h") == 0) {
      usage(stdout, 1);
      exit(0);
    }

    if (strcmp(argv[cur_arg], "+x") == 0) {
      barrier.directions |= BarrierPositiveX;
      cur_arg++;
    } else if (strcmp(argv[cur_arg], "-x") == 0) {
      barrier.directions |= BarrierNegativeX;
      cur_arg++;
    } else if (strcmp(argv[cur_arg], "+y") == 0) {
      barrier.directions |= BarrierPositiveY;
      cur_arg++;
    } else if (strcmp(argv[cur_arg], "-y") == 0) {
      barrier.directions |= BarrierNegativeY;
      cur_arg++;
    } else if (strcmp(argv[cur_arg], "-d") == 0)
      cur_arg = parse_condition(++cur_arg, argc, argv, (Condition *) &distance);
    else if (strcmp(argv[cur_arg], "-s") == 0)
      cur_arg = parse_condition(++cur_arg, argc, argv, &max_speed);
    else if (strcmp(argv[cur_arg], "-S") == 0)
      cur_arg = parse_condition(++cur_arg, argc, argv, &min_speed);
    else if (strcmp(argv[cur_arg], "-t") == 0)
      cur_arg = parse_condition(++cur_arg, argc, argv, (Condition *) &doubletap);
    else {
      int coord_arg = cur_arg;
      barrier.pos.x = strtod(argv[coord_arg++], &end);
      if (!*end && coord_arg < argc)
        barrier.pos.y = strtod(argv[coord_arg++], &end);
      if (!*end && coord_arg < argc)
        barrier.disp.x = strtod(argv[coord_arg++], &end) - barrier.pos.x;
      if (!*end && coord_arg < argc)
        barrier.disp.y = strtod(argv[coord_arg++], &end) - barrier.pos.y;
      if (coord_arg != cur_arg + 4)
        error("Not enough coordinates provided");
      cur_arg = coord_arg;
      if (*end)
        error("Argument '%s' not recognized", argv[--coord_arg]);
      continue;
    }

  }

  if (barrier.directions == 0)
    error("At least one unobstructed barrier DIRECTION required");

  if (barrier.disp.x != 0 && barrier.disp.y != 0)
    error("Invalid coordinates (barrier must be one pixel thick)");
}


static void handle_xevent(void)
{
  XEvent xev;
  XGenericEventCookie* cookie;
  XNextEvent(dpy, &xev);

  if (xev.type == GenericEvent) {
    cookie = &xev.xcookie;
    if (!XGetEventData(dpy, cookie))
      return;

    if (cookie->extension == xi2_opcode) {
      if (cookie->evtype == XI_BarrierHit)
        handle_barrier_hit(cookie->data);
      if (cookie->evtype == XI_BarrierLeave)
        handle_barrier_leave(cookie->data);
    }

    XFreeEventData(dpy, cookie);
  } else
    dbg("[unexpected event; type=%d]\n", xev.type);
}

int main(int argc, char** argv)
{
  int xfd, nfds;
  fd_set rfds;

  progname = strrchr(argv[0], '/');
  progname = progname ? progname + 1 : argv[0];

  set_options(argc, argv);

  if (!(dpy = XOpenDisplay(NULL)))
    error("Failed to connect to X server");

  check_extensions();

  rootwin = XDefaultRootWindow(dpy);

  unsigned char mask_bits[XIMaskLen(XI_LASTEVENT)] = { 0, };
  XIEventMask mask = {
    .deviceid = XIAllMasterDevices,
    .mask = mask_bits,
    .mask_len = sizeof(mask_bits),
  };
  XISetMask(mask_bits, XI_BarrierHit);
  XISetMask(mask_bits, XI_BarrierLeave);
  XISelectEvents(dpy, rootwin, &mask, 1);

  xfd = XConnectionNumber(dpy);
  nfds = xfd + 1;

  dbg("barrier: %.0f %.0f %.0f %.0f\n", 
      barrier.pos.x, barrier.pos.y,
      barrier.pos.x + barrier.disp.x, barrier.pos.y + barrier.disp.y);

  PointerBarrier pb = XFixesCreatePointerBarrier(dpy, rootwin,
      barrier.pos.x, barrier.pos.y,
      barrier.pos.x + barrier.disp.x, barrier.pos.y + barrier.disp.y,
      barrier.directions, 0, NULL);

  barrier.xid = pb;

  XSync(dpy, False);
  for (;;) {
    FD_ZERO(&rfds);
    FD_SET(xfd, &rfds);

    if (select(nfds, &rfds, NULL, NULL, NULL) < 0) {
      perror("select");
      exit(1);
    }

    if (FD_ISSET(xfd, &rfds))
      handle_xevent();
  }
}
