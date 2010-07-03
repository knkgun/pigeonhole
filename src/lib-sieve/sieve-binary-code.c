/* Copyright (c) 2002-2010 Dovecot Sieve authors, see the included COPYING file
 */

#include "lib.h"
#include "str.h"
#include "str-sanitize.h"
#include "mempool.h"
#include "buffer.h"
#include "hash.h"
#include "array.h"
#include "ostream.h"

#include "sieve-common.h"
#include "sieve-error.h"
#include "sieve-extensions.h"
#include "sieve-code.h"
#include "sieve-script.h"

#include "sieve-binary-private.h"

/*
 * Forward declarations
 */

static inline sieve_size_t sieve_binary_emit_dynamic_data
	(struct sieve_binary_block *sblock, const void *data, size_t size);

/*
 * Emission functions
 */

/* Low-level emission functions */

static inline void _sieve_binary_emit_data
(struct sieve_binary_block *sblock, const void *data, sieve_size_t size) 
{	  
	buffer_append(sblock->data, data, size);
}

static inline void _sieve_binary_emit_byte
(struct sieve_binary_block *sblock, unsigned char byte)
{
	_sieve_binary_emit_data(sblock, &byte, 1);
}

static inline void _sieve_binary_update_data
(struct sieve_binary_block *sblock, sieve_size_t address, const void *data, 
	sieve_size_t size) 
{
	buffer_write(sblock->data, address, data, size);
}

sieve_size_t sieve_binary_emit_data
(struct sieve_binary_block *sblock, const void *data, sieve_size_t size)
{
	sieve_size_t address = _sieve_binary_block_get_size(sblock);

	_sieve_binary_emit_data(sblock, data, size);

	return address;
}

sieve_size_t sieve_binary_emit_byte
(struct sieve_binary_block *sblock, unsigned char byte) 
{
	sieve_size_t address = _sieve_binary_block_get_size(sblock);

	_sieve_binary_emit_data(sblock, &byte, 1);
	
	return address;
}

void sieve_binary_update_data
(struct sieve_binary_block *sblock, sieve_size_t address, const void *data, 
	sieve_size_t size) 
{
	_sieve_binary_update_data(sblock, address, data, size);
}

/* Offset emission functions */

sieve_size_t sieve_binary_emit_offset
(struct sieve_binary_block *sblock, int offset) 
{
	int i;
	sieve_size_t address = _sieve_binary_block_get_size(sblock);

	for ( i = 3; i >= 0; i-- ) {
		char c = (char) (offset >> (i * 8));
		_sieve_binary_emit_data(sblock, &c, 1);
	}
	
	return address;
}

void sieve_binary_resolve_offset
(struct sieve_binary_block *sblock, sieve_size_t address) 
{
	int i;
	int offset = _sieve_binary_block_get_size(sblock) - address; 
	
	for ( i = 3; i >= 0; i-- ) {
		char c = (char) (offset >> (i * 8));	
		_sieve_binary_update_data(sblock, address + 3 - i, &c, 1);
	}
}

/* Literal emission */

sieve_size_t sieve_binary_emit_integer
(struct sieve_binary_block *sblock, sieve_number_t integer)
{
	sieve_size_t address = _sieve_binary_block_get_size(sblock);
	int i;
	char buffer[sizeof(sieve_number_t) + 1];
	int bufpos = sizeof(buffer) - 1;
  
	buffer[bufpos] = integer & 0x7F;
	bufpos--;
	integer >>= 7;
	while ( integer > 0 ) {
		buffer[bufpos] = integer & 0x7F;
		bufpos--;
		integer >>= 7;  
	}
  
	bufpos++;
	if ( (sizeof(buffer) - bufpos) > 1 ) { 
		for ( i = bufpos; i < ((int) sizeof(buffer) - 1); i++) {
			buffer[i] |= 0x80;
		}
	} 
  
	_sieve_binary_emit_data
		(sblock, buffer + bufpos, sizeof(buffer) - bufpos);

	return address;
}

static inline sieve_size_t sieve_binary_emit_dynamic_data
(struct sieve_binary_block *sblock, const void *data, sieve_size_t size)
{
	sieve_size_t address = sieve_binary_emit_integer
		(sblock, (sieve_number_t) size);

	_sieve_binary_emit_data(sblock, data, size);
  
	return address;
}

sieve_size_t sieve_binary_emit_cstring
(struct sieve_binary_block *sblock, const char *str)
{
	sieve_size_t address = sieve_binary_emit_dynamic_data
		(sblock, (void *) str, (sieve_size_t) strlen(str));
	_sieve_binary_emit_byte(sblock, 0);
  
	return address;
}

sieve_size_t sieve_binary_emit_string
(struct sieve_binary_block *sblock, const string_t *str)
{
	sieve_size_t address = sieve_binary_emit_dynamic_data
		(sblock, (void *) str_data(str), (sieve_size_t) str_len(str));
	_sieve_binary_emit_byte(sblock, 0);
	
	return address;
}

/*
 * Extension emission
 */

sieve_size_t sieve_binary_emit_extension
(struct sieve_binary_block *sblock, const struct sieve_extension *ext,
	unsigned int offset)
{
	sieve_size_t address = _sieve_binary_block_get_size(sblock);
	struct sieve_binary_extension_reg *ereg = NULL;

	(void)sieve_binary_extension_register(sblock->sbin, ext, &ereg);

	i_assert(ereg != NULL);

	_sieve_binary_emit_byte(sblock, offset + ereg->index);
	return address;
}

