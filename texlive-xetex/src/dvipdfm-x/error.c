/* This is dvipdfmx, an eXtended version of dvipdfm by Mark A. Wicks.

    Copyright (C) 2002-2020 by Jin-Hwan Cho and Shunsaku Hirata,
    the dvipdfmx project team.

    Copyright (C) 1998, 1999 by Mark A. Wicks <mwicks@kettering.edu>

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program; if not, write to the Free Software
    Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA.
*/

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

#if defined(WIN32)
__declspec(dllimport) unsigned long __stdcall GetEnvironmentVariableA (const char *, char *, unsigned long);
#endif

#include "dvipdfmx.h"
#include "error.h"

#define DPX_MESG        0
#define DPX_MESG_WARN   1
#define DPX_MESG_ERROR  2

static int _mesg_type = DPX_MESG;
#define WANT_NEWLINE() (_mesg_type != DPX_MESG_WARN && _mesg_type != DPX_MESG_ERROR)

static int  really_quiet = 0;
jmp_buf dpx_exit_env;
int     dpx_exit_active = 0;
int     dpx_exit_code   = 0;
static int dpx_recoverable_flags = 0;
static char dpx_recoverable_message[512] = {0};

void
dpx_clear_recoverable_issues (void)
{
  dpx_recoverable_flags = 0;
  dpx_recoverable_message[0] = '\0';
}

void
dpx_record_recoverable_issue (int flag, const char *fmt, ...)
{
  va_list argp;

  dpx_recoverable_flags |= flag;
  if (dpx_recoverable_message[0] != '\0')
    return;

  va_start(argp, fmt);
  vsnprintf(dpx_recoverable_message, sizeof(dpx_recoverable_message), fmt, argp);
  va_end(argp);
  dpx_recoverable_message[sizeof(dpx_recoverable_message) - 1] = '\0';
  DPX_TRACE("recoverable issue: %s", dpx_recoverable_message);
}

int
dpx_recoverable_issue_flags (void)
{
  return dpx_recoverable_flags;
}

const char *
dpx_recoverable_issue_message (void)
{
  return dpx_recoverable_message;
}

void
dpx_exit (int code)
{
  if (dpx_exit_active) {
    dpx_exit_code = code;
    longjmp(dpx_exit_env, 1);
  }
  exit(code);
}

static void
dpx_trace_v (const char *fmt, va_list argp)
{
#if defined(WIN32)
  char path_buf[4096];
  unsigned long n = GetEnvironmentVariableA("STEMTEX_XDVIPDFMX_TRACE", path_buf, sizeof(path_buf));
  const char *path = (n > 0 && n < sizeof(path_buf)) ? path_buf : NULL;
#else
  const char *path = getenv("STEMTEX_XDVIPDFMX_TRACE");
#endif
  FILE *fp;

  if (!path || !*path)
    return;

  fp = fopen(path, "ab");
  if (!fp)
    return;

  fprintf(fp, "[xdvipdfmx] ");
  vfprintf(fp, fmt, argp);
  fprintf(fp, "\n");
  fflush(fp);
  fclose(fp);
}

void
DPX_TRACE (const char *fmt, ...)
{
  va_list argp;

  va_start(argp, fmt);
  dpx_trace_v(fmt, argp);
  va_end(argp);
}

void
shut_up (int quietness)
{
  really_quiet = quietness;
}

void
MESG (const char *fmt, ...)
{
  va_list argp;

  if (really_quiet < 1) {
    va_start(argp, fmt);
    vfprintf(stderr, fmt, argp);
    va_end(argp);
    _mesg_type = DPX_MESG;
  }
}

void
WARN (const char *fmt, ...)
{
  va_list argp;

  if (really_quiet < 2) {
    if (WANT_NEWLINE())
      fprintf(stderr, "\n");
    fprintf(stderr, "%s:warning: ", my_name);
    va_start(argp, fmt);
    vfprintf(stderr, fmt, argp);
    va_end(argp);
    fprintf(stderr, "\n");

    _mesg_type = DPX_MESG_WARN;
  }
}

void
ERROR (const char *fmt, ...)
{
  va_list argp;

  if (really_quiet < 3) {
    if (WANT_NEWLINE())
      fprintf(stderr, "\n");
    fprintf(stderr, "%s:fatal: ", my_name);
    va_start(argp, fmt);
    dpx_trace_v(fmt, argp);
    va_end(argp);
    va_start(argp, fmt);
    vfprintf(stderr, fmt, argp);
    va_end(argp);
    fprintf(stderr, "\n");
  }
  error_cleanup();
  dpx_exit(1);
}
