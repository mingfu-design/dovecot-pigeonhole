/* Copyright (c) 2002-2008 Dovecot Sieve authors, see the included COPYING file
 */

#include "lib.h"
#include "str.h"
#include "strfuncs.h"
#include "md5.h"
#include "hostpid.h"
#include "str-sanitize.h"
#include "message-address.h"
#include "message-date.h"
#include "ioloop.h"

#include "rfc2822.h"

#include "sieve-common.h"
#include "sieve-code.h"
#include "sieve-address.h"
#include "sieve-extensions.h"
#include "sieve-commands.h"
#include "sieve-actions.h"
#include "sieve-validator.h"
#include "sieve-generator.h"
#include "sieve-interpreter.h"
#include "sieve-dump.h"
#include "sieve-result.h"
#include "sieve-message.h"

#include "ext-vacation-common.h"

#include <stdio.h>

/* 
 * Forward declarations 
 */
 
static const struct sieve_argument vacation_days_tag;
static const struct sieve_argument vacation_subject_tag;
static const struct sieve_argument vacation_from_tag;
static const struct sieve_argument vacation_addresses_tag;
static const struct sieve_argument vacation_mime_tag;
static const struct sieve_argument vacation_handle_tag;

/* 
 * Vacation command 
 *	
 * Syntax: 
 *    vacation [":days" number] [":subject" string]
 *                 [":from" string] [":addresses" string-list]
 *                 [":mime"] [":handle" string] <reason: string>
 */

static bool cmd_vacation_registered
	(struct sieve_validator *validator, 
		struct sieve_command_registration *cmd_reg);
static bool cmd_vacation_pre_validate
	(struct sieve_validator *validator, struct sieve_command_context *cmd); 
static bool cmd_vacation_validate
	(struct sieve_validator *validator, struct sieve_command_context *cmd);
static bool cmd_vacation_generate
	(const struct sieve_codegen_env *cgenv, struct sieve_command_context *ctx);

const struct sieve_command vacation_command = { 
	"vacation",
	SCT_COMMAND, 
	1, 0, FALSE, FALSE, 
	cmd_vacation_registered,
	cmd_vacation_pre_validate, 
	cmd_vacation_validate, 
	cmd_vacation_generate, 
	NULL 
};

/*
 * Vacation command tags
 */

/* Forward declarations */

static bool cmd_vacation_validate_number_tag
	(struct sieve_validator *validator, struct sieve_ast_argument **arg, 
		struct sieve_command_context *cmd);
static bool cmd_vacation_validate_string_tag
	(struct sieve_validator *validator, struct sieve_ast_argument **arg, 
		struct sieve_command_context *cmd);
static bool cmd_vacation_validate_stringlist_tag
	(struct sieve_validator *validator, struct sieve_ast_argument **arg, 
		struct sieve_command_context *cmd);
static bool cmd_vacation_validate_mime_tag
	(struct sieve_validator *validator, struct sieve_ast_argument **arg, 
		struct sieve_command_context *cmd);

/* Argument objects */

static const struct sieve_argument vacation_days_tag = { 
	"days", 
	NULL, NULL,
	cmd_vacation_validate_number_tag, 
	NULL, NULL 
};

static const struct sieve_argument vacation_subject_tag = { 
	"subject", 
	NULL, NULL,
	cmd_vacation_validate_string_tag, 
	NULL, NULL 
};

static const struct sieve_argument vacation_from_tag = { 
	"from", 
	NULL, NULL,
	cmd_vacation_validate_string_tag, 
	NULL, NULL 
};

static const struct sieve_argument vacation_addresses_tag = { 
	"addresses", 
	NULL, NULL,
	cmd_vacation_validate_stringlist_tag, 
	NULL, NULL 
};

static const struct sieve_argument vacation_mime_tag = { 
	"mime",	
	NULL, NULL, 
	cmd_vacation_validate_mime_tag,
	NULL, NULL
};

static const struct sieve_argument vacation_handle_tag = { 
	"handle", 
	NULL, NULL, 
	cmd_vacation_validate_string_tag, 
	NULL, NULL 
};

/* Codes for optional arguments */

enum cmd_vacation_optional {
	OPT_END,
	OPT_DAYS,
	OPT_SUBJECT,
	OPT_FROM,
	OPT_ADDRESSES,
	OPT_MIME
};

