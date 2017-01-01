/* Copyright (c) 2002-2017 Pigeonhole authors, see the included COPYING file
 */

#include "lib.h"
#include "md5.h"
#include "ioloop.h"
#include "str.h"
#include "str-sanitize.h"
#include "array.h"

#include "sieve-common.h"
#include "sieve-settings.h"
#include "sieve-error.h"
#include "sieve-extensions.h"
#include "sieve-message.h"
#include "sieve-code.h"
#include "sieve-runtime.h"
#include "sieve-interpreter.h"
#include "sieve-actions.h"
#include "sieve-result.h"

#include "ext-duplicate-common.h"

/*
 * Extension configuration
 */

#define EXT_DUPLICATE_DEFAULT_PERIOD (12*60*60)
#define EXT_DUPLICATE_DEFAULT_MAX_PERIOD (2*24*60*60)

bool ext_duplicate_load
(const struct sieve_extension *ext, void **context)
{
	struct sieve_instance *svinst = ext->svinst;
	struct ext_duplicate_config *config;
	sieve_number_t default_period, max_period;

	if ( *context != NULL )
		ext_duplicate_unload(ext);

	if ( !sieve_setting_get_duration_value
		(svinst, "sieve_duplicate_default_period", &default_period) ) {
		default_period = EXT_DUPLICATE_DEFAULT_PERIOD;
	}

	if ( !sieve_setting_get_duration_value
		(svinst, "sieve_duplicate_max_period", &max_period) ) {
		max_period = EXT_DUPLICATE_DEFAULT_MAX_PERIOD;
	}

	config = i_new(struct ext_duplicate_config, 1);
	config->default_period = default_period;
	config->max_period = max_period;

	*context = (void *) config;
	return TRUE;
}

void ext_duplicate_unload
(const struct sieve_extension *ext)
{
	struct ext_duplicate_config *config =
		(struct ext_duplicate_config *) ext->context;

	i_free(config);
}

/*
 * Duplicate_mark action
 */

struct act_duplicate_mark_data {
	const char *handle;
	unsigned int period;
	unsigned char hash[MD5_RESULTLEN];
	bool last:1;
};

static void act_duplicate_mark_print
	(const struct sieve_action *action,
		const struct sieve_result_print_env *rpenv, bool *keep);
static void act_duplicate_mark_finish
	(const struct sieve_action *action,
		const struct sieve_action_exec_env *aenv, void *tr_context, int status);

static const struct sieve_action_def act_duplicate_mark = {
	.name = "duplicate_mark",
	.print = act_duplicate_mark_print,
	.finish = act_duplicate_mark_finish
};

static void act_duplicate_mark_print
(const struct sieve_action *action,
	const struct sieve_result_print_env *rpenv, bool *keep ATTR_UNUSED)
{
	struct act_duplicate_mark_data *data =
		(struct act_duplicate_mark_data *) action->context;
	const char *last = (data->last ? " last" : "");

	if (data->handle != NULL) {
		sieve_result_action_printf(rpenv, "track%s duplicate with handle: %s",
			last, str_sanitize(data->handle, 128));
	} else {
		sieve_result_action_printf(rpenv, "track%s duplicate", last);
	}
}

static void act_duplicate_mark_finish
(const struct sieve_action *action,
	const struct sieve_action_exec_env *aenv,
	void *tr_context ATTR_UNUSED, int status)
{
	const struct sieve_script_env *senv = aenv->scriptenv;
	struct act_duplicate_mark_data *data =
		(struct act_duplicate_mark_data *) action->context;

	if ( status == SIEVE_EXEC_OK ) {
		/* Message was handled successfully, so track duplicate for this
		 * message.
		 */
		sieve_action_duplicate_mark
			(senv, data->hash, sizeof(data->hash), ioloop_time + data->period);
	}
}

/*
 * Duplicate checking
 */

struct ext_duplicate_handle {
	const char *handle;
	bool last:1;
	bool duplicate:1;
};

struct ext_duplicate_context {
	ARRAY(struct ext_duplicate_handle) handles;

	bool nohandle_duplicate:1;
	bool nohandle_checked:1;
};

