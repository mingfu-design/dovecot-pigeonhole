#ifndef __SIEVE_OBJECTS_H
#define __SIEVE_OBJECTS_H

struct sieve_object {
	const char *identifier;
	const struct sieve_operand *operand;
	unsigned int code;
};

#define SIEVE_OBJECT(identifier, operand, code) \
	{ identifier, operand, code }

void sieve_opr_object_emit
	(struct sieve_binary *sbin, const struct sieve_object *obj, int ext_id);

const struct sieve_object *sieve_opr_object_read_data
	(struct sieve_binary *sbin, const struct sieve_operand *operand,
		const struct sieve_operand_class *opclass, sieve_size_t *address);

const struct sieve_object *sieve_opr_object_read
	(const struct sieve_runtime_env *renv, 
		const struct sieve_operand_class *opclass, sieve_size_t *address);

bool sieve_opr_object_dump
	(const struct sieve_dumptime_env *denv, 
		const struct sieve_operand_class *opclass, sieve_size_t *address,
		const struct sieve_object **object_r);


#endif /* __SIEVE_OBJECTS_H */