/* 
 * Vacation operation 
 */

static bool ext_vacation_operation_dump
	(const struct sieve_operation *op,	
		const struct sieve_dumptime_env *denv, sieve_size_t *address);
static int ext_vacation_operation_execute
	(const struct sieve_operation *op, 
		const struct sieve_runtime_env *renv, sieve_size_t *address);

const struct sieve_operation vacation_operation = { 
	"VACATION",
	&vacation_extension,
	0,
	ext_vacation_operation_dump, 
	ext_vacation_operation_execute
};

/* 
 * Vacation action 
 */

/* Forward declarations */

static int act_vacation_check_duplicate
	(const struct sieve_runtime_env *renv, 
		const struct sieve_action_data *act, 
		const struct sieve_action_data *act_other);
int act_vacation_check_conflict
	(const struct sieve_runtime_env *renv, 
		const struct sieve_action_data *act, 
		const struct sieve_action_data *act_other);
static void act_vacation_print
	(const struct sieve_action *action, 
		const struct sieve_result_print_env *rpenv, void *context, bool *keep);	
static bool act_vacation_commit
	(const struct sieve_action *action,	const struct sieve_action_exec_env *aenv, 
		void *tr_context, bool *keep);

/* Action object */

const struct sieve_action act_vacation = {
	"vacation",
	SIEVE_ACTFLAG_SENDS_RESPONSE,
	act_vacation_check_duplicate, 
	act_vacation_check_conflict,
	act_vacation_print,
	NULL, NULL,
	act_vacation_commit,
	NULL
};

/* Action context information */
		
struct act_vacation_context {
	const char *reason;

	sieve_number_t days;
	const char *subject;
	const char *handle;
	bool mime;
	const char *from;
	const char *from_normalized;	
	const char *const *addresses;
};

/*
 * Command validation context
 */
 
struct cmd_vacation_context_data {
	string_t *from;
	string_t *subject;
	
	bool mime;
	
	string_t *handle;
};

/* 
 * Tag validation 
 */

static bool cmd_vacation_validate_number_tag
(struct sieve_validator *validator, struct sieve_ast_argument **arg, 
	struct sieve_command_context *cmd)
{
	struct sieve_ast_argument *tag = *arg;
	
	/* Detach the tag itself */
	*arg = sieve_ast_arguments_detach(*arg,1);
	
	/* Check syntax:
	 *   :days number
	 */
	if ( !sieve_validate_tag_parameter
		(validator, cmd, tag, *arg, SAAT_NUMBER) ) {
		return FALSE;
	}

	/* Enforce :days > 0 */
	if ( sieve_ast_argument_number(*arg) == 0 ) {
		sieve_ast_argument_number_set(*arg, 1);
	}

	/* Skip parameter */
	*arg = sieve_ast_argument_next(*arg);
	
	return TRUE;
}

static bool cmd_vacation_validate_string_tag
(struct sieve_validator *validator, struct sieve_ast_argument **arg, 
	struct sieve_command_context *cmd)
{
	struct sieve_ast_argument *tag = *arg;
	struct cmd_vacation_context_data *ctx_data = 
		(struct cmd_vacation_context_data *) cmd->data; 

	/* Detach the tag itself */
	*arg = sieve_ast_arguments_detach(*arg,1);
	
	/* Check syntax:
	 *   :subject string
	 *   :from string
	 *   :handle string
	 */
	if ( !sieve_validate_tag_parameter
		(validator, cmd, tag, *arg, SAAT_STRING) ) {
		return FALSE;
	}

	if ( tag->argument == &vacation_from_tag ) {
		if ( sieve_argument_is_string_literal(*arg) ) {
			string_t *address = sieve_ast_argument_str(*arg);
			const char *error;
	 		bool result;
	 		
	 		T_BEGIN {
	 			result = sieve_address_validate(address, &error);
	 
				if ( !result ) {
					sieve_argument_validate_error(validator, *arg, 
						"specified :from address '%s' is invalid for vacation action: %s", 
						str_sanitize(str_c(address), 128), error);
				}
			} T_END;
		
			if ( !result )
				return FALSE;
		}
		
		ctx_data->from = sieve_ast_argument_str(*arg);
		
		/* Skip parameter */
		*arg = sieve_ast_argument_next(*arg);
		
	} else if ( tag->argument == &vacation_subject_tag ) {
		ctx_data->subject = sieve_ast_argument_str(*arg);
		
		/* Skip parameter */
		*arg = sieve_ast_argument_next(*arg);
		
	} else if ( tag->argument == &vacation_handle_tag ) {
		ctx_data->handle = sieve_ast_argument_str(*arg);
		
		/* Detach optional argument (emitted as mandatory) */
		*arg = sieve_ast_arguments_detach(*arg,1);
	}
			
