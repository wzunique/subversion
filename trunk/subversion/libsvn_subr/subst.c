/*
 * subst.c :  generic eol/keyword substitution routines
 *
 * ====================================================================
 * Copyright (c) 2000-2004 CollabNet.  All rights reserved.
 *
 * This software is licensed as described in the file COPYING, which
 * you should have received as part of this distribution.  The terms
 * are also available at http://subversion.tigris.org/license-1.html.
 * If newer versions of this license are posted there, you may use a
 * newer version instead, at your option.
 *
 * This software consists of voluntary contributions made by many
 * individuals.  For exact contribution history, see the revision
 * history and logs, available at http://subversion.tigris.org/.
 * ====================================================================
 */



#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <apr_general.h>  /* for strcasecmp() */
#include <apr_pools.h>
#include <apr_tables.h>
#include <apr_file_io.h>
#include <apr_strings.h>

#include "svn_cmdline.h"
#include "svn_types.h"
#include "svn_string.h"
#include "svn_time.h"
#include "svn_path.h"
#include "svn_error.h"
#include "svn_utf.h"
#include "svn_io.h"
#include "svn_subst.h"
#include "svn_pools.h"
#include "svn_ebcdic.h"

#include "svn_private_config.h"

/* The Repository Default EOL used for files which
 * use the 'native' eol style.
 */
#define SVN_SUBST__DEFAULT_EOL_STR SVN_UTF8_NEWLINE_STR

/**
 * The textual elements of a detranslated special file.  One of these
 * strings must appear as the first element of any special file as it
 * exists in the repository or the text base.
 */
#define SVN_SUBST__SPECIAL_LINK_STR "\x6c\x69\x6e\x6b"         /* "link"   */
#define CR_STR                      "\x43\x52"                 /* "CR"     */
#define CRLF_STR                    "\x43\x52\x4c\x46"         /* "CRLF"   */
#define DOT_TMP_STR                 "\x2E\x74\x6D\x70"         /* ".tmp"   */
#define LINK_SPACE_STR              "\x6c\x69\x6e\x6b\x20"     /* "link "  */
#define LF_STR                      "\x4c\x46"                 /* "LF"     */
#define NATIVE_STR                  "\x6e\x61\x74\x69\x76\x65" /* "native" */

void 
svn_subst_eol_style_from_value (svn_subst_eol_style_t *style,
                                const char **eol,
                                const char *value)
{
  if (value == NULL)
    {
      /* property doesn't exist. */
      *eol = NULL;
      if (style)
        *style = svn_subst_eol_style_none;
    }
  else if (! strcmp (NATIVE_STR, value))
    {
#if !APR_CHARSET_EBCDIC
      *eol = APR_EOL_STR;       /* whee, a portability library! */
#else
      /* Assume "native" eol-style is applied only to ascii text files, which 
       * is the most likely case, and set eol as an ASCII CR. 
       * 
       * TODO: Handle ebcdic encoded text files; perhaps another possible
       * value for svn:eol-style: "ebcdic".  This might also pave the way to
       * meaningful diffs of ebcdic text files... */
      *eol = SVN_UTF8_NEWLINE_STR;
#endif
      if (style)
        *style = svn_subst_eol_style_native;
    }
  else if (! strcmp (LF_STR, value))
    {
      *eol = SVN_UTF8_NEWLINE_STR;
      if (style)
        *style = svn_subst_eol_style_fixed;
    }
  else if (! strcmp (CR_STR, value))
    {
      *eol = SVN_UTF8_CR_STR;
      if (style)
        *style = svn_subst_eol_style_fixed;
    }
  else if (! strcmp (CRLF_STR, value))
    {
      *eol = SVN_UTF8_CR_STR SVN_UTF8_NEWLINE_STR;
      if (style)
        *style = svn_subst_eol_style_fixed;
    }
  else
    {
      *eol = NULL;
      if (style)
        *style = svn_subst_eol_style_unknown;
    }
}


svn_boolean_t
svn_subst_translation_required (svn_subst_eol_style_t style,
                                const char *eol,
                                apr_hash_t *keywords,
                                svn_boolean_t special,
                                svn_boolean_t force_eol_check)
{
#if !APR_CHARSET_EBCDIC
  return (special || keywords
          || (style != svn_subst_eol_style_none && force_eol_check)
          || (style == svn_subst_eol_style_native &&
              strcmp (APR_EOL_STR, SVN_SUBST__DEFAULT_EOL_STR) != 0)
          || (style == svn_subst_eol_style_fixed &&
              strcmp (APR_EOL_STR, eol) != 0));
#else
  return (special || keywords
          || (style != svn_subst_eol_style_none && force_eol_check)
          || (style == svn_subst_eol_style_native &&
              strcmp (SVN_UTF8_NEWLINE_STR, SVN_SUBST__DEFAULT_EOL_STR) != 0)
          || (style == svn_subst_eol_style_fixed &&
              strcmp (SVN_UTF8_NEWLINE_STR, eol) != 0));
#endif
}


svn_error_t *
svn_subst_translate_to_normal_form (const char *src,
                                    const char *dst,
                                    svn_subst_eol_style_t eol_style,
                                    const char *eol_str,
                                    svn_boolean_t always_repair_eols,
                                    apr_hash_t *keywords,
                                    svn_boolean_t special,
                                    apr_pool_t *pool)
{

  if (eol_style == svn_subst_eol_style_native)
    eol_str = SVN_SUBST__DEFAULT_EOL_STR;
  else if (! (eol_style == svn_subst_eol_style_fixed
              || eol_style == svn_subst_eol_style_none))
    return svn_error_create (SVN_ERR_IO_UNKNOWN_EOL, NULL, NULL);

  return svn_subst_copy_and_translate3 (src, dst, eol_str,
                                        eol_style == svn_subst_eol_style_fixed
                                        || always_repair_eols,
                                        keywords,
                                        FALSE /* contract keywords */,
                                        special,
                                        pool);
}


/* Helper function for svn_subst_build_keywords */

/* Given a printf-like format string, return a string with proper
 * information filled in.
 *
 * Important API note: This function is the core of the implementation of
 * svn_subst_build_keywords (all versions), and as such must implement the
 * tolerance of NULL and zero inputs that that function's documention
 * stipulates.
 *
 * The format codes:
 *
 * %a author of this revision
 * %b basename of the URL of this file
 * %d short format of date of this revision
 * %D long format of date of this revision
 * %r number of this revision
 * %u URL of this file
 * %% a literal %
 *
 * All memory is allocated out of @a pool.
 */
