/**
 * @file trace.c
 * @brief Optional in-process frame tracing for benchmarks.
 */

/* See LICENSE for license details. */

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "trace.h"

static int trace_fd = -1;
static uint64_t trace_frame_number;
static uint64_t trace_trigger_ns;
static uint64_t trace_draw_ns;
static uint64_t trace_cells;
static void (*trace_sync_cb)(void*);
static void* trace_sync_data;

static uint64_t monotonic_ns(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static void write_all(int fd, const void* data, size_t size) {
  const char* cursor = data;
  while (size > 0) {
    ssize_t written = write(fd, cursor, size);
    if (written < 0) {
      if (errno == EINTR) continue;
      return;
    }
    cursor += written;
    size -= (size_t)written;
  }
}

void trace_init(const char* path) {
  if (!path || !*path) return;
  trace_fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
  if (trace_fd < 0) {
    fprintf(stderr, "st: cannot open trace file %s: %s\n", path,
            strerror(errno));
    return;
  }
  fcntl(trace_fd, F_SETFD, FD_CLOEXEC);
  TraceHeader header = {TRACE_MAGIC, TRACE_VERSION, sizeof(TraceRecord), 0};
  write_all(trace_fd, &header, sizeof(header));
}

void trace_set_sync(void (*sync)(void*), void* data) {
  trace_sync_cb = sync;
  trace_sync_data = data;
}

void trace_frame_trigger(void) {
  if (trace_fd < 0) return;
  trace_trigger_ns = monotonic_ns();
}

void trace_frame_begin(void) {
  if (trace_fd < 0) return;
  trace_draw_ns = monotonic_ns();
  trace_cells = 0;
}

void trace_frame_end(uint64_t cells) {
  if (trace_fd < 0) return;
  trace_cells = cells;
}

void trace_frame_flush(void) {
  if (trace_fd < 0 || !trace_draw_ns) return;
  TraceRecord record = {
    .frame = ++trace_frame_number,
    .trigger_ns = trace_trigger_ns,
    .draw_ns = trace_draw_ns,
    .flush_ns = monotonic_ns(),
    .sync_ns = 0,
    .cells = trace_cells,
  };
  if (trace_sync_cb) {
    trace_sync_cb(trace_sync_data);
    record.sync_ns = monotonic_ns();
  }
  write_all(trace_fd, &record, sizeof(record));
  trace_trigger_ns = 0;
  trace_draw_ns = 0;
  trace_cells = 0;
}

void trace_frame_cancel(void) {
  trace_trigger_ns = 0;
  trace_draw_ns = 0;
  trace_cells = 0;
}

void trace_finish(void) {
  if (trace_fd < 0) return;
  close(trace_fd);
  trace_fd = -1;
}
