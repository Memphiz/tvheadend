/*
 *  tvheadend, simple muxer that just passes the input along
 *  Copyright (C) 2012 John Törnblom
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <htmlui://www.gnu.org/licenses/>.
 */

#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <assert.h>

#include "tvheadend.h"
#include "streaming.h"
#include "epg.h"
#include "service.h"
#include "input/mpegts/dvb.h"
#include "muxer_pass.h"
#include "dvr/dvr.h"

typedef struct pass_muxer {
  muxer_t;

  /* File descriptor stuff */
  off_t pm_off;
  int   pm_fd;
  int   pm_seekable;
  int   pm_error;

  /* Filename is also used for logging */
  char *pm_filename;

  /* TS muxing */
  uint16_t  pm_pmt_pid;
  uint8_t   pm_pmt_cc;
  uint8_t  *pm_pmt;
  uint16_t  pm_pmt_version;
  uint16_t  pm_service_id;

  mpegts_psi_table_t pm_pat;
  mpegts_psi_table_t pm_sdt;
  mpegts_psi_table_t pm_eit;

} pass_muxer_t;


static void
pass_muxer_write(muxer_t *m, const void *data, size_t size);


/** 
 * PMT generator
 */
static int
pass_muxer_build_pmt(const streaming_start_t *ss, uint8_t *buf0, int maxlen,
	      int version, int pcrpid)
{
  int c, tlen, dlen, l, i;
  uint8_t *buf, *buf1;

  buf = buf0;

  if(maxlen < 12)
    return -1;

  buf[0] = 2; /* table id, always 2 */

  /* program id */
  buf[3] = ss->ss_service_id >> 8;
  buf[4] = ss->ss_service_id & 0xff;

  buf[5] = 0xc1; /* current_next_indicator + version */
  buf[5] |= (version & 0x1F) << 1;

  buf[6] = 0; /* section number */
  buf[7] = 0; /* last section number */

  buf[8] = 0xe0 | (pcrpid >> 8);
  buf[9] =         pcrpid;

  buf[10] = 0xf0; /* Program info length */
  buf[11] = 0x00; /* We dont have any such things atm */

  buf += 12;
  tlen = 12;

  for(i = 0; i < ss->ss_num_components; i++) {
    const streaming_start_component_t *ssc = &ss->ss_components[i];

    dlen = 0;

    switch(ssc->ssc_type) {
    case SCT_MPEG2VIDEO:
      c = 0x02;
      break;

    case SCT_MPEG2AUDIO:
      c = 0x04;
      dlen = 6;
      break;

    case SCT_EAC3:
      c = 0x06;
      dlen = 9;
      break;

    case SCT_DVBSUB:
      c = 0x06;
      dlen = 10;
      break;

    case SCT_MP4A:
    case SCT_AAC:
      c = 0x11;
      dlen = 6;
      break;

    case SCT_H264:
      c = 0x1b;
      break;

    case SCT_HEVC:
      c = 0x24;
      break;

    case SCT_AC3:
      c = 0x81;
      dlen = 9;
      break;

    default:
      continue;
    }

    if (tlen + 5 + dlen > maxlen) {
      tvhwarn("pass", "unable to add stream %d %s%s%s (PID %i) - no space",
              ssc->ssc_index, streaming_component_type2txt(ssc->ssc_type),
              ssc->ssc_lang[0] ? " " : "", ssc->ssc_lang, ssc->ssc_pid);
      continue;
    }

    buf[0] = c;
    buf[1] = 0xe0 | (ssc->ssc_pid >> 8);
    buf[2] =         ssc->ssc_pid;

    buf1 = &buf[3];
    tlen += 5;
    buf  += 5;

    switch(ssc->ssc_type) {
    case SCT_MPEG2AUDIO:
    case SCT_MP4A:
    case SCT_AAC:
      buf[0] = DVB_DESC_LANGUAGE;
      buf[1] = 4;
      memcpy(&buf[2],ssc->ssc_lang,3);
      buf[5] = 0; /* Main audio */
      break;
    case SCT_DVBSUB:
      buf[0] = DVB_DESC_SUBTITLE;
      buf[1] = 8;
      memcpy(&buf[2],ssc->ssc_lang,3);
      buf[5] = 16; /* Subtitling type */
      buf[6] = ssc->ssc_composition_id >> 8; 
      buf[7] = ssc->ssc_composition_id;
      buf[8] = ssc->ssc_ancillary_id >> 8; 
      buf[9] = ssc->ssc_ancillary_id;
      break;
    case SCT_AC3:
      buf[0] = DVB_DESC_LANGUAGE;
      buf[1] = 4;
      memcpy(&buf[2],ssc->ssc_lang,3);
      buf[5] = 0; /* Main audio */
      buf[6] = DVB_DESC_AC3;
      buf[7] = 1;
      buf[8] = 0; /* XXX: generate real AC3 desc */
      break;
    case SCT_EAC3:
      buf[0] = DVB_DESC_LANGUAGE;
      buf[1] = 4;
      memcpy(&buf[2],ssc->ssc_lang,3);
      buf[5] = 0; /* Main audio */
      buf[6] = DVB_DESC_EAC3;
      buf[7] = 1;
      buf[8] = 0; /* XXX: generate real EAC3 desc */
      break;
    default:
      break;
    }

    tlen += dlen;
    buf  += dlen;

    buf1[0] = 0xf0 | (dlen >> 8);
    buf1[1] =         dlen;
  }

  l = tlen - 3 + 4;

  buf0[1] = 0xb0 | (l >> 8);
  buf0[2] =         l;

  return dvb_table_append_crc32(buf0, tlen, maxlen);
}