	return TRUE;
}

static bool cmd_vacation_validate_stringlist_tag
(struct sieve_validator *validator, struct sieve_ast_argument **arg, 
	struct sieve_command_context *cmd)
{
	struct sieve_ast_argument *tag = *arg;

	/* Detach the tag itself */
	*arg = sieve_ast_arguments_detach(*arg,1);
	
	/* Check syntax:
	 *   :addresses string-list
	 */
	if ( !sieve_validate_tag_parameter
		(validator, cmd, tag, *arg, SAAT_STRING_LIST) ) {
		return FALSE;
	}
	
	/* Skip parameter */
	*arg = sieve_ast_argument_next(*arg);

	return TRUE;
}

static bool cmd_vacation_validate_mime_tag
(struct sieve_validator *validator ATTR_UNUSED, struct sieve_ast_argument **arg, 
	struct sieve_command_context *cmd)
{
	struct cmd_vacation_context_data *ctx_data = 
		(struct cmd_vacation_context_data *) cmd->data; 

	ctx_data->mime = TRUE;

	/* Skip tag */
	*arg = sieve_ast_argument_next(*arg);
	
	return TRUE;
}

/* 
 * Command registration 
 */

static bool cmd_vacation_registered
(struct sieve_validator *validator, struct sieve_command_registration *cmd_reg) 
{
	sieve_validator_register_tag
		(validator, cmd_reg, &vacation_days_tag, OPT_DAYS); 	
	sieve_validator_register_tag
		(validator, cmd_reg, &vacation_subject_tag, OPT_SUBJECT); 	
	sieve_validator_register_tag
		(validator, cmd_reg, &vacation_from_tag, OPT_FROM); 	
	sieve_validator_register_tag
		(validator, cmd_reg, &vacation_addresses_tag, OPT_ADDRESSES); 	
	sieve_validator_register_tag
		(validator, cmd_reg, &vacation_mime_tag, OPT_MIME); 	
	sieve_validator_register_tag
		(validator, cmd_reg, &vacation_handle_tag, 0); 	

	return TRUE;
}

/* 
 * Command validation 
 */
 
static bool cmd_vacation_pre_validate
(struct sieve_validator *validator ATTR_UNUSED, 
	struct sieve_command_context *cmd) 
{
	struct cmd_vacation_context_data *ctx_data;
	
	/* Assign context */
	ctx_data = p_new(sieve_command_pool(cmd), 
		struct cmd_vacation_context_data, 1);
	cmd->data = ctx_data;

	return TRUE;
}

static const char _handle_empty_subject[] = "<default-subject>";
static const char _handle_empty_from[] = "<default-from>";
static const char _handle_mime_enabled[] = "<MIME>";
static const char _handle_mime_disabled[] = "<NO-MIME>";

