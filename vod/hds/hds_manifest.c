#include "hds_manifest.h"
#include "hds_fragment.h"
#include "hds_amf0_encoder.h"

// manifest constants
#define HDS_MANIFEST_HEADER								\
	"<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"		\
	"<manifest\n"										\
	"  xmlns=\"http://ns.adobe.com/f4m/1.0\">\n"		\
	"  <id>%V</id>\n"									\
	"  <duration>%uD.%03uD</duration>\n"				\
	"  <streamType>recorded</streamType>\n"

#define HDS_BOOTSTRAP_HEADER							\
	"  <bootstrapInfo\n"								\
	"    profile=\"named\"\n"							\
	"    id=\"bootstrap%uD\">"

#define HDS_BOOTSTRAP_FOOTER							\
	"</bootstrapInfo>\n"

#define HDS_MEDIA_HEADER_PREFIX_VIDEO					\
	"  <media\n"										\
	"    bitrate=\"%uD\"\n"								\
	"    width=\"%uD\"\n"								\
	"    height=\"%uD\"\n"								\
	"    url=\""

#define HDS_MEDIA_HEADER_PREFIX_AUDIO					\
	"  <media\n"										\
	"    bitrate=\"%uD\"\n"								\
	"    url=\""

#define HDS_MEDIA_HEADER_SUFFIX							\
	"\"\n"												\
	"    bootstrapInfoId=\"bootstrap%uD\">\n"			\
	"    <metadata>"


#define HDS_MEDIA_FOOTER								\
	"</metadata>\n"										\
	"  </media>\n"

#define HDS_MANIFEST_FOOTER								\
	"</manifest>\n"

#define AFRT_BASE_ATOM_SIZE (ATOM_HEADER_SIZE + sizeof(afrt_atom_t) + sizeof(u_char))
#define ASRT_ATOM_SIZE (ATOM_HEADER_SIZE + sizeof(asrt_atom_t) + sizeof(asrt_entry_t))
#define ABST_BASE_ATOM_SIZE (ATOM_HEADER_SIZE + sizeof(abst_atom_t) + ASRT_ATOM_SIZE + sizeof(u_char) + AFRT_BASE_ATOM_SIZE)

// typedefs
typedef struct {
	u_char version[1];
	u_char flags[3];
	u_char bootstrap_version[4];
	u_char extra_flags[1];				// 2 bit profile, 1 bit live, 1 bit update
	u_char timescale[4];
	u_char current_media_time[8];
	u_char smpte_offset[8];
	u_char movie_identifier[1];			// this struct assumes the string is empty
	u_char server_entries[1];
	u_char quality_entries[1];
	u_char drm_data[1];					// this struct assumes the string is empty
	u_char metadata[1];					// this struct assumes the string is empty
	u_char segment_run_table_count[1];
} abst_atom_t;

typedef struct {
	u_char version[1];
	u_char flags[3];
	u_char quality_entries[1];
	u_char segment_run_entries[4];
} asrt_atom_t;

typedef struct {
	u_char first_segment[4];
	u_char fragments_per_segment[4];
} asrt_entry_t;

typedef struct {
	u_char version[1];
	u_char flags[3];
	u_char timescale[4];
	u_char quality_entries[1];
	u_char fragment_run_entries[4];
} afrt_atom_t;

typedef struct {
	u_char first_fragment[4];
	u_char first_fragment_timestamp[8];
	u_char fragment_duration[4];
} afrt_entry_t;

