/*
** Copyright (C) 2011-2012 Erik de Castro Lopo <erikd@mega-nerd.com>
**
** This program is free software; you can redistribute it and/or modify
** it under the terms of the GNU Lesser General Public License as published by
** the Free Software Foundation; either version 2.1 of the License, or
** (at your option) any later version.
**
** This program is distributed in the hope that it will be useful,
** but WITHOUT ANY WARRANTY; without even the implied warranty of
** MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
** GNU Lesser General Public License for more details.
**
** You should have received a copy of the GNU Lesser General Public License
** along with this program; if not, write to the Free Software
** Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
*/

#include	"sfconfig.h"

#include	<stdio.h>
#include	<stdlib.h>
#include	<string.h>
#include	<math.h>

#include	"sndfile.h"
#include	"sfendian.h"
#include	"common.h"
#include	"ALAC/alac_codec.h"
#include	"ALAC/ALACBitUtilities.h"

#define		ALAC_MAX_FRAME_SIZE		32800

#define		ALAC_PACKET_MARKER		0xa1acc0d3

#define		kuki_MARKER				MAKE_MARKER ('k', 'u', 'k', 'i')
#define		pakt_MARKER				MAKE_MARKER ('p', 'a', 'k', 't')

typedef struct
{	uint32_t	current, count, allocated ;
	uint32_t	packet_size [] ;
} PAKT_INFO ;

typedef struct ALAC_PRIVATE_tag
{	PAKT_INFO	* pakt_info ;

	sf_count_t	input_data_pos ;

	int			channels, final_write_block ;

	uint32_t	frames_this_block, partial_block_frames, frames_per_block, kuki_size ;
	uint32_t	bits_per_sample, decode_bytes_per_frame_is_this_needed ;


	/* Can't have a decoder and an encoder at the same time so stick
	** them in an un-named union.
	*/
	union
	{	ALAC_DECODER decoder ;
		ALAC_ENCODER encoder ;
	} ;

	char enctmpname [256] ;
	FILE *enctmp ;

	int	buffer	[ALAC_MAX_FRAME_SIZE / 4] ;

} ALAC_PRIVATE ;

/*============================================================================================
*/

static int alac_reader_init (SF_PRIVATE *psf, const ALAC_DECODER_INFO * info) ;
static int alac_writer_init (SF_PRIVATE *psf) ;

static sf_count_t alac_reader_calc_frames (SF_PRIVATE *psf, ALAC_PRIVATE *plac) ;

static sf_count_t alac_read_s (SF_PRIVATE *psf, short *ptr, sf_count_t len) ;
static sf_count_t alac_read_i (SF_PRIVATE *psf, int *ptr, sf_count_t len) ;
static sf_count_t alac_read_f (SF_PRIVATE *psf, float *ptr, sf_count_t len) ;
static sf_count_t alac_read_d (SF_PRIVATE *psf, double *ptr, sf_count_t len) ;

static sf_count_t alac_write_s (SF_PRIVATE *psf, const short *ptr, sf_count_t len) ;
static sf_count_t alac_write_i (SF_PRIVATE *psf, const int *ptr, sf_count_t len) ;
static sf_count_t alac_write_f (SF_PRIVATE *psf, const float *ptr, sf_count_t len) ;
static sf_count_t alac_write_d (SF_PRIVATE *psf, const double *ptr, sf_count_t len) ;

static sf_count_t	alac_seek	(SF_PRIVATE *psf, int mode, sf_count_t offset) ;

static int	alac_close		(SF_PRIVATE *psf) ;
static int	alac_byterate	(SF_PRIVATE *psf) ;

static int alac_decode_block (SF_PRIVATE *psf, ALAC_PRIVATE *plac) ;
static int alac_encode_block (SF_PRIVATE *psf, ALAC_PRIVATE *plac) ;

static uint32_t alac_kuki_read (SF_PRIVATE * psf, uint32_t kuki_offset, uint8_t * kuki, size_t kuki_maxlen) ;

static PAKT_INFO * alac_pakt_alloc (uint32_t initial_count) ;
static PAKT_INFO * alac_pakt_read_decode (SF_PRIVATE * psf, uint32_t pakt_offset) ;
static PAKT_INFO * alac_pakt_append (PAKT_INFO * info, uint32_t value) ;
static uint8_t * alac_pakt_encode (const PAKT_INFO *info, uint32_t * pakt_size) ;
static sf_count_t alac_pakt_block_offset (const PAKT_INFO *info, uint32_t block) ;