static bool cmd_vacation_validate
(struct sieve_validator *validator, struct sieve_command_context *cmd) 
{ 	
	struct sieve_ast_argument *arg = cmd->first_positional;
	struct cmd_vacation_context_data *ctx_data = 
		(struct cmd_vacation_context_data *) cmd->data; 

	if ( !sieve_validate_positional_argument
		(validator, cmd, arg, "reason", 1, SAAT_STRING) ) {
		return FALSE;
	}
	
	if ( !sieve_validator_argument_activate(validator, cmd, arg, FALSE) )
		return FALSE;
		
	/* Construct handle if not set explicitly */
	if ( ctx_data->handle == NULL ) {
		string_t *reason = sieve_ast_argument_str(arg);
		unsigned int size = str_len(reason);
		
		/* Precalculate the size of it all */
		size += ctx_data->subject == NULL ? 
			sizeof(_handle_empty_subject) - 1 : str_len(ctx_data->subject);
		size += ctx_data->from == NULL ? 
			sizeof(_handle_empty_from) - 1 : str_len(ctx_data->from); 
		size += ctx_data->mime ? 
			sizeof(_handle_mime_enabled) - 1 : sizeof(_handle_mime_disabled) - 1; 
			
		/* Construct the string */
		ctx_data->handle = str_new(sieve_command_pool(cmd), size);
		str_append_str(ctx_data->handle, reason);
		
		if ( ctx_data->subject != NULL )
			str_append_str(ctx_data->handle, ctx_data->subject);
		else
			str_append(ctx_data->handle, _handle_empty_subject);
		
		if ( ctx_data->from != NULL )
			str_append_str(ctx_data->handle, ctx_data->from);
		else
			str_append(ctx_data->handle, _handle_empty_from);
			
		str_append(ctx_data->handle, 
			ctx_data->mime ? _handle_mime_enabled : _handle_mime_disabled );
	}
	
	return TRUE;
}

/*
 * Code generation
 */
 
static bool cmd_vacation_generate
(const struct sieve_codegen_env *cgenv, struct sieve_command_context *ctx) 
{
	struct cmd_vacation_context_data *ctx_data = 
		(struct cmd_vacation_context_data *) ctx->data;
		 
	sieve_operation_emit_code(cgenv->sbin, &vacation_operation);

	/* Emit source line */
	sieve_code_source_line_emit(cgenv->sbin, sieve_command_source_line(ctx));

	/* Generate arguments */
	if ( !sieve_generate_arguments(cgenv, ctx, NULL) )
		return FALSE;	

	sieve_opr_string_emit(cgenv->sbin, ctx_data->handle);
		
	return TRUE;
}

/* 
 * Code dump
 */
 
static bool ext_vacation_operation_dump
(const struct sieve_operation *op ATTR_UNUSED,
	const struct sieve_dumptime_env *denv, sieve_size_t *address)
{	
	int opt_code = 1;
	
	sieve_code_dumpf(denv, "VACATION");
	sieve_code_descend(denv);	

	/* Source line */
	if ( !sieve_code_source_line_dump(denv, address) )
		return FALSE;

	/* Dump optional operands */
	if ( sieve_operand_optional_present(denv->sbin, address) ) {
		while ( opt_code != 0 ) {
			sieve_code_mark(denv);
			
			if ( !sieve_operand_optional_read(denv->sbin, address, &opt_code) ) 
				return FALSE;

			switch ( opt_code ) {
			case 0:
				break;
			case OPT_DAYS:
				if ( !sieve_opr_number_dump(denv, address, "days") )
					return FALSE;
				break;
			case OPT_SUBJECT:
				if ( !sieve_opr_string_dump(denv, address, "subject") )
					return FALSE;
				break;
			case OPT_FROM:
				if ( !sieve_opr_string_dump(denv, address, "from") )
					return FALSE;
				break;
			case OPT_ADDRESSES:
				if ( !sieve_opr_stringlist_dump(denv, address, "addresses") )
					return FALSE;
				break;
			case OPT_MIME:
				sieve_code_dumpf(denv, "mime");	
				break;
			
			default:
				return FALSE;
			}
		}
	}
	
	/* Dump reason and handle operands */
	return 
		sieve_opr_string_dump(denv, address, "reason") &&
		sieve_opr_string_dump(denv, address, "handle");
}

/* 
 * Code execution
 */
 