static u_char*
hds_write_abst_atom(
	u_char* p, 
	segment_durations_t* segment_durations)
{
	segment_duration_item_t* cur_item;
	segment_duration_item_t* last_item = segment_durations->items + segment_durations->item_count;
	uint64_t start_offset = 0;
	uint64_t timestamp;
	uint32_t duration;
	size_t afrt_atom_size = AFRT_BASE_ATOM_SIZE;
	size_t asrt_atom_size = ASRT_ATOM_SIZE;
	size_t abst_atom_size = ABST_BASE_ATOM_SIZE;

	afrt_atom_size += (segment_durations->item_count + 1) * sizeof(afrt_entry_t);
	abst_atom_size += (segment_durations->item_count + 1) * sizeof(afrt_entry_t);

	// abst
	write_atom_header(p, abst_atom_size, 'a', 'b', 's', 't');
	write_be32(p, 0);					// version + flags
	write_be32(p, 1);					// bootstrap info version
	*p++ = 0;							// profile, live, update
	write_be32(p, HDS_TIMESCALE);		// timescale
	write_be32(p, 0);					// current media time - high
	write_be32(p, segment_durations->duration_millis);	// current media time - low
	write_be64(p, 0LL);					// smpte offset
	*p++ = 0;							// movie identifier
	*p++ = 0;							// server entries
	*p++ = 0;							// quality entries
	*p++ = 0;							// drm data
	*p++ = 0;							// metadata
	*p++ = 1;							// segment run table count

	// abst.asrt
	write_atom_header(p, asrt_atom_size, 'a', 's', 'r', 't');
	write_be32(p, 0);					// version + flags
	*p++ = 0;							// quality entries
	write_be32(p, 1);					// segment run entries
	// entry #1
	write_be32(p, 1);					// first segment
	write_be32(p, segment_durations->segment_count);		// fragments per segment

	// abst
	*p++ = 1;							// fragment run table count

	// abst.afrt
	write_atom_header(p, afrt_atom_size, 'a', 'f', 'r', 't');
	write_be32(p, 0);					// version + flags
	write_be32(p, HDS_TIMESCALE);		// timescale
	*p++ = 0;							// quality entries
	write_be32(p, segment_durations->item_count + 1);	// fragment run entries

	// write the afrt entries
	for (cur_item = segment_durations->items; cur_item < last_item; cur_item++)
	{
		timestamp = rescale_time(start_offset, segment_durations->timescale, HDS_TIMESCALE);
		duration = rescale_time(cur_item->duration, segment_durations->timescale, HDS_TIMESCALE);

		write_be32(p, cur_item->segment_index + 1);		// first fragment
		write_be64(p, timestamp);		// first fragment timestamp
		write_be32(p, duration);		// fragment duration
		start_offset += cur_item->duration * cur_item->repeat_count;
	}

	// last entry
	write_be32(p, 0);					// first fragment
	write_be64(p, 0LL);					// first fragment timestamp
	write_be32(p, 0);					// fragment duration
	*p++ = 0;							// discontinuity indicator (0 = end of presentation)

	return p;
}

static u_char*
hds_write_base64_abst_atom(u_char* p, u_char* temp_buffer, segment_durations_t* segment_durations)
{
	vod_str_t binary;
	vod_str_t base64;
	
	binary.data = temp_buffer;
	binary.len = hds_write_abst_atom(binary.data, segment_durations) - binary.data;

	base64.data = p;

	vod_encode_base64(&base64, &binary);

	return p + base64.len;
}