static void ext_duplicate_hash
(string_t *handle, const char *value, size_t value_len, bool last,
	unsigned char hash_r[])
{
	static const char *id = "sieve duplicate";
	struct md5_context md5ctx;

	md5_init(&md5ctx);
	md5_update(&md5ctx, id, strlen(id));
	if (last)
		md5_update(&md5ctx, "0", 1);
	else
		md5_update(&md5ctx, "+", 1);
	if (handle != NULL) {
		md5_update(&md5ctx, "h-", 2);
		md5_update(&md5ctx, str_c(handle), str_len(handle));
	} else {
		md5_update(&md5ctx, "default", 7);
	}
	md5_update(&md5ctx, value, value_len);
	md5_final(&md5ctx, hash_r);
}

int ext_duplicate_check
(const struct sieve_runtime_env *renv, string_t *handle,
	const char *value, size_t value_len, sieve_number_t period,
	bool last)
{
	const struct sieve_extension *this_ext = renv->oprtn->ext;
	const struct sieve_script_env *senv = renv->scriptenv;
	struct ext_duplicate_context *rctx;
	bool duplicate = FALSE;
	pool_t msg_pool = NULL, result_pool = NULL;
	struct act_duplicate_mark_data *act;

	if ( !sieve_action_duplicate_check_available(senv) ) {
		sieve_runtime_warning(renv, NULL, "duplicate test: "
			"duplicate checking not available in this context");
		return 0;
	}
		
	if ( value == NULL )
		return 0;

	/* Get context; find out whether duplicate was checked earlier */
	rctx = (struct ext_duplicate_context *)
		sieve_message_context_extension_get(renv->msgctx, this_ext);

	if ( rctx == NULL ) {
		/* Create context */
		msg_pool = sieve_message_context_pool(renv->msgctx);
		rctx = p_new(msg_pool, struct ext_duplicate_context, 1);
		sieve_message_context_extension_set(renv->msgctx, this_ext, (void *)rctx);
	} else {
		if ( handle == NULL ) {
			if ( rctx->nohandle_checked  ) {
				/* Already checked for duplicate */
				return ( rctx->nohandle_duplicate ? 1 : 0 );
			}
		} else if ( array_is_created(&rctx->handles) ) {
			const struct ext_duplicate_handle *record;
			array_foreach (&rctx->handles, record) {
				if ( strcmp(record->handle, str_c(handle)) == 0 &&
					record->last == last )
					return ( record->duplicate ? 1 : 0 );
			}
		}
	}

	result_pool = sieve_result_pool(renv->result);
	act = p_new(result_pool, struct act_duplicate_mark_data, 1);
	if (handle != NULL)
		act->handle = p_strdup(result_pool, str_c(handle));
	act->period = period;
	act->last = last;

	/* Create hash */
	ext_duplicate_hash(handle, value, value_len, last, act->hash);

	/* Check duplicate */
	duplicate = sieve_action_duplicate_check(senv, act->hash, sizeof(act->hash));

	if (!duplicate && last) {
		unsigned char no_last_hash[MD5_RESULTLEN];

		/* Check for entry without :last */
		ext_duplicate_hash(handle, value, value_len, FALSE, no_last_hash);
		sieve_action_duplicate_check(senv, no_last_hash, sizeof(no_last_hash));
	}

	/* We may only mark the message as duplicate when Sieve script executes
	 * successfully; therefore defer this operation until successful result
	 * execution.
	 */
	if (!duplicate || last) {
		if ( sieve_result_add_action
			(renv, NULL, &act_duplicate_mark, NULL, (void *) act, 0, FALSE) < 0 )
			return -1;
	}

	/* Cache result */
	if ( handle == NULL ) {
		rctx->nohandle_duplicate = duplicate;
		rctx->nohandle_checked = TRUE;
	} else {
		struct ext_duplicate_handle *record;

		if ( msg_pool == NULL )
			msg_pool = sieve_message_context_pool(renv->msgctx);
		if ( !array_is_created(&rctx->handles) )
			p_array_init(&rctx->handles, msg_pool, 64);
		record = array_append_space(&rctx->handles);
		record->handle = p_strdup(msg_pool, str_c(handle));
		record->last = last;
		record->duplicate = duplicate;
	}

	return ( duplicate ? 1 : 0 );
}