static svn_string_t *
keyword_printf (const char *fmt,
                const char *rev,
                const char *url,
                apr_time_t date,
                const char *author,
                apr_pool_t *pool)
{
  svn_stringbuf_t *value = svn_stringbuf_ncreate ("", 0, pool);
  const char *cur;
  int n;

  for (;;)
    {
      cur = fmt;

      while (*cur != '\0' && *cur != '%')
        cur++;

      if ((n = cur - fmt) > 0) /* Do we have an as-is string? */
        svn_stringbuf_appendbytes (value, fmt, n);

      if (*cur == '\0')
        break;

      switch (cur[1])
        {
        case 'a': /* author of this revision */
          if (author)
            svn_stringbuf_appendcstr (value, author);
          break;
        case 'b': /* basename of this file */
          if (url)
            {
              const char *base_name
                = svn_path_uri_decode (svn_path_basename (url, pool), pool);
              svn_stringbuf_appendcstr (value, base_name);
            }
          break;
        case 'd': /* short format of date of this revision */
          if (date)
            {
              apr_time_exp_t exploded_time;
              const char *human;

              apr_time_exp_gmt (&exploded_time, date);

              human = APR_PSPRINTF2 (pool, "%04d-%02d-%02d %02d:%02d:%02dZ",
                                     exploded_time.tm_year + 1900,
                                     exploded_time.tm_mon + 1,
                                     exploded_time.tm_mday,
                                     exploded_time.tm_hour,
                                     exploded_time.tm_min,
                                     exploded_time.tm_sec);

              svn_stringbuf_appendcstr (value, human);
            }
          break;
        case 'D': /* long format of date of this revision */
          if (date)
            svn_stringbuf_appendcstr (value,
                                      svn_time_to_human_cstring (date, pool));
          break;
        case 'r': /* number of this revision */
          if (rev)
            svn_stringbuf_appendcstr (value, rev);
          break;
        case 'u': /* URL of this file */
          if (url)
            svn_stringbuf_appendcstr (value, url);
          break;
        case '%': /* '%%' => a literal % */
          svn_stringbuf_appendbytes (value, cur, 1);
          break;
        case '\0': /* '%' as the last character of the string. */
          svn_stringbuf_appendbytes (value, cur, 1);
          /* Now go back one character, since this was just a one character
           * sequence, whereas all others are two characters, and we do not
           * want to skip the null terminator entirely and carry on
           * formatting random memory contents. */
          cur--;
          break;
        default: /* Unrecognized code, just print it literally. */
          svn_stringbuf_appendbytes (value, cur, 2);
          break;
        }

      /* Format code is processed - skip it, and get ready for next chunk. */
      fmt = cur + 2;
    }

  return svn_string_create_from_buf (value, pool);
}

/* Convert an old-style svn_subst_keywords_t struct * into a new-style
 * keywords hash.  Keyword values are shallow copies, so the produced
 * hash must not be assumed to have lifetime longer than the struct it
 * is based on.  A NULL input causes a NULL output. */
static apr_hash_t *
kwstruct_to_kwhash (const svn_subst_keywords_t *kwstruct,
                    apr_pool_t *pool)
{
  apr_hash_t *kwhash;

  if (kwstruct == NULL)
    return NULL;

  kwhash = apr_hash_make(pool);

  if (kwstruct->revision)
    {
      apr_hash_set (kwhash, SVN_KEYWORD_REVISION_LONG,
                    APR_HASH_KEY_STRING, kwstruct->revision);
      apr_hash_set (kwhash, SVN_KEYWORD_REVISION_MEDIUM,
                    APR_HASH_KEY_STRING, kwstruct->revision);
      apr_hash_set (kwhash, SVN_KEYWORD_REVISION_SHORT,
                    APR_HASH_KEY_STRING, kwstruct->revision);
    }
  if (kwstruct->date)
    {
      apr_hash_set (kwhash, SVN_KEYWORD_DATE_LONG,
                    APR_HASH_KEY_STRING, kwstruct->date);
      apr_hash_set (kwhash, SVN_KEYWORD_DATE_SHORT,
                    APR_HASH_KEY_STRING, kwstruct->date);
    }
  if (kwstruct->author)
    {
      apr_hash_set (kwhash, SVN_KEYWORD_AUTHOR_LONG,
                    APR_HASH_KEY_STRING, kwstruct->author);
      apr_hash_set (kwhash, SVN_KEYWORD_AUTHOR_SHORT,
                    APR_HASH_KEY_STRING, kwstruct->author);
    }
  if (kwstruct->url)
    {
      apr_hash_set (kwhash, SVN_KEYWORD_URL_LONG,
                    APR_HASH_KEY_STRING, kwstruct->url);
      apr_hash_set (kwhash, SVN_KEYWORD_URL_SHORT,
                    APR_HASH_KEY_STRING, kwstruct->url);
    }
  if (kwstruct->id)
    {
      apr_hash_set (kwhash, SVN_KEYWORD_ID,
                    APR_HASH_KEY_STRING, kwstruct->id);
    }

  return kwhash;
}

svn_error_t *
svn_subst_build_keywords (svn_subst_keywords_t *kw,
                          const char *keywords_val,
                          const char *rev,
                          const char *url,
                          apr_time_t date,
                          const char *author,
                          apr_pool_t *pool)
{
  apr_hash_t *kwhash;
  const svn_string_t *val;

  SVN_ERR (svn_subst_build_keywords2 (&kwhash, keywords_val, rev,
                                      url, date, author, pool));

  /* The behaviour of pre-1.3 svn_subst_build_keywords, which we are
   * replicating here, is to write to a slot in the svn_subst_keywords_t
   * only if the relevant keyword was present in keywords_val, otherwise
   * leaving that slot untouched. */

  val = apr_hash_get(kwhash, SVN_KEYWORD_REVISION_LONG, APR_HASH_KEY_STRING);
  if (val)
    kw->revision = val;

  val = apr_hash_get(kwhash, SVN_KEYWORD_DATE_LONG, APR_HASH_KEY_STRING);
  if (val)
    kw->date = val;

  val = apr_hash_get(kwhash, SVN_KEYWORD_AUTHOR_LONG, APR_HASH_KEY_STRING);
  if (val)
    kw->author = val;

  val = apr_hash_get(kwhash, SVN_KEYWORD_URL_LONG, APR_HASH_KEY_STRING);
  if (val)
    kw->url = val;

  val = apr_hash_get(kwhash, SVN_KEYWORD_ID, APR_HASH_KEY_STRING);
  if (val)
    kw->id = val;

  return SVN_NO_ERROR;
}

