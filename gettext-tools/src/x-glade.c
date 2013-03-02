/* xgettext glade backend.
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

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

/* Specification.  */
#include "x-glade.h"

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "message.h"
#include "xgettext.h"
#include "error.h"
#include "xerror.h"
#include "xvasprintf.h"
#include "basename.h"
#include "progname.h"
#include "xalloc.h"
#include "hash.h"
#include "po-charset.h"
#include "gettext.h"
#include "libexpat-compat.h"

#define _(s) gettext(s)


/* glade1 was an XML based format.  Some example files are contained in
   tests (inside of xgettext-glade-1) and in libglade1 releases.
   glade2 had also an XML based format.  Some example files are contained
   in tests (i.e. inside of xgettext-glade-3).
   GtkBuilder is an XML based format.  Some example files are contained
   in tests (i.e. inside of xgettext-gtkbuilder-3).  */


/* ====================== Keyword set customization.  ====================== */

/* If true extract all strings.  */
static bool extract_all = false;

static hash_table keywords;
static bool default_keywords = true;


void
x_glade_extract_all ()
{
  extract_all = true;
}


void
x_glade_keyword (const char *name)
{
  if (name == NULL)
    default_keywords = false;
  else
    {
      if (keywords.table == NULL)
        hash_init (&keywords, 100);

      hash_insert_entry (&keywords, name, strlen (name), NULL);
    }
}

static hash_table tags;

static void
clear_tags ()
{
  if (tags.table != NULL)
    {
      hash_destroy (&tags);
      tags.table = NULL;
    }
}

static void
init_tags (const char **tags_array)
{
  clear_tags ();
  if (default_keywords)
    {
      const char **act;

      hash_init (&tags, 15);
      for (act = tags_array; *act != NULL; act++)
        hash_insert_entry (&tags, *act, strlen (*act), NULL);
    }
}

static void
init_glade1_tags ()
{
  /* When adding new keywords here, also update the documentation in
     xgettext.texi!  */
  static const char *tags_array[] = {
    "label", "title", "text", "format", "copyright", "comments",
    "preview_text", "tooltip", NULL
  };
  init_tags (tags_array);
}

static void
init_glade2_tags ()
{
  /* When adding new keywords here, also update the documentation in
     xgettext.texi!  */
  static const char *tags_array[] = {
    "property", "atkproperty", "atkaction", NULL
  };
  init_tags (tags_array);
}

static void
init_gtkbuilder_tags ()
{
  /* When adding new keywords here, also update the documentation in
     xgettext.texi!  */
  static const char *tags_array[] = {
    "property", "attribute", "col", NULL
  };
  init_tags (tags_array);
}

/* Name must be not-NULL.  */
static bool
is_tag (const char *name)
{
  void *hash_result;
  bool found = false;
  if (keywords.table != NULL)
    found = hash_find_entry (&keywords, name, strlen (name), &hash_result) == 0;
  if (default_keywords && (tags.table != NULL))
    found = hash_find_entry (&tags, name, strlen (name), &hash_result) == 0;
  return found;
}


/* ============================= XML parsing.  ============================= */

#if DYNLOAD_LIBEXPAT || HAVE_LIBEXPAT

/* Accumulator for the extracted messages.  */
static message_list_ty *mlp;

/* Logical filename, used to label the extracted messages.  */
static char *logical_file_name;

/* XML parser.  */
static XML_Parser parser;

struct element_state
{
  bool extract_string;
  bool extract_context;
  char *extracted_context;
  char *extracted_comment;
  int lineno;
  char *buffer;
  size_t bufmax;
  size_t buflen;
};
static struct element_state *stack;
static size_t stack_size;
static size_t stack_depth;

/* Checks that name is the first tag of a GtkBuilder, a
   Glade1 file or a Glade2 file. Default is Glade1.  */
static void check_file_type (const char *name);

/* Stores the information from attributes for their later use.  */
typedef void (*se_handler) (struct element_state *p, const char *name,
                            const char **attributes);
static se_handler do_start_element;

