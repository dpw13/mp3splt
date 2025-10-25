/**********************************************************
 *
 * libmp3splt -- library based on mp3splt,
 *               for mp3/ogg splitting without decoding
 *
 * Copyright (c) 2010 David Belohrad
 * Copyright (c) 2010-2014 Alexandru Munteanu - m@ioalex.net
 *
 * http://mp3splt.sourceforge.net
 *
 *********************************************************/

/**********************************************************
 *
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
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301,
 * USA.
 *
 *********************************************************/

#ifndef NO_PCRE

#include <string.h>
#define PCRE2_CODE_UNIT_WIDTH 8
#include <pcre2.h>

#include "splt.h"

static char *splt_fr_get_pattern(pcre2_match_data *match, char *key);
static void splt_fr_copy_pattern_to_tags(pcre2_match_data *match,
  char *key, int tags_field, splt_tags *tags, int format, int replace_underscores, int *error);
static int splt_fr_get_int_pattern(pcre2_match_data *match, char *key);
static void splt_fr_set_char_field_on_tags_and_convert(
  splt_tags *tags, int tags_field, char *pattern, int format, int replace_underscores, int *error);

/*!

\todo Support calculating of the total number of tracks
\todo What does this function do?
*/
splt_tags *splt_fr_parse_from_state(splt_state *state, int *error)
{
  const char *filename_to_split = splt_t_get_filename_to_split(state);
  char *regex = splt_t_get_input_filename_regex(state);
  char *default_comment = splt_t_get_default_comment_tag(state);
  char *default_genre = splt_t_get_default_genre_tag(state);

  char *filename = splt_su_get_fname_without_path_and_extension(filename_to_split, error);
  if (*error < 0) { return NULL; }

  splt_tags *tags = splt_fr_parse(state, filename, regex, default_comment, default_genre, error);

  if (filename)
  {
    free(filename);
    filename = NULL;
  }

  return tags;
}

