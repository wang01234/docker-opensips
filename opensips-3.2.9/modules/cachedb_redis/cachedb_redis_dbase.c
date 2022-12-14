/*
 * Copyright (C) 2011 OpenSIPS Solutions
 *
 * This file is part of opensips, a free SIP server.
 *
 * opensips is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * opensips is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 *
 *
 * history:
 * ---------
 *  2011-09-xx  created (vlad-paiu)
 */

#include "../../dprint.h"
#include "cachedb_redis_dbase.h"
#include "cachedb_redis_utils.h"
#include "../../mem/mem.h"
#include "../../ut.h"
#include "../../pt.h"
#include "../../cachedb/cachedb.h"

#include <string.h>
#include <hiredis/hiredis.h>

#define QUERY_ATTEMPTS 2

int redis_query_tout = CACHEDB_REDIS_DEFAULT_TIMEOUT;
int redis_connnection_tout = CACHEDB_REDIS_DEFAULT_TIMEOUT;
int shutdown_on_error = 0;
int use_tls = 0;
int enable_raw_query_quoting;

struct tls_mgm_binds tls_api;

/*
 *	- redis_raw_query_send_old()
 *	- redis_raw_query_send_new()
 */
int (*redis_raw_query_send)(cachedb_con *connection, redisReply **reply,
		cdb_raw_entry ***_, int __, int *___, str *attr);

redisContext *redis_get_ctx(char *ip, int port)
{
	struct timeval tv;
	static char warned = 0;
	redisContext *ctx;

	if (!redis_connnection_tout) {
		if (!warned++)
			LM_WARN("Connecting to redis without timeout might block your server\n");
		ctx = redisConnect(ip,port);
	} else {
		tv.tv_sec = redis_connnection_tout / 1000;
		tv.tv_usec = (redis_connnection_tout * 1000) % 1000000;
		ctx = redisConnectWithTimeout(ip,port,tv);
	}
	if (ctx && ctx->err != REDIS_OK) {
		LM_ERR("failed to open redis connection %s:%hu - %s\n",ip,
				(unsigned short)port,ctx->errstr);
		return NULL;
	}

	if (redis_query_tout) {
		tv.tv_sec = redis_query_tout / 1000;
		tv.tv_usec = (redis_query_tout * 1000) % 1000000;
		if (redisSetTimeout(ctx, tv) != REDIS_OK) {
			LM_ERR("Cannot set query timeout to %dms\n", redis_query_tout);
			return NULL;
		}
	}
	return ctx;
}

#ifdef HAVE_REDIS_SSL
static void tls_print_errstack(void)
{
	int code;

	while ((code = ERR_get_error())) {
		LM_ERR("TLS errstack: %s\n", ERR_error_string(code, 0));
	}
}

static int redis_init_ssl(char *url_extra_opts, redisContext *ctx,
	struct tls_domain **tls_dom)
{
	str tls_dom_name;
	SSL *ssl;
	struct tls_domain *d;

	if (tls_dom == NULL) {
		if (strncmp(url_extra_opts, CACHEDB_TLS_DOM_PARAM,
				CACHEDB_TLS_DOM_PARAM_LEN)) {
			LM_ERR("Invalid Redis URL parameter: %s\n", url_extra_opts);
			return -1;
		}

		tls_dom_name.s = url_extra_opts + CACHEDB_TLS_DOM_PARAM_LEN;
		tls_dom_name.len = strlen(tls_dom_name.s);
		if (!tls_dom_name.len) {
			LM_ERR("Empty TLS domain name in Redis URL\n");
			return -1;
		}

		d = tls_api.find_client_domain_name(&tls_dom_name);
		if (d == NULL) {
			LM_ERR("TLS domain: %.*s not found\n",
				tls_dom_name.len, tls_dom_name.s);
			return -1;
		}

		*tls_dom = d;
	} else {
		d = *tls_dom;
	}

	ssl = SSL_new(((void**)d->ctx)[process_no]);
	if (!ssl) {
		LM_ERR("failed to create SSL structure (%d:%s)\n", errno, strerror(errno));
		tls_print_errstack();
		tls_api.release_domain(*tls_dom);
		return -1;
	}

	if (redisInitiateSSL(ctx, ssl) != REDIS_OK) {
		printf("Failed to init Redis SSL: %s\n", ctx->errstr);
		tls_api.release_domain(*tls_dom);
		return -1;
	}

