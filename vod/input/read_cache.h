#ifndef __READ_CACHE_H__
#define __READ_CACHE_H__

// includes
#include "../common.h"

// typedefs
typedef struct {
	u_char* buffer;
	uint32_t buffer_size;		// size of data read
	void* source;				// opaque context that indicates from where the buffer should be read
	uint64_t start_offset;
	uint64_t end_offset;
} cache_buffer_t;

typedef struct {
	request_context_t* request_context;
	cache_buffer_t* buffers;
	cache_buffer_t* buffers_end;
	cache_buffer_t* target_buffer;
	size_t buffer_count;
	size_t buffer_size;
	size_t alignment;
	bool_t reuse_buffers;
} read_cache_state_t;

// functions
void read_cache_init(
	read_cache_state_t* state, 
	request_context_t* request_context, 
	size_t buffer_size, 
	size_t alignment);
	
vod_status_t read_cache_allocate_buffer_slots(
	read_cache_state_t* state,
	size_t buffer_count);

bool_t read_cache_get_from_cache(
	read_cache_state_t* state, 
	int cache_slot_id, 
	void* source,
	uint64_t offset, 
	u_char** buffer, 
	uint32_t* size);

void read_cache_disable_buffer_reuse(
	read_cache_state_t* state);

vod_status_t read_cache_get_read_buffer(
	read_cache_state_t* state, 
	void** source,
	uint64_t* out_offset,
	u_char** buffer, 
	uint32_t* size);
	
void read_cache_read_completed(
	read_cache_state_t* state, 
	ssize_t bytes_read);

#endif // __READ_CACHE_H__
