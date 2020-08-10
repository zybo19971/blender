/*
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 */

/** \file
 * \ingroup clog
 */

#include <assert.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* Disable for small single threaded programs
 * to avoid having to link with pthreads. */
#ifdef WITH_CLOG_PTHREADS
#  include "atomic_ops.h"
#  include <pthread.h>
#endif

/* For 'isatty' to check for color. */
#if defined(__unix__) || defined(__APPLE__) || defined(__HAIKU__)
#  include <sys/time.h>
#  include <unistd.h>
#endif

#if defined(_MSC_VER)
#  include <io.h>
#  include <windows.h>
#endif

/* For printing timestamp. */
#define __STDC_FORMAT_MACROS
#include <errno.h>
#include <inttypes.h>

/* Only other dependency (could use regular malloc too). */
#include "MEM_guardedalloc.h"

/* own include. */
#include "CLG_log.h"

/* Local utility defines */
#define STREQ(a, b) (strcmp(a, b) == 0)
#define STREQLEN(a, b, n) (strncmp(a, b, n) == 0)

#ifdef _WIN32
#  define PATHSEP_CHAR '\\'
#else
#  define PATHSEP_CHAR '/'
#endif

/* -------------------------------------------------------------------- */
/** \name Internal Types
 * \{ */

typedef struct CLG_IDFilter {
  struct CLG_IDFilter *next;
  /** Over alloc. */
  char match[0];
} CLG_IDFilter;

typedef struct CLogContext {
  /** Single linked list of types.  */
  CLG_LogType *types;
  CLG_LogRecordList log_records;

#ifdef WITH_CLOG_PTHREADS
  pthread_mutex_t types_lock;
#endif

  /* exclude, include filters.  */
  CLG_IDFilter *filters[2];
  bool use_color;
  bool use_basename;
  bool use_timestamp;

  /** Owned. */
  int output;
  FILE *output_file;

  /** For timer (log_use_timestamp). */
  uint64_t timestamp_tick_start;

  /** For new types. */
  struct {
    unsigned short level;
    unsigned short severity_level;
  } default_type;

  struct {
    void (*fatal_fn)(void *file_handle);
    void (*backtrace_fn)(void *file_handle);
  } callbacks;

  bool use_stdout;
  bool always_show_warnings;
  /** used only is use_stdout is false */
  char output_file_path[256];
} CLogContext;

/** \} */

/* -------------------------------------------------------------------- */
/** \name Mini Buffer Functionality
 *
 * Use so we can do a single call to write.
 * \{ */

/* TODO (grzelins) temporary fix for handling big log messages */
#define CLOG_BUF_LEN_INIT 4096

typedef struct CLogStringBuf {
  char *data;
  uint len;
  uint len_alloc;
  bool is_alloc;
} CLogStringBuf;

static void clg_str_init(CLogStringBuf *cstr, char *buf_stack, uint buf_stack_len)
{
  cstr->data = buf_stack;
  cstr->len_alloc = buf_stack_len;
  cstr->len = 0;
  cstr->is_alloc = false;
}

static void clg_str_free(CLogStringBuf *cstr)
{
  if (cstr->is_alloc) {
    MEM_freeN(cstr->data);
  }
}

static void clg_str_reserve(CLogStringBuf *cstr, const uint len)
{
  if (len > cstr->len_alloc) {
    cstr->len_alloc *= 2;
    if (len > cstr->len_alloc) {
      cstr->len_alloc = len;
    }

    if (cstr->is_alloc) {
      cstr->data = MEM_reallocN(cstr->data, cstr->len_alloc);
    }
    else {
      /* Copy the static buffer. */
      char *data = MEM_mallocN(cstr->len_alloc, __func__);
      memcpy(data, cstr->data, cstr->len);
      cstr->data = data;
      cstr->is_alloc = true;
    }
  }
}