	LM_DBG("TLS enabled for this connection\n");

	return 0;
}
#endif

int redis_connect_node(redis_con *con,cluster_node *node)
{
	redisReply *rpl;

	node->context = redis_get_ctx(node->ip,node->port);
	if (!node->context)
		return -1;

#ifdef HAVE_REDIS_SSL
	if (use_tls && con->id->extra_options &&
		redis_init_ssl(con->id->extra_options, node->context,
			&node->tls_dom) < 0) {
		redisFree(node->context);
		node->context = NULL;
		return -1;
	}
#endif

	if (con->id->password) {
		rpl = redisCommand(node->context,"AUTH %s",con->id->password);
		if (rpl == NULL || rpl->type == REDIS_REPLY_ERROR) {
			LM_ERR("failed to auth to redis - %.*s\n",
				rpl?(unsigned)rpl->len:7,rpl?rpl->str:"FAILURE");
			freeReplyObject(rpl);
			goto error;
		}
		LM_DBG("AUTH [password] -  %.*s\n",(unsigned)rpl->len,rpl->str);
		freeReplyObject(rpl);
	}

	if ((con->flags & REDIS_SINGLE_INSTANCE) && con->id->database) {
		rpl = redisCommand(node->context,"SELECT %s",con->id->database);
		if (rpl == NULL || rpl->type == REDIS_REPLY_ERROR) {
			LM_ERR("failed to select database %s - %.*s\n",con->id->database,
				rpl?(unsigned)rpl->len:7,rpl?rpl->str:"FAILURE");
			freeReplyObject(rpl);
			goto error;
		}

		LM_DBG("SELECT [%s] - %.*s\n",con->id->database,(unsigned)rpl->len,rpl->str);
		freeReplyObject(rpl);
	}

	return 0;

error:
	redisFree(node->context);
	node->context = NULL;
	if (use_tls && node->tls_dom) {
		tls_api.release_domain(node->tls_dom);
		node->tls_dom = NULL;
	}
	return -1;
}

int redis_reconnect_node(redis_con *con,cluster_node *node)
{
	LM_DBG("reconnecting node %s:%d \n",node->ip,node->port);

	/* close the old connection */
	if(node->context) {
		redisFree(node->context);
		node->context = NULL;
	}

	return redis_connect_node(con,node);
}

int redis_connect(redis_con *con)
{
	redisContext *ctx;
	redisReply *rpl;
	cluster_node *it;
	int len;
	struct tls_domain *tls_dom = NULL;

	/* connect to redis DB */
	ctx = redis_get_ctx(con->id->host,con->id->port);
	if (!ctx)
		return -1;

#ifdef HAVE_REDIS_SSL
	if (use_tls && con->id->extra_options &&
		redis_init_ssl(con->id->extra_options, ctx, &tls_dom) < 0) {
		redisFree(ctx);
		return -1;
	}
#endif

	/* auth using password, if any */
	if (con->id->password) {
		rpl = redisCommand(ctx,"AUTH %s",con->id->password);
		if (rpl == NULL || rpl->type == REDIS_REPLY_ERROR) {
			LM_ERR("failed to auth to redis - %.*s\n",
				rpl?(unsigned)rpl->len:7,rpl?rpl->str:"FAILURE");
			if (rpl!=NULL)
				freeReplyObject(rpl);
			goto error;
		}
		LM_DBG("AUTH [password] -  %.*s\n",(unsigned)rpl->len,rpl->str);
		freeReplyObject(rpl);
	}

	rpl = redisCommand(ctx,"CLUSTER NODES");
	if (rpl == NULL || rpl->type == REDIS_REPLY_ERROR) {
		/* single instace mode */
		con->flags |= REDIS_SINGLE_INSTANCE;
		len = strlen(con->id->host);
		con->nodes = pkg_malloc(sizeof(cluster_node) + len + 1);
		if (con->nodes == NULL) {
			LM_ERR("no more pkg\n");
			if (rpl!=NULL)
				freeReplyObject(rpl);
			goto error;
		}
		con->nodes->ip = (char *)(con->nodes + 1);

		strcpy(con->nodes->ip,con->id->host);
		con->nodes->port = con->id->port;
		con->nodes->start_slot = 0;
		con->nodes->end_slot = 4096;
		con->nodes->context = NULL;
		con->nodes->next = NULL;
		LM_DBG("single instance mode\n");
	} else {
		/* cluster instance mode */
		con->flags |= REDIS_CLUSTER_INSTANCE;
		con->slots_assigned = 0;
		LM_DBG("cluster instance mode\n");
		if (build_cluster_nodes(con,rpl->str,rpl->len) < 0) {
			LM_ERR("failed to parse Redis cluster info\n");
			freeReplyObject(rpl);
			goto error;
		}
	}

	if (rpl!=NULL)
		freeReplyObject(rpl);
	redisFree(ctx);

	if (use_tls && tls_dom)
		tls_api.release_domain(tls_dom);

	con->flags |= REDIS_INIT_NODES;

	for (it=con->nodes;it;it=it->next) {

		if (it->end_slot > con->slots_assigned )
			con->slots_assigned = it->end_slot;

		if (redis_connect_node(con,it) < 0) {
			LM_ERR("failed to init connection \n");
			return -1;
		}
	}

	return 0;

error:
	redisFree(ctx);
	if (use_tls && tls_dom)
		tls_api.release_domain(tls_dom);
	return -1;
}

