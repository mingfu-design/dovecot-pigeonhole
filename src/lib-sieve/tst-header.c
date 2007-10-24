#include <stdio.h>

#include "sieve-commands.h"
#include "sieve-commands-private.h"
#include "sieve-validator.h"
#include "sieve-generator.h"
#include "sieve-interpreter.h"

/* Opcodes */

static bool tst_header_opcode_dump(struct sieve_interpreter *interpreter);

const struct sieve_opcode tst_header_opcode = 
	{ tst_header_opcode_dump, NULL };

/* Test registration */

bool tst_header_registered(struct sieve_validator *validator, struct sieve_command_registration *cmd_reg) 
{
	/* The order of these is not significant */
	sieve_validator_link_comparator_tag(validator, cmd_reg);
	sieve_validator_link_match_type_tags(validator, cmd_reg);

	return TRUE;
}

/* Test validation */

bool tst_header_validate(struct sieve_validator *validator, struct sieve_command_context *tst) 
{ 		
	struct sieve_ast_argument *arg;
	
	/* Check header test syntax (optional tags are registered above):
	 *   header [COMPARATOR] [MATCH-TYPE]
	 *     <header-names: string-list> <key-list: string-list>
	 */
	if ( !sieve_validate_command_arguments(validator, tst, 2, &arg) ||
		!sieve_validate_command_subtests(validator, tst, 0) ) 
		return FALSE;
	
	tst->data = arg;
		
	if ( sieve_ast_argument_type(arg) != SAAT_STRING && sieve_ast_argument_type(arg) != SAAT_STRING_LIST ) {
		sieve_command_validate_error(validator, tst, 
			"the header test expects a string-list as first argument (header names), but %s was found", 
			sieve_ast_argument_name(arg));
		return FALSE; 
	}
	
	arg = sieve_ast_argument_next(arg);
	if ( sieve_ast_argument_type(arg) != SAAT_STRING && sieve_ast_argument_type(arg) != SAAT_STRING_LIST ) {
		sieve_command_validate_error(validator, tst, 
			"the header test expects a string-list as second argument (key list), but %s was found", 
			sieve_ast_argument_name(arg));
		return FALSE; 
	}
	
	return TRUE;
}

/* Test generation */

bool tst_header_generate
	(struct sieve_generator *generator,	struct sieve_command_context *ctx) 
{
	struct sieve_ast_argument *arg = (struct sieve_ast_argument *) ctx->data;
	sieve_generator_emit_opcode(generator, SIEVE_OPCODE_HEADER);

	/* Emit header names */  	
	if ( !sieve_generator_emit_stringlist_argument(generator, arg) ) 
		return FALSE;
	
	/* Emit key list */
	arg = sieve_ast_argument_next(arg);
	if ( !sieve_generator_emit_stringlist_argument(generator, arg) ) 
		return FALSE;

	return TRUE;
}

/* Code dump */

static bool tst_header_opcode_dump(struct sieve_interpreter *interpreter)
{
    printf("HEADER\n");
    sieve_interpreter_dump_operand(interpreter);
    sieve_interpreter_dump_operand(interpreter);

    return TRUE;
}
