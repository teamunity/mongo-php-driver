#include "collection.h"
#include "types.h"
#include "read_preference.h"
#include "manager.h"
#include "str.h"
#include "utils.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Helpers */
static char *mongo_connection_type(int type)
{
	switch (type) {
		case MONGO_NODE_STANDALONE: return "STANDALONE";
		case MONGO_NODE_PRIMARY: return "PRIMARY";
		case MONGO_NODE_SECONDARY: return "SECONDARY";
		case MONGO_NODE_ARBITER: return "ARBITER";
		case MONGO_NODE_MONGOS: return "MONGOS";
		default:
			return "UNKNOWN?";
	}
}
static void mongo_print_connection_info(mongo_con_manager *manager, mongo_connection *con, int level)
{
	int i;

	mongo_manager_log(manager, MLOG_RS, level,
		"- connection: type: %s, socket: %d, ping: %d, hash: %s",
		mongo_connection_type(con->connection_type),
		con->socket,
		con->ping_ms,
		con->hash
	);
	for (i = 0; i < con->tag_count; i++) {
		mongo_manager_log(manager, MLOG_RS, level,
			"  - tag: %s", con->tags[i]
		);
	}
}

void mongo_print_connection_iterate_wrapper(mongo_con_manager *manager, void *elem)
{
	mongo_connection *con = (mongo_connection*) elem;

	mongo_print_connection_info(manager, con, MLOG_FINE);
}

char *mongo_read_preference_type_to_name(int type)
{
	switch (type) {
		case MONGO_RP_PRIMARY:             return "primary";
		case MONGO_RP_PRIMARY_PREFERRED:   return "primary preferred";
		case MONGO_RP_SECONDARY:           return "secondary";
		case MONGO_RP_SECONDARY_PREFERRED: return "secondary preferred";
		case MONGO_RP_NEAREST:             return "nearest";
	}
	return "unknown";
}

/* Collecting the correct servers */
static mcon_collection *filter_connections(mongo_con_manager *manager, int types, mongo_read_preference *rp)
{
	mcon_collection *col;
	mongo_con_manager_item *ptr = manager->connections;
	col = mcon_init_collection(sizeof(mongo_connection*));

	mongo_manager_log(manager, MLOG_RS, MLOG_FINE, "filter_connections: adding connections:");
	while (ptr) {
		if (ptr->connection->connection_type & types) {
			mongo_print_connection_info(manager, ptr->connection, MLOG_FINE);
			mcon_collection_add(col, ptr->connection);
		}
		ptr = ptr->next;
	}
	mongo_manager_log(manager, MLOG_RS, MLOG_FINE, "filter_connections: done");

	return col;
}

/* Wrappers for the different collection types */
static mcon_collection *mongo_rp_collect_primary(mongo_con_manager *manager, mongo_read_preference *rp)
{
	return filter_connections(manager, MONGO_NODE_PRIMARY, rp);
}

static mcon_collection *mongo_rp_collect_primary_and_secondary(mongo_con_manager *manager, mongo_read_preference *rp)
{
	return filter_connections(manager, MONGO_NODE_PRIMARY | MONGO_NODE_SECONDARY, rp);
}

static mcon_collection *mongo_rp_collect_secondary(mongo_con_manager *manager, mongo_read_preference *rp)
{
	return filter_connections(manager, MONGO_NODE_SECONDARY, rp);
}

static mcon_collection *mongo_rp_collect_any(mongo_con_manager *manager, mongo_read_preference *rp)
{
	/* We add the MONGO_NODE_STANDALONE and MONGO_NODE_MONGOS here, because
	 * that's needed for the MULTIPLE connection type. Right now, that only is
	 * used for MONGO_RP_NEAREST, and MONGO_RP_NEAREST uses this function (see
	 * below in mongo_find_all_candidate_servers(). */
	return filter_connections(manager, MONGO_NODE_STANDALONE | MONGO_NODE_PRIMARY | MONGO_NODE_SECONDARY | MONGO_NODE_MONGOS, rp);
}

