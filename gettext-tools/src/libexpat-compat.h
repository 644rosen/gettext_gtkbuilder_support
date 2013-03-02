/* xgettext libexpat compatibility.
   Copyright (C) 2002-2003, 2005-2009, 2013 Free Software Foundation, Inc.

   This file was written by Bruno Haible <haible@clisp.cons.org>, 2002.

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

#include <stdbool.h>
#include <stdint.h>
#if DYNLOAD_LIBEXPAT
# include <dlfcn.h>
#else
# if HAVE_LIBEXPAT
#  include <expat.h>
# endif
#endif

/* ======================= Different libexpat ABIs.  ======================= */

/* There are three different ABIs of libexpat, regarding the functions
   XML_GetCurrentLineNumber and XML_GetCurrentColumnNumber.
   In expat < 2.0, they return an 'int'.
   In expat >= 2.0, they return
     - a 'long' if expat was compiled with the default flags, or
     - a 'long long' if expat was compiled with -DXML_LARGE_SIZE.
   But the <expat.h> include file does not contain the information whether
   expat was compiled with -DXML_LARGE_SIZE; so the include file is lying!
   For this information, we need to call XML_GetFeatureList(), for
   expat >= 2.0.1; for expat = 2.0.0, we have to assume the default flags.  */

#if !DYNLOAD_LIBEXPAT

# if XML_MAJOR_VERSION >= 2

/* expat >= 2.0 -> Return type is 'int64_t' worst-case.  */

/* Put the function pointers into variables, because some GCC 4 versions
   generate an abort when we convert symbol address to different function
   pointer types.  */

/* Return true if libexpat was compiled with -DXML_LARGE_SIZE.  */
bool is_XML_LARGE_SIZE_ABI (void);

int64_t GetCurrentLineNumber (XML_Parser parser);
#  define XML_GetCurrentLineNumber GetCurrentLineNumber

int64_t GetCurrentColumnNumber (XML_Parser parser);
#  define XML_GetCurrentColumnNumber GetCurrentColumnNumber

# else

/* expat < 2.0 -> Return type is 'int'.  */

# endif

#endif

/* ===================== Dynamic loading of libexpat.  ===================== */

#if DYNLOAD_LIBEXPAT
typedef struct
  {
    int major;
    int minor;
    int micro;
  }
  XML_Expat_Version;
enum XML_FeatureEnum { XML_FEATURE_END = 0 };
typedef struct
  {
    enum XML_FeatureEnum feature;
    const char *name;
    long int value;
  }
  XML_Feature;
typedef void *XML_Parser;
typedef char XML_Char;
typedef char XML_LChar;
enum XML_Error { XML_ERROR_NONE };
typedef void (*XML_StartElementHandler) (void *userData, const XML_Char *name, const XML_Char **atts);
typedef void (*XML_EndElementHandler) (void *userData, const XML_Char *name);
typedef void (*XML_CharacterDataHandler) (void *userData, const XML_Char *s, int len);
typedef void (*XML_CommentHandler) (void *userData, const XML_Char *data);

XML_Expat_Version XML_ExpatVersionInfo (void);
const XML_Feature * XML_GetFeatureList (void);

enum XML_Size_ABI { is_int, is_long, is_int64_t };
enum XML_Size_ABI get_XML_Size_ABI (void);

XML_Parser XML_ParserCreate (const XML_Char *encoding);
void XML_SetElementHandler (XML_Parser parser,
                            XML_StartElementHandler start,
                            XML_EndElementHandler end);
void XML_SetCharacterDataHandler (XML_Parser parser,
                                  XML_CharacterDataHandler handler);
void XML_SetCommentHandler (XML_Parser parser, XML_CommentHandler handler);
int XML_Parse (XML_Parser parser, const char *s, int len, int isFinal);
enum XML_Error XML_GetErrorCode (XML_Parser parser);
int64_t XML_GetCurrentLineNumber (XML_Parser parser);
int64_t XML_GetCurrentColumnNumber (XML_Parser parser);
const XML_LChar * XML_ErrorString (int code);
void XML_ParserFree (XML_Parser parser);

bool load_libexpat ();

#define LIBEXPAT_AVAILABLE() (load_libexpat ())

#elif HAVE_LIBEXPAT

#define LIBEXPAT_AVAILABLE() true

#endif
