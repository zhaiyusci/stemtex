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

#ifndef _ERROR_H_
#define _ERROR_H_

#include "system.h"

extern void error_cleanup (void);

/* Avoid conflict with NO_ERROR from <winerror.h>.  */
#undef NO_ERROR

#define FATAL_ERROR -1
#define NO_ERROR 0
#define RECOVERABLE_ERROR 2

#define DPX_RECOVERABLE_MISSING_FONT 0x01

#include <assert.h>
#include <setjmp.h>
#include <stdio.h>

extern void shut_up (int quietness);

/* Avoid conflict with ERROR from <winnt.h>.  */
#undef ERROR

extern void ERROR (const char *fmt, ...);
extern void MESG  (const char *fmt, ...);
extern void WARN  (const char *fmt, ...);
extern void DPX_TRACE (const char *fmt, ...);
extern void dpx_exit (int code);
extern void dpx_clear_recoverable_issues (void);
extern void dpx_record_recoverable_issue (int flag, const char *fmt, ...);
extern int dpx_recoverable_issue_flags (void);
extern const char *dpx_recoverable_issue_message (void);

extern jmp_buf dpx_exit_env;
extern int     dpx_exit_active;
extern int     dpx_exit_code;

#define ASSERT(e) assert(e)

#if defined(WIN32)
#undef vfprintf
#define vfprintf win32_vfprintf
#endif

#endif /* _ERROR_H_ */