/*============================================================================================
** ALAC Reader initialisation function.
*/

int
alac_init (SF_PRIVATE *psf, const ALAC_DECODER_INFO * info)
{	int error ;

	if ((psf->codec_data = calloc (1, sizeof (ALAC_PRIVATE))) == NULL)
		return SFE_MALLOC_FAILED ;

	switch (psf->file.mode)
	{	case SFM_RDWR :
			return SFE_BAD_MODE_RW ;

		case SFM_READ :
			if ((error = alac_reader_init (psf, info)))
				return error ;
			break ;

		case SFM_WRITE :
			if ((error = alac_writer_init (psf)))
				return error ;
			break ;

		default :
			psf_log_printf (psf, "%s : Bad psf->file.mode.\n", __func__) ;
			return SFE_INTERNAL ;
		} ;

	psf->codec_close = alac_close ;
	psf->byterate = alac_byterate ;

	return 0 ;
} /* aiff_alac_init */

void
alac_get_desc_chunk_items (int subformat, uint32_t *fmt_flags, uint32_t *frames_per_packet)
{	switch (subformat)
	{	case SF_FORMAT_ALAC_16 :
			*fmt_flags = 1 ;
			break ;
		case SF_FORMAT_ALAC_20 :
			*fmt_flags = 2 ;
			break ;
		case SF_FORMAT_ALAC_24 :
			*fmt_flags = 3 ;
			break ;
		case SF_FORMAT_ALAC_32 :
			*fmt_flags = 4 ;
			break ;
		default :
			break ;
		} ;
	*frames_per_packet = ALAC_FRAME_LENGTH ;
} /* alac_get_desc_chunk_items */

static int
alac_close	(SF_PRIVATE *psf)
{	ALAC_PRIVATE *plac ;
	BUF_UNION	ubuf ;

	plac = psf->codec_data ;

	if (psf->file.mode == SFM_WRITE)
	{	ALAC_ENCODER *penc = &plac->encoder ;
		SF_CHUNK_INFO chunk_info ;
		sf_count_t readcount ;
		uint8_t *kuki_data [plac->kuki_size] ;
		uint32_t pakt_size ;

		plac->final_write_block = 1 ;

		/*	If a block has been partially assembled, write it out as the final block. */
		if (plac->partial_block_frames && plac->partial_block_frames < plac->frames_per_block)
			alac_encode_block (psf, plac) ;

		alac_get_magic_cookie (penc, kuki_data, &plac->kuki_size) ;

		memset (&chunk_info, 0, sizeof (chunk_info)) ;
		chunk_info.id_size = snprintf (chunk_info.id, sizeof (chunk_info.id), "kuki") ;
		chunk_info.data = kuki_data ;
		chunk_info.datalen = plac->kuki_size ;
		psf_save_write_chunk (&psf->wchunks, &chunk_info) ;

		memset (&chunk_info, 0, sizeof (chunk_info)) ;
		chunk_info.id_size = snprintf (chunk_info.id, sizeof (chunk_info.id), "pakt") ;
		chunk_info.data = alac_pakt_encode (plac->pakt_info, &pakt_size) ;
		chunk_info.datalen = pakt_size ;
		psf_save_write_chunk (&psf->wchunks, &chunk_info) ;

		psf->write_header (psf, 1) ;

		fseek (plac->enctmp, 0, SEEK_SET) ;
		while ((readcount = fread (ubuf.ucbuf, 1, sizeof (ubuf.ucbuf), plac->enctmp)) > 0)
			psf_fwrite (ubuf.ucbuf, 1, readcount, psf) ;
		fclose (plac->enctmp) ;
		remove (plac->enctmpname) ;
		} ;

	if (plac->pakt_info)
		free (plac->pakt_info) ;
	plac->pakt_info = NULL ;

	return 0 ;
} /* alac_close */

static int
alac_byterate	(SF_PRIVATE *psf)
{
	if (psf->file.mode == SFM_READ)
		return (psf->datalength * psf->sf.samplerate) / psf->sf.frames ;

	return -1 ;
} /* alac_byterate */