svn_error_t *
svn_subst_build_keywords2 (apr_hash_t **kw,
                           const char *keywords_val,
                           const char *rev,
                           const char *url,
                           apr_time_t date,
                           const char *author,
                           apr_pool_t *pool)
{
  apr_array_header_t *keyword_tokens;
  int i;
  *kw = apr_hash_make (pool);

  keyword_tokens = svn_cstring_split (keywords_val,
                                      SVN_UTF8_SPACE_STR \
                                      SVN_UTF8_TAB_STR \
                                      SVN_UTF8_VTAB_STR \
                                      SVN_UTF8_NEWLINE_STR \
                                      SVN_UTF8_BS_STR \
                                      SVN_UTF8_CR_STR \
                                      SVN_UTF8_FF_STR,
                                      TRUE /* chop */, pool);

  for (i = 0; i < keyword_tokens->nelts; ++i)
    {
      const char *keyword = APR_ARRAY_IDX (keyword_tokens, i, const char *);

      if ((! strcmp (keyword, SVN_KEYWORD_REVISION_LONG))
          || (! strcmp (keyword, SVN_KEYWORD_REVISION_MEDIUM))
          || (! strcasecmp (keyword, SVN_KEYWORD_REVISION_SHORT)))
        {
          svn_string_t *revision_val;

          revision_val = keyword_printf ("%r", rev, url, date, author, pool);
          apr_hash_set (*kw, SVN_KEYWORD_REVISION_LONG,
                        APR_HASH_KEY_STRING, revision_val);
          apr_hash_set (*kw, SVN_KEYWORD_REVISION_MEDIUM,
                        APR_HASH_KEY_STRING, revision_val);
          apr_hash_set (*kw, SVN_KEYWORD_REVISION_SHORT,
                        APR_HASH_KEY_STRING, revision_val);
        }
      else if ((! strcmp (keyword, SVN_KEYWORD_DATE_LONG))
               || (! strcasecmp (keyword, SVN_KEYWORD_DATE_SHORT)))
        {
          svn_string_t *date_val;

          date_val = keyword_printf ("%D", rev, url, date, author, pool);
          apr_hash_set (*kw, SVN_KEYWORD_DATE_LONG,
                        APR_HASH_KEY_STRING, date_val);
          apr_hash_set (*kw, SVN_KEYWORD_DATE_SHORT,
                        APR_HASH_KEY_STRING, date_val);
        }
      else if ((! strcmp (keyword, SVN_KEYWORD_AUTHOR_LONG))
               || (! strcasecmp (keyword, SVN_KEYWORD_AUTHOR_SHORT)))
        {
          svn_string_t *author_val;

          author_val = keyword_printf ("%a", rev, url, date, author, pool);
          apr_hash_set (*kw, SVN_KEYWORD_AUTHOR_LONG,
                        APR_HASH_KEY_STRING, author_val);
          apr_hash_set (*kw, SVN_KEYWORD_AUTHOR_SHORT,
                        APR_HASH_KEY_STRING, author_val);
        }
      else if ((! strcmp (keyword, SVN_KEYWORD_URL_LONG))
               || (! strcasecmp (keyword, SVN_KEYWORD_URL_SHORT)))
        {
          svn_string_t *url_val;

          url_val = keyword_printf ("%u", rev, url, date, author, pool);
          apr_hash_set (*kw, SVN_KEYWORD_URL_LONG,
                        APR_HASH_KEY_STRING, url_val);
          apr_hash_set (*kw, SVN_KEYWORD_URL_SHORT,
                        APR_HASH_KEY_STRING, url_val);
        }
      else if ((! strcasecmp (keyword, SVN_KEYWORD_ID)))
        {
          svn_string_t *id_val;

          id_val = keyword_printf ("%b %r %d %a", rev, url, date, author,
                                   pool);
          apr_hash_set (*kw, SVN_KEYWORD_ID,
                        APR_HASH_KEY_STRING, id_val);
        }
    }

  return SVN_NO_ERROR;
}


/*** Helpers for svn_subst_translate_stream2 ***/


/* Write out LEN bytes of BUF into STREAM. */
static svn_error_t *
translate_write (svn_stream_t *stream,
                 const void *buf,
                 apr_size_t len)
{
  apr_size_t wrote = len;
  svn_error_t *write_err = svn_stream_write (stream, buf, &wrote);
  if ((write_err) || (len != wrote))
    return write_err;

  return SVN_NO_ERROR;
}


/* Perform the substition of VALUE into keyword string BUF (with len
   *LEN), given a pre-parsed KEYWORD (and KEYWORD_LEN), and updating
   *LEN to the new size of the substituted result.  Return TRUE if all
   goes well, FALSE otherwise.  If VALUE is NULL, keyword will be
   contracted, else it will be expanded.  */
static svn_boolean_t
translate_keyword_subst (char *buf,
                         apr_size_t *len,
                         const char *keyword,
                         apr_size_t keyword_len,
                         const svn_string_t *value)
{
  char *buf_ptr;

  /* Make sure we gotz good stuffs. */
  assert (*len <= SVN_KEYWORD_MAX_LEN);
  assert ((buf[0] == SVN_UTF8_DOLLAR) && (buf[*len - 1] == SVN_UTF8_DOLLAR));

  /* Need at least a keyword and two $'s. */
  if (*len < keyword_len + 2)
    return FALSE;

  /* The keyword needs to match what we're looking for. */
  if (strncmp (buf + 1, keyword, keyword_len))
    return FALSE;

  buf_ptr = buf + 1 + keyword_len;

  /* Check for fixed-length expansion. 
   * The format of fixed length keyword and its data is
   * Unexpanded keyword:         "$keyword::       $"
   * Expanded keyword:           "$keyword:: value $"
   * Expanded kw with filling:   "$keyword:: value   $"
   * Truncated keyword:          "$keyword:: longval#$"
   */
  if ((buf_ptr[0] == SVN_UTF8_COLON) /* first char after keyword is ':' */
      && (buf_ptr[1] == SVN_UTF8_COLON) /* second char after keyword is ':' */
      && (buf_ptr[2] == SVN_UTF8_SPACE) /* third char after keyword is ' ' */
      && ((buf[*len - 2] == SVN_UTF8_SPACE)  /* has ' ' for next to last character */
          || (buf[*len - 2] == SVN_UTF8_POUND)) /* .. or has '#' for next to last
                                        character */
      && ((6 + keyword_len) < *len))  /* holds "$kw:: x $" at least */
    {
      /* This is fixed length keyword, so *len remains unchanged */
      apr_size_t max_value_len = *len - (6 + keyword_len);

      if (! value)
        {
          /* no value, so unexpand */
          buf_ptr += 2;
          while (*buf_ptr != SVN_UTF8_DOLLAR)
            *(buf_ptr++) = SVN_UTF8_SPACE;
        }
      else 
        {
          if (value->len <= max_value_len) 
            { /* replacement not as long as template, pad with spaces */
              strncpy (buf_ptr + 3, value->data, value->len);
              buf_ptr += 3 + value->len;
              while (*buf_ptr != SVN_UTF8_DOLLAR)
                *(buf_ptr++) = SVN_UTF8_SPACE;
            }
          else
            {
              /* replacement needs truncating */
              strncpy (buf_ptr + 3, value->data, max_value_len);
              buf[*len - 2] = SVN_UTF8_POUND;
              buf[*len - 1] = SVN_UTF8_DOLLAR;
            }
        }
      return TRUE;
    }

  /* Check for unexpanded keyword. */
  else if ((buf_ptr[0] == SVN_UTF8_DOLLAR)          /* "$keyword$" */
           || ((buf_ptr[0] == SVN_UTF8_COLON) 
               && (buf_ptr[1] == SVN_UTF8_DOLLAR))) /* "$keyword:$" */
    {
      /* unexpanded... */
      if (value)
        {
          /* ...so expand. */
          buf_ptr[0] = SVN_UTF8_COLON;
          buf_ptr[1] = SVN_UTF8_SPACE;
          if (value->len)
            {
              apr_size_t vallen = value->len;

              /* "$keyword: value $" */
              if (vallen > (SVN_KEYWORD_MAX_LEN - 5 - keyword_len))
                vallen = SVN_KEYWORD_MAX_LEN - 5 - keyword_len;
              strncpy (buf_ptr + 2, value->data, vallen);
              buf_ptr[2 + vallen] = SVN_UTF8_SPACE;
              buf_ptr[2 + vallen + 1] = SVN_UTF8_DOLLAR;
              *len = 5 + keyword_len + vallen;
            }
          else
            {
              /* "$keyword: $"  */
              buf_ptr[2] = SVN_UTF8_DOLLAR;
              *len = 4 + keyword_len;
            }
        }
      else
        {
          /* ...but do nothing. */
        }
      return TRUE;
    }

  /* Check for expanded keyword. */
  else if ((*len >= 4 + keyword_len ) /* holds at least "$keyword: $" */
           && (buf_ptr[0] == SVN_UTF8_COLON)
              /* first char after keyword is ':' */
           && (buf_ptr[1] == SVN_UTF8_SPACE)
              /* second char after keyword is ' ' */
           && (buf[*len - 2] == SVN_UTF8_SPACE))
              /* has ' ' for next to last character */
    {
      /* expanded... */
      if (! value)
        {
          /* ...so unexpand. */
          buf_ptr[0] = SVN_UTF8_DOLLAR;
          *len = 2 + keyword_len;
        }
      else
        {
          /* ...so re-expand. */
          buf_ptr[0] = SVN_UTF8_COLON;
          buf_ptr[1] = SVN_UTF8_SPACE;
          if (value->len)
            {
              apr_size_t vallen = value->len;

              /* "$keyword: value $" */
              if (vallen > (SVN_KEYWORD_MAX_LEN - 5))
                vallen = SVN_KEYWORD_MAX_LEN - 5;
              strncpy (buf_ptr + 2, value->data, vallen);
              buf_ptr[2 + vallen] = SVN_UTF8_SPACE;
              buf_ptr[2 + vallen + 1] = SVN_UTF8_DOLLAR;
              *len = 5 + keyword_len + vallen;
            }
          else
            {
              /* "$keyword: $"  */
              buf_ptr[2] = SVN_UTF8_DOLLAR;
              *len = 4 + keyword_len;
            }
        }
      return TRUE;
    }
  
  return FALSE;
}                         