redis_con* redis_new_connection(struct cachedb_id* id)
{
	redis_con *con;

	if (id == NULL) {
		LM_ERR("null cachedb_id\n");
		return 0;
	}

	if (id->flags & CACHEDB_ID_MULTIPLE_HOSTS) {
		LM_ERR("multiple hosts are not supported for redis\n");
		return 0;
	}

	con = pkg_malloc(sizeof(redis_con));
	if (con == NULL) {
		LM_ERR("no more pkg \n");
		return 0;
	}

	memset(con,0,sizeof(redis_con));
	con->id = id;
	con->ref = 1;

	if (redis_connect(con) < 0) {
		LM_ERR("failed to connect to DB\n");
		if (shutdown_on_error) {
			pkg_free(con);
			return NULL;
		}
	}

	return con;
}

cachedb_con *redis_init(str *url)
{
	return cachedb_do_init(url,(void *)redis_new_connection);
}

void redis_free_connection(cachedb_pool_con *con)
{
	redis_con * c;

	LM_DBG("in redis_free_connection\n");

	if (!con) return;
	c = (redis_con *)con;
	destroy_cluster_nodes(c);
	pkg_free(c);
}

void redis_destroy(cachedb_con *con) {
	LM_DBG("in redis_destroy\n");
	cachedb_do_close(con,redis_free_connection);
}