static int ext_vacation_operation_execute
(const struct sieve_operation *op ATTR_UNUSED,
	const struct sieve_runtime_env *renv, sieve_size_t *address)
{	
	struct sieve_side_effects_list *slist = NULL;
	struct act_vacation_context *act;
	pool_t pool;
	int opt_code = 1;
	sieve_number_t days = 7;
	bool mime = FALSE;
	struct sieve_coded_stringlist *addresses = NULL;
	string_t *reason, *subject = NULL, *from = NULL, *handle = NULL; 
	unsigned int source_line;
	const char *from_normalized = NULL;

	/*
	 * Read operands
	 */
		
	/* Source line */
	if ( !sieve_code_source_line_read(renv, address, &source_line) ) {
		sieve_runtime_trace_error(renv, "invalid source line");
		return SIEVE_EXEC_BIN_CORRUPT;
	}
	
	/* Optional operands */	
	if ( sieve_operand_optional_present(renv->sbin, address) ) {
		while ( opt_code != 0 ) {
			if ( !sieve_operand_optional_read(renv->sbin, address, &opt_code) ) {
				sieve_runtime_trace_error(renv, "invalid optional operand");
				return SIEVE_EXEC_BIN_CORRUPT;
			}

			switch ( opt_code ) {
			case 0:
				break;
			case OPT_DAYS:
				if ( !sieve_opr_number_read(renv, address, &days) ) {
					sieve_runtime_trace_error(renv, 
						"invalid days operand");
					return SIEVE_EXEC_BIN_CORRUPT;
				}
	
				/* Enforce days > 0 (just to be sure) */
				if ( days == 0 )
					days = 1;
				break;
			case OPT_SUBJECT:
				if ( !sieve_opr_string_read(renv, address, &subject) ) {
					sieve_runtime_trace_error(renv, 
						"invalid subject operand");
					return SIEVE_EXEC_BIN_CORRUPT;
				}
				break;
			case OPT_FROM:
				if ( !sieve_opr_string_read(renv, address, &from) ) {
					sieve_runtime_trace_error(renv, 
						"invalid from address operand");
					return SIEVE_EXEC_BIN_CORRUPT;
				}
				break;
			case OPT_ADDRESSES:
				if ( (addresses=sieve_opr_stringlist_read(renv, address))
					== NULL ) {
					sieve_runtime_trace_error(renv, 
						"invalid addresses operand");
					return SIEVE_EXEC_BIN_CORRUPT;
				}
				break;
			case OPT_MIME:
				mime = TRUE;
				break;
			default:
				sieve_runtime_trace_error(renv, 
					"unknown optional operand");
				return SIEVE_EXEC_BIN_CORRUPT;
			}
		}
	}
	
	/* Reason operand */
	if ( !sieve_opr_string_read(renv, address, &reason) ) {
		sieve_runtime_trace_error(renv, "invalid reason operand");
		return SIEVE_EXEC_BIN_CORRUPT;
	}
	
	/* Handle operand */
	if ( !sieve_opr_string_read(renv, address, &handle) ) {
		sieve_runtime_trace_error(renv, "invalid handle operand");
		return SIEVE_EXEC_BIN_CORRUPT;
	}
	
	/*
	 * Perform operation
	 */

	sieve_runtime_trace(renv, "VACATION action");	

	/* Check and normalize :from address */
	if ( from != NULL ) {
		const char *error;

		from_normalized = sieve_address_normalize(from, &error);
	
		if ( from_normalized == NULL) {
			sieve_runtime_error(renv, 
				sieve_error_script_location(renv->script, source_line),
				"specified :from address '%s' is invalid for vacation action: %s",
				str_sanitize(str_c(from), 128), error);
   		}
	}

	/* Add vacation action to the result */

	pool = sieve_result_pool(renv->result);
	act = p_new(pool, struct act_vacation_context, 1);
	act->reason = p_strdup(pool, str_c(reason));
	act->handle = p_strdup(pool, str_c(handle));
	act->days = days;
	act->mime = mime;
	if ( subject != NULL )
		act->subject = p_strdup(pool, str_c(subject));
	if ( from != NULL ) {
		act->from = p_strdup(pool, str_c(from));
		act->from_normalized = p_strdup(pool, from_normalized);
	}

	if ( addresses != NULL )
		sieve_coded_stringlist_read_all(addresses, pool, &(act->addresses));
		
	return ( sieve_result_add_action
		(renv, &act_vacation, slist, source_line, (void *) act, 0) >= 0 );
}

/*
 * Action
 */

/* Runtime verification */

static int act_vacation_check_duplicate
(const struct sieve_runtime_env *renv ATTR_UNUSED,
	const struct sieve_action_data *act, 
	const struct sieve_action_data *act_other)
{
	if ( !act_other->executed ) {
		sieve_runtime_error(renv, act->location, 
			"duplicate vacation action not allowed "
			"(previously triggered one was here: %s)", act_other->location);
		return -1;
	}

	/* Not an error if executed in preceeding script */
	return 1;
}