/*
 * Rewrite a PAT packet to only include the service included in the transport stream.
 */

static void
pass_muxer_pat_cb(mpegts_psi_table_t *mt, const uint8_t *buf, int len)
{
  pass_muxer_t *pm;
  uint8_t out[16], *ob;
  int ol, l;

  if (buf[0])
    return;

  pm = (pass_muxer_t*)mt->mt_opaque;

  memcpy(out, buf, 5 + 3);

  out[1] = 0x80;
  out[2] = 13; /* section_length (number of bytes after this field, including CRC) */
  out[7] = 0;

  out[8] = (pm->pm_service_id & 0xff00) >> 8;
  out[9] = pm->pm_service_id & 0x00ff;
  out[10] = 0xe0 | ((pm->pm_pmt_pid & 0x1f00) >> 8);
  out[11] = pm->pm_pmt_pid & 0x00ff;

  ol = dvb_table_append_crc32(out, 12, sizeof(out));

  if (ol > 0 && (l = dvb_table_remux(mt, out, ol, &ob)) > 0) {
    pass_muxer_write((muxer_t *)pm, ob, l);
    free(ob);
  }
}

/*
 *
 */
static void
pass_muxer_sdt_cb(mpegts_psi_table_t *mt, const uint8_t *buf, int len)
{
  pass_muxer_t *pm;
  uint8_t out[1024], *ob;
  uint16_t sid;
  int l, ol;

  /* filter out the other transponders */
  if (buf[0] != 0x42)
    return;

  pm = (pass_muxer_t*)mt->mt_opaque;
  ol   = 8 + 3;
  memcpy(out, buf, ol);
  buf += ol;
  len -= ol;
  while (len >= 5) {
    sid = (buf[0] << 8) | buf[1];
    l = (buf[3] & 0x0f) << 8 | buf[4];
    if (sid != pm->pm_service_id) {
      buf += l + 5;
      len -= l + 5;
      continue;
    }
    if (sizeof(out) < ol + l + 5 + 4 /* crc */) {
      tvherror("pass", "SDT entry too long (%i)", l);
      return;
    }
    memcpy(out + ol, buf, l + 5);
    /* set free CA */
    out[ol + 3] = out[ol + 3] & ~0x10;
    ol += l + 5;
    break;
  }

  /* update section length */
  out[1] = (out[1] & 0xf0) | ((ol + 4 - 3) >> 8);
  out[2] = (ol + 4 - 3) & 0xff;

  ol = dvb_table_append_crc32(out, ol, sizeof(out));

  if (ol > 0 && (l = dvb_table_remux(mt, out, ol, &ob)) > 0) {
    pass_muxer_write((muxer_t *)pm, ob, l);
    free(ob);
  }
}