/* Parse BUF (whose length is *LEN) for Subversion keywords.  If a
   keyword is found, optionally perform the substitution on it in
   place, update *LEN with the new length of the translated keyword
   string, and return TRUE.  If this buffer doesn't contain a known
   keyword pattern, leave BUF and *LEN untouched and return FALSE.

   See the docstring for svn_subst_copy_and_translate for how the
   EXPAND and KEYWORDS parameters work.

   NOTE: It is assumed that BUF has been allocated to be at least
   SVN_KEYWORD_MAX_LEN bytes longs, and that the data in BUF is less
   than or equal SVN_KEYWORD_MAX_LEN in length.  Also, any expansions
   which would result in a keyword string which is greater than
   SVN_KEYWORD_MAX_LEN will have their values truncated in such a way
   that the resultant keyword string is still valid (begins with
   "$Keyword:", ends in " $" and is SVN_KEYWORD_MAX_LEN bytes long).  */
static svn_boolean_t
translate_keyword (char *buf,
                   apr_size_t *len,
                   svn_boolean_t expand,
                   apr_hash_t *keywords)
{
  const svn_string_t *value;
  char keyword_name[SVN_KEYWORD_MAX_LEN + 1];
  apr_size_t i;

  /* Make sure we gotz good stuffs. */
  assert (*len <= SVN_KEYWORD_MAX_LEN);
  assert ((buf[0] == SVN_UTF8_DOLLAR) && (buf[*len - 1] == SVN_UTF8_DOLLAR));

  /* Early return for ignored keywords */
  if (! keywords)
    return FALSE;

  /* Extract the name of the keyword */
  for (i = 0; i < *len - 2 && buf[i + 1] != SVN_UTF8_COLON; i++)
    keyword_name[i] = buf[i + 1];
  keyword_name[i] = '\0';

  value = apr_hash_get (keywords, keyword_name, APR_HASH_KEY_STRING);

  if (value)
    {
      return translate_keyword_subst (buf, len,
                                      keyword_name, strlen (keyword_name),
                                      expand ? value : NULL);
    }

  return FALSE;
}


/* Translate NEWLINE_BUF (length of NEWLINE_LEN) to the newline format
   specified in EOL_STR (length of EOL_STR_LEN), and write the
   translated thing to FILE (whose path is DST_PATH).  

   SRC_FORMAT (length *SRC_FORMAT_LEN) is a cache of the first newline
   found while processing SRC_PATH.  If the current newline is not the
   same style as that of SRC_FORMAT, look to the REPAIR parameter.  If
   REPAIR is TRUE, ignore the inconsistency, else return an
   SVN_ERR_IO_INCONSISTENT_EOL error.  If we are examining the first
   newline in the file, copy it to {SRC_FORMAT, *SRC_FORMAT_LEN} to
   use for later consistency checks. */
static svn_error_t *
translate_newline (const char *eol_str,
                   apr_size_t eol_str_len,
                   char *src_format,
                   apr_size_t *src_format_len,
                   char *newline_buf,
                   apr_size_t newline_len,
                   svn_stream_t *dst,
                   svn_boolean_t repair)
{
  /* If this is the first newline we've seen, cache it
     future comparisons, else compare it with our cache to
     check for consistency. */
  if (*src_format_len)
    {
      /* Comparing with cache.  If we are inconsistent and
         we are NOT repairing the file, generate an error! */
      if ((! repair) &&
          ((*src_format_len != newline_len) ||
           (strncmp (src_format, newline_buf, newline_len)))) 
        return svn_error_create (SVN_ERR_IO_INCONSISTENT_EOL, NULL, NULL);
    }
  else
    {
      /* This is our first line ending, so cache it before
         handling it. */
      strncpy (src_format, newline_buf, newline_len);
      *src_format_len = newline_len;
    }
  /* Translate the newline */
  return translate_write (dst, eol_str, eol_str_len);
}



/*** Public interfaces. ***/

svn_boolean_t
svn_subst_keywords_differ (const svn_subst_keywords_t *a,
                           const svn_subst_keywords_t *b,
                           svn_boolean_t compare_values)
{
  if (((a == NULL) && (b == NULL)) /* no A or B */
      /* no A, and B has no contents */
      || ((a == NULL) 
          && (b->revision == NULL)
          && (b->date == NULL)
          && (b->author == NULL)
          && (b->url == NULL))
      /* no B, and A has no contents */
      || ((b == NULL)           && (a->revision == NULL)
          && (a->date == NULL)
          && (a->author == NULL)
          && (a->url == NULL))
      /* neither A nor B has any contents */
      || ((a != NULL) && (b != NULL) 
          && (b->revision == NULL)
          && (b->date == NULL)
          && (b->author == NULL)
          && (b->url == NULL)
          && (a->revision == NULL)
          && (a->date == NULL)
          && (a->author == NULL)
          && (a->url == NULL)))
    {
      return FALSE;
    }
  else if ((a == NULL) || (b == NULL))
    return TRUE;
  
  /* Else both A and B have some keywords. */
  
  if ((! a->revision) != (! b->revision))
    return TRUE;
  else if ((compare_values && (a->revision != NULL))
           && (strcmp (a->revision->data, b->revision->data) != 0))
    return TRUE;
    
  if ((! a->date) != (! b->date))
    return TRUE;
  else if ((compare_values && (a->date != NULL))
           && (strcmp (a->date->data, b->date->data) != 0))
    return TRUE;
    
  if ((! a->author) != (! b->author))
    return TRUE;
  else if ((compare_values && (a->author != NULL))
           && (strcmp (a->author->data, b->author->data) != 0))
    return TRUE;
  
  if ((! a->url) != (! b->url))
    return TRUE;
  else if ((compare_values && (a->url != NULL))
           && (strcmp (a->url->data, b->url->data) != 0))
    return TRUE;
  
  /* Else we never found a difference, so they must be the same. */  
  
  return FALSE;
}