int act_vacation_check_conflict
(const struct sieve_runtime_env *renv,
	const struct sieve_action_data *act, 
	const struct sieve_action_data *act_other)
{
	if ( (act_other->action->flags & SIEVE_ACTFLAG_SENDS_RESPONSE) > 0 ) {
		if ( !act_other->executed ) {
			sieve_runtime_error(renv, act->location, 
				"vacation action conflicts with other action: "
				"the %s action (%s) also sends a response back to the sender",	
				act_other->action->name, act_other->location);
			return -1;
		} else {
			/* Not an error if executed in preceeding script */
			return 1;
		}
	}

	return 0;
}

/* Result printing */
 
static void act_vacation_print
(const struct sieve_action *action ATTR_UNUSED, 
	const struct sieve_result_print_env *rpenv, void *context, 
	bool *keep ATTR_UNUSED)	
{
	struct act_vacation_context *ctx = (struct act_vacation_context *) context;
	
	sieve_result_action_printf( rpenv, "send vacation message:");
	sieve_result_printf(rpenv, "    => days   : %d\n", ctx->days);
	if ( ctx->subject != NULL )
		sieve_result_printf(rpenv, "    => subject: %s\n", ctx->subject);
	if ( ctx->from != NULL )
		sieve_result_printf(rpenv, "    => from   : %s\n", ctx->from);
	if ( ctx->handle != NULL )
		sieve_result_printf(rpenv, "    => handle : %s\n", ctx->handle);
	sieve_result_printf(rpenv, "\nSTART MESSAGE\n%s\nEND MESSAGE\n", ctx->reason);
}

/* Result execution */

/* Headers known to be associated with mailing lists 
 */
static const char * const _list_headers[] = {
	"list-id",
	"list-owner",
	"list-subscribe",
	"list-post",	
	"list-unsubscribe",
	"list-help",
	"list-archive",
	NULL
};

/* Headers that should be searched for the user's own mail address(es) 
 */

static const char * const _my_address_headers[] = {
	"to",
	"cc",
	"bcc",
	"resent-to",	
	"resent-cc",
	"resent-bcc",
	NULL
};

static inline bool _is_system_address(const char *address)
{
	if ( strncasecmp(address, "MAILER-DAEMON", 13) == 0 )
		return TRUE;

	if ( strncasecmp(address, "LISTSERV", 8) == 0 )
		return TRUE;

	if ( strncasecmp(address, "majordomo", 9) == 0 )
		return TRUE;

	if ( strstr(address, "-request@") != NULL )
		return TRUE;

	if ( strncmp(address, "owner-", 6) == 0 )
		return TRUE;

	return FALSE;
}

static inline bool _contains_my_address
	(const char * const *headers, const char *my_address)
{
	const char *const *hdsp = headers;
	bool result = FALSE;
	
	while ( *hdsp != NULL && !result ) {
		const struct message_address *addr;

		T_BEGIN {
	
			addr = message_address_parse
				(pool_datastack_create(), (const unsigned char *) *hdsp, 
					strlen(*hdsp), 256, FALSE);

			while ( addr != NULL && !result ) {
				if (addr->domain != NULL) {
					i_assert(addr->mailbox != NULL);

					if ( strcmp(t_strconcat(addr->mailbox, "@", addr->domain, NULL),
						my_address) == 0 ) {
						result = TRUE;
						break;
					}
				}

				addr = addr->next;
			}
		} T_END;
		
		hdsp++;
	}
	
	return result;
}

