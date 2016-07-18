#include <nchan_module.h>
#include <assert.h>

#include "redis_nginx_adapter.h"

#include <util/nchan_msgid.h>
#include <util/nchan_rbtree.h>
#include <store/store_common.h>

#include "cluster.h"

#define DEBUG_LEVEL NGX_LOG_WARN
//#define DEBUG_LEVEL NGX_LOG_DEBUG

#define DBG(fmt, args...) ngx_log_error(DEBUG_LEVEL, ngx_cycle->log, 0, "REDISTORE(CLUSTER): " fmt, ##args)
#define ERR(fmt, args...) ngx_log_error(NGX_LOG_ERR, ngx_cycle->log, 0, "REDISTORE(CLUSTER): " fmt, ##args)


static rbtree_seed_t              redis_cluster_node_id_tree;

static void *redis_data_rbtree_node_cluster_id(void *data) {
  return &(*(rdstore_data_t **)data)->node.id;
}
void redis_cluster_init_postconfig(ngx_conf_t *cf) {
  rbtree_init(&redis_cluster_node_id_tree, "redis cluster node (by id) data", redis_data_rbtree_node_cluster_id, NULL, NULL);
}

static void redis_cluster_info_callback(redisAsyncContext *ac, void *rep, void *privdata);
static void redis_get_cluster_nodes_callback(redisAsyncContext *ac, void *rep, void *privdata);

static void redis_check_if_cluster_ready_handler(ngx_event_t *ev) {
  rdstore_data_t   *rdata = ev->data;
  if(rdata->status != DISCONNECTED && rdata->ctx) {
    redis_get_cluster_info(rdata);
  }
  
  ngx_free(ev);
}



void redis_get_cluster_info(rdstore_data_t *rdata) {
  redisAsyncCommand(rdata->ctx, redis_cluster_info_callback, NULL, "CLUSTER INFO");
}

static void redis_cluster_info_callback(redisAsyncContext *ac, void *rep, void *privdata) {
  redisReply        *reply = rep;
  rdstore_data_t    *rdata = ac->data;
    if(ac->err || !reply || reply->type != REDIS_REPLY_STRING) {
    redisCheckErrorCallback(ac, reply, privdata);
    return;
  }
  
  if(ngx_strstrn((u_char *)reply->str, "cluster_state:ok", 15)) {
    redis_get_cluster_nodes(rdata);
  }
  else {
    ngx_log_error(NGX_LOG_WARN, ngx_cycle->log, 0, "Nchan: Redis cluster not ready");
    ngx_event_t      *evt = ngx_calloc(sizeof(*evt), ngx_cycle->log);
    nchan_init_timer(evt, redis_check_if_cluster_ready_handler, rdata);
    //rdt_set_status(rdata, WAITING_FOR_CLUSTER_READY, ac);
    ngx_add_timer(evt, 1000);
  }
}








static void *rbtree_cluster_hashslots_id(void *data) {
  return &((redis_cluster_keyslot_range_node_t *)data)->range;
}
static uint32_t rbtree_cluster_hashslots_bucketer(void *vid) {
  return 1; //no buckets
}
static ngx_int_t rbtree_cluster_hashslots_compare(void *v1, void *v2) {
  redis_cluster_slot_range_t   *r1 = v1;
  redis_cluster_slot_range_t   *r2 = v2;
  
  if(r2->max < r1->min) {
    //r2 is strictly left of r1
    return -1;
  }
  else if(r2->min > r1->max) {
    //r2 is structly right of r1
    return 1;
  }
  else {
    //there's an overlap
    return 0;
  }
}





void redis_get_cluster_nodes(rdstore_data_t *rdata) {
  redisAsyncCommand(rdata->ctx, redis_get_cluster_nodes_callback, NULL, "CLUSTER NODES");
}

#define nchan_scan_chr(cur, chr, str, description)   \
  what=description;                           \
  str.data = (u_char *)ngx_strchr(cur, chr);  \
  if(str.data) {                              \
    str.len = str.data - cur;                 \
    str.data = cur;                           \
    cur+=str.len+1;                           \
  }                                           \
  else                                        \
    goto fail
    
#define nchan_scan_chr_until_end_of_line(cur, str, description) \
  what=description;                           \
  str.data = (u_char *)ngx_strchr(cur, '\n'); \
  if(!str.data)                               \
    str.data = (u_char *)ngx_strchr(cur, '\0');\
  if(str.data) {                              \
    str.len = str.data - cur;                 \
    str.data = cur;                           \
    cur+=str.len;                             \
  }                                           \
  else                                        \
    goto fail
    

#define nchan_scan_str(str_src, cur, chr, str, description)\
  what=description;                           \
  str.data = (u_char *)memchr(cur, chr, str_src.len - (cur - str_src.data));\
  if(!str.data)                               \
    str.data = str_src.data + str_src.len;    \
  if(str.data) {                              \
    str.len = str.data - cur;                 \
    str.data = cur;                           \
    cur+=str.len+1;                           \
  }                                           \
  else                                        \
    goto fail

