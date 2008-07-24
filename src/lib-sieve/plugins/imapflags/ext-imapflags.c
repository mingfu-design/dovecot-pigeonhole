/* Extension imapflags
 * ------------------
 *
 * Authors: Stephan Bosch
 * Specification: draft-ietf-sieve-imapflags-05
 * Implementation: full 
 * Status: experimental, largely untested
 *
 */
 
#include "lib.h"
#include "mempool.h"
#include "str.h"

#include "sieve-common.h"

#include "sieve-code.h"
#include "sieve-extensions.h"
#include "sieve-actions.h"
#include "sieve-commands.h"
#include "sieve-validator.h"
#include "sieve-generator.h"
#include "sieve-interpreter.h"

#include "ext-imapflags-common.h"


/* Forward declarations */

static bool ext_imapflags_load(int ext_id);
static bool ext_imapflags_validator_load(struct sieve_validator *valdtr);
static bool ext_imapflags_runtime_load
	(const struct sieve_runtime_env *renv);

/* Operations */

extern const struct sieve_operation setflag_operation;
extern const struct sieve_operation addflag_operation;
extern const struct sieve_operation removeflag_operation;
extern const struct sieve_operation hasflag_operation;

const struct sieve_operation *imapflags_operations[] = 
	{ &setflag_operation, &addflag_operation, &removeflag_operation, &hasflag_operation };

/* Operands */

const struct sieve_operand flags_side_effect_operand;

/* Extension definitions */

int ext_imapflags_my_id;

const struct sieve_extension imapflags_extension = { 
	"imap4flags", 
	&ext_imapflags_my_id,
	ext_imapflags_load,
	ext_imapflags_validator_load, 
	NULL, NULL,
	ext_imapflags_runtime_load, 
	NULL, NULL,
	SIEVE_EXT_DEFINE_OPERATIONS(imapflags_operations), 
	SIEVE_EXT_DEFINE_OPERAND(flags_side_effect_operand)
};

static bool ext_imapflags_load(int ext_id)
{
	ext_imapflags_my_id = ext_id;

	return TRUE;
}

extern const struct sieve_side_effect_extension imapflags_seffect_extension;

/* Load extension into validator */

static bool ext_imapflags_validator_load
	(struct sieve_validator *valdtr)
{
	/* Register commands */
	sieve_validator_register_command(valdtr, &cmd_setflag);
	sieve_validator_register_command(valdtr, &cmd_addflag);
	sieve_validator_register_command(valdtr, &cmd_removeflag);
	sieve_validator_register_command(valdtr, &tst_hasflag);
	
	ext_imapflags_attach_flags_tag(valdtr, "keep");
	ext_imapflags_attach_flags_tag(valdtr, "fileinto");

	return TRUE;
}

/*
 * Interpreter context
 */

static bool ext_imapflags_runtime_load
	(const struct sieve_runtime_env *renv)
{
	ext_imapflags_runtime_init(renv);
	
	return TRUE;
}