/*============================================================================================
** ALAC initialisation Functions.
*/

static int
alac_reader_init (SF_PRIVATE *psf, const ALAC_DECODER_INFO * info)
{	ALAC_PRIVATE	*plac ;
	uint32_t		kuki_size ;
	uint8_t			kuki [512] ;

	if (info == NULL)
	{	psf_log_printf (psf, "%s : ALAC_DECODER_INFO is NULL.\n", __func__) ;
		return SFE_INTERNAL ;
		} ;

	plac = psf->codec_data ;

	plac->channels			= psf->sf.channels ;
	plac->frames_per_block	= info->frames_per_packet ;
	plac->bits_per_sample	= info->bits_per_sample ;
	plac->decode_bytes_per_frame_is_this_needed = info->bits_per_sample / 8 * psf->sf.channels ;

	if (plac->pakt_info != NULL)
		free (plac->pakt_info) ;
	plac->pakt_info = alac_pakt_read_decode (psf, info->pakt_offset) ;


	if (plac->pakt_info == NULL)
	{	psf_log_printf (psf, "%s : alac_pkt_read() returns NULL.\n", __func__) ;
		return SFE_MALLOC_FAILED ;
		} ;

	/* Read in the ALAC cookie data and pass it to the init function. */
	kuki_size = alac_kuki_read (psf, info->kuki_offset, kuki, sizeof (kuki)) ;

	alac_decoder_init (&plac->decoder, kuki, kuki_size) ;

	switch (info->bits_per_sample)
	{	case 16 :
		case 20 :
		case 24 :
		case 32 :
			psf->read_short		= alac_read_s ;
			psf->read_int		= alac_read_i ;
			psf->read_float		= alac_read_f ;
			psf->read_double	= alac_read_d ;
			break ;

		default :
			printf ("%s : info->bits_per_sample %u\n", __func__, info->bits_per_sample) ;
			return SFE_UNSUPPORTED_ENCODING ;
		} ;

	psf->codec_close	= alac_close ;
	psf->seek			= alac_seek ;

	psf->sf.frames		= alac_reader_calc_frames (psf, plac) ;
	alac_seek (psf, SFM_READ, 0) ;

	return 0 ;
} /* alac_reader_init */

static int
alac_writer_init (SF_PRIVATE *psf)
{	const char		*tmpdir = NULL ;
	ALAC_PRIVATE	*plac ;
	uint32_t		alac_format_flags = 0 ;

	if (psf->file.mode != SFM_WRITE)
		return SFE_BAD_MODE_RW ;

	if ((plac = calloc (1, sizeof (ALAC_PRIVATE))) == NULL)
		return SFE_MALLOC_FAILED ;

	psf->codec_data = plac ;

	plac->channels	= psf->sf.channels ;
	plac->kuki_size = alac_get_magic_cookie_size (psf->sf.channels) ;

	psf->write_short	= alac_write_s ;
	psf->write_int		= alac_write_i ;
	psf->write_float	= alac_write_f ;
	psf->write_double	= alac_write_d ;

	switch (SF_CODEC (psf->sf.format))
	{	case SF_FORMAT_ALAC_16 :
			alac_format_flags	= 1 ;
			plac->bits_per_sample = 16 ;
			break ;

		case SF_FORMAT_ALAC_20 :
			alac_format_flags	= 2 ;
			plac->bits_per_sample = 20 ;
			break ;

		case SF_FORMAT_ALAC_24 :
			alac_format_flags	= 3 ;
			plac->bits_per_sample = 24 ;
			break ;

		case SF_FORMAT_ALAC_32 :
			alac_format_flags	= 4 ;
			plac->bits_per_sample = 32 ;
			break ;

		default :
			psf_log_printf (psf, "%s : Can't figure out bits per sample.\n", __func__) ;
			return SFE_UNIMPLEMENTED ;
		} ;

	plac->frames_per_block = ALAC_FRAME_LENGTH ;

	plac->pakt_info = alac_pakt_alloc (2000) ;

	if (OS_IS_WIN32)
		tmpdir = getenv ("TEMP") ;
	else
	{	tmpdir = getenv ("TMPDIR") ;
		tmpdir = tmpdir == NULL ? "/tmp" : tmpdir ;
		} ;

	if (tmpdir == NULL)
		return SFE_NO_TEMP_DIR ;

	snprintf (plac->enctmpname, sizeof (plac->enctmpname), "%s/%x%x.alac", tmpdir, psf_rand_int32 (), psf_rand_int32 ()) ;
	if ((plac->enctmp = fopen (plac->enctmpname, "wb+")) == NULL)
		return SFE_ALAC_FAIL_TMPFILE ;

	alac_encoder_init (&plac->encoder, psf->sf.samplerate, psf->sf.channels, alac_format_flags, ALAC_FRAME_LENGTH) ;

	return 0 ;
} /* alac_writer_init */

