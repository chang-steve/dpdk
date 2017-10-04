/*-
 *   BSD LICENSE
 *
 *   Copyright(c) 2017 Intel Corporation. All rights reserved.
 *   All rights reserved.
 *
 *   Redistribution and use in source and binary forms, with or without
 *   modification, are permitted provided that the following conditions
 *   are met:
 *
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in
 *       the documentation and/or other materials provided with the
 *       distribution.
 *     * Neither the name of Intel Corporation nor the names of its
 *       contributors may be used to endorse or promote products derived
 *       from this software without specific prior written permission.
 *
 *   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *   "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 *   A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 *   OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 *   SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 *   LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 *   DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 *   THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 *   (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 *   OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <string.h>

#include <rte_eal.h>
#include <rte_eal_memconfig.h>
#include <rte_memory.h>
#include <rte_memzone.h>
#include <rte_malloc.h>
#include <rte_errno.h>

#include "rte_member.h"
#include "rte_member_ht.h"

int librte_member_logtype;

TAILQ_HEAD(rte_member_list, rte_tailq_entry);
static struct rte_tailq_elem rte_member_tailq = {
	.name = "RTE_MEMBER",
};
EAL_REGISTER_TAILQ(rte_member_tailq)

struct rte_member_setsum *
rte_member_find_existing(const char *name)
{
	struct rte_member_setsum *setsum = NULL;
	struct rte_tailq_entry *te;
	struct rte_member_list *member_list;

	member_list = RTE_TAILQ_CAST(rte_member_tailq.head, rte_member_list);

	rte_rwlock_read_lock(RTE_EAL_TAILQ_RWLOCK);
	TAILQ_FOREACH(te, member_list, next) {
		setsum = (struct rte_member_setsum *) te->data;
		if (strncmp(name, setsum->name, RTE_MEMBER_NAMESIZE) == 0)
			break;
	}
	rte_rwlock_read_unlock(RTE_EAL_TAILQ_RWLOCK);

	if (te == NULL) {
		rte_errno = ENOENT;
		return NULL;
	}
	return setsum;
}

void
rte_member_free(struct rte_member_setsum *setsum)
{
	struct rte_member_list *member_list;
	struct rte_tailq_entry *te;

	if (setsum == NULL)
		return;
	member_list = RTE_TAILQ_CAST(rte_member_tailq.head, rte_member_list);
	rte_rwlock_write_lock(RTE_EAL_TAILQ_RWLOCK);
	TAILQ_FOREACH(te, member_list, next) {
		if (te->data == (void *)setsum)
			break;
	}
	if (te == NULL) {
		rte_rwlock_write_unlock(RTE_EAL_TAILQ_RWLOCK);
		return;
	}
	TAILQ_REMOVE(member_list, te, next);
	rte_rwlock_write_unlock(RTE_EAL_TAILQ_RWLOCK);

	switch (setsum->type) {
	case RTE_MEMBER_TYPE_HT:
		rte_member_free_ht(setsum);
		break;
	default:
		break;
	}
	rte_free(setsum);
	rte_free(te);
}

struct rte_member_setsum *
rte_member_create(const struct rte_member_parameters *params)
{
	struct rte_tailq_entry *te;
	struct rte_member_list *member_list;
	struct rte_member_setsum *setsum;
	int ret;

	if (params == NULL) {
		rte_errno = EINVAL;
		return NULL;
	}

	if (params->key_len == 0 ||
			params->prim_hash_seed == params->sec_hash_seed) {
		rte_errno = EINVAL;
		RTE_MEMBER_LOG(ERR, "Create setsummary with "
					"invalid parameters\n");
		return NULL;
	}

	member_list = RTE_TAILQ_CAST(rte_member_tailq.head, rte_member_list);

	rte_rwlock_write_lock(RTE_EAL_TAILQ_RWLOCK);

	TAILQ_FOREACH(te, member_list, next) {
		setsum = (struct rte_member_setsum *) te->data;
		if (strncmp(params->name, setsum->name,
				RTE_MEMBER_NAMESIZE) == 0)
			break;
	}
	setsum = NULL;
	if (te != NULL) {
		rte_errno = EEXIST;
		te = NULL;
		goto error_unlock_exit;
	}
	te = rte_zmalloc("MEMBER_TAILQ_ENTRY", sizeof(*te), 0);
	if (te == NULL) {
		RTE_MEMBER_LOG(ERR, "tailq entry allocation failed\n");
		goto error_unlock_exit;
	}

	/* Create a new setsum structure */
	setsum = (struct rte_member_setsum *) rte_zmalloc_socket(params->name,
			sizeof(struct rte_member_setsum), RTE_CACHE_LINE_SIZE,
			params->socket_id);
	if (setsum == NULL) {
		RTE_MEMBER_LOG(ERR, "Create setsummary failed\n");
		goto error_unlock_exit;
	}
	snprintf(setsum->name, sizeof(setsum->name), "%s", params->name);
	setsum->type = params->type;
	setsum->socket_id = params->socket_id;
	setsum->key_len = params->key_len;
	setsum->num_set = params->num_set;
	setsum->prim_hash_seed = params->prim_hash_seed;
	setsum->sec_hash_seed = params->sec_hash_seed;

	switch (setsum->type) {
	case RTE_MEMBER_TYPE_HT:
		ret = rte_member_create_ht(setsum, params);
		break;
	default:
		goto error_unlock_exit;
	}
	if (ret < 0)
		goto error_unlock_exit;

	RTE_MEMBER_LOG(DEBUG, "Creating a setsummary table with "
			"mode %u\n", setsum->type);

	te->data = (void *)setsum;
	TAILQ_INSERT_TAIL(member_list, te, next);
	rte_rwlock_write_unlock(RTE_EAL_TAILQ_RWLOCK);
	return setsum;