static mcon_collection* mongo_find_all_candidate_servers(mongo_con_manager *manager, mongo_read_preference *rp)
{
	mongo_manager_log(manager, MLOG_RS, MLOG_FINE, "- all servers");
	/* Depending on read preference type, run the correct algorithm */
	switch (rp->type) {
		case MONGO_RP_PRIMARY:
			return mongo_rp_collect_primary(manager, rp);
			break;
		case MONGO_RP_PRIMARY_PREFERRED:
		case MONGO_RP_SECONDARY_PREFERRED:
			return mongo_rp_collect_primary_and_secondary(manager, rp);
			break;
		case MONGO_RP_SECONDARY:
			return mongo_rp_collect_secondary(manager, rp);
			break;
		case MONGO_RP_NEAREST:
			return mongo_rp_collect_any(manager, rp);
			break;
		default:
			return NULL;
	}
}

static int candidate_matches_tags(mongo_con_manager *manager, mongo_connection *con, mongo_read_preference_tagset *tagset)
{
	int i, j, found = 0;

	mongo_manager_log(manager, MLOG_RS, MLOG_FINE, "candidate_matches_tags: checking tags on %s", con->hash);
	for (i = 0; i < tagset->tag_count; i++) {
		for (j = 0; j < con->tag_count; j++) {
			if (strcmp(tagset->tags[i], con->tags[j]) == 0) {
				found++;
				mongo_manager_log(manager, MLOG_RS, MLOG_FINE, "candidate_matches_tags: found %s", con->tags[j]);
			}
		}
	}
	if (found == tagset->tag_count) {
		mongo_manager_log(manager, MLOG_RS, MLOG_FINE, "candidate_matches_tags: all tags matched for %s", con->hash);
		return 1;
	} else {
		mongo_manager_log(manager, MLOG_RS, MLOG_FINE, "candidate_matches_tags: not all tags matched for %s", con->hash);
		return 0;
	}
}

static mcon_collection* mongo_filter_candidates_by_tagset(mongo_con_manager *manager, mcon_collection *candidates, mongo_read_preference_tagset *tagset)
{
	int              i;
	mcon_collection *tmp;

	tmp = mcon_init_collection(sizeof(mongo_connection*));
	for (i = 0; i < candidates->count; i++) {
		if (candidate_matches_tags(manager, (mongo_connection *) candidates->data[i], tagset)) {
			mcon_collection_add(tmp, candidates->data[i]);
		}
	}
	return tmp;
}

char *mongo_read_preference_squash_tagset(mongo_read_preference_tagset *tagset)
{
	int    i;
	struct mcon_str str = { 0 };

	for (i = 0; i < tagset->tag_count; i++) {
		if (i) {
			mcon_str_addl(&str, ", ", 2, 0);
		}
		mcon_str_add(&str, tagset->tags[i], 0);
	}
	return str.d;
}

mcon_collection* mongo_find_candidate_servers(mongo_con_manager *manager, mongo_read_preference *rp)
{
	int              i;
	mcon_collection *all, *filtered;

	mongo_manager_log(manager, MLOG_RS, MLOG_FINE, "finding candidate servers");
	all = mongo_find_all_candidate_servers(manager, rp);
	if (rp->tagset_count != 0) {
		mongo_manager_log(manager, MLOG_RS, MLOG_FINE, "limiting by tagsets");
		/* If we have tagsets configured for the replicaset then we need to do
		 * some more filtering */
		for (i = 0; i < rp->tagset_count; i++) {
			char *tmp_ts = mongo_read_preference_squash_tagset(rp->tagsets[i]);

			mongo_manager_log(manager, MLOG_RS, MLOG_FINE, "checking tagset: %s", tmp_ts);
			filtered = mongo_filter_candidates_by_tagset(manager, all, rp->tagsets[i]);
			mongo_manager_log(manager, MLOG_RS, MLOG_FINE, "tagset %s matched %d candidates", tmp_ts, filtered->count);
			free(tmp_ts);

			if (filtered->count > 0) {
				mcon_collection_free(all);
				return filtered;
			}
		}
		mcon_collection_free(all);
		return NULL;
	} else {
		return all;
	}
}