/*============================================================================================
** ALAC block decoder and encoder.
*/

static inline uint32_t
alac_reader_next_packet_size (PAKT_INFO * info)
{	if (info->current >= info->count)
		return 0 ;
	return info->packet_size [info->current++] ;
} /* alac_reader_next_packet_size */

static sf_count_t
alac_reader_calc_frames (SF_PRIVATE *psf, ALAC_PRIVATE *plac)
{	sf_count_t	frames = 0 ;
	uint32_t	current_pos = 1, blocks = 0 ;

	plac->pakt_info->current = 0 ;

	while (current_pos < psf->filelength && current_pos > 0)
	{	current_pos = alac_reader_next_packet_size (plac->pakt_info) ;
		blocks = current_pos > 0 ? blocks + 1 : blocks ;
		} ;

	/* Only count full blocks. */
	frames = plac->frames_per_block * (blocks - 1) ;

	alac_seek (psf, SFM_READ, frames) ;
	alac_decode_block (psf, plac) ;
	frames += plac->frames_this_block ;

	plac->pakt_info->current = 0 ;

	return frames ;
} /* alac_reader_calc_frames */

static int
alac_decode_block (SF_PRIVATE *psf, ALAC_PRIVATE *plac)
{	ALAC_DECODER *pdec = &plac->decoder ;
	uint8_t		byte_buffer [ALAC_MAX_FRAME_SIZE] ;
	uint32_t	packet_size ;
	BitBuffer	bit_buffer ;

	packet_size = alac_reader_next_packet_size (plac->pakt_info) ;
	if (packet_size == 0)
	{	if (plac->pakt_info->current < plac->pakt_info->count)
			psf_log_printf (psf, "packet_size is 0 (%d of %d)\n", plac->pakt_info->current, plac->pakt_info->count) ;
		return 0 ;
		} ;

	psf_fseek (psf, plac->input_data_pos, SEEK_SET) ;

	if (packet_size > SIGNED_SIZEOF (byte_buffer))
	{	psf_log_printf (psf, "%s : bad packet_size (%zd)\n", __func__, packet_size) ;
		return 0 ;
		} ;

	if ((packet_size != psf_fread (byte_buffer, 1, packet_size, psf)))
		return 0 ;

	BitBufferInit (&bit_buffer, byte_buffer, packet_size) ;

	plac->input_data_pos += packet_size ;
	plac->frames_this_block = 0 ;
	alac_decode (pdec, &bit_buffer, plac->buffer, plac->frames_per_block, psf->sf.channels, &plac->frames_this_block) ;

	plac->partial_block_frames = 0 ;

	return 1 ;
} /* alac_decode_block */


static int
alac_encode_block (SF_PRIVATE * UNUSED (psf), ALAC_PRIVATE *plac)
{	ALAC_ENCODER *penc = &plac->encoder ;
	uint8_t	byte_buffer [ALAC_MAX_FRAME_SIZE] ;
	int32_t num_bytes = 0 ;

// printf ("%s : raw_bytes %d -> ", __func__, plac->partial_block_frames * plac->bits_per_sample / 8) ;
	alac_encode (penc, plac->channels, plac->partial_block_frames, plac->buffer, byte_buffer, &num_bytes) ;
// printf ("encoded_bytes %d\n", num_bytes) ;

	fwrite (byte_buffer, 1, num_bytes, plac->enctmp) ;
	alac_pakt_append (plac->pakt_info, num_bytes) ;

	plac->partial_block_frames = 0 ;

	return 1 ;
} /* alac_encode_block */



/*============================================================================================
** ALAC read functions.
*/