/*
 *
 */
static void
pass_muxer_eit_cb(mpegts_psi_table_t *mt, const uint8_t *buf, int len)
{
  pass_muxer_t *pm;
  uint16_t sid;
  uint8_t *out;
  int olen;

  /* filter out the other transponders */
  if ((buf[0] < 0x50 && buf[0] != 0x4e) || buf[0] > 0x5f || len < 14)
    return;

  pm = (pass_muxer_t*)mt->mt_opaque;
  sid = (buf[3] << 8) | buf[4];
  if (sid != pm->pm_service_id)
    return;

  /* TODO: set free_CA_mode bit to zero */

  len = dvb_table_append_crc32((uint8_t *)buf, len, len + 4);

  if (len > 0 && (olen = dvb_table_remux(mt, buf, len, &out)) > 0) {
    pass_muxer_write((muxer_t *)pm, out, olen);
    free(out);
  }
}

/**
 * Figure out the mime-type for the muxed data stream
 */
static const char*
pass_muxer_mime(muxer_t* m, const struct streaming_start *ss)
{
  int i;
  int has_audio;
  int has_video;
  muxer_container_type_t mc;
  const streaming_start_component_t *ssc;
  const source_info_t *si = &ss->ss_si;

  has_audio = 0;
  has_video = 0;

  for(i=0; i < ss->ss_num_components; i++) {
    ssc = &ss->ss_components[i];

    if(ssc->ssc_disabled)
      continue;

    has_video |= SCT_ISVIDEO(ssc->ssc_type);
    has_audio |= SCT_ISAUDIO(ssc->ssc_type);
  }

  if(si->si_type == S_MPEG_TS)
    mc = MC_MPEGTS;
  else if(si->si_type == S_MPEG_PS)
    mc = MC_MPEGPS;
  else
    mc = MC_UNKNOWN;

  if(has_video)
    return muxer_container_type2mime(mc, 1);
  else if(has_audio)
    return muxer_container_type2mime(mc, 0);
  else
    return muxer_container_type2mime(MC_UNKNOWN, 0);
}


/**
 * Generate the pmt and pat from a streaming start message
 */
static int
pass_muxer_reconfigure(muxer_t* m, const struct streaming_start *ss)
{
  pass_muxer_t *pm = (pass_muxer_t*)m;
  pm->pm_pmt_pid = ss->ss_pmt_pid;
  pm->pm_service_id = ss->ss_service_id;

  if (pm->m_config.m_rewrite_pmt) {
    pm->pm_pmt = realloc(pm->pm_pmt, 188);
    memset(pm->pm_pmt, 0xff, 188);
    pm->pm_pmt[0] = 0x47;
    pm->pm_pmt[1] = 0x40 | (ss->ss_pmt_pid >> 8);
    pm->pm_pmt[2] = 0x00 | (ss->ss_pmt_pid >> 0);
    pm->pm_pmt[3] = 0x10;
    pm->pm_pmt[4] = 0x00;
    if(pass_muxer_build_pmt(ss, pm->pm_pmt+5, 183, pm->pm_pmt_version,
			    ss->ss_pcr_pid) < 0) {
      pm->m_errors++;
      tvhlog(LOG_ERR, "pass", "%s: Unable to build pmt", pm->pm_filename);
      return -1;
    }
    pm->pm_pmt_version++;
  }

  return 0;
}


/**
 * Init the passthrough muxer with streams
 */
static int
pass_muxer_init(muxer_t* m, const struct streaming_start *ss, const char *name)
{
  return pass_muxer_reconfigure(m, ss);
}


