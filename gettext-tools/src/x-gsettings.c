/* xgettext gsettings backend.
   Copyright (C) 2013 Free Software Foundation, Inc.

   Most of this file is based on x-glade.c:
   written by Bruno Haible <haible@clisp.cons.org>, 2002.

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
#include "x-gsettings.h"

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


/* GSettings schemalist is an XML based format.  */


/* ====================== Keyword set customization.  ====================== */

/* If true extract all strings.  */
static bool extract_all = false;

static hash_table keywords;
static bool default_keywords = true;

void
x_gsettings_extract_all ()
{
  extract_all = true;
}

void
x_gsettings_keyword (const char *name)
{
  if (name == NULL)
    default_keywords = false;
  else
    {
      if (keywords.table == NULL)
        hash_init (&keywords, 20);

      hash_insert_entry (&keywords, name, strlen (name), NULL);
    }
}

static void
init_keywords ()
{
  if (default_keywords)
    {
      /* When adding new keywords here, also update the documentation in
         xgettext.texi!  */
      x_gsettings_keyword ("description");
      x_gsettings_keyword ("summary");
      x_gsettings_keyword ("default");
    }
}

/* Name must be not-NULL.  */
static bool
is_tag (const char *name)
{
  void *hash_result;
  if (keywords.table != NULL)
    return hash_find_entry (&keywords, name, strlen (name), &hash_result) == 0;
  return false;
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
/* Used for msgid extraction.  */
#define MSGID_ARGNUM 1
/* Used for msgctxt extraction.  */
#define MSGCTXT_ARGNUM 2

/* Returns a pointer to a static callshapes prepared to extract only
   msgid.
   TAG: Keyword to set. Must not be NULL.  */
static const struct callshapes *
get_shapes (const char *tag, bool has_context)
{
  static struct callshapes shapes;
  static bool initialized = false;
  if (!initialized)
    {
      shapes.nshapes = 1;
      shapes.shapes[0].argnum1 = MSGID_ARGNUM;
      shapes.shapes[0].argnum2 = 0;
      shapes.shapes[0].argnum1_glib_context = false;
      shapes.shapes[0].argnum2_glib_context = false;
      shapes.shapes[0].argtotal = 0;
      string_list_init (&shapes.shapes[0].xcomments);
      initialized = true;
    }
  shapes.keyword = tag;
  shapes.keyword_len = strlen (tag) - 1; /* Not-NUL terminated.  */
  shapes.shapes[0].argnumc = has_context? MSGCTXT_ARGNUM : 0;

  return &shapes;
}

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
start_element_handler (void *user_data, const char *name,
                       const char **attributes)
{
  struct element_state *p;
  const char *extracted_l10n = NULL;
  const char *extracted_comment = NULL;
  const char *extracted_context = NULL;

  /* Increase stack depth.  */
  stack_depth++;
  ensure_stack_size (stack_depth + 1);

  /* Don't extract a string for the containing element.  */
  stack[stack_depth - 1].extract_string = false;

  p = &stack[stack_depth];
  p->extract_string = extract_all;
  p->extracted_comment = NULL;
  p->extracted_context = NULL;
  p->lineno = XML_GetCurrentLineNumber (parser);
  p->buffer = NULL;
  p->bufmax = 0;
  p->buflen = 0;

  /* The correct tags have been already inserted. */
  if (!p->extract_string)
    p->extract_string = is_tag (name);

  if (p->extract_string)
    {
      const char **attp = attributes;
      while (*attp != NULL)
        {
          if (strcmp (attp[0], "comments") == 0)
            extracted_comment = attp[1];
          else if (strcmp (attp[0], "context") == 0)
            extracted_context = attp[1];
          else if (strcmp (attp[0], "l10n") == 0)
            extracted_l10n = attp[1];
          attp += 2;
        }

      /* FIXME: Time localization also should be extracted.  */
      if ((strcmp (name, "default") == 0)
          && ((extracted_l10n == NULL)
              || (strcmp (extracted_l10n, "time") == 0)))
        p->extract_string = false;

      p->extracted_comment =
        (extracted_comment != NULL
         ? xstrdup (extracted_comment)
         : NULL);
      p->extracted_context =
        (extracted_context != NULL
         ? xstrdup (extracted_context)
         : NULL);
    }

  if (!p->extract_string)
    savable_comment_reset ();
}

static char *
extract_quotation_marks (const char *str, size_t last)
{
  if (last > 0)
    {
      if ((str[0] == '"' && str[last] == '"')
          || (str[0] == '\'' && str[last]  == '\''))
        {
          /* Erase first quotation mark.  */
          char *tmp = xstrdup (str + 1);
          /* Erase last quotation mark.  */
          tmp[last - 1] = '\0';
          return tmp;
        }
    }
  return str;
}

/* Callback called when </element> is seen.  */
static void
end_element_handler (void *user_data, const char *name)
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

          if (strcmp (name, "default") == 0)
            {
              char *tmp = extract_quotation_marks (p->buffer, p->buflen);
              if (tmp != p->buffer)
                {
                  free (p->buffer);
                  p->buffer = tmp;
                }
            }

          store_message (p, get_shapes (name, p->extracted_context != NULL));
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
character_data_handler (void *user_data, const char *s, int len)
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
comment_handler (void *user_data, const char *data)
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

static void
do_extract_gsettings (FILE *fp,
                      const char *real_filename,
                      const char *logical_filename,
                      msgdomain_list_ty *mdlp)
{
  mlp = mdlp->item[0]->messages;

  /* expat feeds us strings in UTF-8 encoding.  */
  xgettext_current_source_encoding = po_charset_utf8;

  logical_file_name = xstrdup (logical_filename);

  init_keywords ();

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
  free (logical_file_name);
  logical_file_name = NULL;
  parser = NULL;
}

#endif

void
extract_gsettings (FILE *fp,
                   const char *real_filename,
                   const char *logical_filename,
                   flag_context_list_table_ty *flag_table,
                   msgdomain_list_ty *mdlp)
{
#if DYNLOAD_LIBEXPAT || HAVE_LIBEXPAT
  if (LIBEXPAT_AVAILABLE ())
    do_extract_gsettings (fp, real_filename, logical_filename, mdlp);
  else
#endif
    {
      multiline_error (xstrdup (""),
                       xasprintf (_("\
Language \"gsettings\" is not supported. %s relies on expat.\n\
This version was built without expat.\n"),
                                  basename (program_name)));
      exit (EXIT_FAILURE);
    }
}