vod_status_t
hds_packager_build_manifest(
	request_context_t* request_context,
	hds_manifest_config_t* conf,
	vod_str_t* manifest_id,
	segmenter_conf_t* segmenter_conf,
	media_set_t* media_set,
	vod_str_t* result)
{
	media_track_t** cur_sequence_tracks;
	media_sequence_t* cur_sequence;
	media_track_t* track;
	segment_durations_t* segment_durations;
	uint32_t bitrate;
	uint32_t index;
	uint32_t abst_atom_size;
	uint32_t max_abst_atom_size = 0;
	size_t result_size;
	vod_status_t rc;
	u_char* temp_buffer;
	u_char* p;

	segment_durations = vod_alloc(
		request_context->pool, 
		media_set->sequence_count * sizeof(*segment_durations));
	if (segment_durations == NULL)
	{
		vod_log_debug0(VOD_LOG_DEBUG_LEVEL, request_context->log, 0,
			"hds_packager_build_manifest: vod_alloc failed (1)");
		return VOD_ALLOC_FAILED;
	}

	// calculate the result size
	result_size = 
		sizeof(HDS_MANIFEST_HEADER) - 1 + 2 * VOD_INT32_LEN + manifest_id->len + 
		sizeof(HDS_MANIFEST_FOOTER);

	index = 0;
	for (cur_sequence = media_set->sequences; cur_sequence < media_set->sequences_end; cur_sequence++)
	{
		cur_sequence_tracks = cur_sequence->filtered_clips[0].longest_track;

		rc = segmenter_conf->get_segment_durations(
			request_context,
			segmenter_conf,
			media_set,
			cur_sequence,
			MEDIA_TYPE_NONE,
			&segment_durations[index]);
		if (rc != VOD_OK)
		{
			return rc;
		}

		abst_atom_size = ABST_BASE_ATOM_SIZE + (segment_durations[index].item_count + 1) * sizeof(afrt_entry_t);
		if (abst_atom_size > max_abst_atom_size)
		{
			max_abst_atom_size = abst_atom_size;
		}

		result_size += 
			sizeof(HDS_BOOTSTRAP_HEADER) - 1 + VOD_INT32_LEN + 
				vod_base64_encoded_length(abst_atom_size) +
			sizeof(HDS_BOOTSTRAP_FOOTER) - 1;
		
		result_size += 
			sizeof(HDS_MEDIA_HEADER_PREFIX_VIDEO) - 1 + 3 * VOD_INT32_LEN +
			conf->fragment_file_name_prefix.len + sizeof("-f-v-a-") - 1 + 3 * VOD_INT32_LEN + 
			sizeof(HDS_MEDIA_HEADER_SUFFIX) - 1 + VOD_INT32_LEN + 
				vod_base64_encoded_length(amf0_max_total_size) +
			sizeof(HDS_MEDIA_FOOTER) - 1;

		index++;
	}

	// allocate the buffers
	result->data = vod_alloc(request_context->pool, result_size);
	if (result->data == NULL)
	{
		vod_log_debug0(VOD_LOG_DEBUG_LEVEL, request_context->log, 0,
			"hds_packager_build_manifest: vod_alloc failed (2)");
		return VOD_ALLOC_FAILED;
	}

	temp_buffer = vod_alloc(request_context->pool, vod_max(amf0_max_total_size, max_abst_atom_size));
	if (temp_buffer == NULL)
	{
		vod_log_debug0(VOD_LOG_DEBUG_LEVEL, request_context->log, 0,
			"hds_packager_build_manifest: vod_alloc failed (1)");
		return VOD_ALLOC_FAILED;
	}

	// print the manifest header
	p = vod_sprintf(result->data, HDS_MANIFEST_HEADER,
		manifest_id,
		(uint32_t)(media_set->total_duration / 1000),
		(uint32_t)(media_set->total_duration % 1000));

	// bootstrap tags
	index = 0;
	for (cur_sequence = media_set->sequences; cur_sequence < media_set->sequences_end; cur_sequence++)
	{
		cur_sequence_tracks = cur_sequence->filtered_clips[0].longest_track;

		p = vod_sprintf(p, HDS_BOOTSTRAP_HEADER, index);

		p = hds_write_base64_abst_atom(p, temp_buffer, &segment_durations[index]);

		p = vod_copy(p, HDS_BOOTSTRAP_FOOTER, sizeof(HDS_BOOTSTRAP_FOOTER) - 1);

		index++;

	}

	// media tags
	index = 0;
	for (cur_sequence = media_set->sequences; cur_sequence < media_set->sequences_end; cur_sequence++)
	{
		cur_sequence_tracks = cur_sequence->filtered_clips[0].longest_track;

		if (cur_sequence_tracks[MEDIA_TYPE_VIDEO] != NULL)
		{
			track = cur_sequence_tracks[MEDIA_TYPE_VIDEO];
			bitrate = track->media_info.bitrate;
			if (cur_sequence_tracks[MEDIA_TYPE_AUDIO] != NULL)
			{
				bitrate += cur_sequence_tracks[MEDIA_TYPE_AUDIO]->media_info.bitrate;
			}

			p = vod_sprintf(p, HDS_MEDIA_HEADER_PREFIX_VIDEO,
				bitrate / 1000,
				(uint32_t)track->media_info.u.video.width,
				(uint32_t)track->media_info.u.video.height);
		}
		else
		{
			track = cur_sequence_tracks[MEDIA_TYPE_AUDIO];
			p = vod_sprintf(p, HDS_MEDIA_HEADER_PREFIX_AUDIO,
				track->media_info.bitrate / 1000);
		}

		// url
		p = vod_copy(p, conf->fragment_file_name_prefix.data, conf->fragment_file_name_prefix.len);
		if (media_set->has_multi_sequences)
		{
			p = vod_sprintf(p, "-f%uD", cur_sequence->index + 1);
		}

		if (cur_sequence_tracks[MEDIA_TYPE_VIDEO] != NULL)
		{
			p = vod_sprintf(p, "-v%uD", cur_sequence_tracks[MEDIA_TYPE_VIDEO]->index + 1);
		}

		if (cur_sequence_tracks[MEDIA_TYPE_AUDIO] != NULL)
		{
			p = vod_sprintf(p, "-a%uD", cur_sequence_tracks[MEDIA_TYPE_AUDIO]->index + 1);
		}
		*p++ = '-';

		p = vod_sprintf(p, HDS_MEDIA_HEADER_SUFFIX, index++);

		p = hds_amf0_write_base64_metadata(p, temp_buffer, cur_sequence_tracks);

		p = vod_copy(p, HDS_MEDIA_FOOTER, sizeof(HDS_MEDIA_FOOTER) - 1);
	}

	// manifest footer
	p = vod_copy(p, HDS_MANIFEST_FOOTER, sizeof(HDS_MANIFEST_FOOTER) - 1);

	result->len = p - result->data;
	
	if (result->len > result_size)
	{
		vod_log_error(VOD_LOG_ERR, request_context->log, 0,
			"hds_packager_build_manifest: result length %uz exceeded allocated length %uz",
			result->len, result_size);
		return VOD_UNEXPECTED;
	}

	vod_free(request_context->pool, temp_buffer);

	return VOD_OK;
}