static sf_count_t
alac_read_s (SF_PRIVATE *psf, short *ptr, sf_count_t len)
{	ALAC_PRIVATE *plac ;
	int			*iptr ;
	int			k, readcount ;
	sf_count_t	total = 0 ;

	if ((plac = (ALAC_PRIVATE*) psf->codec_data) == NULL)
		return 0 ;

	while (len > 0)
	{	if (plac->partial_block_frames >= plac->frames_this_block && alac_decode_block (psf, plac) == 0)
			break ;

		readcount = (plac->frames_this_block - plac->partial_block_frames) * plac->channels ;
		readcount = readcount > len ? len : readcount ;

		iptr = plac->buffer + plac->partial_block_frames * plac->channels ;

		for (k = 0 ; k < readcount ; k++)
			ptr [total + k] = iptr [k] >> 16 ;

		plac->partial_block_frames += readcount / plac->channels ;
		total += readcount ;
		len -= readcount ;
		} ;

	return total ;
} /* alac_read_s */

static sf_count_t
alac_read_i (SF_PRIVATE *psf, int *ptr, sf_count_t len)
{	ALAC_PRIVATE *plac ;
	int			*iptr ;
	int			k, readcount ;
	sf_count_t	total = 0 ;

	if ((plac = (ALAC_PRIVATE*) psf->codec_data) == NULL)
		return 0 ;

	while (len > 0)
	{	if (plac->partial_block_frames >= plac->frames_this_block && alac_decode_block (psf, plac) == 0)
			break ;

		readcount = (plac->frames_this_block - plac->partial_block_frames) * plac->channels ;
		readcount = readcount > len ? len : readcount ;

		iptr = plac->buffer + plac->partial_block_frames * plac->channels ;

		for (k = 0 ; k < readcount ; k++)
			ptr [total + k] = iptr [k] ;

		plac->partial_block_frames += readcount / plac->channels ;
		total += readcount ;
		len -= readcount ;
		} ;

	return total ;
} /* alac_read_i */

static sf_count_t
alac_read_f (SF_PRIVATE *psf, float *ptr, sf_count_t len)
{	ALAC_PRIVATE *plac ;
	int			*iptr ;
	int			k, readcount ;
	sf_count_t	total = 0 ;
	float		normfact ;

	if ((plac = (ALAC_PRIVATE*) psf->codec_data) == NULL)
		return 0 ;

	normfact = (psf->norm_float == SF_TRUE) ? 1.0 / ((float) 0x80000000) : 1.0 ;

	while (len > 0)
	{	if (plac->partial_block_frames >= plac->frames_this_block && alac_decode_block (psf, plac) == 0)
			break ;

		readcount = (plac->frames_this_block - plac->partial_block_frames) * plac->channels ;
		readcount = readcount > len ? len : readcount ;

		iptr = plac->buffer + plac->partial_block_frames * plac->channels ;

		for (k = 0 ; k < readcount ; k++)
			ptr [total + k] = normfact * iptr [k] ;

		plac->partial_block_frames += readcount / plac->channels ;
		total += readcount ;
		len -= readcount ;
		} ;

	return total ;
} /* alac_read_f */

static sf_count_t
alac_read_d (SF_PRIVATE *psf, double *ptr, sf_count_t len)
{	ALAC_PRIVATE *plac ;
	int			*iptr ;
	int			k, readcount ;
	sf_count_t	total = 0 ;
	double		normfact ;

	if ((plac = (ALAC_PRIVATE*) psf->codec_data) == NULL)
		return 0 ;

	normfact = (psf->norm_double == SF_TRUE) ? 1.0 / ((float) 0x80000000) : 1.0 ;

	while (len > 0)
	{	if (plac->partial_block_frames >= plac->frames_this_block && alac_decode_block (psf, plac) == 0)
			break ;

		readcount = (plac->frames_this_block - plac->partial_block_frames) * plac->channels ;
		readcount = readcount > len ? len : readcount ;

		iptr = plac->buffer + plac->partial_block_frames * plac->channels ;

		for (k = 0 ; k < readcount ; k++)
			ptr [total + k] = normfact * iptr [k] ;

		plac->partial_block_frames += readcount / plac->channels ;
		total += readcount ;
		len -= readcount ;
		} ;

	return total ;
} /* alac_read_d */

/*============================================================================================
*/