static bool act_vacation_send	
	(const struct sieve_action_exec_env *aenv, struct act_vacation_context *ctx)
{
	const struct sieve_message_data *msgdata = aenv->msgdata;
	const struct sieve_script_env *senv = aenv->scriptenv;
	void *smtp_handle;
	FILE *f;
 	const char *outmsgid;
 	const char *const *headers;
	int ret;

	/* Check smpt functions just to be sure */

	if ( senv->smtp_open == NULL || senv->smtp_close == NULL ) {
		sieve_result_warning(aenv, "vacation action has no means to send mail.");
		return TRUE;
	}

	/* Open smtp session */

	smtp_handle = senv->smtp_open(msgdata->return_path, NULL, &f);
	outmsgid = sieve_message_get_new_id(senv);

	/* Produce a proper reply */

	rfc2822_header_field_write(f, "X-Sieve", SIEVE_IMPLEMENTATION);    
	rfc2822_header_field_write(f, "Message-ID", outmsgid);
	rfc2822_header_field_write(f, "Date", message_date_create(ioloop_time));

	if ( ctx->from != NULL && *(ctx->from) != '\0' )
		rfc2822_header_field_printf(f, "From", "%s", ctx->from);
	else
		rfc2822_header_field_printf(f, "From", "<%s>", msgdata->to_address);
		
	/* FIXME: If From header of message has same address, we should use that in 
	 * stead properly include the phrase part.
	 */
	rfc2822_header_field_printf(f, "To", "<%s>", msgdata->return_path);

	rfc2822_header_field_printf(f, "Subject", "%s", 
		str_sanitize(ctx->subject, 256));

	/* Compose proper in-reply-to and references headers */
	
	ret = mail_get_headers
		(aenv->msgdata->mail, "references", &headers);
			
	if ( msgdata->id != NULL ) {
		rfc2822_header_field_write(f, "In-Reply-To", msgdata->id);
	
		if ( ret >= 0 && headers[0] != NULL )
			rfc2822_header_field_write
				(f, "References", t_strconcat(headers[0], " ", msgdata->id, NULL));
		else
			rfc2822_header_field_write(f, "References", msgdata->id);
	} else if ( ret >= 0 && headers[0] != NULL ) {
		rfc2822_header_field_write(f, "References", headers[0]);
	}
			
	rfc2822_header_field_write(f, "Auto-Submitted", "auto-replied (vacation)");
	rfc2822_header_field_write(f, "Precedence", "bulk");
	
	rfc2822_header_field_write(f, "MIME-Version", "1.0");
    
	if ( !ctx->mime ) {
		rfc2822_header_field_write(f, "Content-Type", "text/plain; charset=utf-8");
		rfc2822_header_field_write(f, "Content-Transfer-Encoding", "8bit");
		fprintf(f, "\r\n");
	}

	fprintf(f, "%s\r\n", ctx->reason);

	/* Close smtp session */    
	if ( !senv->smtp_close(smtp_handle) ) {
		sieve_result_error(aenv, 
			"failed to send vacation response to <%s> "
			"(refer to server log for more information)", 
			str_sanitize(msgdata->return_path, 128));	
		return TRUE;
	}
	
	return TRUE;
}

static void act_vacation_hash
(const struct sieve_message_data *msgdata, struct act_vacation_context *vctx, 
	unsigned char hash_r[])
{
	struct md5_context ctx;

	md5_init(&ctx);
	md5_update(&ctx, msgdata->return_path, strlen(msgdata->return_path));

	md5_update(&ctx, vctx->handle, strlen(vctx->handle));
	
	md5_final(&ctx, hash_r);
}