static void clg_str_append_with_len(CLogStringBuf *cstr, const char *str, const uint len)
{
  uint len_next = cstr->len + len;
  clg_str_reserve(cstr, len_next);
  char *str_dst = cstr->data + cstr->len;
  memcpy(str_dst, str, len);
#if 0 /* no need. */
  str_dst[len] = '\0';
#endif
  cstr->len = len_next;
}

static void clg_str_append(CLogStringBuf *cstr, const char *str)
{
  clg_str_append_with_len(cstr, str, strlen(str));
}

ATTR_PRINTF_FORMAT(2, 0)
static void clg_str_vappendf(CLogStringBuf *cstr, const char *fmt, va_list args)
{
  /* Use limit because windows may use '-1' for a formatting error. */
  const uint len_max = 65535;
  while (true) {
    uint len_avail = cstr->len_alloc - cstr->len;

    va_list args_cpy;
    va_copy(args_cpy, args);
    int retval = vsnprintf(cstr->data + cstr->len, len_avail, fmt, args_cpy);
    va_end(args_cpy);

    if (retval < 0) {
      /* Some encoding error happened, not much we can do here, besides skipping/cancelling this
       * message. */
      break;
    }
    else if ((uint)retval <= len_avail) {
      /* Copy was successful. */
      cstr->len += (uint)retval;
      break;
    }
    else {
      /* vsnprintf was not successful, due to lack of allocated space, retval contains expected
       * length of the formated string, use it to allocate required amount of memory. */
      uint len_alloc = cstr->len + (uint)retval;
      if (len_alloc >= len_max) {
        /* Safe upper-limit, just in case... */
        break;
      }
      clg_str_reserve(cstr, len_alloc);
      len_avail = cstr->len_alloc - cstr->len;
    }
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Internal Utilities
 * \{ */

enum eCLogColor {
  COLOR_DEFAULT,
  COLOR_RED,
  COLOR_GREEN,
  COLOR_YELLOW,

  COLOR_RESET,
};
#define COLOR_LEN (COLOR_RESET + 1)

static const char *clg_color_table[COLOR_LEN] = {NULL};

static void clg_color_table_init(bool use_color)
{
  for (int i = 0; i < COLOR_LEN; i++) {
    clg_color_table[i] = "";
  }
  if (use_color) {
#ifdef _WIN32
    /* TODO */
#else
    clg_color_table[COLOR_DEFAULT] = "\033[1;37m";
    clg_color_table[COLOR_RED] = "\033[1;31m";
    clg_color_table[COLOR_GREEN] = "\033[1;32m";
    clg_color_table[COLOR_YELLOW] = "\033[1;33m";
    clg_color_table[COLOR_RESET] = "\033[0m";
#endif
  }
}

static const char *clg_severity_str[CLG_SEVERITY_LEN] = {
    [CLG_SEVERITY_DEBUG] = "DEBUG",
    [CLG_SEVERITY_VERBOSE] = "VERBOSE",
    [CLG_SEVERITY_INFO] = "INFO",
    [CLG_SEVERITY_WARN] = "WARN",
    [CLG_SEVERITY_ERROR] = "ERROR",
    [CLG_SEVERITY_FATAL] = "FATAL",
};

const char *clg_severity_as_text(enum CLG_Severity severity)
{
  bool ok = (unsigned int)severity < CLG_SEVERITY_LEN;
  assert(ok);
  if (ok) {
    return clg_severity_str[severity];
  }
  else {
    return "INVALID_SEVERITY";
  }
}

static enum eCLogColor clg_severity_to_color(enum CLG_Severity severity)
{
  assert((unsigned int)severity < CLG_SEVERITY_LEN);
  enum eCLogColor color = COLOR_DEFAULT;
  switch (severity) {
    case CLG_SEVERITY_DEBUG:
      color = COLOR_DEFAULT;
      break;
    case CLG_SEVERITY_VERBOSE:
      color = COLOR_DEFAULT;
      break;
    case CLG_SEVERITY_INFO:
      color = COLOR_DEFAULT;
      break;
    case CLG_SEVERITY_WARN:
      color = COLOR_YELLOW;
      break;
    case CLG_SEVERITY_ERROR:
    case CLG_SEVERITY_FATAL:
      color = COLOR_RED;
      break;
    default:
      /* should never get here. */
      assert(false);
  }
  return color;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Context Type Access
 * \{ */

/**
 * Filter the indentifier based on very basic globbing.
 * - `foo` exact match of `foo`.
 * - `foo.bar` exact match for `foo.bar`
 * - `foo.*` match for `foo` & `foo.bar` & `foo.bar.baz`
 * - `*` matches everything.
 */
static bool clg_ctx_filter_check(CLogContext *ctx, const char *identifier)
{
  const int identifier_len = strlen(identifier);
  for (uint i = 0; i < 2; i++) {
    const CLG_IDFilter *flt = ctx->filters[i];
    while (flt != NULL) {
      const int len = strlen(flt->match);
      if (STREQ(flt->match, "*") || ((len == identifier_len) && (STREQ(identifier, flt->match)))) {
        return (bool)i;
      }
      if ((len >= 2) && (STREQLEN(".*", &flt->match[len - 2], 2))) {
        if (((identifier_len == len - 2) && STREQLEN(identifier, flt->match, len - 2)) ||
            ((identifier_len >= len - 1) && STREQLEN(identifier, flt->match, len - 1))) {
          return (bool)i;
        }
      }
      flt = flt->next;
    }
  }
  return false;
}

/**
 * \note This should never be called per logging call.
 * Searching is only to get an initial handle.
 */
static CLG_LogType *clg_ctx_type_find_by_name(CLogContext *ctx, const char *identifier)
{
  for (CLG_LogType *ty = ctx->types; ty; ty = ty->next) {
    if (STREQ(identifier, ty->identifier)) {
      return ty;
    }
  }
  return NULL;
}

static CLG_LogType *clg_ctx_type_register(CLogContext *ctx, const char *identifier)
{
  assert(clg_ctx_type_find_by_name(ctx, identifier) == NULL);
  CLG_LogType *ty = MEM_callocN(sizeof(*ty), __func__);
  ty->next = ctx->types;
  ctx->types = ty;
  strncpy(ty->identifier, identifier, sizeof(ty->identifier) - 1);
  ty->ctx = ctx;
  ty->level = ctx->default_type.level;
  ty->severity_level = ctx->default_type.severity_level;

  if (clg_ctx_filter_check(ctx, ty->identifier)) {
    ty->flag |= CLG_FLAG_USE;
  }
  return ty;
}

static void clg_ctx_fatal_action(CLogContext *ctx)
{
  if (ctx->callbacks.fatal_fn != NULL) {
    ctx->callbacks.fatal_fn(ctx->output_file);
  }
  fflush(ctx->output_file);
  abort();
}

static void clg_ctx_backtrace(CLogContext *ctx)
{
  /* Note: we avoid writing fo 'FILE', for backtrace we make an exception,
   * if necessary we could have a version of the callback that writes to file
   * descriptor all at once. */
  ctx->callbacks.backtrace_fn(ctx->output_file);
  fflush(ctx->output_file);
}

static uint64_t clg_timestamp_ticks_get(void)
{
  uint64_t tick;
#if defined(_MSC_VER)
  tick = GetTickCount64();
#else
  struct timeval tv;
  gettimeofday(&tv, NULL);
  tick = tv.tv_sec * 1000 + tv.tv_usec / 1000;
#endif
  return tick;
}

CLG_LogRecord *clog_log_record_init(CLG_LogType *type,
                                    enum CLG_Severity severity,
                                    unsigned short verbosity,
                                    const char *file_line,
                                    const char *function,
                                    const char *message)
{
  CLG_LogRecord *log_record = MEM_callocN(sizeof(*log_record), "ClogRecord");
  log_record->type = type;
  log_record->severity = severity;
  log_record->severity = severity;
  log_record->verbosity = verbosity;
  log_record->timestamp = clg_timestamp_ticks_get() - type->ctx->timestamp_tick_start;
  log_record->file_line = file_line;
  log_record->function = function;

  char *_message = MEM_callocN(strlen(message) + 1, __func__);
  strcpy(_message, message);
  log_record->message = _message;
  return log_record;
}

void clog_log_record_free(CLG_LogRecord *log_record)
{
  MEM_freeN(log_record->message);
  MEM_freeN(log_record);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Logging API
 * \{ */

static void write_timestamp(CLogStringBuf *cstr, const uint64_t timestamp_tick_start)
{
  char timestamp_str[64];
  const uint64_t timestamp = clg_timestamp_ticks_get() - timestamp_tick_start;
  const uint timestamp_len = snprintf(timestamp_str,
                                      sizeof(timestamp_str),
                                      "%" PRIu64 ".%03u ",
                                      timestamp / 1000,
                                      (uint)(timestamp % 1000));
  clg_str_append_with_len(cstr, timestamp_str, timestamp_len);
}

static void write_severity(CLogStringBuf *cstr, enum CLG_Severity severity, bool use_color)
{
  assert((unsigned int)severity < CLG_SEVERITY_LEN);
  if (use_color) {
    enum eCLogColor color = clg_severity_to_color(severity);
    clg_str_append(cstr, clg_color_table[color]);
    clg_str_append(cstr, clg_severity_as_text(severity));
    clg_str_append(cstr, clg_color_table[COLOR_RESET]);
  }
  else {
    clg_str_append(cstr, clg_severity_as_text(severity));
  }
}

static void write_type(CLogStringBuf *cstr, CLG_LogType *lg)
{
  clg_str_append(cstr, " (");
  clg_str_append(cstr, lg->identifier);
  clg_str_append(cstr, "): ");
}

static void write_file_line_fn(CLogStringBuf *cstr,
                               const char *file_line,
                               const char *fn,
                               const bool use_basename)
{
  uint file_line_len = strlen(file_line);
  if (use_basename) {
    uint file_line_offset = file_line_len;
    while (file_line_offset-- > 0) {
      if (file_line[file_line_offset] == PATHSEP_CHAR) {
        file_line_offset++;
        break;
      }
    }
    file_line += file_line_offset;
    file_line_len -= file_line_offset;
  }
  clg_str_append_with_len(cstr, file_line, file_line_len);

  clg_str_append(cstr, " ");
  clg_str_append(cstr, fn);
  clg_str_append(cstr, ": ");
}

/** Clog version of BLI_addtail (to avoid making dependency) */
static void CLG_record_append(CLG_LogRecordList *listbase, CLG_LogRecord *link)
{

  if (link == NULL) {
    return;
  }

  link->next = NULL;
  link->prev = listbase->last;

  if (listbase->last) {
    listbase->last->next = link;
  }
  if (listbase->first == NULL) {
    listbase->first = link;
  }
  listbase->last = link;
}

void CLG_log_str(CLG_LogType *lg,
                 enum CLG_Severity severity,
                 unsigned short verbosity,
                 const char *file_line,
                 const char *fn,
                 const char *message)
{
  CLogStringBuf cstr;
  char cstr_stack_buf[CLOG_BUF_LEN_INIT];
  clg_str_init(&cstr, cstr_stack_buf, sizeof(cstr_stack_buf));

  if (lg->ctx->use_timestamp) {
    write_timestamp(&cstr, lg->ctx->timestamp_tick_start);
  }

  write_severity(&cstr, severity, lg->ctx->use_color);
  if (severity <= CLG_SEVERITY_VERBOSE) {
    char verbosity_str[8];
    sprintf(verbosity_str, ":%u", verbosity);
    clg_str_append(&cstr, verbosity_str);
  }
  write_type(&cstr, lg);

  {
    write_file_line_fn(&cstr, file_line, fn, lg->ctx->use_basename);
    clg_str_append(&cstr, message);
  }
  clg_str_append(&cstr, "\n");

  /* could be optional */
  int bytes_written = write(lg->ctx->output, cstr.data, cstr.len);
  (void)bytes_written;

  clg_str_free(&cstr);

  CLG_LogRecord *log_record = clog_log_record_init(
      lg, severity, verbosity, file_line, fn, message);
  CLG_record_append(&(lg->ctx->log_records), log_record);

  if (lg->ctx->callbacks.backtrace_fn) {
    clg_ctx_backtrace(lg->ctx);
  }

  if (severity == CLG_SEVERITY_FATAL) {
    clg_ctx_fatal_action(lg->ctx);
  }
}


/* TODO (grzelins) there is problem with handling big messages (example is report from duplicating object) */
void CLG_logf(CLG_LogType *lg,
              enum CLG_Severity severity,
              unsigned short verbosity,
              const char *file_line,
              const char *fn,
              const char *fmt,
              ...)
{
  CLogStringBuf cstr;
  char cstr_stack_buf[CLOG_BUF_LEN_INIT];
  clg_str_init(&cstr, cstr_stack_buf, sizeof(cstr_stack_buf));

  if (lg->ctx->use_timestamp) {
    write_timestamp(&cstr, lg->ctx->timestamp_tick_start);
  }

  write_severity(&cstr, severity, lg->ctx->use_color);
  if (severity <= CLG_SEVERITY_VERBOSE) {
    char verbosity_str[8];
    sprintf(verbosity_str, ":%u", verbosity);
    clg_str_append(&cstr, verbosity_str);
  }
  write_type(&cstr, lg);

  write_file_line_fn(&cstr, file_line, fn, lg->ctx->use_basename);

  int cstr_size_before_va = cstr.len;
  {
    va_list ap;
    va_start(ap, fmt);
    clg_str_vappendf(&cstr, fmt, ap);
    va_end(ap);
  }

  size_t mem_size = cstr.len - cstr_size_before_va + 1;  // +1 to null terminate?
  char *message = MEM_callocN(mem_size, "LogMessage");
  strcpy(message, cstr.data + cstr_size_before_va);

  CLG_LogRecord *log_record = clog_log_record_init(
      lg, severity, verbosity, file_line, fn, message);
  CLG_record_append(&(lg->ctx->log_records), log_record);
  MEM_freeN(message);

  clg_str_append(&cstr, "\n");

  /* could be optional */
  int bytes_written = write(lg->ctx->output, cstr.data, cstr.len);
  (void)bytes_written;

  clg_str_free(&cstr);

  if (lg->ctx->callbacks.backtrace_fn) {
    clg_ctx_backtrace(lg->ctx);
  }

  if (severity == CLG_SEVERITY_FATAL) {
    clg_ctx_fatal_action(lg->ctx);
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Logging Context API
 * \{ */

static void CLG_ctx_output_update(CLogContext *ctx)
{
  if (ctx->use_stdout && ctx->output_file != stdout) {
    /* set output to stdout */
    if (ctx->output_file != NULL) {
      fclose(ctx->output_file);
    }
    ctx->output_file = stdout;
  }

  if (!ctx->use_stdout) {
    FILE *fp = fopen(ctx->output_file_path, "w");
    if (fp == NULL) {
      const char *err_msg = errno ? strerror(errno) : "unknown";
      printf("Log output error: %s '%s'.\n", err_msg, ctx->output_file_path);
      return;
    }
    ctx->output_file = fp;
  }
  ctx->output = fileno(ctx->output_file);
#if defined(__unix__) || defined(__APPLE__)
  ctx->use_color = isatty(ctx->output);
#endif
}

static char *CLG_ctx_file_output_path_get(CLogContext *ctx)
{
  return ctx->output_file_path;
}

static void CLG_ctx_file_output_path_set(CLogContext *ctx, const char *value)
{
  if (strcmp(ctx->output_file_path, value) != 0) {
    strcpy(ctx->output_file_path, value);
    if (!ctx->use_stdout) {
      CLG_ctx_output_update(ctx);
    }
  }
}

static bool CLG_ctx_use_stdout_get(CLogContext *ctx)
{
  return ctx->use_stdout;
}

static void CLG_ctx_use_stdout_set(CLogContext *ctx, bool value)
{
  if (ctx->use_stdout == value) {
    return;
  }
  ctx->use_stdout = value;
  CLG_ctx_output_update(ctx);
}

static bool CLG_ctx_output_use_basename_get(CLogContext *ctx)
{
  return ctx->use_basename;
}

static void CLG_ctx_output_use_basename_set(CLogContext *ctx, int value)
{
  ctx->use_basename = (bool)value;
}

/** always show Fatals, Errors and Warnings, regardless if log is in use */
static bool CLG_ctx_always_show_warnings_get(CLogContext *ctx)
{
  return ctx->always_show_warnings;
}

static void CLG_ctx_always_show_warnings_set(CLogContext *ctx, bool value)
{
  ctx->always_show_warnings = value;
}

static bool CLG_ctx_output_use_timestamp_get(CLogContext *ctx)
{
  return ctx->use_timestamp;
}

static void CLG_ctx_output_use_timestamp_set(CLogContext *ctx, int value)
{
  ctx->use_timestamp = (bool)value;
}

/** Action on fatal severity. */
static void CLG_ctx_fatal_fn_set(CLogContext *ctx, void (*fatal_fn)(void *file_handle))
{
  ctx->callbacks.fatal_fn = fatal_fn;
}

static void CLG_ctx_backtrace_fn_set(CLogContext *ctx, void (*backtrace_fn)(void *file_handle))
{
  ctx->callbacks.backtrace_fn = backtrace_fn;
}

static void clg_ctx_type_filter_append(CLG_IDFilter **flt_list,
                                       const char *type_match,
                                       int type_match_len)
{
  if (type_match_len == 0) {
    return;
  }
  CLG_IDFilter *flt = MEM_callocN(sizeof(*flt) + (type_match_len + 1), __func__);
  flt->next = *flt_list;
  *flt_list = flt;
  memcpy(flt->match, type_match, type_match_len);
  /* no need to null terminate since we calloc'd */
}

static void CLG_ctx_type_filter_exclude(CLogContext *ctx,
                                        const char *type_match,
                                        int type_match_len)
{
  clg_ctx_type_filter_append(&ctx->filters[0], type_match, type_match_len);
}

static void CLG_ctx_type_filter_include(CLogContext *ctx,
                                        const char *type_match,
                                        int type_match_len)
{
  clg_ctx_type_filter_append(&ctx->filters[1], type_match, type_match_len);
}

static void CLG_ctx_type_filters_clear(CLogContext *ctx)
{
  for (uint i = 0; i < 2; i++) {
    while (ctx->filters[i] != NULL) {
      CLG_IDFilter *item = ctx->filters[i];
      ctx->filters[i] = item->next;
      MEM_freeN(item);
    }
  }
}

static void CLG_ctx_type_filter_set(CLogContext *ctx, const char *glob_str)
{
  CLG_ctx_type_filters_clear(ctx);
  const char *str_step = glob_str;
  while (*str_step) {
    const char *str_step_end = strchr(str_step, ',');
    int str_step_len = str_step_end ? (str_step_end - str_step) : strlen(str_step);

    if (str_step[0] == '^') {
      CLG_ctx_type_filter_exclude(ctx, str_step + 1, str_step_len - 1);
    }
    else {
      CLG_ctx_type_filter_include(ctx, str_step, str_step_len);
    }

    if (str_step_end) {
      /* Typically only be one, but don't fail on multiple. */
      while (*str_step_end == ',') {
        str_step_end++;
      }
      str_step = str_step_end;
    }
    else {
      break;
    }
  }

  CLG_LogType *log_type_iter = ctx->types;
  while (log_type_iter) {
    if (clg_ctx_filter_check(ctx, log_type_iter->identifier)) {
      log_type_iter->flag |= CLG_FLAG_USE;
    }
    else {
      log_type_iter->flag &= ~CLG_FLAG_USE;
    }
    log_type_iter = log_type_iter->next;
  }
}

static enum CLG_Severity CLG_ctx_severity_level_get(CLogContext *ctx)
{
  return ctx->default_type.severity_level;
}

static void CLG_ctx_severity_level_set(CLogContext *ctx, enum CLG_Severity level)
{
  ctx->default_type.severity_level = level;
  for (CLG_LogType *ty = ctx->types; ty; ty = ty->next) {
    ty->severity_level = level;
  }
}

static unsigned short CLG_ctx_level_get(CLogContext *ctx)
{
  return ctx->default_type.level;
}

static void CLG_ctx_level_set(CLogContext *ctx, unsigned short level)
{
  ctx->default_type.level = level;
  for (CLG_LogType *ty = ctx->types; ty; ty = ty->next) {
    ty->level = level;
  }
}

static CLG_LogRecordList *CLG_ctx_log_record_get(CLogContext *ctx)
{
  return &ctx->log_records;
}

static CLogContext *CLG_ctx_init(void)
{
  CLogContext *ctx = MEM_callocN(sizeof(*ctx), __func__);
#ifdef WITH_CLOG_PTHREADS
  pthread_mutex_init(&ctx->types_lock, NULL);
#endif
  ctx->use_color = true;
  ctx->use_timestamp = CLG_DEFAULT_USE_TIMESTAMP;
  ctx->use_basename = CLG_DEFAULT_USE_BASENAME;
  ctx->default_type.severity_level = CLG_DEFAULT_SEVERITY;
  ctx->default_type.level = CLG_DEFAULT_LEVEL;
  ctx->use_stdout = CLG_DEFAULT_USE_STDOUT;
  ctx->always_show_warnings = CLG_DEFAULT_ALWAYS_SHOW_WARNINGS;
  ctx->timestamp_tick_start = clg_timestamp_ticks_get();

  /* enable all loggers by default */
  CLG_ctx_type_filter_include(
      ctx, CLG_DEFAULT_LOG_TYPE_FILTER, strlen(CLG_DEFAULT_LOG_TYPE_FILTER));

  CLG_ctx_output_update(ctx);

  return ctx;
}

static void CLG_ctx_free(CLogContext *ctx)
{
  CLG_LogRecord *log = ctx->log_records.first, *log_next = NULL;
  while (log) {
    log_next = log->next;
    clog_log_record_free(log);
    log = log_next;
  }
  if (ctx->output_file != NULL) {
    fclose(ctx->output_file);
  }
  ctx->log_records.first = NULL;
  ctx->log_records.last = NULL;

  /* unregister all types */
  while (ctx->types != NULL) {
    CLG_LogType *item = ctx->types;
    ctx->types = item->next;
    MEM_freeN(item);
  }

  CLG_ctx_type_filters_clear(ctx);

#ifdef WITH_CLOG_PTHREADS
  pthread_mutex_destroy(&ctx->types_lock);
#endif
  MEM_freeN(ctx);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Public Logging API
 *
 * Currently uses global context.
 * \{ */

/* We could support multiple at once, for now this seems not needed. */
static struct CLogContext *g_ctx = NULL;

void CLG_init(void)
{
  g_ctx = CLG_ctx_init();

  clg_color_table_init(g_ctx->use_color);
}

void CLG_exit(void)
{
  CLG_ctx_free(g_ctx);
}

void CLG_file_output_path_set(const char *CLG_file_output_path_get)
{
  CLG_ctx_file_output_path_set(g_ctx, CLG_file_output_path_get);
}

char *CLG_file_output_path_get()
{
  return CLG_ctx_file_output_path_get(g_ctx);
}

bool CLG_use_stdout_get()
{
  return CLG_ctx_use_stdout_get(g_ctx);
}

void CLG_use_stdout_set(bool value)
{
  CLG_ctx_use_stdout_set(g_ctx, value);
}

bool CLG_output_use_basename_get()
{
  return CLG_ctx_output_use_basename_get(g_ctx);
}

void CLG_output_use_basename_set(int value)
{
  CLG_ctx_output_use_basename_set(g_ctx, value);
}

bool CLG_always_show_warnings_get()
{
  return CLG_ctx_always_show_warnings_get(g_ctx);
}

void CLG_always_show_warnings_set(bool value)
{
  CLG_ctx_always_show_warnings_set(g_ctx, value);
}

bool CLG_output_use_timestamp_get()
{
  return CLG_ctx_output_use_timestamp_get(g_ctx);
}

void CLG_output_use_timestamp_set(int value)
{
  CLG_ctx_output_use_timestamp_set(g_ctx, value);
}

void CLG_fatal_fn_set(void (*fatal_fn)(void *file_handle))
{
  CLG_ctx_fatal_fn_set(g_ctx, fatal_fn);
}

void CLG_backtrace_fn_set(void (*fatal_fn)(void *file_handle))
{
  CLG_ctx_backtrace_fn_set(g_ctx, fatal_fn);
}

void CLG_type_filter_exclude(const char *type_match, int type_match_len)
{
  CLG_ctx_type_filter_exclude(g_ctx, type_match, type_match_len);
}

void CLG_type_filter_set(const char *glob_str)
{
  CLG_ctx_type_filter_set(g_ctx, glob_str);
}

int CLG_type_filter_get(char *buff, int buff_len)
{
  int written = 0;
  CLG_IDFilter *filters_iter = g_ctx->filters[0]; /* exclude filters */
  while (filters_iter) {
    if (filters_iter->next == NULL) {
      written += sprintf(buff + written, "^%s", filters_iter->match);
    }
    else {
      written += sprintf(buff + written, "^%s,", filters_iter->match);
    }
    filters_iter = filters_iter->next;
  }

  filters_iter = g_ctx->filters[1]; /* include filters */
  if (written != 0 && buff[written - 1] != ',') {
    written += sprintf(buff + written, ",");
  }
  while (filters_iter) {
    if (filters_iter->next == NULL) {
      written += sprintf(buff + written, "%s", filters_iter->match);
    }
    else {
      written += sprintf(buff + written, "%s,", filters_iter->match);
    }
    filters_iter = filters_iter->next;
  }
  assert(written <= buff_len);
  return written;
}

void CLG_type_filter_include(const char *type_match, int type_match_len)
{
  CLG_ctx_type_filter_include(g_ctx, type_match, type_match_len);
}

void CLG_type_filters_clear()
{
  CLG_ctx_type_filters_clear(g_ctx);
}

void CLG_severity_level_set(enum CLG_Severity level)
{
  CLG_ctx_severity_level_set(g_ctx, level);
}

enum CLG_Severity CLG_severity_level_get()
{
  return CLG_ctx_severity_level_get(g_ctx);
}

void CLG_level_set(unsigned short level)
{
  CLG_ctx_level_set(g_ctx, level);
}

unsigned short CLG_level_get()
{
  return CLG_ctx_level_get(g_ctx);
}

CLG_LogRecordList *CLG_log_record_get()
{
  return CLG_ctx_log_record_get(g_ctx);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Logging Reference API
 * Use to avoid lookups each time.
 * \{ */

void CLG_logref_init(CLG_LogRef *clg_ref)
{
#ifdef WITH_CLOG_PTHREADS
  /* Only runs once when initializing a static type in most cases. */
  pthread_mutex_lock(&g_ctx->types_lock);
#endif
  if (clg_ref->type == NULL) {
    CLG_LogType *clg_ty = clg_ctx_type_find_by_name(g_ctx, clg_ref->identifier);
    if (clg_ty == NULL) {
      clg_ty = clg_ctx_type_register(g_ctx, clg_ref->identifier);
    }
#ifdef WITH_CLOG_PTHREADS
    atomic_cas_ptr((void **)&clg_ref->type, clg_ref->type, clg_ty);
#else
    clg_ref->type = clg_ty;
#endif
  }
#ifdef WITH_CLOG_PTHREADS
  pthread_mutex_unlock(&g_ctx->types_lock);
#endif
}

/** \} */