/* Stores the character data passed.  */
typedef const struct callshapes * (*shapes_handler) (const char *tag,
                                                     bool extract_context);
static shapes_handler get_shapes;

/* =========================== Implementation.  =========================== */

/* Ensures stack_size >= size.  */
static void
ensure_stack_size (size_t size)
{
  if (size > stack_size)
    {
      stack_size = 2 * stack_size;
      if (stack_size < size)
        stack_size = size;
      stack =
        (struct element_state *)
        xrealloc (stack, stack_size * sizeof (struct element_state));
    }
}

/* NOTE: We do not check argtotal.  */
#define DONE_ARGNUM 0
/* Used for Glade2 context extraction.  */
#define MSGID_ARGNUM 1
/* Used for GtkBuilder context extraction.  */
#define MSGCTXT_ARGNUM 2

/* Stores the message extracted.
   P: p->buffer must not be NULL. It will be NULL after this call.
      p->extracted context will be NULL after this call.
   SHAPES: Create the arglist_parser with this properties.  */
static void
store_message (struct element_state *p, const struct callshapes *shapes)
{
  /* Create specific parser.  */
  struct arglist_parser *ap = arglist_parser_alloc (mlp, shapes);

  /* Store the extracted string.  */
  arglist_parser_remember (ap, MSGID_ARGNUM, p->buffer,
                           null_context, logical_file_name,
                           p->lineno, savable_comment);
  p->buffer = NULL;

  /* Store the extracted context, if any.  */
  if (p->extracted_context != NULL)
    {
      arglist_parser_remember (ap, MSGCTXT_ARGNUM, p->extracted_context,
                               null_context, logical_file_name,
                               p->lineno, savable_comment);
      p->extracted_context = NULL;
    }

  /* Store the extracted comment, if any.  */
  if (p->extracted_comment != NULL)
    string_list_append (&ap->alternative[0].xcomments,
                        p->extracted_comment);

  /* Call remember_a_message.
     NOTE: ap->alternative[0].argtotal should be 0.  */
  arglist_parser_done (ap, DONE_ARGNUM);
}


/* Callback called when <element> is seen.  */
static void
start_element_handler (void *userData, const char *name,
                       const char **attributes)
{
  struct element_state *p;

  if (stack_depth == 0)
    check_file_type (name);

  /* Increase stack depth.  */
  stack_depth++;
  ensure_stack_size (stack_depth + 1);

  /* Don't extract a string for the containing element.  */
  stack[stack_depth - 1].extract_string = false;

  p = &stack[stack_depth];
  p->extract_string = extract_all;
  p->extract_context = false;
  p->extracted_comment = NULL;
  p->extracted_context = NULL;
  p->lineno = XML_GetCurrentLineNumber (parser);
  p->buffer = NULL;
  p->bufmax = 0;
  p->buflen = 0;

  /* The correct tags have been already inserted. */
  if (!p->extract_string)
    p->extract_string = is_tag (name);

  /* Do actual work.  */
  do_start_element (p, name, attributes);

  if (!p->extract_string)
    savable_comment_reset ();
}


/* Callback called when </element> is seen.  */
static void
end_element_handler (void *userData, const char *name)
{
  struct element_state *p = &stack[stack_depth];

  /* Actually extract string.  */
  if (p->extract_string)
    {
      /* Don't extract the empty string.  */
      if (p->buflen > 0)
        {
          if (p->buflen == p->bufmax)
            p->buffer = (char *) xrealloc (p->buffer, p->buflen + 1);
          p->buffer[p->buflen] = '\0';

          store_message (p, get_shapes (name, p->extract_context));
        }
    }

  /* Free memory for this stack level.  */
  if (p->extracted_comment != NULL)
    free (p->extracted_comment);
  if (p->extracted_context != NULL)
    free (p->extracted_context);
  if (p->buffer != NULL)
    free (p->buffer);

  /* Decrease stack depth.  */
  stack_depth--;

  savable_comment_reset ();
}

