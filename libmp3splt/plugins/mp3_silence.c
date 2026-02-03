/**********************************************************
 * libmp3splt -- library based on mp3splt,
 *               for mp3/ogg splitting without decoding
 *
 * Copyright (c) 2002-2005 M. Trotta - <mtrotta@users.sourceforge.net>
 * Copyright (c) 2005-2014 Alexandru Munteanu - m@ioalex.net
 *
 *********************************************************/

/**********************************************************
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
 *********************************************************/

#include <stdio.h>

#include "mp3.h"
#include "mp3_silence.h"
#include "mp3_utils.h"
#include "silence_processors.h"

static void splt_mp3_scan_silence_and_process(splt_state *state, off_t begin_offset,
  float max_threshold, int min_bits, unsigned long length,
  splt_scan_silence_processor_t process_silence, splt_scan_silence_data *ssd, int *error);
static mad_fixed_t splt_mp3_pcm_silence(const mad_fixed_t *samples, int length, mad_fixed_t *lpf_level);
static int splt_mp3_entropy_silence(splt_mp3_state *mp3state, int channel);
static int splt_mp3_gain_silence(splt_mp3_state *mp3state, int channel);

/*! scan for silence

\return
 - the number of silence points found
 - -1 on error
\param state The central structure libmp3splt keeps all of its data in
\param error Contains the error number if an error has occoured
\param length The time length to scan [in seconds]
\param threshold The threshold that tells noise from silence
*/
int splt_mp3_scan_silence(splt_state *state, off_t begin, unsigned long length, float threshold,
  int min_bits, float min_len, int shots, short output, int *error,
  splt_scan_silence_processor_t silence_processor)
{
  splt_scan_silence_data *ssd =
    splt_scan_silence_data_new(state, output, min_len, shots, SPLT_TRUE);
  if (ssd == NULL)
  {
    *error = SPLT_ERROR_CANNOT_ALLOCATE_MEMORY;
    return -1;
  }

  splt_mp3_scan_silence_and_process(
    state, begin, threshold, min_bits, length, silence_processor, ssd, error);

  int found = ssd->found;

  splt_free_scan_silence_data(&ssd);

  if (*error < 0) { found = -1; }

  return found;
}

static void log_silence(splt_state *state, unsigned long time_cs, int channel, mad_fixed_t max_sample, mad_fixed_t avg_sample, int ent_bits, int gain) {
  FILE *full_log_file_descriptor = splt_t_get_silence_full_log_file_descriptor(state);
  if (!full_log_file_descriptor) { return; }

  float time = time_cs / 100.f;

  float max_db = splt_co_convert_to_db(mad_f_todouble(max_sample));
  float avg_db = splt_co_convert_to_db(mad_f_todouble(avg_sample));

  fprintf(full_log_file_descriptor, "%f,%d,%f,%d,%f,%d,%d,%d\n", time,
    channel, max_db, max_sample, avg_db, avg_sample, ent_bits, gain);
}