static void redis_get_cluster_nodes_callback(redisAsyncContext *ac, void *rep, void *privdata) {
  redisReply                    *reply = rep;
  rdstore_data_t                *rdata = ac->data;
  ngx_rbtree_node_t             *rbtree_node = NULL;
  rdstore_data_t                *ctnode = NULL, *my_ctnode = NULL; //cluster tree node
  redis_cluster_t               *cluster = NULL;
  ngx_int_t                      num_master_nodes = 0;
  
  if(ac->err || !reply || reply->type != REDIS_REPLY_STRING) {
    redisCheckErrorCallback(ac, reply, privdata);
    return;
  }
  
  u_char     *line, *cur;
  char       *what;
  ngx_str_t   id, address, flags, master_id, ping_sent, pong_recv, config_epoch, link_state, slots;
  ngx_str_t   my_slots, slot;
  ngx_int_t   master_node;
  
  for(line = (u_char *)reply->str-1; line != NULL; line = (u_char *)ngx_strchr(cur, '\n')) {
    cur = line+1;
    if(cur[0]=='\0') {
      break;
    }
    nchan_scan_chr(cur, ' ', id, "id");
    nchan_scan_chr(cur, ' ', address, "address");
    nchan_scan_chr(cur, ' ', flags, "flags");
    if(nchan_ngx_str_substr(&flags, "master")) {
      num_master_nodes ++;
      master_node = 1;
      nchan_scan_chr(cur, ' ', master_id, "master_id");
      nchan_scan_chr(cur, ' ', ping_sent, "ping_sent");
      nchan_scan_chr(cur, ' ', pong_recv, "pong_recv");
      nchan_scan_chr(cur, ' ', config_epoch, "config_epoch");
      nchan_scan_chr(cur, ' ', link_state, "link_state");
      nchan_scan_chr_until_end_of_line(cur, slots, "slots");
      //ERR("%V %V %V %V %V", &id, &address, &flags, &link_state, &slots);
    }
    else {
      //ERR("%V %V %V %s", &id, &address, &flags, "SLAVE!!");
      master_node = 0;
    }
    
    if(!nchan_ngx_str_substr(&flags, "myself")) {
      if((rbtree_node = rbtree_find_node(&redis_cluster_node_id_tree, &id)) != NULL) {
        //do any other nodes already have the cluster set? if so, use that cluster struct.
        ctnode = *(rdstore_data_t **)rbtree_data_from_node(rbtree_node);
        if(ctnode->node.cluster) {
          if(cluster)
            assert(cluster == ctnode->node.cluster);
          else
            cluster = ctnode->node.cluster;
        }
      }
    }
    else if(master_node) {
      //myself and master!
      
      struct {
        rdstore_data_t   *rdata;
        u_char            chr;
      } *rdata_ptr_and_buf;
      
      my_slots = slots;
      if((rbtree_node = rbtree_find_node(&redis_cluster_node_id_tree, &id)) != NULL) {
        //node already known. what do?...
        rdstore_data_t *my_rdata;
        my_rdata = *(rdstore_data_t **)rbtree_data_from_node(rbtree_node);
        
        assert(my_rdata == rdata);
        my_ctnode = my_rdata;
        ERR("%p %V %V already added to redis_cluster_node_id_tree... weird... how?...", rdata, &rdata->node.id, &my_rdata->node.id);
      }
      else {
        if((rbtree_node = rbtree_create_node(&redis_cluster_node_id_tree, sizeof(*rdata) + id.len + address.len + my_slots.len)) == NULL) {
          ERR("can't create rbtree node for redis connection");
          return;
        }
        
        rdata_ptr_and_buf = rbtree_data_from_node(rbtree_node);
        rdata_ptr_and_buf->rdata = rdata;
        
        rdata->node.id.data = &rdata_ptr_and_buf->chr;
        nchan_strcpy(&rdata->node.id, &id, 0);
        
        rdata->node.address.data = &rdata_ptr_and_buf->chr + id.len;
        nchan_strcpy(&rdata->node.address, &address, 0);
        
        rdata->node.slots.data = &rdata_ptr_and_buf->chr + id.len + address.len;
        nchan_strcpy(&rdata->node.slots, &my_slots, 0);
        
        if(rbtree_insert_node(&redis_cluster_node_id_tree, rbtree_node) != NGX_OK) {
          ERR("couldn't insert redis cluster node ");
          rbtree_destroy_node(&redis_cluster_node_id_tree, rbtree_node);
          assert(0);
        }
        
        my_ctnode = *(rdstore_data_t **)rbtree_data_from_node(rbtree_node);
      }
    }
    else {
      //don't care about slaves. disconnect!
      //TODO
      assert(0);
    }
  }
  if(my_ctnode) {
    rdstore_data_t                     **ptr_rdata;
    ngx_str_t                            slot_min_str, slot_max_str;
    ngx_int_t                            slot_min, slot_max;
    u_char                              *dash;
    redis_cluster_slot_range_t           range;
    redis_cluster_keyslot_range_node_t  *keyslot_tree_node;
    if(!cluster) {
      //cluster struct not made by any node yet. make it so!
      cluster = ngx_alloc(sizeof(*cluster), ngx_cycle->log); //TODO: don't allocate from heap, use a pool or something
      
      rbtree_init(&cluster->hashslots, "redis cluster node (by id) data", rbtree_cluster_hashslots_id, rbtree_cluster_hashslots_bucketer, rbtree_cluster_hashslots_compare);
      
      cluster->size = num_master_nodes;
      cluster->uscf = rdata->lcf->redis.upstream;
      cluster->pool = NULL;
      nchan_list_init(&cluster->nodes, sizeof(rdstore_data_t *));
      
    }
    rdata->node.cluster = cluster;
    ptr_rdata = nchan_list_append(&cluster->nodes);
    *ptr_rdata = rdata;
    
    //hash slots
    for(cur = my_slots.data; cur < my_slots.data + my_slots.len; /*void*/) {
      nchan_scan_str(my_slots, cur, ',', slot, "slotrange");
      if(slot.data[0] == '[') {
        //transitional special slot. ignore it.
        continue;
      }
      dash = (u_char *)memchr(slot.data, '-', slot.len);
      
      if(dash) {
        slot_min_str.data = slot.data;
        slot_min_str.len = dash - slot.data;
        
        slot_max_str.data = dash + 1;
        slot_max_str.len = slot.len - (slot_max_str.data - slot.data);
      }
      else {
        slot_min_str = slot;
        slot_max_str = slot;
      }
      
      slot_min = ngx_atoi(slot_min_str.data, slot_min_str.len);
      slot_max = ngx_atoi(slot_max_str.data, slot_max_str.len);
      
      DBG("slots: %i - %i", slot_min, slot_max);
      
      range.min = slot_min;
      range.max = slot_max;
      
      if((rbtree_node = rbtree_find_node(&cluster->hashslots, &range)) == NULL) {
        if((rbtree_node = rbtree_create_node(&cluster->hashslots, sizeof(*keyslot_tree_node))) == NULL) {
          assert(0);
        }
        keyslot_tree_node = rbtree_data_from_node(rbtree_node);
        keyslot_tree_node->range = range;
        keyslot_tree_node->rdata = rdata;
        
        
        if(rbtree_insert_node(&cluster->hashslots, rbtree_node) != NGX_OK) {
          ERR("couldn't insert redis cluster node ");
          rbtree_destroy_node(&cluster->hashslots, rbtree_node);
          assert(0);
        }
      }
      else {
        //overlapping range found! uh oh!
        assert(0);
      }
      
    }
    
  }
  
  
  return;
  
fail:
  ERR("scan failed %s, line: %s", what, line);
  
}