void sieve_binary_emit_extension_object
(struct sieve_binary_block *sblock, const struct sieve_extension_objects *objs,
	unsigned int code)
{
	if ( objs->count > 1 )
		_sieve_binary_emit_byte(sblock, code);
}

/*
 * Code retrieval
 */

#define ADDR_CODE_READ(block) \
	size_t _code_size; \
	const signed char *_code = buffer_get_data((block)->data, &_code_size)
 
#define ADDR_CODE_AT(address) \
	((signed char) (_code[*address]))
#define ADDR_DATA_AT(address) \
	((unsigned char) (_code[*address]))
#define ADDR_POINTER(address) \
	((const char *) (&_code[*address]))

#define ADDR_BYTES_LEFT(address) \
	((_code_size) - (*address))
#define ADDR_JUMP(address, offset) \
	(*address) += offset

/* Literals */

bool sieve_binary_read_byte
(struct sieve_binary_block *sblock, sieve_size_t *address, unsigned int *byte_r) 
{
	ADDR_CODE_READ(sblock);
	
	if ( ADDR_BYTES_LEFT(address) >= 1 ) {
		if ( byte_r != NULL )
			*byte_r = ADDR_DATA_AT(address);
		ADDR_JUMP(address, 1);
			
		return TRUE;
	}
	
	*byte_r = 0;
	return FALSE;
}

bool sieve_binary_read_code
(struct sieve_binary_block *sblock, sieve_size_t *address, signed int *code_r) 
{	
	ADDR_CODE_READ(sblock);

	if ( ADDR_BYTES_LEFT(address) >= 1 ) {
		if ( code_r != NULL )
			*code_r = ADDR_CODE_AT(address);
		ADDR_JUMP(address, 1);
			
		return TRUE;
	}
	
	*code_r = 0;
	return FALSE;
}


bool sieve_binary_read_offset
(struct sieve_binary_block *sblock, sieve_size_t *address, int *offset_r) 
{
	uint32_t offs = 0;
	ADDR_CODE_READ(sblock);
	
	if ( ADDR_BYTES_LEFT(address) >= 4 ) {
		int i; 
	  
		for ( i = 0; i < 4; i++ ) {
			offs = (offs << 8) + ADDR_DATA_AT(address);
			ADDR_JUMP(address, 1);
		}
	  
		if ( offset_r != NULL )
			*offset_r = (int) offs;
			
		return TRUE;
	}
	
	return FALSE;
}

/* FIXME: might need negative numbers in the future */
bool sieve_binary_read_integer
(struct sieve_binary_block *sblock, sieve_size_t *address, sieve_number_t *int_r) 
{
	int bits = sizeof(sieve_number_t) * 8;
	*int_r = 0;

	ADDR_CODE_READ(sblock);
  
	if ( ADDR_BYTES_LEFT(address) == 0 )
		return FALSE;
  
	while ( (ADDR_DATA_AT(address) & 0x80) > 0 ) {
		if ( ADDR_BYTES_LEFT(address) > 0 && bits > 0) {
			*int_r |= ADDR_DATA_AT(address) & 0x7F;
			ADDR_JUMP(address, 1);
    
			*int_r <<= 7;
			bits -= 7;
		} else {
			/* This is an error */
			return FALSE;
		}
	}
  
	*int_r |= ADDR_DATA_AT(address) & 0x7F;
	ADDR_JUMP(address, 1);
  
	return TRUE;
}

bool sieve_binary_read_string
(struct sieve_binary_block *sblock, sieve_size_t *address, string_t **str_r) 
{
	unsigned int strlen = 0;

	ADDR_CODE_READ(sblock);
  
	if ( !sieve_binary_read_unsigned(sblock, address, &strlen) ) 
		return FALSE;
    	  
	if ( strlen > ADDR_BYTES_LEFT(address) ) 
		return FALSE;
 
 	if ( str_r != NULL )  
		*str_r = t_str_new_const(ADDR_POINTER(address), strlen);
	ADDR_JUMP(address, strlen);
	
	if ( ADDR_CODE_AT(address) != 0 )
		return FALSE;
	
	ADDR_JUMP(address, 1);
  
	return TRUE;
}

bool sieve_binary_read_extension
(struct sieve_binary_block *sblock, sieve_size_t *address, 
	unsigned int *offset_r, const struct sieve_extension **ext_r)
{
	unsigned int code;
	unsigned int offset = *offset_r;
	const struct sieve_extension *ext = NULL;

	ADDR_CODE_READ(sblock);

	if ( ADDR_BYTES_LEFT(address) <= 0 )
		return FALSE;

	(*offset_r) = code = ADDR_DATA_AT(address);
	ADDR_JUMP(address, 1);

	if ( code >= offset ) {
		ext = sieve_binary_extension_get_by_index(sblock->sbin, code - offset);
		
		if ( ext == NULL ) 
			return FALSE;
	}

	(*ext_r) = ext;

	return TRUE;
}

const void *sieve_binary_read_extension_object
(struct sieve_binary_block *sblock, sieve_size_t *address, 
	const struct sieve_extension_objects *objs)
{
	unsigned int code;

	ADDR_CODE_READ(sblock);

	if ( objs->count == 0 ) 
		return NULL;

	if ( objs->count == 1 )
		return objs->objects;

	if ( ADDR_BYTES_LEFT(address) <= 0 )
		return NULL;

	code = ADDR_DATA_AT(address);
	ADDR_JUMP(address, 1);	

	if ( code >= objs->count )
		return NULL;

	return ((const void *const *) objs->objects)[code];
}