#define redis_run_command(con,key,fmt,args...) \
	do {\
		con = (redis_con *)connection->data; \
		if (!(con->flags & REDIS_INIT_NODES) && redis_connect(con) < 0) { \
			LM_ERR("failed to connect to DB\n"); \
			return -9; \
		} \
		node = get_redis_connection(con,key); \
		if (node == NULL) { \
			LM_ERR("Bad cluster configuration\n"); \
			return -10; \
		} \
		if (node->context == NULL) { \
			if (redis_reconnect_node(con,node) < 0) { \
				return -1; \
			} \
		} \
		for (i = QUERY_ATTEMPTS; i; i--) { \
			reply = redisCommand(node->context,fmt,##args); \
			if (reply == NULL || reply->type == REDIS_REPLY_ERROR) { \
				LM_INFO("Redis query failed: %p %.*s\n",\
					reply,reply?(unsigned)reply->len:7,reply?reply->str:"FAILURE"); \
				if (reply) \
					freeReplyObject(reply); \
				if (node->context->err == REDIS_OK || redis_reconnect_node(con,node) < 0) { \
					i = 0; break; \
				}\
			} else break; \
		} \
		if (i==0) { \
			LM_ERR("giving up on query\n"); \
			return -1; \
		} \
		if (i != QUERY_ATTEMPTS) \
			LM_INFO("successfully ran query after %d failed attempt(s)\n", \
			        QUERY_ATTEMPTS - i); \
	} while (0)

int redis_get(cachedb_con *connection,str *attr,str *val)
{
	redis_con *con;
	cluster_node *node;
	redisReply *reply;
	int i;

	if (!attr || !val || !connection) {
		LM_ERR("null parameter\n");
		return -1;
	}

	redis_run_command(con,attr,"GET %b",attr->s,attr->len);

	if (reply->type == REDIS_REPLY_NIL || reply->str == NULL
			|| reply->len == 0) {
		LM_DBG("no such key - %.*s\n",attr->len,attr->s);
		val->s = NULL;
		val->len = 0;
		freeReplyObject(reply);
		return -2;
	}

	LM_DBG("GET %.*s  - %.*s\n",attr->len,attr->s,(unsigned)reply->len,reply->str);

	val->s = pkg_malloc(reply->len);
	if (val->s == NULL) {
		LM_ERR("no more pkg\n");
		freeReplyObject(reply);
		return -1;
	}

	memcpy(val->s,reply->str,reply->len);
	val->len = reply->len;
	freeReplyObject(reply);
	return 0;
}

int redis_set(cachedb_con *connection,str *attr,str *val,int expires)
{
	redis_con *con;
	cluster_node *node;
	redisReply *reply;
	int i;

	if (!attr || !val || !connection) {
		LM_ERR("null parameter\n");
		return -1;
	}

	redis_run_command(con,attr,"SET %b %b",attr->s,attr->len,val->s,val->len);

	LM_DBG("set %.*s to %.*s - status = %d - %.*s\n",attr->len,attr->s,val->len,
			val->s,reply->type,(unsigned)reply->len,reply->str);

	freeReplyObject(reply);

	if (expires) {
		redis_run_command(con,attr,"EXPIRE %b %d",attr->s,attr->len,expires);

		LM_DBG("set %.*s to expire in %d s - %.*s\n",attr->len,attr->s,expires,
				(unsigned)reply->len,reply->str);

		freeReplyObject(reply);
	}

	return 0;
}

/* returns 0 in case of successful remove
 * returns 1 in case of key not existent
 * return -1 in case of error */
int redis_remove(cachedb_con *connection,str *attr)
{
	redis_con *con;
	cluster_node *node;
	redisReply *reply;
	int ret=0,i;

	if (!attr || !connection) {
		LM_ERR("null parameter\n");
		return -1;
	}

	redis_run_command(con,attr,"DEL %b",attr->s,attr->len);

	if (reply->integer == 0) {
		LM_DBG("Key %.*s does not exist in DB\n",attr->len,attr->s);
		ret = 1;
	} else
		LM_DBG("Key %.*s successfully removed\n",attr->len,attr->s);

	freeReplyObject(reply);
	return ret;
}

/* returns the new value of the counter */
int redis_add(cachedb_con *connection,str *attr,int val,int expires,int *new_val)
{
	redis_con *con;
	cluster_node *node;
	redisReply *reply;
	int i;

	if (!attr || !connection) {
		LM_ERR("null parameter\n");
		return -1;
	}

	redis_run_command(con,attr,"INCRBY %b %d",attr->s,attr->len,val);

	if (new_val)
		*new_val = reply->integer;
	freeReplyObject(reply);

	if (expires) {
		redis_run_command(con,attr,"EXPIRE %b %d",attr->s,attr->len,expires);

		LM_DBG("set %.*s to expire in %d s - %.*s\n",attr->len,attr->s,expires,
				(unsigned)reply->len,reply->str);

		freeReplyObject(reply);
	}

	return 0;
}

int redis_sub(cachedb_con *connection,str *attr,int val,int expires,int *new_val)
{
	redis_con *con;
	cluster_node *node;
	redisReply *reply;
	int i;

	if (!attr || !connection) {
		LM_ERR("null parameter\n");
		return -1;
	}

	redis_run_command(con,attr,"DECRBY %b %d",attr->s,attr->len,val);

	if (new_val)
		*new_val = reply->integer;
	freeReplyObject(reply);

	if (expires) {
		redis_run_command(con,attr,"EXPIRE %b %d",attr->s,attr->len,expires);

		LM_DBG("set %.*s to expire in %d s - %.*s\n",attr->len,attr->s,expires,
				(unsigned)reply->len,reply->str);

		freeReplyObject(reply);
	}

	return 0;
}

int redis_get_counter(cachedb_con *connection,str *attr,int *val)
{
	redis_con *con;
	cluster_node *node;
	redisReply *reply;
	int i,ret;
	str response;

	if (!attr || !val || !connection) {
		LM_ERR("null parameter\n");
		return -1;
	}

	redis_run_command(con,attr,"GET %b",attr->s,attr->len);

	if (reply->type == REDIS_REPLY_NIL || reply->str == NULL
			|| reply->len == 0) {
		LM_DBG("no such key - %.*s\n",attr->len,attr->s);
		return -2;
	}

	LM_DBG("GET %.*s  - %.*s\n",attr->len,attr->s,(unsigned)reply->len,reply->str);

	response.s=reply->str;
	response.len=reply->len;

	if (str2sint(&response,&ret) != 0) {
		LM_ERR("Not a counter \n");
		freeReplyObject(reply);
		return -3;
	}

	if (val)
		*val = ret;

	freeReplyObject(reply);
	return 0;
}

int redis_raw_query_handle_reply(redisReply *reply,cdb_raw_entry ***ret,
		int expected_kv_no,int *reply_no)
{
	int current_size=0,len,i;

	/* start with a single returned document */
	*ret = pkg_malloc(1 * sizeof(cdb_raw_entry *));
	if (*ret == NULL) {
		LM_ERR("No more PKG mem\n");
		goto error;
	}

	**ret = pkg_malloc(expected_kv_no * sizeof(cdb_raw_entry));
	if (**ret == NULL) {
		LM_ERR("No more pkg mem\n");
		goto error;
	}

	switch (reply->type) {
		case REDIS_REPLY_STRING:
			(*ret)[current_size][0].val.s.s = pkg_malloc(reply->len);
			if (! (*ret)[current_size][0].val.s.s ) {
				LM_ERR("No more pkg \n");
				goto error;
			}

			memcpy((*ret)[current_size][0].val.s.s,reply->str,reply->len);
			(*ret)[current_size][0].val.s.len = reply->len;
			(*ret)[current_size][0].type = CDB_STR;

			current_size++;
			break;
		case REDIS_REPLY_INTEGER:
			(*ret)[current_size][0].val.n = reply->integer;
			(*ret)[current_size][0].type = CDB_INT32;
			current_size++;
			break;
		case REDIS_REPLY_NIL:
			(*ret)[current_size][0].type = CDB_NULL;
			(*ret)[current_size][0].val.s.s = NULL;
			(*ret)[current_size][0].val.s.len = 0;
			current_size++;
			break;
		case REDIS_REPLY_ARRAY:
			for (i=0;i<reply->elements;i++) {
				switch (reply->element[i]->type) {
					case REDIS_REPLY_STRING:
					case REDIS_REPLY_INTEGER:
					case REDIS_REPLY_NIL:
						if (current_size > 0) {
							*ret = pkg_realloc(*ret,(current_size + 1) * sizeof(cdb_raw_entry *));
							if (*ret == NULL) {
								LM_ERR("No more pkg\n");
								goto error;
							}
							(*ret)[current_size] = pkg_malloc(expected_kv_no * sizeof(cdb_raw_entry));
							if ((*ret)[current_size] == NULL) {
								LM_ERR("No more pkg\n");
								goto error;
							}
						}


						if (reply->element[i]->type == REDIS_REPLY_INTEGER) {
							(*ret)[current_size][0].val.n = reply->element[i]->integer;
							(*ret)[current_size][0].type = CDB_INT32;
						} else if (reply->element[i]->type == REDIS_REPLY_NIL) {
							(*ret)[current_size][0].val.s.s = NULL;
							(*ret)[current_size][0].val.s.len = 0;
							(*ret)[current_size][0].type = CDB_NULL;
						} else {
							(*ret)[current_size][0].val.s.s = pkg_malloc(reply->element[i]->len);
							if (! (*ret)[current_size][0].val.s.s ) {
								pkg_free((*ret)[current_size]);
								LM_ERR("No more pkg \n");
								goto error;
							}

							memcpy((*ret)[current_size][0].val.s.s,reply->element[i]->str,reply->element[i]->len);
							(*ret)[current_size][0].val.s.len = reply->element[i]->len;
							(*ret)[current_size][0].type = CDB_STR;
						}

						current_size++;
						break;
					default:
						LM_DBG("Unexpected data type %d found in array - skipping \n",reply->element[i]->type);
				}
			}
			break;
		default:
			LM_ERR("unhandled Redis datatype %d\n", reply->type);
			goto error;
	}

	if (current_size == 0)
		pkg_free((*ret)[0]);

	*reply_no = current_size;
	freeReplyObject(reply);
	return 1;

error:
	if (current_size == 0 && *ret)
		pkg_free((*ret)[0]);

	if (*ret) {
		for (len = 0;len<current_size;len++) {
			if ( (*ret)[len][0].type == CDB_STR)
				pkg_free((*ret)[len][0].val.s.s);
			pkg_free((*ret)[len]);
		}
		pkg_free(*ret);
	}

	*ret = NULL;
	*reply_no=0;

	freeReplyObject(reply);
	return -1;
}

#define MAP_SET_MAX_FIELDS 128
int redis_raw_query_send_old(cachedb_con *connection, redisReply **reply,
		cdb_raw_entry ***_, int __, int *___, str *attr)
{
	int i, argc = 0;
	const char *argv[MAP_SET_MAX_FIELDS];
	size_t argvlen[MAP_SET_MAX_FIELDS];
	redis_con *con;
	cluster_node *node;
	str key, st, arg;
	char *p;

	con = (redis_con *)connection->data;

	if (!(con->flags & REDIS_INIT_NODES) && redis_connect(con) < 0) {
		LM_ERR("failed to connect to DB\n");
		return -9;
	}

	st = *attr;
	trim(&st);
	while (st.len > 0 && (p = q_memchr(st.s, ' ', st.len))) {
		if (argc == MAP_SET_MAX_FIELDS) {
			LM_ERR("max raw query args exceeded (%d)\n", MAP_SET_MAX_FIELDS);
			return -1;
		}

		arg.s = st.s;
		arg.len = p - st.s;
		trim(&arg);

		argv[argc] = arg.s;
		argvlen[argc++] = arg.len;

		st.len -= p - st.s + 1;
		st.s = p + 1;
		trim(&st);
	}

	if (st.len > 0) {
		argv[argc] = st.s;
		argvlen[argc++] = st.len;
	}

	if (argc < 2) {
		LM_ERR("malformed Redis RAW query: '%.*s' (%d)\n",
		       attr->len, attr->s, attr->len);
		return -1;
	}

	/* TODO - although in most of the cases the targetted key is the 2nd query string,
		that's not always the case ! - make this 100% */
	key.s = (char *)argv[1];
	key.len = argvlen[1];

#ifdef EXTRA_DEBUG
	LM_DBG("raw query key: %.*s\n", key.len, key.s);
	for (i = 0; i < argc; i++)
		LM_DBG("raw query arg %d: '%.*s' (%d)\n", i, (int)argvlen[i], argv[i],
		       (int)argvlen[i]);
#endif

	node = get_redis_connection(con, &key);
	if (node == NULL) {
		LM_ERR("Bad cluster configuration\n");
		return -10;
	}

	if (node->context == NULL) {
		if (redis_reconnect_node(con,node) < 0) {
			return -1;
		}
	}

	for (i = QUERY_ATTEMPTS; i; i--) {
		*reply = redisCommandArgv(node->context, argc, argv, argvlen);
		if (*reply == NULL || (*reply)->type == REDIS_REPLY_ERROR) {
			LM_INFO("Redis query failed: %.*s\n",
				*reply?(unsigned)((*reply)->len):7,*reply?(*reply)->str:"FAILURE");
			if (*reply)
				freeReplyObject(*reply);
			if (node->context->err == REDIS_OK || redis_reconnect_node(con,node) < 0) {
				i = 0; break;
			}
		} else break;
	}

	if (i==0) {
		LM_ERR("giving up on query\n");
		return -1;
	}

	if (i != QUERY_ATTEMPTS)
		LM_INFO("successfully ran query after %d failed attempt(s)\n",
		        QUERY_ATTEMPTS - i);

	return 0;
}

int redis_raw_query_send_new(cachedb_con *connection, redisReply **reply,
		cdb_raw_entry ***_, int __, int *___, str *attr)
{
	int i, argc = 0, squoted = 0, dquoted = 0;
	const char *argv[MAP_SET_MAX_FIELDS];
	size_t argvlen[MAP_SET_MAX_FIELDS];
	redis_con *con;
	cluster_node *node;
	str key, st;
	char *p, *lim, *arg = NULL;

	con = (redis_con *)connection->data;

	if (!(con->flags & REDIS_INIT_NODES) && redis_connect(con) < 0) {
		LM_ERR("failed to connect to DB\n");
		return -9;
	}

	st = *attr;
	trim(&st);

	/* allow script developers to enclose swaths of text with single/double
	 * quotes, in case any of their raw query string arguments must include
	 * whitespace chars.  The enclosing quotes shall not be passed to Redis. */
	for (p = st.s, lim = p + st.len; p < lim; p++) {
		if ((dquoted && *p != '"') || (squoted && *p != '\''))
			continue;

		if (argc == MAP_SET_MAX_FIELDS) {
			LM_ERR("max raw query args exceeded (%d)\n", MAP_SET_MAX_FIELDS);
			goto bad_query;
		}

		if (dquoted || squoted) {
			if (p+1 < lim && !is_ws(*(p+1)))
				goto bad_query;

			argv[argc]++;
			argvlen[argc] = p - argv[argc];
			argc++;
			dquoted = squoted = 0;
		} else if (*p == '"') {
			dquoted = 1;
			argv[argc] = p;
		} else if (*p == '\'') {
			squoted = 1;
			argv[argc] = p;
		} else if (is_ws(*p)) {
			if (!arg)
				continue;

			argv[argc] = arg;
			argvlen[argc++] = p - arg;
			arg = NULL;
		} else if (!arg) {
			arg = p;
		}
	}

	if (squoted || dquoted) {
		LM_ERR("unterminated quoted query argument\n");
		goto bad_query;
	}

	if (arg) {
		argv[argc] = arg;
		argvlen[argc++] = st.s + st.len - arg;
	}

	if (argc < 2)
		goto bad_query;

	/* TODO - although in most of the cases the targetted key is the 2nd query string,
		that's not always the case ! - make this 100% */
	key.s = (char *)argv[1];
	key.len = argvlen[1];

#ifdef EXTRA_DEBUG
	LM_DBG("raw query key: %.*s\n", key.len, key.s);
	for (i = 0; i < argc; i++)
		LM_DBG("raw query arg %d: '%.*s' (%d)\n", i, (int)argvlen[i], argv[i],
		       (int)argvlen[i]);
#endif

	node = get_redis_connection(con, &key);
	if (node == NULL) {
		LM_ERR("Bad cluster configuration\n");
		return -10;
	}

	if (node->context == NULL) {
		if (redis_reconnect_node(con,node) < 0) {
			return -1;
		}
	}

	for (i = QUERY_ATTEMPTS; i; i--) {
		*reply = redisCommandArgv(node->context, argc, argv, argvlen);
		if (*reply == NULL || (*reply)->type == REDIS_REPLY_ERROR) {
			LM_INFO("Redis query failed: %.*s\n",
				*reply?(unsigned)((*reply)->len):7,*reply?(*reply)->str:"FAILURE");
			if (*reply)
				freeReplyObject(*reply);
			if (node->context->err == REDIS_OK || redis_reconnect_node(con,node) < 0) {
				i = 0; break;
			}
		} else break;
	}

	if (i==0) {
		LM_ERR("giving up on query\n");
		return -1;
	}

	if (i != QUERY_ATTEMPTS)
		LM_INFO("successfully ran query after %d failed attempt(s)\n",
		        QUERY_ATTEMPTS - i);

	return 0;

bad_query:
	LM_ERR("malformed Redis RAW query: '%.*s' (%d)\n",
	       attr->len, attr->s, attr->len);
	return -1;
}

int redis_raw_query(cachedb_con *connection,str *attr,cdb_raw_entry ***rpl,int expected_kv_no,int *reply_no)
{
	redisReply *reply;

	if (!attr || !connection) {
		LM_ERR("null parameter\n");
		return -1;
	}

	if (redis_raw_query_send(connection,&reply,rpl,expected_kv_no,reply_no,attr) < 0) {
		LM_ERR("Failed to send query to server \n");
		return -1;
	}

	switch (reply->type) {
		case REDIS_REPLY_ERROR:
			LM_ERR("Error encountered when running Redis raw query [%.*s]\n",
			attr->len,attr->s);
			return -1;
		case REDIS_REPLY_NIL:
			LM_DBG("Redis raw query [%.*s] failed - no such key\n",attr->len,attr->s);
			freeReplyObject(reply);
			return -2;
		case REDIS_REPLY_STATUS:
			LM_DBG("Received a status of %.*s from Redis \n",(unsigned)reply->len,reply->str);
			if (reply_no)
				*reply_no = 0;
			freeReplyObject(reply);
			return 1;
		default:
			/* some data arrived - yay */

			if (rpl == NULL) {
				LM_DBG("Received reply type %d but script writer not interested in it \n",reply->type);
				freeReplyObject(reply);
				return 1;
			}
			return redis_raw_query_handle_reply(reply,rpl,expected_kv_no,reply_no);
	}

	return 1;
}