/* Callback called when some text is seen.  */
static void
character_data_handler (void *userData, const char *s, int len)
{
  struct element_state *p = &stack[stack_depth];

  /* Accumulate character data.  */
  if (len > 0)
    {
      if (p->buflen + len > p->bufmax)
        {
          p->bufmax = 2 * p->bufmax;
          if (p->bufmax < p->buflen + len)
            p->bufmax = p->buflen + len;
          p->buffer = (char *) xrealloc (p->buffer, p->bufmax);
        }
      memcpy (p->buffer + p->buflen, s, len);
      p->buflen += len;
    }
}

/* Callback called when some comment text is seen.  */
static void
comment_handler (void *userData, const char *data)
{
  /* Split multiline comment into lines, and remove leading and trailing
     whitespace.  */
  char *copy = xstrdup (data);
  char *p;
  char *q;

  for (p = copy; (q = strchr (p, '\n')) != NULL; p = q + 1)
    {
      while (p[0] == ' ' || p[0] == '\t')
        p++;
      while (q > p && (q[-1] == ' ' || q[-1] == '\t'))
        q--;
      *q = '\0';
      savable_comment_add (p);
    }
  q = p + strlen (p);
  while (p[0] == ' ' || p[0] == '\t')
    p++;
  while (q > p && (q[-1] == ' ' || q[-1] == '\t'))
    q--;
  *q = '\0';
  savable_comment_add (p);
  free (copy);
}

/* -----------------------------------------------------------------
                          Glade 1 and default.
   -----------------------------------------------------------------  */

/* Returns a pointer to a static callshapes prepared to extract only
   msgid and comments.
   TAG: Keyword to set. Must not be NULL.
   EXTRACT_CONTEXT: Not used.  */
static const struct callshapes *
get_glade_shapes (const char *tag, bool extract_context)
{
  static struct callshapes glade_shapes;
  static bool initialized = false;
  if (!initialized)
    {
      glade_shapes.nshapes = 1;
      glade_shapes.shapes[0].argnum1 = MSGID_ARGNUM;
      glade_shapes.shapes[0].argnum2 = 0;
      glade_shapes.shapes[0].argnumc = 0;
      glade_shapes.shapes[0].argnum1_glib_context = false;
      glade_shapes.shapes[0].argnum2_glib_context = false;
      glade_shapes.shapes[0].argtotal = 0;
      string_list_init (&glade_shapes.shapes[0].xcomments);
      initialized = true;
    }
  glade_shapes.keyword = tag;
  glade_shapes.keyword_len = strlen (tag) - 1; /* Not-NUL terminated.  */

  return &glade_shapes;
}

static void
glade_start_element (struct element_state *p, const char *name,
                     const char **attributes)
{
}

/* -----------------------------------------------------------------
                                Glade 2.
   -----------------------------------------------------------------  */

/* Returns a pointer to a static callshapes prepared to extract or not
   glib syntax strings.
   TAG: Keyword to set. Must not be NULL.
   EXTRACT_CONTEXT: true for strings with glib msgctxt syntax
   (Glade2 with attribute context="yes").  */
static const struct callshapes *
get_glade2_shapes (const char *tag, bool extract_context)
{
  static struct callshapes glade_shapes;
  static bool initialized = false;
  if (!initialized)
    {
      glade_shapes.nshapes = 1;
      glade_shapes.shapes[0].argnum1 = MSGID_ARGNUM;
      glade_shapes.shapes[0].argnum2 = 0;
      glade_shapes.shapes[0].argnumc = 0;
      glade_shapes.shapes[0].argnum2_glib_context = false;
      glade_shapes.shapes[0].argtotal = 0;
      string_list_init (&glade_shapes.shapes[0].xcomments);
      initialized = true;
    }
  glade_shapes.keyword = tag;
  glade_shapes.keyword_len = strlen (tag) - 1; /* Not-NUL terminated.  */
  glade_shapes.shapes[0].argnum1_glib_context = extract_context;

  return &glade_shapes;
}