/* Sorting the servers */
static int mongo_rp_sort_primary_preferred(const void* a, const void *b)
{
	mongo_connection *con_a = *(mongo_connection**) a;
	mongo_connection *con_b = *(mongo_connection**) b;

	/* First we prefer primary over secondary, and if the field type is the
	 * same, we sort on ping_ms again. *_SECONDARY is a higher constant value
	 * than *_PRIMARY, so we sort descendingly by connection_type */
	if (con_a->connection_type > con_b->connection_type) {
		return 1;
	} else if (con_a->connection_type < con_b->connection_type) {
		return -1;
	} else {
		if (con_a->ping_ms > con_b->ping_ms) {
			return 1;
		} else if (con_a->ping_ms < con_b->ping_ms) {
			return -1;
		}
	}
	return 0;
}

static int mongo_rp_sort_secondary_preferred(const void* a, const void *b)
{
	mongo_connection *con_a = *(mongo_connection**) a;
	mongo_connection *con_b = *(mongo_connection**) b;

	/* First we prefer secondary over primary, and if the field type is the
	 * same, we sort on ping_ms again. *_SECONDARY is a higher constant value
	 * than *_PRIMARY. */
	if (con_a->connection_type < con_b->connection_type) {
		return 1;
	} else if (con_a->connection_type > con_b->connection_type) {
		return -1;
	} else {
		if (con_a->ping_ms > con_b->ping_ms) {
			return 1;
		} else if (con_a->ping_ms < con_b->ping_ms) {
			return -1;
		}
	}
	return 0;
}

static int mongo_rp_sort_any(const void* a, const void *b)
{
	mongo_connection *con_a = *(mongo_connection**) a;
	mongo_connection *con_b = *(mongo_connection**) b;

	if (con_a->ping_ms > con_b->ping_ms) {
		return 1;
	} else if (con_a->ping_ms < con_b->ping_ms) {
		return -1;
	}
	return 0;
}

/* This method is the master for selecting the correct algorithm for the order
 * of servers in which to try the candidate servers that we've previously found */
mcon_collection *mongo_sort_servers(mongo_con_manager *manager, mcon_collection *col, mongo_read_preference *rp)
{
	mongo_connection_sort_t *sort_function;

	switch (rp->type) {
		case MONGO_RP_PRIMARY:
			/* Should not really have to do anything as there is only going to
			 * be one server */
			sort_function = mongo_rp_sort_any;
			break;

		case MONGO_RP_PRIMARY_PREFERRED:
			sort_function = mongo_rp_sort_primary_preferred;
			break;

		case MONGO_RP_SECONDARY:
			sort_function = mongo_rp_sort_any;
			break;

		case MONGO_RP_SECONDARY_PREFERRED:
			sort_function = mongo_rp_sort_secondary_preferred;
			break;

		case MONGO_RP_NEAREST:
			sort_function = mongo_rp_sort_any;
			break;

		default:
			return NULL;
	}
	mongo_manager_log(manager, MLOG_RS, MLOG_FINE, "mongo_sort_servers: sorting");
	qsort(col->data, col->count, sizeof(mongo_connection*), sort_function);
	mcon_collection_iterate(manager, col, mongo_print_connection_iterate_wrapper);
	mongo_manager_log(manager, MLOG_RS, MLOG_FINE, "mongo_sort_servers: done");
	return col;
}

mcon_collection *mongo_select_nearest_servers(mongo_con_manager *manager, mcon_collection *col, mongo_read_preference *rp)
{
	mcon_collection *filtered;
	int              i, nearest_ping;

	filtered = mcon_init_collection(sizeof(mongo_connection*));

	mongo_manager_log(manager, MLOG_RS, MLOG_FINE, "select server: only nearest");

	switch (rp->type) {
		case MONGO_RP_PRIMARY:
		case MONGO_RP_PRIMARY_PREFERRED:
		case MONGO_RP_SECONDARY:
		case MONGO_RP_SECONDARY_PREFERRED:
		case MONGO_RP_NEAREST:
			/* The nearest ping time is in the first element */
			nearest_ping = ((mongo_connection*)col->data[0])->ping_ms;
			mongo_manager_log(manager, MLOG_RS, MLOG_FINE, "select server: nearest is %dms", nearest_ping);

			/* FIXME: Change to iterator later */
			for (i = 0; i < col->count; i++) {
				if (((mongo_connection*)col->data[i])->ping_ms <= nearest_ping + MONGO_RP_CUTOFF) {
					mcon_collection_add(filtered, col->data[i]);
				}
			}
			break;

		default:
			return NULL;
	}

	/* Clean up the old collection that we no longer need */
	mcon_collection_free(col);

	return filtered;
}