svn_boolean_t
svn_subst_keywords_differ2 (apr_hash_t *a,
                            apr_hash_t *b,
                            svn_boolean_t compare_values,
                            apr_pool_t *pool)
{
  apr_hash_index_t *hi;
  unsigned int a_count, b_count;

  /* An empty hash is logically equal to a NULL,
   * as far as this API is concerned. */
  a_count = (a == NULL) ? 0 : apr_hash_count (a);
  b_count = (b == NULL) ? 0 : apr_hash_count (b);

  if (a_count != b_count)
    return TRUE;

  if (a_count == 0)
    return FALSE;

  /* The hashes are both non-NULL, and have the same number of items.
   * We must check that every item of A is present in B. */
  for (hi = apr_hash_first(pool, a); hi; hi = apr_hash_next(hi))
    {
      const void *key;
      apr_ssize_t klen;
      void *void_a_val;
      svn_string_t *a_val, *b_val;

      apr_hash_this (hi, &key, &klen, &void_a_val);
      a_val = void_a_val;
      b_val = apr_hash_get (b, key, klen);

      if (!b_val || (compare_values && !svn_string_compare (a_val, b_val)))
        return TRUE;
    }

  return FALSE;
}

svn_error_t *
svn_subst_translate_stream2 (svn_stream_t *s, /* src stream */
                             svn_stream_t *d, /* dst stream */
                             const char *eol_str,
                             svn_boolean_t repair,
                             const svn_subst_keywords_t *keywords,
                             svn_boolean_t expand,
                             apr_pool_t *pool)
{
  apr_hash_t *kh = kwstruct_to_kwhash (keywords, pool);

  return svn_subst_translate_stream3 (s, d, eol_str, repair, kh, expand, pool);
}

/* Baton for translate_chunk() to store its state in. */
struct translation_baton
{
  const char *eol_str;
  svn_boolean_t repair;
  apr_hash_t *keywords;
  svn_boolean_t expand;

  /* Characters (excluding the terminating NUL character) which
     may trigger a translation action, hence are 'interesting' */
  const char *interesting;

  /* Length of the string EOL_STR points to. */
  apr_size_t eol_str_len;

  /* Buffer to cache any newline state between translation chunks */
  char newline_buf[2];

  /* Offset (within newline_buf) of the first *unused* character */
  apr_size_t newline_off;

  /* Buffer to cache keyword-parsing state between translation chunks */
  char keyword_buf[SVN_KEYWORD_MAX_LEN];

  /* Offset (within keyword-buf) to the first *unused* character */
  apr_size_t keyword_off;

  /* EOL style used in the chunk-source */
  char src_format[2];

  /* Length of the EOL style string found in the chunk-source,
     or zero if none encountered yet */
  apr_size_t src_format_len;
};


/* Allocate a baton for use with translate_chunk() in POOL and
 * initialize it for the first iteration.
 *
 * The caller must assure that EOL_STR and KEYWORDS at least
 * have the same life time as that of POOL.
 *
 */

static struct translation_baton *
create_translation_baton (const char *eol_str,
                          svn_boolean_t repair,
                          apr_hash_t *keywords,
                          svn_boolean_t expand,
                          apr_pool_t *pool)
{
  struct translation_baton *b = apr_palloc (pool, sizeof (*b));

  /* For efficiency, convert an empty set of keywords to NULL. */
  if (keywords && (apr_hash_count (keywords) == 0))
    keywords = NULL;

  b->eol_str = eol_str;
  b->eol_str_len = eol_str ? strlen (eol_str) : 0;
  b->repair = repair;
  b->keywords = keywords;
  b->expand = expand;
  b->interesting = (eol_str && keywords)
                   ? SVN_UTF8_DOLLAR_STR SVN_UTF8_CR_STR SVN_UTF8_NEWLINE_STR
                   : eol_str
                   ? SVN_UTF8_CR_STR SVN_UTF8_NEWLINE_STR
                   : SVN_UTF8_DOLLAR_STR;
  b->newline_off = 0;
  b->keyword_off = 0;
  b->src_format_len = 0;

  return b;
}

/* Translate eols and keywords of a 'chunk' of characters BUF of size BUFLEN
 * according to the settings and state stored in baton B.
 *
 * Write output to stream DST.
 *
 * To finish a series of chunk translations, flush all buffers by calling
 * this routine with a NULL value for BUF.
 *
 * Use POOL for temporary allocations.
 */
static svn_error_t *
translate_chunk (svn_stream_t *dst,
                 struct translation_baton *b,
                 const char *buf,
                 apr_size_t buflen,
                 apr_pool_t *pool)
{
  const char *p;
  apr_size_t len;

  if (buf)
    {
      /* precalculate some oft-used values */
      const char *end = buf + buflen;
      const char *interesting = b->interesting;

      /* At the beginning of this loop, assume that we might be in an
       * interesting state, i.e. with data in the newline or keyword
       * buffer.  First try to get to the boring state so we can copy
       * a run of boring characters; then try to get back to the
       * interesting state by processing an interesting character,
       * and repeat. */
      for (p = buf; p < end;)
        {
          /* Try to get to the boring state, if necessary. */
          if (b->newline_off)
            {
              if (*p == SVN_UTF8_NEWLINE)
                b->newline_buf[b->newline_off++] = *p++;

              SVN_ERR (translate_newline (b->eol_str, b->eol_str_len,
                                          b->src_format,
                                          &b->src_format_len, b->newline_buf,
                                          b->newline_off, dst, b->repair));

              b->newline_off = 0;
            }
          else if (b->keyword_off && *p == SVN_UTF8_DOLLAR)
            {
              /* If translation fails, treat this '$' as a starting '$'. */
              b->keyword_buf[b->keyword_off++] = SVN_UTF8_DOLLAR;
              if (translate_keyword (b->keyword_buf, &b->keyword_off,
                                     b->expand, b->keywords))
                p++;
              else
                b->keyword_off--;

              SVN_ERR (translate_write (dst, b->keyword_buf, b->keyword_off));

              b->keyword_off = 0;
            }
          else if (b->keyword_off == SVN_KEYWORD_MAX_LEN - 1
                   || (b->keyword_off
                       && (*p == SVN_UTF8_CR || *p == SVN_UTF8_NEWLINE)))
            {
              /* No closing '$' found; flush the keyword buffer. */
              SVN_ERR (translate_write (dst, b->keyword_buf, b->keyword_off));

              b->keyword_off = 0;
            }
          else if (b->keyword_off)
            {
              b->keyword_buf[b->keyword_off++] = *p++;
              continue;
            }

          /* We're in the boring state; look for interest characters. */
          len = 0;

          /* We wanted memcspn(), but lacking that, the loop below has
             the same effect.

             Also, skip NUL characters explicitly, since strchr()
             considers them part of the string argument,
             but we don't consider them interesting
          */
          while ((p + len) < end
                 && (! p[len] || ! strchr (interesting, p[len])))
            len++;

          if (len)
            SVN_ERR (translate_write (dst, p, len));

          p += len;

          /* Set up state according to the interesting character, if any. */
          if (p < end)
            {
              switch (*p)
                {
                case SVN_UTF8_DOLLAR:
                  b->keyword_buf[b->keyword_off++] = *p++;
                  break;
                case SVN_UTF8_CR:
                  b->newline_buf[b->newline_off++] = *p++;
                  break;
                case SVN_UTF8_NEWLINE:
                  b->newline_buf[b->newline_off++] = *p++;

                  SVN_ERR (translate_newline (b->eol_str, b->eol_str_len,
                                              b->src_format,
                                              &b->src_format_len,
                                              b->newline_buf,
                                              b->newline_off, dst, b->repair));

                  b->newline_off = 0;
                  break;

                }
            }
        }
    }
  else
    {
      if (b->newline_off)
        SVN_ERR (translate_newline (b->eol_str, b->eol_str_len, b->src_format,
                                    &b->src_format_len, b->newline_buf,
                                    b->newline_off, dst, b->repair));

      if (b->keyword_off)
        SVN_ERR (translate_write (dst, b->keyword_buf, b->keyword_off));
    }

  return SVN_NO_ERROR;
}

