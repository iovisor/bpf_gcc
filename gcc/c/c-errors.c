/* Various diagnostic subroutines for the GNU C language.
   Copyright (C) 2000-2015 Free Software Foundation, Inc.
   Contributed by Gabriel Dos Reis <gdr@codesourcery.com>

This file is part of GCC.

GCC is free software; you can redistribute it and/or modify it under
the terms of the GNU General Public License as published by the Free
Software Foundation; either version 3, or (at your option) any later
version.

GCC is distributed in the hope that it will be useful, but WITHOUT ANY
WARRANTY; without even the implied warranty of MERCHANTABILITY or
FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
for more details.

You should have received a copy of the GNU General Public License
along with GCC; see the file COPYING3.  If not see
<http://www.gnu.org/licenses/>.  */

#include "config.h"
#include "system.h"
#include "coretypes.h"
#include "tm.h"
#include "tree.h"
#include "c-tree.h"
#include "tm_p.h"
#include "flags.h"
#include "diagnostic.h"
#include "opts.h"

/* Issue an ISO C99 pedantic warning MSGID if -pedantic outside C11 mode,
   otherwise issue warning MSGID if -Wc99-c11-compat is specified.
   This function is supposed to be used for matters that are allowed in
   ISO C11 but not supported in ISO C99, thus we explicitly don't pedwarn
   when C11 is specified.  */

bool
pedwarn_c99 (location_t location, int opt, const char *gmsgid, ...)
{
  diagnostic_info diagnostic;
  va_list ap;
  bool warned = false;

  va_start (ap, gmsgid);
  /* If desired, issue the C99/C11 compat warning, which is more specific
     than -pedantic.  */
  if (warn_c99_c11_compat > 0)
    {
      diagnostic_set_info (&diagnostic, gmsgid, &ap, location,
			   (pedantic && !flag_isoc11)
			   ? DK_PEDWARN : DK_WARNING);
      diagnostic.option_index = OPT_Wc99_c11_compat;
      warned = report_diagnostic (&diagnostic);
    }
  /* -Wno-c99-c11-compat suppresses even the pedwarns.  */
  else if (warn_c99_c11_compat == 0)
    ;
  /* For -pedantic outside C11, issue a pedwarn.  */
  else if (pedantic && !flag_isoc11)
    {
      diagnostic_set_info (&diagnostic, gmsgid, &ap, location, DK_PEDWARN);
      diagnostic.option_index = opt;
      warned = report_diagnostic (&diagnostic);
    }
  va_end (ap);
  return warned;
}

/* Issue an ISO C90 pedantic warning MSGID if -pedantic outside C99 mode,
   otherwise issue warning MSGID if -Wc90-c99-compat is specified, or if
   a specific option such as -Wlong-long is specified.
   This function is supposed to be used for matters that are allowed in
   ISO C99 but not supported in ISO C90, thus we explicitly don't pedwarn
   when C99 is specified.  (There is no flag_c90.)  */

void
pedwarn_c90 (location_t location, int opt, const char *gmsgid, ...)
{
  diagnostic_info diagnostic;
  va_list ap;

  va_start (ap, gmsgid);
  /* Warnings such as -Wvla are the most specific ones.  */
  if (opt != OPT_Wpedantic)
    {
      int opt_var = *(int *) option_flag_var (opt, &global_options);
      if (opt_var == 0)
        goto out;
      else if (opt_var > 0)
	{
	  diagnostic_set_info (&diagnostic, gmsgid, &ap, location,
			       (pedantic && !flag_isoc99)
			       ? DK_PEDWARN : DK_WARNING);
	  diagnostic.option_index = opt;
	  report_diagnostic (&diagnostic);
	  goto out;
	}
    }
  /* Maybe we want to issue the C90/C99 compat warning, which is more
     specific than -pedantic.  */
  if (warn_c90_c99_compat > 0)
    {
      diagnostic_set_info (&diagnostic, gmsgid, &ap, location,
			   (pedantic && !flag_isoc99)
			   ? DK_PEDWARN : DK_WARNING);
      diagnostic.option_index = OPT_Wc90_c99_compat;
      report_diagnostic (&diagnostic);
    }
  /* -Wno-c90-c99-compat suppresses the pedwarns.  */
  else if (warn_c90_c99_compat == 0)
    ;
  /* For -pedantic outside C99, issue a pedwarn.  */
  else if (pedantic && !flag_isoc99)
    {
      diagnostic_set_info (&diagnostic, gmsgid, &ap, location, DK_PEDWARN);
      diagnostic.option_index = opt;
      report_diagnostic (&diagnostic);
    }
out:
  va_end (ap);
}