static void
glade2_start_element (struct element_state *p, const char *name,
                      const char **attributes)
{
  /* In Glade 2, all <property> and <atkproperty> elements are translatable
     that have the attribute translatable="yes".
     See <http://library.gnome.org/devel/libglade/unstable/libglade-dtd.html>.
     The translator comment is found in the attribute comments="...".
     See <http://live.gnome.org/TranslationProject/DevGuidelines/Use comments>.
  */
  if (p->extract_string)
    {
      bool has_translatable = false;
      bool has_context = false;
      bool is_atkaction = (strcmp (name, "atkaction") == 0);
      const char *extracted_comment = NULL;
      const char **attp = attributes;
      while (*attp != NULL)
        {
          if (strcmp (attp[0], "translatable") == 0)
            has_translatable = (strcmp (attp[1], "yes") == 0);
          else if (strcmp (attp[0], "comments") == 0)
            extracted_comment = attp[1];
          else if (strcmp (attp[0], "context") == 0)
            has_context = (strcmp (attp[1], "yes") == 0);
          else if (is_atkaction && (strcmp (attp[0], "description") == 0))
            {
              lex_pos_ty pos;

              pos.file_name = logical_file_name;
              pos.line_number = XML_GetCurrentLineNumber (parser);

              remember_a_message (mlp, NULL, xstrdup (attp[1]),
                                  null_context, &pos,
                                  NULL, savable_comment);
              break;
            }
          attp += 2;
        }
      /* Atkaction content is not translatable, unless we are
         extracting all strings.  */
      if (is_atkaction)
        p->extract_string = extract_all;
      else
        {
          /* Changed to no when no translatable found unless we
             are extracting all strings.  */
          p->extract_string = has_translatable || extract_all;
          p->extract_context = has_context;
          if (p->extracted_comment != NULL)
            free (p->extracted_comment);
          p->extracted_comment =
            (has_translatable && extracted_comment != NULL
             ? xstrdup (extracted_comment)
             : NULL);
        }
    }
}


/* -----------------------------------------------------------------
                             GtkBuilder.
   -----------------------------------------------------------------  */

/* Returns a pointer to a static callshapes prepared to store
   msgctxt and msgid.
   TAG: Keyword to set. Must not be NULL.
   HAS_CONTEXT: True if msgctxt is not NULL.  */
static const struct callshapes *
get_gtkbuilder_shapes (const char *tag, bool extract_context)
{
  static struct callshapes gtkbuilder_shapes;
  static bool initialized = false;
  if (!initialized)
    {
      gtkbuilder_shapes.nshapes = 1;
      gtkbuilder_shapes.shapes[0].argnum1 = MSGID_ARGNUM;
      gtkbuilder_shapes.shapes[0].argnum2 = 0;
      gtkbuilder_shapes.shapes[0].argnum1_glib_context = false;
      gtkbuilder_shapes.shapes[0].argnum2_glib_context = false;
      gtkbuilder_shapes.shapes[0].argtotal = 0;
      string_list_init (&gtkbuilder_shapes.shapes[0].xcomments);
      initialized = true;
    }
  gtkbuilder_shapes.keyword = tag;
  gtkbuilder_shapes.keyword_len = strlen (tag) - 1; /* Not-NUL terminated.  */
  gtkbuilder_shapes.shapes[0].argnumc = extract_context ? MSGCTXT_ARGNUM : 0;
  return &gtkbuilder_shapes;
}

static void
gtkbuilder_start_element (struct element_state *p, const char *name,
                          const char **attributes)
{
  const char **attp = attributes;
  bool has_translatable = false;
  const char *extracted_comment = NULL;
  const char *extracted_context = NULL;
  /* Even when extract_all is true we must look for coments and context.  */
  while (*attp != NULL)
    {
      if (strcmp (attp[0], "translatable") == 0)
        has_translatable = (strcmp (attp[1], "yes") == 0);
      else if (strcmp (attp[0], "comments") == 0)
        extracted_comment = attp[1];
      else if (strcmp (attp[0], "context") == 0)
        extracted_context = attp[1];
      attp += 2;
    }

  if (p->extract_string)
    p->extract_string = has_translatable || extract_all;

  if (p->extract_string)
    {
      p->extracted_comment =
        (extracted_comment != NULL
         ? xstrdup (extracted_comment)
         : NULL);
      p->extract_context = (extracted_context != NULL);
      p->extracted_context =
        (p->extract_context
         ? xstrdup (extracted_context)
         : NULL);
    }
}