/* Baton for use with translated stream callbacks. */
struct translated_stream_baton
{
  /* Stream to take input from (before translation) on read
     /write output to (after translation) on write. */
  svn_stream_t *stream;

  /* Input/Output translation batons to make them separate chunk streams. */
  struct translation_baton *in_baton, *out_baton;

  /* Remembers whether any write operations have taken place;
     if so, we need to flush the output chunk stream. */
  svn_boolean_t written;

  /* Buffer to hold translated read data. */
  svn_stringbuf_t *readbuf;

  /* Offset of the first non-read character in readbuf. */
  apr_size_t readbuf_off;

  /* Buffer to hold read data
     between svn_stream_read() and translate_chunk(). */
  char *buf;

  /* Pool in which (only!) this baton is allocated. */
  apr_pool_t *pool;

  /* Pool for callback iterations */
  apr_pool_t *iterpool;
};


static svn_error_t *
translated_stream_read (void *baton,
                        char *buffer,
                        apr_size_t *len)
{
  struct translated_stream_baton *b = baton;
  apr_size_t readlen = SVN__STREAM_CHUNK_SIZE;
  apr_size_t unsatisfied = *len;
  apr_size_t off = 0;
  apr_pool_t *iterpool;

  iterpool = b->iterpool;
  while (readlen == SVN__STREAM_CHUNK_SIZE && unsatisfied > 0)
    {
      apr_size_t to_copy;

      svn_pool_clear (iterpool);
      /* fill read buffer, if necessary */
      if (! (b->readbuf_off < b->readbuf->len))
        {
          svn_stream_t *buf_stream;

          svn_stringbuf_setempty (b->readbuf);
          SVN_ERR (svn_stream_read (b->stream, b->buf, &readlen));
          buf_stream = svn_stream_from_stringbuf (b->readbuf, iterpool);

          SVN_ERR (translate_chunk (buf_stream, b->in_baton, b->buf,
                                    readlen, iterpool));

          if (readlen != SVN__STREAM_CHUNK_SIZE)
            SVN_ERR (translate_chunk (buf_stream, b->in_baton, NULL, 0,
                                      iterpool));

          SVN_ERR (svn_stream_close (buf_stream));
        }

      /* Satisfy from the read buffer */
      to_copy = (b->readbuf->len > unsatisfied)
        ? unsatisfied : b->readbuf->len;
      memcpy (buffer + off, b->readbuf->data + b->readbuf_off, to_copy);
      off += to_copy;
      b->readbuf_off += to_copy;
      unsatisfied -= to_copy;
    }

  *len -= unsatisfied;

  return SVN_NO_ERROR;
}

static svn_error_t *
translated_stream_write (void *baton,
                         const char *buffer,
                         apr_size_t *len)
{
  struct translated_stream_baton *b = baton;
  svn_pool_clear (b->iterpool);

  b->written = TRUE;
  SVN_ERR (translate_chunk (b->stream, b->out_baton, buffer, *len,
                            b->iterpool));

  return SVN_NO_ERROR;
}

static svn_error_t *
translated_stream_close (void *baton)
{
  struct translated_stream_baton *b = baton;

  if (b->written)
    SVN_ERR (translate_chunk (b->stream, b->out_baton, NULL, 0, b->iterpool));

  SVN_ERR (svn_stream_close (b->stream));

  svn_pool_destroy (b->pool);   /* Also destroys the baton itself */
  return SVN_NO_ERROR;
}


svn_stream_t *
svn_subst_stream_translated (svn_stream_t *stream,
                             const char *eol_str,
                             svn_boolean_t repair,
                             apr_hash_t *keywords,
                             svn_boolean_t expand,
                             apr_pool_t *pool)
{
  apr_pool_t *baton_pool = svn_pool_create (pool);
  struct translated_stream_baton *baton
    = apr_palloc (baton_pool, sizeof (*baton));
  svn_stream_t *s = svn_stream_create (baton, baton_pool);

  /* Make sure EOL_STR and KEYWORDS are allocated in POOL, as
     required by create_translation_baton() */
  if (eol_str)
    eol_str = apr_pstrdup (baton_pool, eol_str);
  if (keywords)
    {
      if (apr_hash_count (keywords) == 0)
        keywords = NULL;
      else
        {
          /* deep copy the hash to make sure it's allocated in POOL */
          apr_hash_t *copy = apr_hash_make (baton_pool);
          apr_hash_index_t *hi;

          for (hi = apr_hash_first (pool, keywords);
               hi; hi = apr_hash_next (hi))
            {
              const void *key;
              void *val;
              apr_hash_this (hi, &key, NULL, &val);
              apr_hash_set (copy, apr_pstrdup (baton_pool, key),
                            APR_HASH_KEY_STRING,
                            svn_string_dup (val, baton_pool));
            }

          keywords = copy;
        }
    }

  /* Setup the baton fields */
  baton->stream = stream;
  baton->in_baton
    = create_translation_baton (eol_str, repair, keywords, expand, baton_pool);
  baton->out_baton
    = create_translation_baton (eol_str, repair, keywords, expand, baton_pool);
  baton->written = FALSE;
  baton->readbuf = svn_stringbuf_create ("", baton_pool);
  baton->readbuf_off = 0;
  baton->iterpool = svn_pool_create (baton_pool);
  baton->pool = baton_pool;
  baton->buf = apr_palloc (baton->pool, SVN__STREAM_CHUNK_SIZE + 1);

  /* Setup the stream methods */
  svn_stream_set_read (s, translated_stream_read);
  svn_stream_set_write (s, translated_stream_write);
  svn_stream_set_close (s, translated_stream_close);

  return s;
}


svn_error_t *
svn_subst_translate_stream3 (svn_stream_t *s, /* src stream */
                             svn_stream_t *d, /* dst stream */
                             const char *eol_str,
                             svn_boolean_t repair,
                             apr_hash_t *keywords,
                             svn_boolean_t expand,
                             apr_pool_t *pool)
{
  apr_pool_t *subpool = svn_pool_create (pool);
  apr_pool_t *iterpool = svn_pool_create (subpool);
  struct translation_baton *baton;
  apr_size_t readlen = SVN__STREAM_CHUNK_SIZE;
  char *buf = apr_palloc (subpool, SVN__STREAM_CHUNK_SIZE);

  /* The docstring requires that *some* translation be requested. */
  assert (eol_str || keywords);

  baton = create_translation_baton (eol_str, repair, keywords, expand, pool);
  while (readlen == SVN__STREAM_CHUNK_SIZE)
    {
      svn_pool_clear (iterpool);
      SVN_ERR (svn_stream_read (s, buf, &readlen));
      SVN_ERR (translate_chunk (d, baton, buf, readlen, iterpool));
    }

  SVN_ERR (translate_chunk (d, baton, NULL, 0, iterpool));

  svn_pool_destroy (subpool); /* also destroys iterpool */
  return SVN_NO_ERROR;
}


