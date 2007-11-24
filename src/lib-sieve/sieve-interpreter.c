#include <stdio.h>
#include <string.h>

#include "lib.h"
#include "mempool.h"
#include "array.h"
#include "hash.h"
#include "mail-storage.h"

#include "sieve-extensions.h"
#include "sieve-commands-private.h"
#include "sieve-generator.h"
#include "sieve-binary.h"
#include "sieve-result.h"
#include "sieve-comparators.h"

#include "sieve-interpreter.h"

struct sieve_interpreter {
	pool_t pool;
			
	/* Object registries */
	ARRAY_DEFINE(ext_contexts, void *); 
		
	/* Execution status */
	
	sieve_size_t pc;  /* Program counter */
	bool stopped;     /* Explicit successful stop requested */
	bool test_result; /* Result of previous test command */
	
	/* Runtime environment environment */
	struct sieve_runtime_env runenv; 
};

struct sieve_interpreter *sieve_interpreter_create(struct sieve_binary *sbin) 
{
	unsigned int i;
	int idx;
	pool_t pool;
	struct sieve_interpreter *interp;
	
	pool = pool_alloconly_create("sieve_interpreter", 4096);	
	interp = p_new(pool, struct sieve_interpreter, 1);
	interp->pool = pool;
	interp->runenv.interp = interp;
	
	interp->runenv.sbin = sbin;
	sieve_binary_ref(sbin);
	sieve_binary_commit(sbin);
	
	interp->pc = 0;

	p_array_init(&interp->ext_contexts, pool, 4);

	/* Pre-load core language features implemented as 'extensions' */
	for ( i = 0; i < sieve_preloaded_extensions_count; i++ ) {
		const struct sieve_extension *ext = sieve_preloaded_extensions[i];
		
		if ( ext->interpreter_load != NULL )
			(void)ext->interpreter_load(interp);		
	}

	/* Load other extensions listed in the binary */
	for ( idx = 0; idx < sieve_binary_extensions_count(sbin); idx++ ) {
		const struct sieve_extension *ext = 
			sieve_binary_extension_get_by_index(sbin, idx, NULL);
		
		if ( ext->interpreter_load != NULL )
			ext->interpreter_load(interp);
	}
	
	return interp;
}

void sieve_interpreter_free(struct sieve_interpreter *interp) 
{
	sieve_binary_unref(&interp->runenv.sbin);
	pool_unref(&(interp->pool));
}

inline pool_t sieve_interpreter_pool(struct sieve_interpreter *interp)
{
	return interp->pool;
}


/* Extension support */

inline void sieve_interpreter_extension_set_context
	(struct sieve_interpreter *interpreter, int ext_id, void *context)
{
	array_idx_set(&interpreter->ext_contexts, (unsigned int) ext_id, &context);	
}

inline const void *sieve_interpreter_extension_get_context
	(struct sieve_interpreter *interpreter, int ext_id) 
{
	void * const *ctx;

	if  ( ext_id < 0 || ext_id >= (int) array_count(&interpreter->ext_contexts) )
		return NULL;
	
	ctx = array_idx(&interpreter->ext_contexts, (unsigned int) ext_id);		

	return *ctx;
}

/* Program counter */

inline void sieve_interpreter_reset(struct sieve_interpreter *interp) 
{
	interp->pc = 0;
	interp->stopped = FALSE;
	interp->test_result = FALSE;
	interp->runenv.msgdata = NULL;
	interp->runenv.result = NULL;
}

inline void sieve_interpreter_stop(struct sieve_interpreter *interp)
{
	interp->stopped = TRUE;
}

inline sieve_size_t sieve_interpreter_program_counter(struct sieve_interpreter *interp)
{
	return interp->pc;
}

inline bool sieve_interpreter_program_jump
	(struct sieve_interpreter *interp, bool jump)
{
	sieve_size_t pc = interp->pc;
	int offset;
	
	if ( !sieve_interpreter_read_offset_operand(interp, &offset) )
		return FALSE;

	if ( pc + offset <= sieve_binary_get_code_size(interp->runenv.sbin) && 
		pc + offset > 0 ) 
	{	
		if ( jump )
			interp->pc = pc + offset;
		
		return TRUE;
	}
	
	return FALSE;
}

inline void sieve_interpreter_set_test_result
	(struct sieve_interpreter *interp, bool result)
{
	interp->test_result = result;
}

inline bool sieve_interpreter_get_test_result
	(struct sieve_interpreter *interp)
{
	return interp->test_result;
}

/* Opcodes and operands */

bool sieve_interpreter_read_offset_operand
	(struct sieve_interpreter *interp, int *offset) 
{
	return sieve_binary_read_offset(interp->runenv.sbin, &(interp->pc), offset);
}
 
/* Code Dump */

static bool sieve_interpreter_dump_operation
	(struct sieve_interpreter *interp) 
{
	const struct sieve_opcode *opcode = 
		sieve_operation_read(interp->runenv.sbin, &(interp->pc));

	if ( opcode != NULL ) {
		printf("%08x: ", interp->pc-1);
	
		if ( opcode->dump != NULL )
			return opcode->dump(opcode, &(interp->runenv), &(interp->pc));
		else if ( opcode->mnemonic != NULL )
			printf("%s\n", opcode->mnemonic);
		else
			return FALSE;
			
		return TRUE;
	}		
	
	return FALSE;
}

void sieve_interpreter_dump_code(struct sieve_interpreter *interp) 
{
	sieve_interpreter_reset(interp);
	
	interp->runenv.result = NULL;
	interp->runenv.msgdata = NULL;
	
	while ( interp->pc < 
		sieve_binary_get_code_size(interp->runenv.sbin) ) {
		if ( !sieve_interpreter_dump_operation(interp) ) {
			printf("Binary is corrupt.\n");
			return;
		}
	}
	
	printf("%08x: [End of code]\n", 
		sieve_binary_get_code_size(interp->runenv.sbin));	
}

/* Code execute */

bool sieve_interpreter_execute_operation
	(struct sieve_interpreter *interp) 
{
	const struct sieve_opcode *opcode = 
		sieve_operation_read(interp->runenv.sbin, &(interp->pc));

	if ( opcode != NULL ) {
		if ( opcode->execute != NULL )
			return opcode->execute(opcode, &(interp->runenv), &(interp->pc));
		else
			return FALSE;
			
		return TRUE;
	}
	
	return FALSE;
}		

bool sieve_interpreter_run
(struct sieve_interpreter *interp, struct sieve_message_data *msgdata,
	struct sieve_result *result) 
{
	sieve_interpreter_reset(interp);
	
	interp->runenv.msgdata = msgdata;
	interp->runenv.result = result;		
	sieve_result_ref(result);
	
	while ( !interp->stopped && 
		interp->pc < sieve_binary_get_code_size(interp->runenv.sbin) ) {
		printf("%08x: ", interp->pc);
		
		if ( !sieve_interpreter_execute_operation(interp) ) {
			printf("Execution aborted.\n");
			sieve_result_unref(&result);
			return FALSE;
		}
	}
	
	interp->runenv.result = NULL;
	interp->runenv.msgdata = NULL;
	
	sieve_result_unref(&result);

	return TRUE;
}