/**
 * Open the muxer as a stream muxer (using a non-seekable socket)
 */
static int
pass_muxer_open_stream(muxer_t *m, int fd)
{
  pass_muxer_t *pm = (pass_muxer_t*)m;

  pm->pm_off      = 0;
  pm->pm_fd       = fd;
  pm->pm_seekable = 0;
  pm->pm_filename = strdup("Live stream");

  return 0;
}


/**
 * Open the file and set the file descriptor
 */
static int
pass_muxer_open_file(muxer_t *m, const char *filename)
{
  int fd;
  pass_muxer_t *pm = (pass_muxer_t*)m;

  tvhtrace("pass", "Creating file \"%s\" with file permissions \"%o\"", filename, pm->m_config.m_file_permissions);
 
  fd = open(filename, O_WRONLY | O_CREAT | O_TRUNC, pm->m_config.m_file_permissions);

  if(fd < 0) {
    pm->pm_error = errno;
    tvhlog(LOG_ERR, "pass", "%s: Unable to create file, open failed -- %s",
	   filename, strerror(errno));
    pm->m_errors++;
    return -1;
  }

  pm->pm_off      = 0;
  pm->pm_seekable = 1;
  pm->pm_fd       = fd;
  pm->pm_filename = strdup(filename);
  return 0;
}


/**
 * Write data to the file descriptor
 */
static void
pass_muxer_write(muxer_t *m, const void *data, size_t size)
{
  pass_muxer_t *pm = (pass_muxer_t*)m;

  if(pm->pm_error) {
    pm->m_errors++;
  } else if(tvh_write(pm->pm_fd, data, size)) {
    pm->pm_error = errno;
    if (!MC_IS_EOS_ERROR(errno))
      tvhlog(LOG_ERR, "pass", "%s: Write failed -- %s", pm->pm_filename,
	     strerror(errno));
    else
      /* this is an end-of-streaming notification */
      m->m_eos = 1;
    m->m_errors++;
    muxer_cache_update(m, pm->pm_fd, pm->pm_off, 0);
    pm->pm_off = lseek(pm->pm_fd, 0, SEEK_CUR);
  } else {
    muxer_cache_update(m, pm->pm_fd, pm->pm_off, 0);
    pm->pm_off += size;
  }
}


/**
 * Write TS packets to the file descriptor
 */
static void
pass_muxer_write_ts(muxer_t *m, pktbuf_t *pb)
{
  pass_muxer_t *pm = (pass_muxer_t*)m;
  int l, pid;
  uint8_t *tsb, *tsb2, *pkt = pb->pb_data;
  size_t  len = pb->pb_size, len2;
  
  /* Rewrite PAT/PMT in operation */
  if (pm->m_config.m_rewrite_pat || pm->m_config.m_rewrite_pmt ||
      pm->m_config.m_rewrite_sdt || pm->m_config.m_rewrite_eit) {

    for (tsb = pb->pb_data, len2 = pb->pb_size, len = 0;
         len2 > 0; tsb += l, len2 -= l) {

      pid = (tsb[1] & 0x1f) << 8 | tsb[2];
      l = mpegts_word_count(tsb, len2, 0x001FFF00);

      /* Process */
      if ( (pm->m_config.m_rewrite_pat && pid == DVB_PAT_PID) ||
           (pm->m_config.m_rewrite_pmt && pid == pm->pm_pmt_pid) ||
           (pm->m_config.m_rewrite_sdt && pid == DVB_SDT_PID) ||
           (pm->m_config.m_rewrite_eit && pid == DVB_EIT_PID) ) {

        /* Flush */
        if (len)
          pass_muxer_write(m, pkt, len);

        /* Store new start point (after these packets) */
        pkt = tsb + l;
        len = 0;

        /* PAT */
        if (pid == DVB_PAT_PID) {

          dvb_table_parse(&pm->pm_pat, tsb, l, 1, 0, pass_muxer_pat_cb);

        /* SDT */
        } else if (pid == DVB_SDT_PID) {
        
          dvb_table_parse(&pm->pm_sdt, tsb, l, 1, 0, pass_muxer_sdt_cb);

        /* EIT */
        } else if (pid == DVB_EIT_PID) {
        
          dvb_table_parse(&pm->pm_eit, tsb, l, 1, 0, pass_muxer_eit_cb);

        /* PMT */
        } else {

          for (tsb2 = tsb; tsb2 < pkt; tsb2 += 188)
            if (tsb2[1] & 0x40) { /* pusi - the first PMT packet */
              pm->pm_pmt[3] = (pm->pm_pmt[3] & 0xf0) | pm->pm_pmt_cc;
              pm->pm_pmt_cc = (pm->pm_pmt_cc + 1) & 0xf;
              pass_muxer_write(m, pm->pm_pmt, 188);
            }

        }

      /* Record */
      } else {
        len += l;
      }
    }
  }

  if (len)
    pass_muxer_write(m, pkt, len);
}