static sf_count_t
alac_seek (SF_PRIVATE *psf, int mode, sf_count_t offset)
{	ALAC_PRIVATE *plac ;
	int			newblock, newsample ;

	if (! psf->codec_data)
		return 0 ;
	plac = (ALAC_PRIVATE*) psf->codec_data ;

	if (psf->datalength < 0 || psf->dataoffset < 0)
	{	psf->error = SFE_BAD_SEEK ;
		return PSF_SEEK_ERROR ;
		} ;

	if (offset == 0)
	{	psf_fseek (psf, psf->dataoffset, SEEK_SET) ;

		plac->frames_this_block = 0 ;
		plac->input_data_pos = psf->dataoffset ;
		plac->pakt_info->current = 0 ;
		return 0 ;
		} ;

	if (offset < 0 || offset > plac->pakt_info->count * plac->frames_per_block)
	{	psf->error = SFE_BAD_SEEK ;
		return	PSF_SEEK_ERROR ;
		} ;

	newblock	= offset / plac->frames_per_block ;
	newsample	= offset % plac->frames_per_block ;

	if (mode == SFM_READ)
	{	plac->input_data_pos = psf->dataoffset + alac_pakt_block_offset (plac->pakt_info, newblock) ;

		plac->pakt_info->current = newblock ;
		alac_decode_block (psf, plac) ;
		plac->partial_block_frames = newsample ;
		}
	else
	{	/* What to do about write??? */
		psf->error = SFE_BAD_SEEK ;
		return	PSF_SEEK_ERROR ;
		} ;

	return newblock * plac->frames_per_block + newsample ;
} /* alac_seek */

/*==========================================================================================
** ALAC Write Functions.
*/

static sf_count_t
alac_write_s (SF_PRIVATE *psf, const short *ptr, sf_count_t len)
{	ALAC_PRIVATE *plac ;
	int			*iptr ;
	int			k, writecount ;
	sf_count_t	total = 0 ;

	if ((plac = (ALAC_PRIVATE*) psf->codec_data) == NULL)
		return 0 ;

	while (len > 0)
	{	writecount = (plac->frames_per_block - plac->partial_block_frames) * plac->channels ;
		writecount = (writecount == 0 || writecount > len) ? len : writecount ;

		iptr = plac->buffer + plac->partial_block_frames * plac->channels ;

		for (k = 0 ; k < writecount ; k++)
			iptr [k] = ptr [total + k] << 16 ;

		plac->partial_block_frames += writecount / plac->channels ;
		total += writecount ;
		len -= writecount ;

		if (plac->partial_block_frames >= plac->frames_per_block)
			alac_encode_block (psf, plac) ;
		} ;

	return total ;
} /* alac_write_s */

static sf_count_t
alac_write_i (SF_PRIVATE *psf, const int *ptr, sf_count_t len)
{	ALAC_PRIVATE *plac ;
	int			*iptr ;
	int			k, writecount ;
	sf_count_t	total = 0 ;

	if ((plac = (ALAC_PRIVATE*) psf->codec_data) == NULL)
		return 0 ;

	while (len > 0)
	{	writecount = (plac->frames_per_block - plac->partial_block_frames) * plac->channels ;
		writecount = (writecount == 0 || writecount > len) ? len : writecount ;

		iptr = plac->buffer + plac->partial_block_frames * plac->channels ;

		for (k = 0 ; k < writecount ; k++)
			iptr [k] = ptr [total + k] ;

		plac->partial_block_frames += writecount / plac->channels ;
		total += writecount ;
		len -= writecount ;

		if (plac->partial_block_frames >= plac->frames_per_block)
			alac_encode_block (psf, plac) ;
		} ;

	return total ;
} /* alac_write_i */

static sf_count_t
alac_write_f (SF_PRIVATE *psf, const float *ptr, sf_count_t len)
{	ALAC_PRIVATE *plac ;
	void		(*convert) (const float *, int *t, int, int) ;
	int			*iptr ;
	int			writecount ;
	sf_count_t	total = 0 ;

	if ((plac = (ALAC_PRIVATE*) psf->codec_data) == NULL)
		return 0 ;

	convert = (psf->add_clipping) ? psf_f2i_clip_array : psf_f2i_array ;

	while (len > 0)
	{	writecount = (plac->frames_per_block - plac->partial_block_frames) * plac->channels ;
		writecount = (writecount == 0 || writecount > len) ? len : writecount ;

		iptr = plac->buffer + plac->partial_block_frames * plac->channels ;

		convert (ptr, iptr, writecount, psf->norm_float) ;

		plac->partial_block_frames += writecount / plac->channels ;
		total += writecount ;
		len -= writecount ;

		if (plac->partial_block_frames >= plac->frames_per_block)
			alac_encode_block (psf, plac) ;
		} ;

	return total ;
} /* alac_write_f */