mongo_connection *mongo_pick_server_from_set(mongo_con_manager *manager, mcon_collection *col, mongo_read_preference *rp)
{
	mongo_connection *con = NULL;
	int entry = rand() % col->count;

	if (rp->type == MONGO_RP_PRIMARY_PREFERRED) {
		if (((mongo_connection*)col->data[0])->connection_type == MONGO_NODE_PRIMARY) {
			mongo_manager_log(manager, MLOG_RS, MLOG_FINE, "pick server: the primary");
			con = (mongo_connection*)col->data[0];
			mongo_print_connection_info(manager, con, MLOG_INFO);
			return con;
		}
	}
	/* For now, we just pick a random server from the set */
	mongo_manager_log(manager, MLOG_RS, MLOG_FINE, "pick server: random element %d", entry);
	con = (mongo_connection*)col->data[entry];
	mongo_print_connection_info(manager, con, MLOG_INFO);
	return con;
}

/* Tagset helpers */
void mongo_read_preference_add_tag(mongo_read_preference_tagset *tagset, char *name, char *value)
{
	tagset->tag_count++;
	tagset->tags = realloc(tagset->tags, tagset->tag_count * sizeof(char*));
	tagset->tags[tagset->tag_count - 1] = malloc(strlen(name) + strlen(value) + 2);
	snprintf(tagset->tags[tagset->tag_count - 1], strlen(name) + strlen(value) + 2, "%s:%s", name, value);
}

void mongo_read_preference_add_tagset(mongo_read_preference *rp, mongo_read_preference_tagset *tagset)
{
	rp->tagset_count++;
	rp->tagsets = realloc(rp->tagsets, rp->tagset_count * sizeof(mongo_read_preference_tagset*));
	rp->tagsets[rp->tagset_count - 1] = tagset;
}

void mongo_read_preference_tagset_dtor(mongo_read_preference_tagset *tagset)
{
	int i;

	for (i = 0; i < tagset->tag_count; i++) {
		free(tagset->tags[i]);
	}
	free(tagset->tags);
	free(tagset);
}

void mongo_read_preference_dtor(mongo_read_preference *rp)
{
	int i;

	for (i = 0; i < rp->tagset_count; i++) {
		mongo_read_preference_tagset_dtor(rp->tagsets[i]);
	}
	free(rp->tagsets);
	/* We are not freeing *rp itself, as that's not always a pointer */
}

void mongo_read_preference_copy(mongo_read_preference *from, mongo_read_preference *to)
{
	int i, j;

	to->type = from->type;
	to->tagset_count = from->tagset_count;
	if (!from->tagset_count) {
		to->tagset_count = 0;
		to->tagsets = NULL;
		return;
	}
	to->tagsets = calloc(1, to->tagset_count * sizeof(mongo_read_preference_tagset*));

	for (i = 0; i < from->tagset_count; i++) {
		to->tagsets[i] = calloc(1, sizeof(mongo_read_preference_tagset));
		to->tagsets[i]->tag_count = from->tagsets[i]->tag_count;
		to->tagsets[i]->tags = calloc(1, to->tagsets[i]->tag_count * sizeof(char*));

		for (j = 0; j < from->tagsets[i]->tag_count; j++) {
			to->tagsets[i]->tags[j] = strdup(from->tagsets[i]->tags[j]);
		}
	}
}

void mongo_read_preference_replace(mongo_read_preference *from, mongo_read_preference *to)
{
	mongo_read_preference_dtor(to);
	mongo_read_preference_copy(from, to);
}