splt_tags *splt_fr_parse(splt_state *state, const char *filename, const char *regex,
  const char *default_comment, const char *default_genre, int *error)
{
  int errorcode;
  size_t erroroffset;

  splt_d_print_debug(state, "filename for regex = _%s_\n", filename);
  splt_d_print_debug(state, "regex = _%s_\n", regex);

  if (regex == NULL)
  {
    *error = SPLT_INVALID_REGEX;
    splt_e_set_error_data(state, _("no regular expression provided"));
    return NULL;
  }

  pcre2_code *re = pcre2_compile((PCRE2_SPTR)regex, 0, PCRE2_CASELESS | PCRE2_UTF, &errorcode, &erroroffset, NULL);
  if (!re)
  {
    *error = SPLT_INVALID_REGEX;
    char *message = splt_su_get_formatted_message(state, "@%u: %d", erroroffset, errorcode);
    splt_e_set_error_data(state, message);
    return NULL;
  }

  pcre2_match_data *match = pcre2_match_data_create_from_pattern(re, NULL);

  int rc = pcre2_match(re, (PCRE2_UCHAR *)filename, strlen(filename), 0, 0, match, NULL);
  if (rc == PCRE2_ERROR_NOMATCH)
  {
    *error = SPLT_REGEX_NO_MATCH;
    pcre2_match_data_free(match);
    pcre2_code_free(re);
    return NULL;
  }

  splt_tags *tags = splt_tu_new_tags(error);
  if (*error < 0)
  {
    pcre2_match_data_free(match);
    pcre2_code_free(re);
    return NULL;
  }
  splt_tu_reset_tags(tags);

  int replace_underscores = splt_o_get_int_option(state, SPLT_OPT_REPLACE_UNDERSCORES_TAG_FORMAT);

  int format = splt_o_get_int_option(state, SPLT_OPT_ARTIST_TAG_FORMAT);
  splt_fr_copy_pattern_to_tags(match, "artist", SPLT_TAGS_ARTIST, tags, format,
    replace_underscores, error);
  if (*error < 0) { goto error; }

  format = splt_o_get_int_option(state, SPLT_OPT_ALBUM_TAG_FORMAT);
  splt_fr_copy_pattern_to_tags(
    match, "album", SPLT_TAGS_ALBUM, tags, format, replace_underscores, error);
  if (*error < 0) { goto error; }

  splt_fr_copy_pattern_to_tags(
    match, "year", SPLT_TAGS_YEAR, tags, SPLT_NO_CONVERSION, SPLT_FALSE, error);
  if (*error < 0) { goto error; }

  format = splt_o_get_int_option(state, SPLT_OPT_COMMENT_TAG_FORMAT);
  char *pattern = splt_fr_get_pattern(match, "comment");
  if (pattern)
  {
    splt_fr_set_char_field_on_tags_and_convert(
      tags, SPLT_TAGS_COMMENT, (char *)pattern, format, replace_underscores, error);
    pcre2_substring_free((PCRE2_UCHAR *)pattern);
    if (*error < 0) { goto error; }
  }
  else { splt_tu_set_field_on_tags(tags, SPLT_TAGS_COMMENT, default_comment); }

  int track = splt_fr_get_int_pattern(match, "tracknum");
  if (track != -1) { splt_tu_set_field_on_tags(tags, SPLT_TAGS_TRACK, &track); }

  //TODO: total tracks support
  int total_tracks = splt_fr_get_int_pattern(match, "tracks");

  format = splt_o_get_int_option(state, SPLT_OPT_TITLE_TAG_FORMAT);
  char *title = splt_fr_get_pattern(match, "title");
  if (title)
  {
    splt_fr_set_char_field_on_tags_and_convert(
      tags, SPLT_TAGS_TITLE, title, format, replace_underscores, error);
    pcre2_substring_free((PCRE2_UCHAR *)title);
    if (*error < 0) { goto error; }
  }
  else
  {
    if (track != -1 && total_tracks != -1)
    {
      title = splt_su_get_formatted_message(state, "Track %d of %d", track, total_tracks);
    }
    else if (track != -1 && total_tracks == -1)
    {
      title = splt_su_get_formatted_message(state, "Track %d", track);
    }

    if (title)
    {
      splt_fr_set_char_field_on_tags_and_convert(
        tags, SPLT_TAGS_TITLE, title, SPLT_NO_CONVERSION, SPLT_FALSE, error);

      free(title);
      title = NULL;

      if (*error < 0) { goto error; }
    }
  }

  char *genre = splt_fr_get_pattern(match, "genre");
  if (genre)
  {
    splt_tu_set_field_on_tags(tags, SPLT_TAGS_GENRE, genre);
    pcre2_substring_free((PCRE2_UCHAR *)genre);
    if (*error < 0) { goto error; }
  }
  else { splt_tu_set_field_on_tags(tags, SPLT_TAGS_GENRE, default_genre); }

  pcre2_match_data_free(match);
  pcre2_code_free(re);

  *error = SPLT_REGEX_OK;

  return tags;

error:
  pcre2_code_free(re);
  splt_tu_free_one_tags(&tags);
  return NULL;
}

static void splt_fr_copy_pattern_to_tags(pcre2_match_data *match,
  char *key, int tags_field, splt_tags *tags, int format, int replace_underscores, int *error)
{
  char *pattern = NULL;
  pattern = splt_fr_get_pattern(match, key);

  splt_fr_set_char_field_on_tags_and_convert(
    tags, tags_field, pattern, format, replace_underscores, error);

  if (pattern) { pcre2_substring_free((PCRE2_UCHAR *)pattern); }
}

static int splt_fr_get_int_pattern(pcre2_match_data *match, char *key)
{
  int value = -1;

  char *pattern = NULL;
  pattern = splt_fr_get_pattern(match, key);
  if (pattern)
  {
    value = atoi(pattern);
    pcre2_substring_free((PCRE2_UCHAR *)pattern);
  }

  return value;
}

static char *splt_fr_get_pattern(pcre2_match_data *match, char *key)
{
  PCRE2_UCHAR *pattern = NULL;

  if (pcre2_substring_get_byname_8(match, (PCRE2_UCHAR *)key, (PCRE2_UCHAR **)&pattern, NULL) ==
      PCRE2_ERROR_NOSUBSTRING)
  {
    return NULL;
  }
  else { return (char *)pattern; }
}

static void splt_fr_set_char_field_on_tags_and_convert(
  splt_tags *tags, int tags_field, char *pattern, int format, int replace_underscores, int *error)
{
  if (replace_underscores) { splt_su_replace_all_char(pattern, '_', ' '); }

  char *converted_pattern = splt_su_convert(pattern, format, error);
  if (*error < 0) { return; }

  splt_tu_set_field_on_tags(tags, tags_field, converted_pattern);

  if (converted_pattern)
  {
    free(converted_pattern);
    converted_pattern = NULL;
  }
}

#endif