/**
 * Write a packet directly to the file descriptor
 */
static int
pass_muxer_write_pkt(muxer_t *m, streaming_message_type_t smt, void *data)
{
  pktbuf_t *pb = (pktbuf_t*)data;
  pass_muxer_t *pm = (pass_muxer_t*)m;

  assert(smt == SMT_MPEGTS);

  switch(smt) {
  case SMT_MPEGTS:
    pass_muxer_write_ts(m, pb);
    break;
  default:
    //TODO: add support for v4l (MPEG-PS)
    break;
  }

  pktbuf_ref_dec(pb);

  return pm->pm_error;
}


/**
 * NOP
 */
static int
pass_muxer_write_meta(muxer_t *m, struct epg_broadcast *eb, const char *comment)
{
  return 0;
}


/**
 * Close the file descriptor
 */
static int
pass_muxer_close(muxer_t *m)
{
  pass_muxer_t *pm = (pass_muxer_t*)m;

  if(pm->pm_seekable && close(pm->pm_fd)) {
    pm->pm_error = errno;
    tvhlog(LOG_ERR, "pass", "%s: Unable to close file, close failed -- %s",
	   pm->pm_filename, strerror(errno));
    pm->m_errors++;
    return -1;
  }

  return 0;
}


/**
 * Free all memory associated with the muxer
 */
static void
pass_muxer_destroy(muxer_t *m)
{
  pass_muxer_t *pm = (pass_muxer_t*)m;

  if(pm->pm_filename)
    free(pm->pm_filename);

  if(pm->pm_pmt)
    free(pm->pm_pmt);

  dvb_table_parse_done(&pm->pm_pat);
  dvb_table_parse_done(&pm->pm_sdt);
  dvb_table_parse_done(&pm->pm_eit);

  free(pm);
}


/**
 * Create a new passthrough muxer
 */
muxer_t*
pass_muxer_create(const muxer_config_t *m_cfg)
{
  pass_muxer_t *pm;

  if(m_cfg->m_type != MC_PASS && m_cfg->m_type != MC_RAW)
    return NULL;

  pm = calloc(1, sizeof(pass_muxer_t));
  pm->m_open_stream  = pass_muxer_open_stream;
  pm->m_open_file    = pass_muxer_open_file;
  pm->m_init         = pass_muxer_init;
  pm->m_reconfigure  = pass_muxer_reconfigure;
  pm->m_mime         = pass_muxer_mime;
  pm->m_write_meta   = pass_muxer_write_meta;
  pm->m_write_pkt    = pass_muxer_write_pkt;
  pm->m_close        = pass_muxer_close;
  pm->m_destroy      = pass_muxer_destroy;
  pm->pm_fd          = -1;

  dvb_table_parse_init(&pm->pm_pat, "pass-pat", DVB_PAT_PID, pm);
  dvb_table_parse_init(&pm->pm_sdt, "pass-sdt", DVB_SDT_PID, pm);
  dvb_table_parse_init(&pm->pm_eit, "pass-eit", DVB_EIT_PID, pm);

  return (muxer_t *)pm;
}

