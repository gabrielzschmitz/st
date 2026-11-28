/*
 * Capture an X11 window by title and report simple pixel statistics.
 * Used by benchmarks/benchmark.py for visual validation.
 */

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char* window_name(Display* dpy, Window window) {
  char* name = NULL;
  Atom net_name = XInternAtom(dpy, "_NET_WM_NAME", False);
  Atom utf8 = XInternAtom(dpy, "UTF8_STRING", False);
  Atom type = None;
  int format = 0;
  unsigned long count = 0, after = 0;
  unsigned char* value = NULL;

  if (XGetWindowProperty(dpy, window, net_name, 0, 1024, False, utf8, &type,
                         &format, &count, &after, &value) == Success &&
      value) {
    if (type == utf8 && format == 8 && count) {
      name = strdup((char*)value);
    }
    XFree(value);
  }
  if (!name && !XFetchName(dpy, window, &name)) name = NULL;
  return name;
}

static int find_window(Display* dpy, Window root, const char* title,
                       Window* result) {
  Window root_return, parent_return, *children = NULL;
  unsigned int count = 0;
  char* name = window_name(dpy, root);

  if (name) {
    int match = strcmp(name, title) == 0;
    free(name);
    if (match) {
      *result = root;
      return 1;
    }
  }

  if (!XQueryTree(dpy, root, &root_return, &parent_return, &children, &count))
    return 0;

  for (unsigned int i = 0; i < count; i++) {
    if (find_window(dpy, children[i], title, result)) {
      if (children) XFree(children);
      return 1;
    }
  }
  if (children) XFree(children);
  return 0;
}

static void list_windows(Display* dpy, Window root, int depth) {
  Window root_return, parent_return, *children = NULL;
  unsigned int count = 0;
  char* name = window_name(dpy, root);
  if (name) {
    printf("%*s0x%lx %s\n", depth * 2, "", (unsigned long)root, name);
    free(name);
  }
  if (!XQueryTree(dpy, root, &root_return, &parent_return, &children, &count))
    return;
  for (unsigned int i = 0; i < count; i++)
    list_windows(dpy, children[i], depth + 1);
  if (children) XFree(children);
}

static unsigned long pixel_component(unsigned long pixel, unsigned long mask) {
  unsigned int shift = 0;
  unsigned long shifted_mask = mask;

  if (!mask) return 0;
  while (!(shifted_mask & 1)) {
    shifted_mask >>= 1;
    shift++;
  }
  return (pixel & mask) >> shift;
}

int main(int argc, char** argv) {
  const char* title = NULL;
  const char* output = NULL;
  int stats = 0;
  int list = 0;

  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "--stats") == 0)
      stats = 1;
    else if (strcmp(argv[i], "--list") == 0)
      list = 1;
    else if (strcmp(argv[i], "--title") == 0 && i + 1 < argc)
      title = argv[++i];
    else if (strcmp(argv[i], "--output") == 0 && i + 1 < argc)
      output = argv[++i];
  }
  if (!title && !list) {
    fprintf(stderr,
            "usage: %s --title TITLE [--output FILE.ppm] [--stats]\n"
            "       %s --list\n",
            argv[0], argv[0]);
    return 2;
  }

  Display* dpy = XOpenDisplay(NULL);
  if (!dpy) {
    fprintf(stderr, "xcapture: cannot open display\n");
    return 1;
  }

  if (list) {
    list_windows(dpy, DefaultRootWindow(dpy), 0);
    XCloseDisplay(dpy);
    return 0;
  }

  Window window = 0;
  if (!find_window(dpy, DefaultRootWindow(dpy), title, &window)) {
    fprintf(stderr, "xcapture: window not found: %s\n", title);
    XCloseDisplay(dpy);
    return 1;
  }

  XWindowAttributes attrs;
  if (!XGetWindowAttributes(dpy, window, &attrs)) {
    fprintf(stderr, "xcapture: cannot read window attributes\n");
    XCloseDisplay(dpy);
    return 1;
  }

  XImage* image = XGetImage(dpy, window, 0, 0, attrs.width, attrs.height,
                            AllPlanes, ZPixmap);
  if (!image) {
    fprintf(stderr, "xcapture: cannot capture window\n");
    XCloseDisplay(dpy);
    return 1;
  }

  unsigned long red_mask = image->red_mask ? image->red_mask : 0xff0000;
  unsigned long green_mask = image->green_mask ? image->green_mask : 0x00ff00;
  unsigned long blue_mask = image->blue_mask ? image->blue_mask : 0x0000ff;
  unsigned long total = (unsigned long)attrs.width * attrs.height;

  if (output) {
    FILE* file = fopen(output, "wb");
    if (!file) {
      fprintf(stderr, "xcapture: cannot open %s: %s\n", output,
              strerror(errno));
      XDestroyImage(image);
      XCloseDisplay(dpy);
      return 1;
    }
    fprintf(file, "P6\n%d %d\n255\n", attrs.width, attrs.height);
    for (int y = 0; y < attrs.height; y++) {
      for (int x = 0; x < attrs.width; x++) {
        unsigned long pixel = XGetPixel(image, x, y);
        unsigned char rgb[3] = {
          (unsigned char)pixel_component(pixel, red_mask),
          (unsigned char)pixel_component(pixel, green_mask),
          (unsigned char)pixel_component(pixel, blue_mask),
        };
        fwrite(rgb, 1, sizeof(rgb), file);
      }
    }
    fclose(file);
  }

  if (stats) {
    const unsigned long bucket_count = 65536;
    unsigned long dominant = 0;
    unsigned long distinct = 0;
    unsigned long* buckets = calloc(bucket_count, sizeof(unsigned long));
    if (!buckets) {
      XDestroyImage(image);
      XCloseDisplay(dpy);
      return 1;
    }
    for (int y = 0; y < attrs.height; y++) {
      for (int x = 0; x < attrs.width; x++) {
        unsigned long pixel = XGetPixel(image, x, y);
        unsigned long key = (pixel * 2654435761u) % bucket_count;
        buckets[key]++;
      }
    }
    for (unsigned long i = 0; i < bucket_count; i++) {
      if (buckets[i] > dominant) dominant = buckets[i];
      if (buckets[i] > 8) distinct++;
    }
    printf("{\"width\":%d,\"height\":%d,\"pixels\":%lu,"
           "\"dominant\":%lu,\"dominant_ratio\":%.6f,\"distinct\":%lu}\n",
           attrs.width, attrs.height, total, dominant,
           total ? (double)dominant / (double)total : 0.0, distinct);
    free(buckets);
  }

  XDestroyImage(image);
  XCloseDisplay(dpy);
  return 0;
}