static bool act_vacation_commit
(const struct sieve_action *action ATTR_UNUSED, 
	const struct sieve_action_exec_env *aenv, void *tr_context, 
	bool *keep ATTR_UNUSED)
{
	const char *const *hdsp;
	const struct sieve_message_data *msgdata = aenv->msgdata;
	const struct sieve_script_env *senv = aenv->scriptenv;
	struct act_vacation_context *ctx = (struct act_vacation_context *) tr_context;
	unsigned char dupl_hash[MD5_RESULTLEN];
	const char *const *headers;
	pool_t pool;

	/* Is the return_path unset ?
	 */
	if ( msgdata->return_path == NULL || *(msgdata->return_path) == '\0' ) {
		sieve_result_log(aenv, "discarded vacation reply to <>");
		return TRUE;
	}    
	
	/* Are we perhaps trying to respond to ourselves ? 
	 * (FIXME: verify this to :addresses as well?)
	 */
	if ( strcmp(msgdata->return_path, msgdata->to_address) == 0 ) {
		sieve_result_log(aenv, "discarded vacation reply to own address");	
		return TRUE;
	}
	
	/* Did whe respond to this user before? */
	if ( senv->duplicate_check != NULL ) {
		act_vacation_hash(msgdata, ctx, dupl_hash);
	
		if ( senv->duplicate_check(dupl_hash, sizeof(dupl_hash), senv->username) ) 
		{
			sieve_result_log(aenv, "discarded duplicate vacation response to <%s>",
				str_sanitize(msgdata->return_path, 128));
			return TRUE;
		}
	}
	
	/* Are we trying to respond to a mailing list ? */
	hdsp = _list_headers;
	while ( *hdsp != NULL ) {
		if ( mail_get_headers
			(msgdata->mail, *hdsp, &headers) >= 0 && headers[0] != NULL ) {	
			/* Yes, bail out */
			sieve_result_log(aenv, 
				"discarding vacation response to mailinglist recipient <%s>", 
				str_sanitize(msgdata->return_path, 128));	
			return TRUE;				 
		}
		hdsp++;
	}
	
	/* Is the message that we are replying to an automatic reply ? */
	if ( mail_get_headers
		(msgdata->mail, "auto-submitted", &headers) >= 0 ) {
		/* Theoretically multiple headers could exist, so lets make sure */
		hdsp = headers;
		while ( *hdsp != NULL ) {
			if ( strcasecmp(*hdsp, "no") != 0 ) {
				sieve_result_log(aenv, 
					"discardig vacation response to auto-submitted message from <%s>", 
					str_sanitize(msgdata->return_path, 128));	
					return TRUE;				 
			}
			hdsp++;
		}
	}
	
	/* Check for the non-standard precedence header */
	if ( mail_get_headers
		(msgdata->mail, "precedence", &headers) >= 0 ) {
		/* Theoretically multiple headers could exist, so lets make sure */
		hdsp = headers;
		while ( *hdsp != NULL ) {
			if ( strcasecmp(*hdsp, "junk") == 0 || strcasecmp(*hdsp, "bulk") == 0 ||
				strcasecmp(*hdsp, "list") == 0 ) {
				sieve_result_log(aenv, 
					"discarding vacation response to precedence=%s message from <%s>", 
					*hdsp, str_sanitize(msgdata->return_path, 128));	
					return TRUE;				 
			}
			hdsp++;
		}
	}
	
	/* Do not reply to system addresses */
	if ( _is_system_address(msgdata->return_path) ) {
		sieve_result_log(aenv, 
			"not sending vacation response to system address <%s>", 
			str_sanitize(msgdata->return_path, 128));	
		return TRUE;				
	} 
	
	/* Is the original message directly addressed to the user or the addresses
	 * specified using the :addresses tag? 
	 */
	hdsp = _my_address_headers;
	while ( *hdsp != NULL ) {
		if ( mail_get_headers_utf8
			(msgdata->mail, *hdsp, &headers) >= 0 && headers[0] != NULL ) {	
			
			if ( _contains_my_address(headers, msgdata->to_address) ) 
				break;
			
			if ( ctx->addresses != NULL ) {
				bool found = FALSE;
				const char * const *my_address = ctx->addresses;
		
				while ( !found && *my_address != NULL ) {
					found = _contains_my_address(headers, *my_address);
					my_address++;
				}
				
				if ( found ) break;
			}
		}
		hdsp++;
	}	

	if ( *hdsp == NULL ) {
		/* No, bail out */
		sieve_result_log(aenv, 
			"discarding vacation response for message implicitly delivered to <%s>",
			( msgdata->to_address == NULL ? "UNKNOWN" : msgdata->to_address ) );	
		return TRUE;				 
	}	
		
	/* Make sure we have a subject for our reply */
	if ( ctx->subject == NULL || *(ctx->subject) == '\0' ) {
		if ( mail_get_headers_utf8
			(msgdata->mail, "subject", &headers) >= 0 && headers[0] != NULL ) {
			pool = sieve_result_pool(aenv->result);
			ctx->subject = p_strconcat(pool, "Auto: ", headers[0], NULL);
		}	else {
			ctx->subject = "Automated reply";
		}
	}	
	
	/* Send the message */
	
	if ( act_vacation_send(aenv, ctx) ) {
		sieve_result_log(aenv, "sent vacation response to <%s>", 
			str_sanitize(msgdata->return_path, 128));	

		/* Mark as replied */
		if ( senv->duplicate_mark != NULL )
			senv->duplicate_mark(dupl_hash, sizeof(dupl_hash), senv->username,
				ioloop_time + ctx->days * (24 * 60 * 60));

		return TRUE;
	}

	return FALSE;
}