static sf_count_t
alac_write_d (SF_PRIVATE *psf, const double *ptr, sf_count_t len)
{	ALAC_PRIVATE *plac ;
	void		(*convert) (const double *, int *t, int, int) ;
	int			*iptr ;
	int			writecount ;
	sf_count_t	total = 0 ;

	if ((plac = (ALAC_PRIVATE*) psf->codec_data) == NULL)
		return 0 ;

	convert = (psf->add_clipping) ? psf_d2i_clip_array : psf_d2i_array ;

	while (len > 0)
	{	writecount = (plac->frames_per_block - plac->partial_block_frames) * plac->channels ;
		writecount = (writecount == 0 || writecount > len) ? len : writecount ;

		iptr = plac->buffer + plac->partial_block_frames * plac->channels ;

		convert (ptr, iptr, writecount, psf->norm_float) ;

		plac->partial_block_frames += writecount / plac->channels ;
		total += writecount ;
		len -= writecount ;

		if (plac->partial_block_frames >= plac->frames_per_block)
			alac_encode_block (psf, plac) ;
		} ;

	return total ;
} /* alac_write_d */

/*==============================================================================
** PAKT_INFO handling.
*/

static PAKT_INFO *
alac_pakt_alloc (uint32_t initial_count)
{	PAKT_INFO * info ;

	if ((info = calloc (1, sizeof (PAKT_INFO) + initial_count * sizeof (info->packet_size [0]))) == NULL)
		return NULL ;

	info->allocated = initial_count ;
	info->current = 0 ;
	info->count = 0 ;

	return info ;
} /* alac_pakt_alloc */

static PAKT_INFO *
alac_pakt_append (PAKT_INFO * info, uint32_t value)
{
	if (info->count >= info->allocated)
	{	PAKT_INFO * temp ;
		uint32_t newcount = info->allocated + info->allocated / 2 ;

		if ((temp = realloc (info, sizeof (PAKT_INFO) + newcount * sizeof (info->packet_size [0]))) == NULL)
			return NULL ;

		info = temp ;
		info->allocated = newcount ;
		} ;

	info->packet_size [info->count++] = value ;
	return info ;
} /* alac_pakt_append */

static PAKT_INFO *
alac_pakt_read_decode (SF_PRIVATE * psf, uint32_t UNUSED (pakt_offset))
{	SF_CHUNK_INFO chunk_info ;
	PAKT_INFO * info = NULL ;
	uint8_t *pakt_data = NULL ;
	uint32_t bcount, value = 1, pakt_size ;
	SF_CHUNK_ITERATOR * chunk_iterator ;


	memset (&chunk_info, 0, sizeof (chunk_info)) ;
	snprintf (chunk_info.id, sizeof (chunk_info.id), "pakt") ;
	chunk_info.id_size = 4 ;

	chunk_iterator = psf->create_chunk_iterator (psf, &chunk_info) ;
	if ((chunk_iterator = psf->create_chunk_iterator (psf, &chunk_info)) == NULL)
	{	printf ("%s %d : no chunk iterator found\n\n", __func__, __LINE__) ;
		free (chunk_info.data) ;
		chunk_info.data = NULL ;
		return NULL ;
		} ;

	psf->get_chunk_size (psf, chunk_iterator, &chunk_info) ;

	pakt_size = chunk_info.datalen ;
	chunk_info.data = pakt_data = malloc (pakt_size + 5) ;

	if ((bcount = psf->get_chunk_data (psf, chunk_iterator, &chunk_info)) != SF_ERR_NO_ERROR)
	{	printf ("%s %d : %s\n\n", __func__, __LINE__, sf_error_number (bcount)) ;
		while (chunk_iterator)
			chunk_iterator = psf->next_chunk_iterator (psf, chunk_iterator) ;
		free (chunk_info.data) ;
		chunk_info.data = NULL ;
		return NULL ;
		} ;

	while (chunk_iterator)
		chunk_iterator = psf->next_chunk_iterator (psf, chunk_iterator) ;

	info = alac_pakt_alloc (pakt_size / 4) ;

	/* Start at 24 bytes in. The chunks have some unknown leading cruft. */
	for (bcount = 24 ; bcount < pakt_size && value != 0 ; )
	{	uint8_t byte ;
		int32_t count = 0 ;

		value = 0 ;
		do
		{	byte = pakt_data [bcount + count] ;
			value = (value << 7) + (byte & 0x7F) ;

			count ++ ;
			if (count > 5 || bcount + count > pakt_size)
			{	printf ("%s %d : Ooops! count %d    bcount %d\n", __func__, __LINE__, count, bcount) ;
				value = 0 ;
				break ;
				} ;
			}
			while (byte & 0x80) ;

		bcount += count ;

		if ((info = alac_pakt_append (info, value)) == NULL)
			goto FreeExit ;
		} ;

	free (pakt_data) ;

	return info ;

FreeExit :
	free (pakt_data) ;
	free (info) ;
	return NULL ;
} /* alac_pakt_read_decode */