svn_error_t *
svn_subst_translate_stream (svn_stream_t *s, /* src stream */
                            svn_stream_t *d, /* dst stream */
                            const char *eol_str,
                            svn_boolean_t repair,
                            const svn_subst_keywords_t *keywords,
                            svn_boolean_t expand)
{
  apr_pool_t *pool = svn_pool_create (NULL);
  svn_error_t *err = svn_subst_translate_stream2 (s, d, eol_str, repair,
                                                  keywords, expand, pool);
  svn_pool_destroy (pool);
  return err;
}


svn_error_t *
svn_subst_translate_cstring (const char *src,
                             const char **dst,
                             const char *eol_str,
                             svn_boolean_t repair,
                             const svn_subst_keywords_t *keywords,
                             svn_boolean_t expand,
                             apr_pool_t *pool)
{
  apr_hash_t *kh = kwstruct_to_kwhash (keywords, pool);

  return svn_subst_translate_cstring2 (src, dst, eol_str, repair,
                                       kh, expand, pool);
}

svn_error_t *
svn_subst_translate_cstring2 (const char *src,
                              const char **dst,
                              const char *eol_str,
                              svn_boolean_t repair,
                              apr_hash_t *keywords,
                              svn_boolean_t expand,
                              apr_pool_t *pool)
{
  svn_stringbuf_t *src_stringbuf, *dst_stringbuf;
  svn_stream_t *src_stream, *dst_stream;
  svn_error_t *err;

  src_stringbuf = svn_stringbuf_create (src, pool);
  
  /* The easy way out:  no translation needed, just copy. */
  if (! (eol_str || (keywords && (apr_hash_count (keywords) > 0))))
    {
      dst_stringbuf = svn_stringbuf_dup (src_stringbuf, pool);
      goto all_good;
    }

  /* Convert our stringbufs into streams. */
  src_stream = svn_stream_from_stringbuf (src_stringbuf, pool);
  dst_stringbuf = svn_stringbuf_create ("", pool);
  dst_stream = svn_stream_from_stringbuf (dst_stringbuf, pool);

  /* Translate src stream into dst stream. */
  err = svn_subst_translate_stream3 (src_stream, dst_stream,
                                     eol_str, repair, keywords, expand, pool);
  if (err)
    {
      svn_error_clear (svn_stream_close (src_stream));
      svn_error_clear (svn_stream_close (dst_stream));
      return err;
    }

  /* clean up nicely. */
  SVN_ERR (svn_stream_close (src_stream));
  SVN_ERR (svn_stream_close (dst_stream));

 all_good:
  *dst = dst_stringbuf->data;
  return SVN_NO_ERROR;
}


svn_error_t *
svn_subst_copy_and_translate (const char *src,
                              const char *dst,
                              const char *eol_str,
                              svn_boolean_t repair,
                              const svn_subst_keywords_t *keywords,
                              svn_boolean_t expand,
                              apr_pool_t *pool)
{
  return svn_subst_copy_and_translate2 (src, dst, eol_str, repair, keywords,
                                        expand, FALSE, pool);
}


/* Given a special file at SRC, generate a textual representation of
   it in a normal file at DST.  Perform all allocations in POOL. */
static svn_error_t *
detranslate_special_file (const char *src,
                          const char *dst,
                          apr_pool_t *pool)
{
  const char *dst_tmp;
  svn_string_t *buf;
  apr_file_t *s, *d;
  svn_stream_t *src_stream, *dst_stream;
  apr_finfo_t finfo;
  
  /* First determine what type of special file we are
     detranslating. */
  SVN_ERR (svn_io_stat (&finfo, src, APR_FINFO_MIN | APR_FINFO_LINK, pool));

  /* Open a temporary destination that we will eventually atomically
     rename into place. */
  SVN_ERR (svn_io_open_unique_file2 (&d, &dst_tmp, dst,
                                     DOT_TMP_STR, svn_io_file_del_none, pool));

  dst_stream = svn_stream_from_aprfile (d, pool);
  
  switch (finfo.filetype) {
  case APR_REG:
    /* Nothing special to do here, just copy the original file's
       contents. */
    SVN_ERR (svn_io_file_open (&s, src, APR_READ | APR_BUFFERED,
                               APR_OS_DEFAULT, pool));
    src_stream = svn_stream_from_aprfile (s, pool);

    SVN_ERR (svn_stream_copy (src_stream, dst_stream, pool));
    break;
  case APR_LNK:
    /* Determine the destination of the link. */
    SVN_ERR (svn_io_read_link (&buf, src, pool));

    SVN_ERR (svn_stream_printf (dst_stream, pool, "%s%s",
                                LINK_SPACE_STR, buf->data));
    break;
  default:
    abort ();
  }

  SVN_ERR (svn_io_file_close (d, pool));

  /* Do the atomic rename from our temporary location. */
  SVN_ERR (svn_io_file_rename (dst_tmp, dst, pool));
  
  return SVN_NO_ERROR;
}


/* Given a file containing a repository representation of a special
   file in SRC, create the appropriate special file at location DST.
   Perform all allocations in POOL. */
static svn_error_t *
create_special_file (const char *src,
                     const char *dst,
                     apr_pool_t *pool)
{
  svn_stringbuf_t *contents;
  char *identifier, *remainder;
  const char *dst_tmp, *src_tmp = NULL;
  svn_error_t *err;
  svn_node_kind_t kind;
  svn_boolean_t is_special;

  /* Check to see if we are being asked to create a special file from
     a special file.  If so, do a temporary detranslation and work
     from there. */
  SVN_ERR (svn_io_check_special_path (src, &kind, &is_special, pool));

  if (is_special)
    {
      SVN_ERR (svn_io_open_unique_file2 (NULL, &src_tmp, dst, DOT_TMP_STR,
                                         svn_io_file_del_none, pool));
      SVN_ERR (detranslate_special_file (src, src_tmp, pool));
      src = src_tmp;
    }
  
  /* Read in the detranslated file. */
  SVN_ERR (svn_stringbuf_from_file (&contents, src, pool));

  /* If there was just a temporary detranslation, remove it now. */
  if (src_tmp)
    SVN_ERR (svn_io_remove_file (src_tmp, pool));
      
  /* Separate off the identifier.  The first space character delimits
     the identifier, after which any remaining characters are specific
     to the actual special device being created. */
  identifier = contents->data;
  for (remainder = identifier; *remainder; remainder++)
    {
      if (*remainder == SVN_UTF8_SPACE)
        {
          *remainder = '\0';
          remainder++;
          break;
        }
    }
           
  if (! strcmp (identifier, SVN_SUBST__SPECIAL_LINK_STR))
    {
      /* For symlinks, the type specific data is just a filesystem
         path that the symlink should reference. */
      err = svn_io_create_unique_link (&dst_tmp, dst, remainder,
                                       DOT_TMP_STR, pool);
    }
  else
    {
      /* We should return a valid error here. */
      return svn_error_createf (SVN_ERR_UNSUPPORTED_FEATURE, NULL,
                                "Unsupported special file type '%s'",
                                identifier);
    }

  /* If we had an error, check to see if it was because this type of
     special device is not supported. */
  if (err)
    {
      if (err->apr_err == SVN_ERR_UNSUPPORTED_FEATURE)
        {
          svn_error_clear (err);
          /* Fall back to just copying the text-base. */
          SVN_ERR (svn_io_open_unique_file2 (NULL, &dst_tmp, dst, DOT_TMP_STR,
                                             svn_io_file_del_none, pool));
          SVN_ERR (svn_io_copy_file (src, dst_tmp, TRUE, pool));
        }
      else
        return err;
    }

  /* Do the atomic rename from our temporary location. */
  SVN_ERR (svn_io_file_rename (dst_tmp, dst, pool));

  return SVN_NO_ERROR;
}