static void splt_mp3_scan_silence_and_process(splt_state *state, off_t begin_offset,
  float max_threshold, int min_bits, unsigned long length,
  splt_scan_silence_processor_t process_silence, splt_scan_silence_data *ssd, int *error)
{
  int found = 0;
  short stop = SPLT_FALSE;
  int do_synth = 1;

  mad_fixed_t threshold = mad_f_tofixed(splt_co_convert_from_db(max_threshold));

  splt_mp3_state *mp3state = state->codec;

  splt_c_put_progress_text(state, SPLT_PROGRESS_SCAN_SILENCE);

  //seek to the begin
  if (fseeko(mp3state->file_input, begin_offset, SEEK_SET) == -1)
  {
    splt_e_set_strerror_msg_with_data(state, splt_t_get_filename_to_split(state));
    *error = SPLT_ERROR_SEEKING_FILE;
    return;
  }

  //initialise mad stuff
  splt_mp3_init_stream_frame(mp3state, MAD_OPTION_NOCHANNEL);
  mad_synth_init(&mp3state->synth);

  mad_timer_reset(&mp3state->timer);

  do {
    int mad_err = SPLT_OK;

    int result = splt_mp3_get_valid_frame(state, &mad_err);

    switch (result)
    {
      case -1:
      case 1:
        //1 we have a valid frame
        mad_timer_add(&mp3state->timer, mp3state->frame.header.duration);
        if (do_synth) mad_synth_frame(&mp3state->synth, &mp3state->frame);
        unsigned long time =
          (unsigned long)mad_timer_count(mp3state->timer, MAD_UNITS_CENTISECONDS);

        int silence_was_found;
        float level = 0.0;
        int ent_bits = 0;
        int channels = MAD_NCHANNELS(&mp3state->frame.header);

        /* Check statistics on each channel for this frame */
        int channel;
        for (channel = 0; channel < channels; channel++) {
          mad_fixed_t avg_sample;
          mad_fixed_t max_sample =
            splt_mp3_pcm_silence(mp3state->synth.pcm.samples[channel], mp3state->synth.pcm.length, &avg_sample);
          silence_was_found = max_sample < threshold;

          level = splt_co_convert_to_db(mad_f_todouble(avg_sample));

          ent_bits = splt_mp3_entropy_silence(mp3state, channel);
          silence_was_found |= ent_bits < min_bits;

          int max_gain = splt_mp3_gain_silence(mp3state, channel);
          silence_was_found |= max_gain < 75;

          if (silence_was_found) {
            log_silence(state, time, channel, max_sample, avg_sample, ent_bits, max_gain);
          }
        }

        int err = SPLT_OK;
        short must_flush = (length > 0 && time >= length);
        if (must_flush || time < 0) {
          ssd->flush = SPLT_TRUE;
          stop = SPLT_TRUE;
        }

        if (stop || stop == -1)
        {
          stop = SPLT_TRUE;
          if (err < 0)
          {
            *error = err;
            goto end;
          }
        }

        if (mp3state->mp3file.len > 0)
        {
          off_t pos = ftello(mp3state->file_input);

          if (state->split.get_silence_level)
          {
            state->split.get_silence_level(time, level, state->split.silence_level_client_data);
          }
          state->split.p_bar->silence_db_level = level;
          state->split.p_bar->silence_ent_bits = ent_bits;
          state->split.p_bar->silence_found_tracks = found;

          //if we don't have silence split,
          //put the 1/4 of progress
          if ((splt_o_get_int_option(state, SPLT_OPT_SPLIT_MODE) != SPLT_OPTION_SILENCE_MODE) &&
              (splt_o_get_int_option(state, SPLT_OPT_SPLIT_MODE) != SPLT_OPTION_TRIM_SILENCE_MODE))
          {
            splt_c_update_progress(
              state, (double)(time), (double)(length), 4, 1 / (float)4, SPLT_DEFAULT_PROGRESS_RATE);
          }
          else
          {
            if (splt_t_split_is_canceled(state))
            {
              //split cancelled
              stop = SPLT_TRUE;
            }
            splt_c_update_progress(state, (double)pos, (double)(mp3state->mp3file.len), 1, 0,
              SPLT_DEFAULT_PROGRESS_RATE);
          }
        }

        //-1 means eof
        if (result == -1) { stop = SPLT_TRUE; }
        break;
      case 0:
        //0 do nothing
        break;
      case -3:
        //error from libmad
        stop = SPLT_TRUE;
        *error = mad_err;
        break;
      default:
        break;
    }

  } while (!stop);

  int err = SPLT_OK;
  //process_silence(-1, -96, -1, SPLT_FALSE, SPLT_FALSE, ssd, &junk, &err);
  if (err < 0) { *error = err; }

  //only if we have silence mode, we set progress to 100%
  if ((splt_o_get_int_option(state, SPLT_OPT_SPLIT_MODE) == SPLT_OPTION_SILENCE_MODE) ||
      (splt_o_get_int_option(state, SPLT_OPT_SPLIT_MODE) == SPLT_OPTION_TRIM_SILENCE_MODE))
  {
    splt_c_update_progress(state, 1.0, 1.0, 1, 1, 1);
  }

end:
  //finish with mad_*
  splt_mp3_finish_stream_frame(mp3state);
  mad_synth_finish(&mp3state->synth);
}

/*!  Get the loudest sample in the granule

Used by mp3_scan_silence

\return
 - max(abs(samples))

Always computes only one frame
*/
static mad_fixed_t splt_mp3_pcm_silence(const mad_fixed_t *samples, int length, mad_fixed_t *lpf_level)
{
  int i;
  mad_fixed_t sample;
  mad_fixed_t max_sample = 0;
  unsigned long long sample_sum = 0;

  for (i = 0; i < length; i++)
  {
    sample = mad_f_abs(samples[i]);
    sample_sum += sample;

    if (sample > max_sample) max_sample = sample;
  }

  *lpf_level = (mad_fixed_t)(sample_sum / length);

  return max_sample;
}

/*!  Find the lowest granule entropy count on the given channel.

Used by mp3_scan_silence

\return
 - Number of bits used to encode DCT coefficients

Always computes only one frame
*/

static int splt_mp3_entropy_silence(splt_mp3_state *mp3state, int channel)
{
  int gr;
  struct sideinfo *si = mp3state->frame.header.extra;
  int ngr = mp3state->frame.header.flags & MAD_FLAG_LSF_EXT ? 2 : 1;
  int min_ent = INT_MAX;

  if (si == NULL) return 0;


  for (gr = 0; gr < ngr; gr++)
  {
    struct granule *granule = &si->gr[gr];

    int cnt = granule->ch[channel].part2_3_length;
    /* Compute the minimum entropy bit count across all granules */
    min_ent = cnt < min_ent ? cnt : min_ent;
  }

  return min_ent;
}

/*!  Find the maximum global gain of all granules in a frame.

Used by mp3_scan_silence

\return
 - Max global gain for this channel

Always computes only one frame
*/
static int splt_mp3_gain_silence(splt_mp3_state *mp3state, int channel)
{
  int gr;
  struct sideinfo *si = mp3state->frame.header.extra;
  int ngr = mp3state->frame.header.flags & MAD_FLAG_LSF_EXT ? 2 : 1;
  int max_gain = 0;

  if (si == NULL) return 0;


  for (gr = 0; gr < ngr; gr++)
  {
    struct granule *granule = &si->gr[gr];

    int gain = granule->ch[channel].global_gain;
    /* Compute the minimum entropy bit count across all granules */
    max_gain = gain > max_gain ? gain : max_gain;
  }

  return max_gain;
}