static uint8_t *
alac_pakt_encode (const PAKT_INFO *info, uint32_t * pakt_size_out)
{	uint8_t	*data ;
	uint32_t k, allocated, pakt_size ;

	allocated = 100 + 2 * info->count ;
	data = calloc (1, allocated) ;

	/* 24 bytes of unknown data at start of 'pakt'. */
	pakt_size = 24 ;

	for (k = 0 ; k < info->count ; k++)
	{	int32_t value = info->packet_size [k] ;

		if ((value & 0x7f) == value)
		{	data [pakt_size++] = value ;
			continue ;
			} ;

		if ((value & 0x3fff) == value)
		{	data [pakt_size++] = (value >> 7) | 0x80 ;
			data [pakt_size++] = value & 0x7f ;
			continue ;
			} ;

		if ((value & 0x1fffff) == value)
		{	data [pakt_size++] = (value >> 14) | 0x80 ;
			data [pakt_size++] = ((value >> 7) & 0x7f) | 0x80 ;
			data [pakt_size++] = value & 0x7f ;
			continue ;
		} ;

		if ((value & 0x0fffffff) == value)
		{	data [pakt_size++] = (value >> 21) | 0x80 ;
			data [pakt_size++] = ((value >> 14) & 0x7f) | 0x80 ;
			data [pakt_size++] = ((value >> 7) & 0x7f) | 0x80 ;
			data [pakt_size++] = value & 0x7f ;
			continue ;
			} ;

		*pakt_size_out = 0 ;
		free (data) ;
		return NULL ;
		} ;

	*pakt_size_out = pakt_size ;
	return data ;
} /* alac_pakt_encode */

static sf_count_t
alac_pakt_block_offset (const PAKT_INFO *info, uint32_t block)
{	sf_count_t offset = 0 ;
	uint32_t k ;

	for (k = 0 ; k < block ; k++)
		offset += info->packet_size [k] ;

	return offset ;
} /* alac_pakt_block_offset */

static uint32_t
alac_kuki_read (SF_PRIVATE * psf, uint32_t kuki_offset, uint8_t * kuki, size_t kuki_maxlen)
{	uint32_t marker ;
	uint64_t kuki_size ;

	if (psf_fseek (psf, kuki_offset, SEEK_SET) != kuki_offset)
		return 0 ;

	psf_fread (&marker, 1, sizeof (marker), psf) ;
	if (marker != MAKE_MARKER ('k', 'u', 'k', 'i'))
		return 0 ;

	psf_fread (&kuki_size, 1, sizeof (kuki_size), psf) ;
	kuki_size = BE2H_64 (kuki_size) ;

	if (kuki_size == 0 || kuki_size > kuki_maxlen)
	{	psf_log_printf (psf, "%s : Bad size (%D) of 'kuki' chunk.\n", __func__, kuki_size) ;
		return 0 ;
		} ;

	psf_fread (kuki, 1, kuki_size, psf) ;

	return kuki_size ;
} /* alac_kuki_read */
