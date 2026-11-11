/**
 * @file trace.h
 * @brief Optional in-process frame tracing for benchmarks.
 */

/* See LICENSE for license details. */

#ifndef ST_TRACE_H
#define ST_TRACE_H

#include <stdint.h>

#define TRACE_MAGIC 0x53544254u /* "STBT" */
#define TRACE_VERSION 2u

typedef struct {
  uint32_t magic;
  uint32_t version;
  uint32_t record_size;
  uint32_t reserved;
} TraceHeader;

/* Version 2 record. Version 1 had no sync_ns field. */
typedef struct {
  uint64_t frame;
  uint64_t trigger_ns;
  uint64_t draw_ns;
  uint64_t flush_ns;
  uint64_t sync_ns;
  uint64_t cells;
} TraceRecord;

void trace_init(const char* path);
void trace_set_sync(void (*sync)(void*), void* data);
void trace_frame_trigger(void);
void trace_frame_begin(void);
void trace_frame_end(uint64_t cells);
void trace_frame_flush(void);
void trace_frame_cancel(void);
void trace_finish(void);

#endif