/* Checks the first tag of the XML tree to set the hook functions.
   NAME: Glade1 -> "GTK-Interface".
         Glade2 -> "glade-interface".
         GtkBuilder -> "interface".  */
static void
check_file_type (const char *name)
{
  if (strcmp (name, "glade-interface") == 0)
    {
      do_start_element = glade2_start_element;
      get_shapes = get_glade2_shapes;
      init_glade2_tags ();
    }
  else if (strcmp (name, "interface") == 0)
    {
      do_start_element = gtkbuilder_start_element;
      get_shapes = get_gtkbuilder_shapes;
      init_gtkbuilder_tags ();
    }
  else
    {
      if (strcmp (name, "GTK-Interface") == 0)
        init_glade1_tags ();
      else
        clear_tags ();
      do_start_element = glade_start_element;
      get_shapes = get_glade_shapes;
    }
}


static void
do_extract_glade (FILE *fp,
                  const char *real_filename, const char *logical_filename,
                  msgdomain_list_ty *mdlp)
{
  mlp = mdlp->item[0]->messages;

  /* expat feeds us strings in UTF-8 encoding.  */
  xgettext_current_source_encoding = po_charset_utf8;

  logical_file_name = xstrdup (logical_filename);

  parser = XML_ParserCreate (NULL);
  if (parser == NULL)
    error (EXIT_FAILURE, 0, _("memory exhausted"));

  XML_SetElementHandler (parser, start_element_handler, end_element_handler);
  XML_SetCharacterDataHandler (parser, character_data_handler);
  XML_SetCommentHandler (parser, comment_handler);

  stack_depth = 0;

  while (!feof (fp))
    {
      char buf[4096];
      int count = fread (buf, 1, sizeof buf, fp);

      if (count == 0)
        {
          if (ferror (fp))
            error (EXIT_FAILURE, errno, _("\
error while reading \"%s\""), real_filename);
          /* EOF reached.  */
          break;
        }

      if (XML_Parse (parser, buf, count, 0) == 0)
        error (EXIT_FAILURE, 0, _("%s:%lu:%lu: %s"), logical_filename,
               (unsigned long) XML_GetCurrentLineNumber (parser),
               (unsigned long) XML_GetCurrentColumnNumber (parser) + 1,
               XML_ErrorString (XML_GetErrorCode (parser)));
    }

  if (XML_Parse (parser, NULL, 0, 1) == 0)
    error (EXIT_FAILURE, 0, _("%s:%lu:%lu: %s"), logical_filename,
           (unsigned long) XML_GetCurrentLineNumber (parser),
           (unsigned long) XML_GetCurrentColumnNumber (parser) + 1,
           XML_ErrorString (XML_GetErrorCode (parser)));

  XML_ParserFree (parser);

  /* Close scanner.  */
  logical_file_name = NULL;
  parser = NULL;
}

#endif

void
extract_glade (FILE *fp,
               const char *real_filename, const char *logical_filename,
               flag_context_list_table_ty *flag_table,
               msgdomain_list_ty *mdlp)
{
#if DYNLOAD_LIBEXPAT || HAVE_LIBEXPAT
  if (LIBEXPAT_AVAILABLE ())
    do_extract_glade (fp, real_filename, logical_filename, mdlp);
  else
#endif
    {
      multiline_error (xstrdup (""),
                       xasprintf (_("\
Language \"glade\" is not supported. %s relies on expat.\n\
This version was built without expat.\n"),
                                  basename (program_name)));
      exit (EXIT_FAILURE);
    }
}