error_unlock_exit:
	rte_rwlock_write_unlock(RTE_EAL_TAILQ_RWLOCK);
	rte_member_free(setsum);
	return NULL;
}

int
rte_member_add(const struct rte_member_setsum *setsum, const void *key,
			member_set_t set_id)
{
	if (setsum == NULL || key == NULL)
		return -EINVAL;

	switch (setsum->type) {
	case RTE_MEMBER_TYPE_HT:
		return rte_member_add_ht(setsum, key, set_id);
	default:
		return -EINVAL;
	}
}

int
rte_member_lookup(const struct rte_member_setsum *setsum, const void *key,
			member_set_t *set_id)
{
	if (setsum == NULL || key == NULL || set_id == NULL)
		return -EINVAL;

	switch (setsum->type) {
	case RTE_MEMBER_TYPE_HT:
		return rte_member_lookup_ht(setsum, key, set_id);
	default:
		return -EINVAL;
	}
}

int
rte_member_lookup_bulk(const struct rte_member_setsum *setsum,
				const void **keys, uint32_t num_keys,
				member_set_t *set_ids)
{
	if (setsum == NULL || keys == NULL || set_ids == NULL)
		return -EINVAL;

	switch (setsum->type) {
	case RTE_MEMBER_TYPE_HT:
		return rte_member_lookup_bulk_ht(setsum, keys, num_keys,
				set_ids);
	default:
		return -EINVAL;
	}
}

int
rte_member_lookup_multi(const struct rte_member_setsum *setsum, const void *key,
				uint32_t match_per_key, member_set_t *set_id)
{
	if (setsum == NULL || key == NULL || set_id == NULL)
		return -EINVAL;

	switch (setsum->type) {
	case RTE_MEMBER_TYPE_HT:
		return rte_member_lookup_multi_ht(setsum, key, match_per_key,
				set_id);
	default:
		return -EINVAL;
	}
}

int
rte_member_lookup_multi_bulk(const struct rte_member_setsum *setsum,
			const void **keys, uint32_t num_keys,
			uint32_t max_match_per_key, uint32_t *match_count,
			member_set_t *set_ids)
{
	if (setsum == NULL || keys == NULL || set_ids == NULL ||
			match_count == NULL)
		return -EINVAL;

	switch (setsum->type) {
	case RTE_MEMBER_TYPE_HT:
		return rte_member_lookup_multi_bulk_ht(setsum, keys, num_keys,
				max_match_per_key, match_count, set_ids);
	default:
		return -EINVAL;
	}
}

int
rte_member_delete(const struct rte_member_setsum *setsum, const void *key,
			member_set_t set_id)
{
	if (setsum == NULL || key == NULL)
		return -EINVAL;

	switch (setsum->type) {
	case RTE_MEMBER_TYPE_HT:
		return rte_member_delete_ht(setsum, key, set_id);
	default:
		return -EINVAL;
	}
}

void
rte_member_reset(const struct rte_member_setsum *setsum)
{
	if (setsum == NULL)
		return;
	switch (setsum->type) {
	case RTE_MEMBER_TYPE_HT:
		rte_member_reset_ht(setsum);
		return;
	default:
		return;
	}
}

RTE_INIT(librte_member_init_log);

static void
librte_member_init_log(void)
{
	librte_member_logtype = rte_log_register("librte.member");
	if (librte_member_logtype >= 0)
		rte_log_set_level(librte_member_logtype, RTE_LOG_DEBUG);
}