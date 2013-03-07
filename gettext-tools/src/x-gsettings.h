/* xgettext gsettings backend.
   Copyright (C) 2013 Free Software Foundation, Inc.
   Written by Miguel Ángel Arruga Vivas <rosen644835@gmail.com>, 2013.

   This program is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 3 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program.  If not, see <http://www.gnu.org/licenses/>.  */

#ifndef X_GSETTINGS_H
#define X_GSETTINGS_H

#include <stdio.h>
#include "message.h"
#include "xgettext.h"

#ifdef __cplusplus
extern "C" {
#endif


#define EXTENSIONS_GSETTINGS \
  { "gsettings.xml", "gsettings"  },                                    \

#define SCANNERS_GSETTINGS \
  { "gsettings",         extract_gsettings, NULL, NULL, NULL },         \

/* Scan a GSettings XML file and add its translatable strings to mdlp.  */
extern void extract_gsettings (FILE *fp, const char *real_filename,
                               const char *logical_filename,
                               flag_context_list_table_ty *flag_table,
                               msgdomain_list_ty *mdlp);


/* Handling of options specific to this language.  */

extern void x_gsettings_extract_all (void);
extern void x_gsettings_keyword (const char *name);


#ifdef __cplusplus
} /* End of extern "C"  */
#endif

#endif  /*  X_GSETTINGS_H  */