typedef struct {
  rdstore_data_t      *rdata;
  ngx_rbtree_node_t   *found;
} rdata_node_finder_data_t;

rbtree_walk_direction_t rdata_node_finder(rbtree_seed_t *seed, void *data, void *privdata) {
  redis_cluster_keyslot_range_node_t *d = data;
  rdata_node_finder_data_t           *pd = privdata;
  
  if(d->rdata == pd->rdata) {
    pd->found = rbtree_node_from_data(d);
    return RBTREE_WALK_STOP;
  }
  else {
    return RBTREE_WALK_LEFT_RIGHT;
  }
}

void redis_cluster_drop_node(rdstore_data_t *rdata) {
  redis_cluster_t           *cluster = rdata->node.cluster;
  rdstore_data_t           **rdata_pptr;
  int                        found = 0;
  ngx_rbtree_node_t         *rbtree_node;
  nchan_list_el_t           *cur;
  rdata_node_finder_data_t   finder_data;
  
  if(!cluster) {
    ERR("not a cluster node");
    return;
  }
  
  
  //remove from hashslots. this is a little tricky, we walk the hashslots tree 
  // until we can's find this rdata
  finder_data.rdata = rdata;
  while(1) {
    finder_data.found = NULL;
    rbtree_conditional_walk(&cluster->hashslots, rdata_node_finder, &finder_data);
    if(finder_data.found != NULL) {
      ERR("destroyed node %p", finder_data.found);
      rbtree_remove_node(&cluster->hashslots, finder_data.found);
      rbtree_destroy_node(&cluster->hashslots, finder_data.found);
    }
    else {
      break;
    }
  }
  
  assert(cluster->nodes.n > 0);
  
  for(cur = cluster->nodes.head; cur != NULL; cur = cur->next) {
    rdata_pptr = nchan_list_data_from_el(cur);
    if(*rdata_pptr == rdata) {
      found = 1;
      break;
    }
  }
  
  assert(found);
  nchan_list_remove(&cluster->nodes, rdata_pptr);
  
  if(cluster->nodes.n == 0) {
    ngx_free(cluster);
  }
  
  rbtree_node = rbtree_find_node(&redis_cluster_node_id_tree, &rdata->node.id);
  assert(rbtree_node);
  rbtree_remove_node(&redis_cluster_node_id_tree, rbtree_node);
  
  rbtree_destroy_node(&redis_cluster_node_id_tree, rbtree_node);
}