svn_error_t *
svn_subst_copy_and_translate2 (const char *src,
                               const char *dst,
                               const char *eol_str,
                               svn_boolean_t repair,
                               const svn_subst_keywords_t *keywords,
                               svn_boolean_t expand,
                               svn_boolean_t special,
                               apr_pool_t *pool)
{
  apr_hash_t *kh = kwstruct_to_kwhash (keywords, pool);

  return svn_subst_copy_and_translate3 (src, dst, eol_str,
                                        repair, kh, expand, special,
                                        pool);
}

svn_error_t *
svn_subst_copy_and_translate3 (const char *src,
                               const char *dst,
                               const char *eol_str,
                               svn_boolean_t repair,
                               apr_hash_t *keywords,
                               svn_boolean_t expand,
                               svn_boolean_t special,
                               apr_pool_t *pool)
{
  const char *dst_tmp = NULL;
  svn_stream_t *src_stream, *dst_stream;
  apr_file_t *s = NULL, *d = NULL;  /* init to null important for APR */
  svn_error_t *err;
  svn_node_kind_t kind;
  svn_boolean_t path_special;

  SVN_ERR (svn_io_check_special_path (src, &kind, &path_special, pool));

  /* If this is a 'special' file, we may need to create it or
     detranslate it. */
  if (special || path_special)
    {
      if (expand)
        SVN_ERR (create_special_file (src, dst, pool));
      else
        SVN_ERR (detranslate_special_file (src, dst, pool));
      
      return SVN_NO_ERROR;
    }

  /* The easy way out:  no translation needed, just copy. */
  if (! (eol_str || (keywords && (apr_hash_count (keywords) > 0))))
    return svn_io_copy_file (src, dst, FALSE, pool);

  /* Open source file. */
  SVN_ERR (svn_io_file_open (&s, src, APR_READ | APR_BUFFERED,
                             APR_OS_DEFAULT, pool));

  /* For atomicity, we translate to a tmp file and
     then rename the tmp file over the real destination. */
  SVN_ERR (svn_io_open_unique_file2 (&d, &dst_tmp, dst,
                                     DOT_TMP_STR,
                                     svn_io_file_del_on_pool_cleanup,
                                     pool));

  /* Now convert our two open files into streams. */
  src_stream = svn_stream_from_aprfile (s, pool);
  dst_stream = svn_stream_from_aprfile (d, pool);

  /* Translate src stream into dst stream. */
  err = svn_subst_translate_stream3 (src_stream, dst_stream, eol_str,
                                     repair, keywords, expand, pool);
  if (err)
    {
      if (err->apr_err == SVN_ERR_IO_INCONSISTENT_EOL)
        return svn_error_createf
          (SVN_ERR_IO_INCONSISTENT_EOL, err,
           _("File '%s' has inconsistent newlines"),
           svn_path_local_style (src, pool));
      else
        return err;
    }

  /* clean up nicely. */
  SVN_ERR (svn_stream_close (src_stream));
  SVN_ERR (svn_stream_close (dst_stream));
  SVN_ERR (svn_io_file_close (s, pool));
  SVN_ERR (svn_io_file_close (d, pool));

  /* Now that dst_tmp contains the translated data, do the atomic rename. */
  SVN_ERR (svn_io_file_rename (dst_tmp, dst, pool));
#if AS400
  /* On the iSeries svn_io_open_unique_file creates a file with a ccsid of 37.
   * So once dst is in place tag it as 1208, which may not be
   * entirely accurate, but is as far as /r and /n are concerned. */
  SVN_ERR (svn_ebcdic_set_file_ccsid (dst, 1208, pool));
#endif

  return SVN_NO_ERROR;
}



svn_error_t *
svn_subst_translate_string (svn_string_t **new_value,
                            const svn_string_t *value,
                            const char *encoding,
                            apr_pool_t *pool)
{
  const char *val_utf8;
  const char *val_utf8_lf;

  if (value == NULL)
    {
      *new_value = NULL;
      return SVN_NO_ERROR;
    }

  if (encoding)
    {
      SVN_ERR (svn_utf_cstring_to_utf8_ex (&val_utf8, value->data,
                                           encoding, NULL, pool));
    }
  else
    {
      SVN_ERR (svn_utf_cstring_to_utf8 (&val_utf8, value->data, pool));
    }

  SVN_ERR (svn_subst_translate_cstring2 (val_utf8,
                                         &val_utf8_lf,
                                         SVN_UTF8_NEWLINE_STR,  /* translate to LF */
                                         FALSE, /* no repair */
                                         NULL,  /* no keywords */
                                         FALSE, /* no expansion */
                                         pool));
  
  *new_value = svn_string_create (val_utf8_lf, pool);

  return SVN_NO_ERROR;
}


svn_error_t *
svn_subst_detranslate_string (svn_string_t **new_value,
                              const svn_string_t *value,
                              svn_boolean_t for_output,
                              apr_pool_t *pool)
{
  svn_error_t *err;
  const char *val_neol;
  const char *val_nlocale_neol;

  if (value == NULL)
    {
      *new_value = NULL;
      return SVN_NO_ERROR;
    }

  SVN_ERR (svn_subst_translate_cstring2 (value->data,
                                         &val_neol,
                                         SVN_UTF8_NEWLINE_STR,  /* 'native' eol */
                                         FALSE, /* no repair */
                                         NULL,  /* no keywords */
                                         FALSE, /* no expansion */
                                         pool));

  if (for_output)
    {
      err = svn_cmdline_cstring_from_utf8 (&val_nlocale_neol, val_neol, pool);
      if (err && (APR_STATUS_IS_EINVAL (err->apr_err)))
        {
          val_nlocale_neol =
            svn_cmdline_cstring_from_utf8_fuzzy (val_neol, pool);
          svn_error_clear (err);
        }
      else if (err)
        return err;
    }
  else
    {
      err = svn_utf_cstring_from_utf8 (&val_nlocale_neol, val_neol, pool);
      if (err && (APR_STATUS_IS_EINVAL (err->apr_err)))
        {
          val_nlocale_neol = svn_utf_cstring_from_utf8_fuzzy (val_neol, pool);
          svn_error_clear (err);
        }
      else if (err)
        return err;
    }

  *new_value = svn_string_create (val_nlocale_neol, pool);

  return SVN_NO_ERROR;
}
