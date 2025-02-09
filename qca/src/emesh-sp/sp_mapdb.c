/*
 * Copyright (c) 2020-2021, The Linux Foundation. All rights reserved.
 *
 * Copyright (c) 2022-2023, Qualcomm Innovation Center, Inc. All rights reserved.
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <linux/kernel.h>
#include <linux/netfilter.h>
#include <linux/etherdevice.h>
#include <linux/if_vlan.h>
#include <net/genetlink.h>
#include <linux/notifier.h>

#include "sp_mapdb.h"
#include "sp_types.h"

/* Spinlock for SMP updating the rule table. */
DEFINE_SPINLOCK(sp_mapdb_lock);

/* SP Rule manager */
static struct sp_mapdb_rule_manager rule_manager;

/* TSL protection of single writer rule update. */
static unsigned long single_writer = 0;

/* Spm generic netlink family */
static struct genl_family sp_genl_family;

/*
 * Registration/Unregistration methods for SPM rule update/add/delete notifications.
 */
static RAW_NOTIFIER_HEAD(sp_mapdb_notifier_chain);

/*
 * sp_mapdb_notifiers_call()
 *	Call registered notifiers.
 */
int sp_mapdb_notifiers_call(struct sp_rule *info, unsigned long val)
{
	return raw_notifier_call_chain(&sp_mapdb_notifier_chain, val, info);
}

/*
 * sp_mapdb_notifier_register()
 *	Register SPM rule event notifiers.
 */
void sp_mapdb_notifier_register(struct notifier_block *nb)
{
	raw_notifier_chain_register(&sp_mapdb_notifier_chain, nb);
}
EXPORT_SYMBOL(sp_mapdb_notifier_register);

/*
 * sp_mapdb_notifier_unregister()
 *	Unregister SPM rule event notifiers.
 */
void sp_mapdb_notifier_unregister(struct notifier_block *nb)
{
	raw_notifier_chain_unregister(&sp_mapdb_notifier_chain, nb);
}
EXPORT_SYMBOL(sp_mapdb_notifier_unregister);

/*
 * sp_mapdb_rules_init()
 * 	Initializes prec_map and ruleid_hashmap.
 */
static inline void sp_mapdb_rules_init(void)
{
	int i;

	spin_lock(&sp_mapdb_lock);
	for (i = 0; i < SP_MAPDB_RULE_MAX_PRECEDENCENUM; i++) {
		INIT_LIST_HEAD(&rule_manager.prec_map[i].rule_list);
	}
	spin_unlock(&sp_mapdb_lock);

	hash_init(rule_manager.rule_id_hashmap);
	rule_manager.rule_count = 0;

	DEBUG_TRACE("%px: Finish Initializing SP ruledb\n", &rule_manager);
}

/*
 * sp_rule_destroy_rcu()
 * 	Destroys a rule node.
 */
static void sp_rule_destroy_rcu(struct rcu_head *head)
{
	struct sp_mapdb_rule_node *node_p =
		container_of(head, struct sp_mapdb_rule_node, rcu);

	DEBUG_INFO("%px: Removed rule id = %d\n", head, node_p->rule.id);
	kfree(node_p);
}

/*
 * sp_mapdb_search_hashentry()
 * 	Find the hashentry that stores the rule_node by the ruleid and rule_type
 */
static struct sp_mapdb_rule_id_hashentry *sp_mapdb_search_hashentry(uint32_t ruleid, uint8_t rule_type)
{
	struct sp_mapdb_rule_id_hashentry *hashentry_iter;

	hash_for_each_possible(rule_manager.rule_id_hashmap, hashentry_iter, hlist, ruleid) {
		if ((hashentry_iter->rule_node->rule.id == ruleid) &&
		     (hashentry_iter->rule_node->rule.classifier_type == rule_type)) {
			return hashentry_iter;
		}
	}

	return NULL;
}

/*
 * sp_mapdb_rule_add()
 * 	Adds (or modifies) the SP rule in the rule table.
 *
 * It will also updating the add_remove_modify and old_prec and field_update argument.
 * old_prec : the precedence of the previous rule.
 * field_update : if fields other than precedence is different from the previous rule.
 */
static sp_mapdb_update_result_t sp_mapdb_rule_add(struct sp_rule *newrule, uint8_t rule_type)
{
	uint8_t newrule_precedence = newrule->rule_precedence;
	uint8_t old_prec;
	bool field_update;
	struct sp_mapdb_rule_node *cur_rule_node = NULL;
	struct sp_mapdb_rule_id_hashentry *cur_hashentry = NULL;
	struct sp_mapdb_rule_node *new_rule_node;
	struct sp_mapdb_rule_id_hashentry *new_hashentry;

	DEBUG_INFO("%px: Try adding rule id = %d with rule_type: %d\n", newrule, newrule->id, rule_type);

	if (rule_manager.rule_count == SP_MAPDB_RULE_MAX) {
		DEBUG_WARN("%px:Ruletable is full. Error adding rule %d, rule_type: %d\n", newrule, newrule->id, rule_type);
		return SP_MAPDB_UPDATE_RESULT_ERR_TBLFULL;
	}

	if (newrule->inner.rule_output >= SP_MAPDB_NO_MATCH) {
		DEBUG_WARN("%px:Invalid rule output value %d (valid range:0-9)\n", newrule, newrule->inner.rule_output);
		return SP_MAPDB_UPDATE_RESULT_ERR_INVALIDENTRY;
	}

	new_rule_node = (struct sp_mapdb_rule_node *)kzalloc(sizeof(struct sp_mapdb_rule_node), GFP_KERNEL);
	if (!new_rule_node) {
		DEBUG_ERROR("%px:Error, allocate rule node failed.\n", newrule);
		return SP_MAPDB_UPDATE_RESULT_ERR_ALLOCNODE;
	}

	memcpy(&new_rule_node->rule, newrule, sizeof(struct sp_rule));
	new_rule_node->rule.classifier_type = rule_type;

	if (newrule_precedence == SP_MAPDB_RULE_MAX_PRECEDENCENUM) {
		new_rule_node->rule.rule_precedence = 0;
		newrule_precedence = new_rule_node->rule.rule_precedence;
	}

	spin_lock(&sp_mapdb_lock);
	cur_hashentry = sp_mapdb_search_hashentry(newrule->id, rule_type);
	if (!cur_hashentry) {
		spin_unlock(&sp_mapdb_lock);
		new_hashentry = (struct sp_mapdb_rule_id_hashentry *)kzalloc(sizeof(struct sp_mapdb_rule_id_hashentry), GFP_KERNEL);
		if (!new_hashentry) {
			DEBUG_ERROR("%px:Error, allocate hashentry failed.\n", newrule);
			kfree(new_rule_node);
			return SP_MAPDB_UPDATE_RESULT_ERR_ALLOCHASH;
		}

		/*
		 * Inserting new rule node and hash entry in prec_map
		 * and hashmap respectively.
		 */
		spin_lock(&sp_mapdb_lock);
		new_hashentry->rule_node = new_rule_node;

		list_add_rcu(&new_rule_node->rule_list, &rule_manager.prec_map[newrule_precedence].rule_list);
		hash_add(rule_manager.rule_id_hashmap, &new_hashentry->hlist,newrule->id);
		rule_manager.rule_count++;
		spin_unlock(&sp_mapdb_lock);

		DEBUG_INFO("%px:Success rule id=%d with rule_type: %d\n",
			   newrule, newrule->id, rule_type);

		/*
		 * Since this is inserting a new rule, the old precendence
		 * and field update do not possess any meaning.
		 */
		sp_mapdb_notifiers_call(newrule, SP_MAPDB_ADD_RULE);

		return SP_MAPDB_UPDATE_RESULT_SUCCESS_ADD;
	}

	cur_rule_node = cur_hashentry->rule_node;

	if (cur_rule_node->rule.rule_precedence == newrule_precedence) {
		list_replace_rcu(&cur_rule_node->rule_list, &new_rule_node->rule_list);
		cur_hashentry->rule_node = new_rule_node;
		spin_unlock(&sp_mapdb_lock);

		DEBUG_INFO("%px:overwrite rule id =%d rule_type: %d success.\n", newrule, newrule->id, rule_type);

		/*
		 * If precedence doesn't change then it has to be some fields modified.
		 */
		sp_mapdb_notifiers_call(newrule, SP_MAPDB_MODIFY_RULE);

		call_rcu(&cur_rule_node->rcu, sp_rule_destroy_rcu);

		return SP_MAPDB_UPDATE_RESULT_SUCCESS_MODIFY;
	}

	list_del_rcu(&cur_rule_node->rule_list);
	list_add_rcu(&new_rule_node->rule_list, &rule_manager.prec_map[newrule_precedence].rule_list);
	cur_hashentry->rule_node = new_rule_node;
	spin_unlock(&sp_mapdb_lock);

	/*
	 * Fields other than rule_precedence can still be updated along with rule_precedence.
	 */
	old_prec = cur_rule_node->rule.rule_precedence;
	field_update = memcmp(&cur_rule_node->rule.inner, &newrule->inner, sizeof(struct sp_rule_inner)) ? true : false;
	DEBUG_INFO("%px:Success rule id=%d rule_type: %d\n", newrule, newrule->id, rule_type);
	sp_mapdb_notifiers_call(newrule, SP_MAPDB_MODIFY_RULE);
	call_rcu(&cur_rule_node->rcu, sp_rule_destroy_rcu);

	return SP_MAPDB_UPDATE_RESULT_SUCCESS_MODIFY;
}

/*
 * sp_mapdb_rule_delete()
 * 	Deletes a rule from the rule table by the rule id and rule_type.
 *
 * The memory for the rule node will also be deleted as hash entry will also be freed.
 */
static sp_mapdb_update_result_t sp_mapdb_rule_delete(uint32_t ruleid, uint8_t rule_type)
{
	struct sp_mapdb_rule_node *tobedeleted;
	struct sp_mapdb_rule_id_hashentry *cur_hashentry = NULL;

	spin_lock(&sp_mapdb_lock);
	if (rule_manager.rule_count == 0) {
		spin_unlock(&sp_mapdb_lock);
		DEBUG_WARN("rule table is empty\n");
		return SP_MAPDB_UPDATE_RESULT_ERR_TBLEMPTY;
	}

	cur_hashentry = sp_mapdb_search_hashentry(ruleid, rule_type);
	if (!cur_hashentry) {
		spin_unlock(&sp_mapdb_lock);
		DEBUG_WARN("there is no such rule as ruleID = %d, rule_type: %d\n", ruleid, rule_type);
		return SP_MAPDB_UPDATE_RESULT_ERR_RULENOEXIST;
	}

	tobedeleted = cur_hashentry->rule_node;
	list_del_rcu(&tobedeleted->rule_list);
	hash_del(&cur_hashentry->hlist);
	kfree(cur_hashentry);
	rule_manager.rule_count--;
	spin_unlock(&sp_mapdb_lock);

	DEBUG_INFO("Successful deletion\n");

	/*
	 * There is no point on having old_prec
	 * and field_update in remove rules case.
	 */
	sp_mapdb_notifiers_call(&tobedeleted->rule, SP_MAPDB_REMOVE_RULE);
	call_rcu(&tobedeleted->rcu, sp_rule_destroy_rcu);

	return SP_MAPDB_UPDATE_RESULT_SUCCESS_DELETE;
}

/*
 * sp_mapdb_rule_match()
 * 	Performs rule match on received skb.
 *
 * It is called per packet basis and fields are checked and compared with the SP rule (rule).
 */
static bool sp_mapdb_rule_match(struct sk_buff *skb, struct sp_rule *rule, uint8_t *smac, uint8_t *dmac)
{
	struct ethhdr *eth_header;
	struct iphdr *iph;
	struct tcphdr *tcphdr;
	struct udphdr *udphdr;
	uint16_t src_port = 0, dst_port = 0;
	struct vlan_hdr *vhdr;
	int16_t vlan_id;
	bool compare_result, sense;
	uint32_t flags = rule->inner.flags;

	if (flags & SP_RULE_FLAG_MATCH_ALWAYS_TRUE) {
		DEBUG_INFO("Basic match case.\n");
		return true;
	}


	if (flags & SP_RULE_FLAG_MATCH_UP) {
		DEBUG_INFO("Matching UP..\n");
		DEBUG_INFO("skb->up = %d , rule->up = %d\n", skb->priority, rule->inner.user_priority);

		compare_result = (skb->priority == rule->inner.user_priority) ? true:false;
		sense = !!(flags & SP_RULE_FLAG_MATCH_UP_SENSE);
		if (!(compare_result ^ sense)) {
			DEBUG_WARN("Match UP failed\n");
			return false;
		}
	}

	if (flags & SP_RULE_FLAG_MATCH_SOURCE_MAC) {
		DEBUG_INFO("Matching SRC..\n");
		DEBUG_INFO("skb src = %pM\n", smac);
		DEBUG_INFO("rule src = %pM\n", rule->inner.sa);

		compare_result = ether_addr_equal(smac, rule->inner.sa);
		sense = !!(flags & SP_RULE_FLAG_MATCH_SOURCE_MAC_SENSE);
		if (!(compare_result ^ sense)) {
			DEBUG_WARN("SRC match failed!\n");
			return false;
		}
	}

	if (flags & SP_RULE_FLAG_MATCH_DST_MAC) {
		DEBUG_INFO("Matching DST..\n");
		DEBUG_INFO("skb dst = %pM\n", dmac);
		DEBUG_INFO("rule dst = %pM\n", rule->inner.da);

		compare_result = ether_addr_equal(dmac, rule->inner.da);
		sense = !!(flags & SP_RULE_FLAG_MATCH_DST_MAC_SENSE);
		if (!(compare_result ^ sense)) {
			DEBUG_WARN("DST match failed!\n");
			return false;
		}
	}

	eth_header = (struct ethhdr *)skb->data;
	if (flags & SP_RULE_FLAG_MATCH_VLAN_ID) {
		uint16_t ether_type = ntohs(eth_header->h_proto);


		if (ether_type == ETH_P_8021Q) {
			vhdr = (struct vlan_hdr *)(skb->data + ETH_HLEN);
			vlan_id = ntohs(vhdr->h_vlan_TCI);

			DEBUG_INFO("Matching VLAN ID..\n");
			DEBUG_INFO("skb vlan = %u\n", vlan_id);
			DEBUG_INFO("rule vlan = %u\n", rule->inner.vlan_id);

			compare_result = vlan_id == rule->inner.vlan_id;
			sense = !!(flags & SP_RULE_FLAG_MATCH_VLAN_ID_SENSE);
			if (!(compare_result ^ sense)) {
				DEBUG_WARN("SKB vlan match failed!\n");
				return false;
			}
		} else {
			return false;
		}
	}

	if (flags & (SP_RULE_FLAG_MATCH_SRC_IPV4 | SP_RULE_FLAG_MATCH_DST_IPV4 |
				SP_RULE_FLAG_MATCH_SRC_PORT | SP_RULE_FLAG_MATCH_DST_PORT |
				SP_RULE_FLAG_MATCH_DSCP | SP_RULE_FLAG_MATCH_PROTOCOL)) {
		if (skb->protocol == ntohs(ETH_P_IP)) {
			/* Check for ip header */
			if (unlikely(!pskb_may_pull(skb, sizeof(*iph)))) {
				DEBUG_INFO("No ip header in skb\n");
				return false;
			}
			iph = ip_hdr(skb);
		} else {
			DEBUG_INFO("Not ip packet protocol: %x \n", skb->protocol);
			return false;
		}
	} else {
		return true;
	}

	if (flags & SP_RULE_FLAG_MATCH_DSCP) {

		uint16_t dscp;

		dscp = ipv4_get_dsfield(ip_hdr(skb)) >> 2;

		DEBUG_INFO("Matching DSCP..\n");
		DEBUG_INFO("skb DSCP = %u\n", dscp);
		DEBUG_INFO("rule DSCP = %u\n", rule->inner.dscp);

		compare_result = dscp == rule->inner.dscp;
		sense = !!(flags & SP_RULE_FLAG_MATCH_DSCP_SENSE);
		if (!(compare_result ^ sense)) {
			DEBUG_WARN("SRC dscp match failed!\n");
			return false;
		}
	}

	if (flags & SP_RULE_FLAG_MATCH_SRC_IPV4) {
		DEBUG_INFO("Matching SRC IP..\n");
		DEBUG_INFO("skb src ipv4 =  %pI4", &iph->saddr);
		DEBUG_INFO("rule src ipv4 =  %pI4", &rule->inner.src_ipv4_addr);

		compare_result = iph->saddr == rule->inner.src_ipv4_addr;
		sense = !!(flags & SP_RULE_FLAG_MATCH_SRC_IPV4_SENSE);
		if (!(compare_result ^ sense)) {
			DEBUG_WARN("SRC ip match failed!\n");
			return false;
		}
	}

	if (flags & SP_RULE_FLAG_MATCH_DST_IPV4) {
		DEBUG_INFO("Matching DST IP..\n");
		DEBUG_INFO("skb dst ipv4 = %pI4", &iph->daddr);
		DEBUG_INFO("rule dst ipv4 = %pI4", &rule->inner.dst_ipv4_addr);

		compare_result = iph->daddr == rule->inner.dst_ipv4_addr;
		sense = !!(flags & SP_RULE_FLAG_MATCH_DST_IPV4_SENSE);
		if (!(compare_result ^ sense)) {
			DEBUG_WARN("DEST ip match failed!\n");
			return false;
		}
	}

	if (flags & SP_RULE_FLAG_MATCH_PROTOCOL) {
		DEBUG_INFO("Matching IP Protocol..\n");
		DEBUG_INFO("skb ip protocol = %u\n", iph->protocol);
		DEBUG_INFO("rule ip protocol = %u\n", rule->inner.protocol_number);

		compare_result = iph->protocol == rule->inner.protocol_number;
		sense = !!(flags & SP_RULE_FLAG_MATCH_PROTOCOL_SENSE);
		if (!(compare_result ^ sense)) {
			DEBUG_WARN("DEST ip match failed!\n");
			return false;
		}
	}

	if (iph->protocol == IPPROTO_TCP) {
		/* Check for tcp header */
		if (unlikely(!pskb_may_pull(skb, sizeof(*tcphdr)))) {
			DEBUG_INFO("No tcp header in skb\n");
			return false;
		}

		tcphdr = tcp_hdr(skb);
		src_port = tcphdr->source;
		dst_port = tcphdr->dest;
	} else if (iph->protocol == IPPROTO_UDP) {
		/* Check for udp header */
		if (unlikely(!pskb_may_pull(skb, sizeof(*udphdr)))) {
			DEBUG_INFO("No udp header in skb\n");
			return false;
		}

		udphdr = udp_hdr(skb);
		src_port = udphdr->source;
		dst_port = udphdr->dest;
	}

	if (flags & SP_RULE_FLAG_MATCH_SRC_PORT) {
		DEBUG_INFO("Matching SRC PORT..\n");
		DEBUG_INFO("skb src port = 0x%x\n", ntohs(src_port));
		DEBUG_INFO("rule srcport = 0x%x\n", rule->inner.src_port);

		compare_result = ntohs(src_port) == rule->inner.src_port;
		sense = !!(flags & SP_RULE_FLAG_MATCH_SRC_PORT_SENSE);
		if (!(compare_result ^ sense)) {
			DEBUG_WARN("SRC port match failed!\n");
			return false;
		}
	}

	if (flags & SP_RULE_FLAG_MATCH_DST_PORT) {
		DEBUG_INFO("Matching DST PORT..\n");
		DEBUG_INFO("skb dst port = 0x%x\n", ntohs(dst_port));
		DEBUG_INFO("rule dst port = 0x%x\n", rule->inner.dst_port);

		compare_result = ntohs(dst_port) == rule->inner.dst_port;
		sense = !!(flags & SP_RULE_FLAG_MATCH_DST_PORT_SENSE);
		if (!(compare_result ^ sense)) {
			DEBUG_WARN("DST port match failed!\n");
			return false;
		}
	}

	return true;
}

/*
 * sp_mapdb_ruletable_search()
 * 	Performs rules match for a skb over an entire rule table.
 *
 * According to the specification, the rules will be enumerated in a precedence descending order.
 * Once there is a match, the enumeration stops.
 * In details, it enumerates from prec_map[0xFE] to prec_map[0] for each packet, and exits loop if there is a
 * rule match. The output value defined in a matched rule
 * will be used to determine which fields(UP,DSCP) will be used for
 * PCP value.
 */
static uint8_t sp_mapdb_ruletable_search(struct sk_buff *skb, uint8_t *smac, uint8_t *dmac)
{
	uint8_t output = SP_MAPDB_NO_MATCH;
	struct sp_mapdb_rule_node *curnode;
	int i, protocol;

	rcu_read_lock();
	if (rule_manager.rule_count == 0) {
		rcu_read_unlock();
		DEBUG_WARN("rule table is empty\n");
		/*
		 * When rule table is empty, default DSCP based
		 * prioritization should be followed
		 */
		output = SP_MAPDB_USE_DSCP;
		goto set_output;
	}
	rcu_read_unlock();

	/*
	 * The iteration loop goes backward because
	 * rules should be matched in the precedence
	 * descending order.
	 */
	for (i = SP_MAPDB_RULE_MAX_PRECEDENCENUM - 1; i >= 0; i--) {
		list_for_each_entry_rcu(curnode, &(rule_manager.prec_map[i].rule_list), rule_list) {
			DEBUG_INFO("Matching with rid = %d (emesh case)\n", curnode->rule.id);
			if (curnode->rule.classifier_type == SP_RULE_TYPE_MESH) {
				if (sp_mapdb_rule_match(skb, &curnode->rule, smac, dmac)) {
					output = curnode->rule.inner.rule_output;
					goto set_output;
				}
			}
		}
	}

set_output:
	switch (output) {
	case SP_MAPDB_USE_UP:
		output = skb->priority;
		break;

	case SP_MAPDB_USE_DSCP:

		/*
		 * >> 2 first(dscp field) and then >>3 (DSCP->PCP mapping)
		 */
		protocol = ntohs(skb->protocol);

		switch (protocol) {
		case ETH_P_IP:
			output = (ipv4_get_dsfield(ip_hdr(skb))) >> 5;
			break;

		case ETH_P_IPV6:
			output = (ipv6_get_dsfield(ipv6_hdr(skb))) >> 5;
			break;

		default:

			/*
			 * non-IP protocol does not have dscp field, apply DEFAULT_PCP.
			 */
			output = SP_MAPDB_RULE_DEFAULT_PCP;
			break;
		}
		break;

	case SP_MAPDB_NO_MATCH:
		output = SP_MAPDB_RULE_DEFAULT_PCP;
		break;

	default:
		break;
	}

	return output;
}

/*
 * sp_mapdb_ruletable_flush()
 * 	Clear the rule table and frees the memory allocated for the rules.
 *
 * It will enumerate all the precedence in the prec_map,
 * and start from the head node in each of the precedence in the prec_map,
 * and free all the rule nodes
 * as well as the associated hashentry, with
 * these precedence.
 */
void sp_mapdb_ruletable_flush(void)
{
	int i;

	struct sp_mapdb_rule_node *node_list, *node, *tmp;
	struct sp_mapdb_rule_id_hashentry *hashentry_iter;
	struct hlist_node *hlist_tmp;
	int hash_bkt;

	spin_lock(&sp_mapdb_lock);
	if (rule_manager.rule_count == 0) {
		spin_unlock(&sp_mapdb_lock);
		DEBUG_WARN("The rule table is already empty. No action needed. \n");
		return;
	}

	for (i = 0; i < SP_MAPDB_RULE_MAX_PRECEDENCENUM; i++) {
		node_list = &rule_manager.prec_map[i];
		/*
		 * tmp as a temporary pointer to store the address of next node.
		 * This is required because we are using list_for_each_entry_safe,
		 * which allows in-loop deletion of the node.
		 *
		 */
		list_for_each_entry_safe(node, tmp, &node_list->rule_list, rule_list) {
			list_del_rcu(&node->rule_list);
			call_rcu(&node->rcu, sp_rule_destroy_rcu);
		}
	}

	/* Free hash list. */
	hash_for_each_safe(rule_manager.rule_id_hashmap, hash_bkt, hlist_tmp, hashentry_iter, hlist) {
		hash_del(&hashentry_iter->hlist);
		kfree(hashentry_iter);
	}
	rule_manager.rule_count = 0;
	spin_unlock(&sp_mapdb_lock);
}
EXPORT_SYMBOL(sp_mapdb_ruletable_flush);

/*
 * sp_mapdb_rule_update()
 * 	Perfoms rule update
 *
 * It will first check the add/remove filter bit of
 * the newrule and pass it to sp_mapdb_rule_add and sp_mapdb_rule_delete acccordingly.
 * sp_mapdb_rule_update will also collect add_remove_modify based on the updated result, as:
 * add = 0, remove = 1, modify = 2
 * and field_update (meaning whether the field(other than precence)
 * is modified), these are useful in perform precise matching in ECM.
 */
sp_mapdb_update_result_t sp_mapdb_rule_update(struct sp_rule *newrule)
{
	sp_mapdb_update_result_t error_code = 0;

	if (!newrule) {
		return SP_MAPDB_UPDATE_RESULT_ERR_NEWRULE_NULLPTR;
	}

	if (test_and_set_bit_lock(0, &single_writer)) {
		DEBUG_ERROR("%px: single writer allowed", newrule);
		return SP_MAPDB_UPDATE_RESULT_ERR_SINGLE_WRITER;
	}

	switch (newrule->cmd) {
	case SP_MAPDB_ADD_REMOVE_FILTER_DELETE:
		error_code = sp_mapdb_rule_delete(newrule->id, newrule->classifier_type);
		break;

	case SP_MAPDB_ADD_REMOVE_FILTER_ADD:
		error_code = sp_mapdb_rule_add(newrule, newrule->classifier_type);
		break;

	default:
		DEBUG_ERROR("%px: Error, unknown Add/Remove filter bit\n", newrule);
		error_code = SP_MAPDB_UPDATE_RESULT_ERR_UNKNOWNBIT;
		break;
	}

	clear_bit_unlock(0, &single_writer);

	return error_code;
}
EXPORT_SYMBOL(sp_mapdb_rule_update);

/*
 * sp_mapdb_rule_print_input_params()
 * 	Print the input parameters of current rule.
 */
static inline void sp_mapdb_rule_print_input_params(struct sp_mapdb_rule_node *curnode)
{
	printk("\n........INPUT PARAMS........\n");
	printk("src_mac: %pM, dst_mac: %pM, src_port: %d, dst_port: %d, ip_version_type: %d\n",
			curnode->rule.inner.sa, curnode->rule.inner.da, curnode->rule.inner.src_port,
			curnode->rule.inner.dst_port, curnode->rule.inner.ip_version_type);
	printk("dscp: %d, dscp remark: %d, vlan id: %d, vlan pcp: %d, vlan pcp remark: %d, protocol number: %d\n",
			curnode->rule.inner.dscp, curnode->rule.inner.dscp_remark, curnode->rule.inner.vlan_id,
			curnode->rule.inner.vlan_pcp, curnode->rule.inner.vlan_pcp_remark, curnode->rule.inner.protocol_number);

	printk("src_ipv4: %pI4, dst_ipv4: %pI4\n", &curnode->rule.inner.src_ipv4_addr, &curnode->rule.inner.dst_ipv4_addr);

	printk("src_ipv6: %pI6: dst_ipv6: %pI6\n", &curnode->rule.inner.src_ipv6_addr, &curnode->rule.inner.dst_ipv6_addr);

	printk("src_ipv4_mask: %pI4, dst_ipv4_mask: %pI4\n", &curnode->rule.inner.src_ipv4_addr_mask, &curnode->rule.inner.dst_ipv4_addr_mask);

	printk("src_ipv6_mask: %pI6: dst_ipv6_mask: %pI6\n", &curnode->rule.inner.src_ipv6_addr_mask, &curnode->rule.inner.dst_ipv6_addr_mask);
	printk("match pattern value: %x: match pattern mask: %x\n", curnode->rule.inner.match_pattern_value, curnode->rule.inner.match_pattern_mask);
	printk("MSCS TID BITMAP: %x: Priority Limit Value: %x\n", curnode->rule.inner.mscs_tid_bitmap, curnode->rule.inner.priority_limit);
	printk("Interface Index : %d\n", curnode->rule.inner.ifindex);
	printk("src_port: 0x%x, dst_port: 0x%x, src_port_range_start: 0x%x, src_port_range_end: 0x%x, dst_port_range_start: 0x%x, dst_port_range_end: 0x%x\n",
			curnode->rule.inner.src_port, curnode->rule.inner.dst_port, curnode->rule.inner.src_port_range_start,
			curnode->rule.inner.src_port_range_end, curnode->rule.inner.dst_port_range_start,
			curnode->rule.inner.dst_port_range_end);
}

/*
 * sp_mapdb_ruletable_print()
 *	Print the rule table.
 */
void sp_mapdb_ruletable_print(void)
{
	int i;
	struct sp_mapdb_rule_node *curnode = NULL;

	rcu_read_lock();
	printk("\n====Rule table start====\nTotal rule count = %d\n", rule_manager.rule_count);
	for (i = SP_MAPDB_RULE_MAX_PRECEDENCENUM - 1; i >= 0; i--) {
		if (!list_empty(&(rule_manager.prec_map[i].rule_list))) {
			list_for_each_entry_rcu(curnode, &(rule_manager.prec_map[i].rule_list), rule_list) {
				printk("\nid: %d, classifier_type: %d, precedence: %d\n", curnode->rule.id, curnode->rule.classifier_type, curnode->rule.rule_precedence);
				sp_mapdb_rule_print_input_params(curnode);
				printk("\n........OUTPUT PARAMS........\n");
				printk("dscp_remark: %d, vlan_pcp_remark: %d\n", curnode->rule.inner.dscp_remark, curnode->rule.inner.vlan_pcp_remark);
				printk("output(priority): %d, service_class_id: %d\n", curnode->rule.inner.rule_output, curnode->rule.inner.service_class_id);
				printk("MSCS TID BITMAP: %x: Priority Limit Value: %x\n", curnode->rule.inner.mscs_tid_bitmap, curnode->rule.inner.priority_limit);
			}
		}
	}
	rcu_read_unlock();
	printk("====Rule table ends====\n");
}

/*
 * sp_mapdb_get_wlan_latency_params()
 *  Get latency parameters associated with a sp rule.
 */
void sp_mapdb_get_wlan_latency_params(struct sk_buff *skb,
		uint8_t *service_interval_dl, uint32_t *burst_size_dl,
		uint8_t *service_interval_ul, uint32_t *burst_size_ul,
		uint8_t *smac, uint8_t *dmac)
{
	struct sp_mapdb_rule_node *curnode;
	int i;

	/*
	 * Look up for matching rule and find WiFi latency parameters
	 */
	rcu_read_lock();

	for (i = SP_MAPDB_RULE_MAX_PRECEDENCENUM - 1; i >= 0; i--) {
		list_for_each_entry_rcu(curnode, &(rule_manager.prec_map[i].rule_list), rule_list) {
			DEBUG_INFO("Matching with rid = %d\n", curnode->rule.id);
			if (sp_mapdb_rule_match(skb, &curnode->rule, smac, dmac)) {
				*service_interval_dl = curnode->rule.inner.service_interval_dl;
				*burst_size_dl = curnode->rule.inner.burst_size_dl;
				*service_interval_ul = curnode->rule.inner.service_interval_ul;
				*burst_size_ul = curnode->rule.inner.burst_size_ul;
				rcu_read_unlock();
				return;
			}
		}
	}

	/*
	 * No match found, set both latency parameters to zero
	 * which is invalid value
	 */
	*service_interval_dl = 0;
	*burst_size_dl = 0;
	*service_interval_ul = 0;
	*burst_size_ul = 0;

	rcu_read_unlock();
}
EXPORT_SYMBOL(sp_mapdb_get_wlan_latency_params);

/*
 * sp_mapdb_apply()
 * 	Assign the desired PCP value into skb->priority.
 */
void sp_mapdb_apply(struct sk_buff *skb, uint8_t *smac, uint8_t *dmac)
{
	rcu_read_lock();
	skb->priority = sp_mapdb_ruletable_search(skb, smac, dmac);
	rcu_read_unlock();
}
EXPORT_SYMBOL(sp_mapdb_apply);

/*
 * sp_mapdb_init()
 * 	Initialize ruledb.
 */
void sp_mapdb_init(void)
{
	sp_mapdb_rules_init();
}

/*
 * sp_mapdb_rule_match_sawf()
 * 	Performs rule match on received skb.
 *
 * It is called per packet basis and fields are checked and compared with the SP rule (rule).
 */
static inline bool sp_mapdb_rule_match_sawf(struct sp_rule *rule, struct sp_rule_input_params *params)
{
	bool compare_result;
	uint32_t flags = rule->inner.flags_sawf;

	if (flags & SP_RULE_FLAG_MATCH_SAWF_IP_VERSION_TYPE) {
		DEBUG_INFO("Matching IP version type..\n");
		DEBUG_INFO("Input ip version type = 0x%x\n", params->ip_version_type);
		DEBUG_INFO("rule ip version type = 0x%x\n", rule->inner.ip_version_type);
		compare_result = params->ip_version_type == rule->inner.ip_version_type;
		if (!compare_result) {
			DEBUG_WARN("IP version match failed!\n");
			return false;
		}
	}

	if (flags & SP_RULE_FLAG_MATCH_SAWF_DST_MAC) {
		DEBUG_INFO("Matching DST..\n");
		DEBUG_INFO("Input dst = %pM\n", params->dst.mac);
		DEBUG_INFO("rule dst = %pM\n", rule->inner.da);
		compare_result = ether_addr_equal(params->dst.mac, rule->inner.da);
		if (!compare_result) {

			/*
			 * If the rule is sawf-scs type, then we further check for
			 * mac address of the netdevice interfaces.
			 */
			if (rule->classifier_type == SP_RULE_TYPE_SAWF_SCS) {
				compare_result = ether_addr_equal(params->dev_addr, rule->inner.da) &&
							(params->ifindex == rule->inner.ifindex);
				if (!compare_result) {
					DEBUG_WARN("Netdev address and device ID match failed!\n");
					return false;
				}
			} else {
				DEBUG_WARN("DST mac address match failed!\n");
				return false;
			}
		}
	}

	if (flags & SP_RULE_FLAG_MATCH_SAWF_DST_PORT) {
		DEBUG_INFO("Matching DST PORT..\n");
		DEBUG_INFO("Input dst port = 0x%x\n", params->dst.port);
		DEBUG_INFO("rule dst port = 0x%x\n", rule->inner.dst_port);
		compare_result = params->dst.port == rule->inner.dst_port;
		if (!compare_result) {
			DEBUG_WARN("DST port match failed!\n");
			return false;
		}
	}

	if ((flags & SP_RULE_FLAG_MATCH_SAWF_DST_PORT_RANGE_START) && (flags & SP_RULE_FLAG_MATCH_SAWF_DST_PORT_RANGE_END)) {
		DEBUG_INFO("Matching DST PORT RANGE..\n");
		DEBUG_INFO("skb dst port = 0x%x\n", params->dst.port);
		DEBUG_INFO("rule dst port range start = 0x%x\n", rule->inner.dst_port_range_start);
		DEBUG_INFO("rule dst port range end = 0x%x\n", rule->inner.dst_port_range_end);

		compare_result = ((params->dst.port >= rule->inner.dst_port_range_start) &&
					(params->dst.port <= rule->inner.dst_port_range_end));
		if (!compare_result) {
			DEBUG_WARN("DST port range match failed!\n");
			return false;
		}
	}

	if (flags & SP_RULE_FLAG_MATCH_SAWF_DST_IPV4) {
		DEBUG_INFO("Matching DST IP..\n");
		DEBUG_INFO("Input dst ipv4 = %pI4", &params->dst.ip.ipv4_addr);
		DEBUG_INFO("rule dst ipv4 = %pI4", &rule->inner.dst_ipv4_addr);

		if (flags & SP_RULE_FLAG_MATCH_SAWF_DST_IPV4_MASK) {
			params->dst.ip.ipv4_addr &= rule->inner.dst_ipv4_addr_mask;
		}

		compare_result = params->dst.ip.ipv4_addr == rule->inner.dst_ipv4_addr;
		if (!compare_result) {
			DEBUG_WARN("DEST ip match failed!\n");
			return false;
		}
	}

	if (flags & SP_RULE_FLAG_MATCH_SAWF_SOURCE_MAC) {
		DEBUG_INFO("Matching SRC..\n");
		DEBUG_INFO("Input src = %pM\n", params->src.mac);
		DEBUG_INFO("rule src = %pM\n", rule->inner.sa);
		compare_result = ether_addr_equal(params->src.mac, rule->inner.sa);
		if (!compare_result) {
			DEBUG_WARN("SRC match failed!\n");
			return false;
		}
	}

	if (flags & SP_RULE_FLAG_MATCH_SAWF_SRC_IPV6) {
                DEBUG_INFO("Matching SRC IPv6..\n");
                DEBUG_INFO("Input src IPv6 =  %pI6", &params->src.ip.ipv6_addr);
                DEBUG_INFO("rule src IPv6 =  %pI6", &rule->inner.src_ipv6_addr);

                if (flags & SP_RULE_FLAG_MATCH_SAWF_SRC_IPV6_MASK) {
                        params->src.ip.ipv6_addr[0] &= rule->inner.src_ipv6_addr_mask[0];
                        params->src.ip.ipv6_addr[1] &= rule->inner.src_ipv6_addr_mask[1];
                        params->src.ip.ipv6_addr[2] &= rule->inner.src_ipv6_addr_mask[2];
                        params->src.ip.ipv6_addr[3] &= rule->inner.src_ipv6_addr_mask[3];
                }

                compare_result = memcmp(params->src.ip.ipv6_addr, rule->inner.src_ipv6_addr, sizeof(uint32_t) * 4);
                if (compare_result) {
                        DEBUG_WARN("SRC IPv6 match failed!\n");
                        return false;
                }
        }

        if (flags & SP_RULE_FLAG_MATCH_SAWF_DST_IPV6) {
                DEBUG_INFO("Matching DST IPv6..\n");
                DEBUG_INFO("Input dst IPv6 = %pI6", &params->dst.ip.ipv6_addr);
                DEBUG_INFO("rule dst IPv6 = %pI6", &rule->inner.dst_ipv6_addr);

                if (flags & SP_RULE_FLAG_MATCH_SAWF_DST_IPV6_MASK) {
                        params->dst.ip.ipv6_addr[0] &= rule->inner.dst_ipv6_addr_mask[0];
                        params->dst.ip.ipv6_addr[1] &= rule->inner.dst_ipv6_addr_mask[1];
                        params->dst.ip.ipv6_addr[2] &= rule->inner.dst_ipv6_addr_mask[2];
                        params->dst.ip.ipv6_addr[3] &= rule->inner.dst_ipv6_addr_mask[3];
                }

                compare_result = memcmp(params->dst.ip.ipv6_addr, rule->inner.dst_ipv6_addr, sizeof(uint32_t) * 4);
                if (compare_result) {
                        DEBUG_WARN("DEST IPv6 match failed!\n");
                        return false;
                }
        }

	if (flags & SP_RULE_FLAG_MATCH_SAWF_SRC_PORT) {
		DEBUG_INFO("Matching SRC PORT..\n");
		DEBUG_INFO("Input src port = 0x%x\n", params->src.port);
		DEBUG_INFO("rule srcport = 0x%x\n", rule->inner.src_port);
		compare_result = params->src.port == rule->inner.src_port;
		if (!compare_result) {
			DEBUG_WARN("SRC port match failed!\n");
			return false;
		}
	}

	if ((flags & SP_RULE_FLAG_MATCH_SAWF_SRC_PORT_RANGE_START) && (flags & SP_RULE_FLAG_MATCH_SAWF_SRC_PORT_RANGE_END)) {
		DEBUG_INFO("Matching SRC PORT RANGE..\n");
		DEBUG_INFO("skb src port = 0x%x\n", params->src.port);
		DEBUG_INFO("rule src port range start = 0x%x\n", rule->inner.src_port_range_start);
		DEBUG_INFO("rule src port range end = 0x%x\n", rule->inner.src_port_range_end);

		compare_result = ((params->src.port >= rule->inner.src_port_range_start) &&
					(params->src.port <= rule->inner.src_port_range_end));
		if (!compare_result) {
			DEBUG_WARN("SRC port range match failed!\n");
			return false;
		}
	}

	if (flags & SP_RULE_FLAG_MATCH_SAWF_SRC_IPV4) {
		DEBUG_INFO("Matching SRC IP..\n");
		DEBUG_INFO("Input src ipv4 =  %pI4", &params->src.ip.ipv4_addr);
		DEBUG_INFO("rule src ipv4 =  %pI4", &rule->inner.src_ipv4_addr);

		if (flags & SP_RULE_FLAG_MATCH_SAWF_SRC_IPV4_MASK) {
			params->src.ip.ipv4_addr &= rule->inner.src_ipv4_addr_mask;
		}

		compare_result = params->src.ip.ipv4_addr == rule->inner.src_ipv4_addr;
		if (!compare_result) {
			DEBUG_WARN("SRC ip match failed!\n");
			return false;
		}
	}

	if (flags & SP_RULE_FLAG_MATCH_SAWF_PROTOCOL) {
		DEBUG_INFO("Matching IP Protocol..\n");
		DEBUG_INFO("Input ip protocol = %u\n", params->protocol);
		DEBUG_INFO("rule ip protocol = %u\n", rule->inner.protocol_number);
		compare_result = params->protocol == rule->inner.protocol_number;
		if (!compare_result) {
			DEBUG_WARN("Protocol match failed!\n");
			return false;
		}
	}

	if (flags & SP_RULE_FLAG_MATCH_SAWF_DSCP) {
		DEBUG_INFO("Matching DSCP..\n");
		DEBUG_INFO("Input DSCP = %u\n", params->dscp);
		DEBUG_INFO("rule DSCP = %u\n", rule->inner.dscp);
		compare_result = params->dscp == rule->inner.dscp;
		if (!compare_result) {
			DEBUG_WARN("DSCP match failed!\n");
			return false;
		}
	}

	if (flags & SP_RULE_FLAG_MATCH_SAWF_VLAN_PCP) {
		uint8_t vlan_pcp;
		if (params->vlan_tci == SP_RULE_INVALID_VLAN_TCI) {
			DEBUG_WARN("Vlan PCP match failed due to invalid vlan tag!\n");
			return false;
		}

		vlan_pcp = (params->vlan_tci & VLAN_PRIO_MASK) >> VLAN_PRIO_SHIFT;

		DEBUG_INFO("Matching PCP..\n");
		DEBUG_INFO("Input Vlan pcp = %u\n", vlan_pcp);
		DEBUG_INFO("rule Vlan PCP = %u\n", rule->inner.vlan_pcp);
		compare_result = vlan_pcp == rule->inner.vlan_pcp;
		if (!compare_result) {
			DEBUG_WARN("Vlan PCP match failed!\n");
			return false;
		}
	}

	if (flags & SP_RULE_FLAG_MATCH_SAWF_VLAN_ID) {
		uint16_t vlan_id;
		if (params->vlan_tci == SP_RULE_INVALID_VLAN_TCI) {
			DEBUG_WARN("Vlan ID match failed due to invalid vlan tag!\n");
			return false;
		}

		vlan_id = params->vlan_tci & VLAN_VID_MASK;
		DEBUG_INFO("Matching Vlan ID..\n");
		DEBUG_INFO("Input Vlan ID = %u\n", vlan_id);
		DEBUG_INFO("rule Vlan ID = %u\n", rule->inner.vlan_id);
		compare_result = vlan_id == rule->inner.vlan_id;
		if (!compare_result) {
			DEBUG_WARN("Vlan ID match failed!\n");
			return false;
		}
	}

	if (flags & SP_RULE_FLAG_MATCH_SCS_SPI) {
		DEBUG_INFO("Matching SPI..\n");
		DEBUG_INFO("Input SPI = %u\n", params->spi);
		DEBUG_INFO("rule match pattern value = %x, match pattern mask = %x\n", rule->inner.match_pattern_value, rule->inner.match_pattern_mask);
		params->spi &= rule->inner.match_pattern_mask;
		compare_result = params->spi == rule->inner.match_pattern_value;
		if (!compare_result) {
			DEBUG_WARN("SPI match failed!\n");
			return false;
		}
	}

	return true;
}

/*
 * sp_mapdb_rule_apply_sawf()
 * 	Assign the desired PCP value into skb->priority,
 * 	return sp_rule_output_params structure
 */
void sp_mapdb_rule_apply_sawf(struct sk_buff *skb, struct sp_rule_input_params *params,
			      struct sp_rule_output_params *rule_output)
{
	int i;
	struct sp_mapdb_rule_node *curnode;
	uint8_t dscp_remark = SP_RULE_INVALID_DSCP_REMARK;
	uint8_t vlan_pcp_remark = SP_RULE_INVALID_VLAN_PCP_REMARK;
	uint8_t service_class_id = SP_RULE_INVALID_SERVICE_CLASS_ID;
	uint8_t output = SP_MAPDB_USE_DSCP;
	uint32_t rule_id = SP_RULE_INVALID_RULE_ID;

	rcu_read_lock();
	if (rule_manager.rule_count == 0) {
		rcu_read_unlock();
		DEBUG_WARN("rule table is empty\n");
		/*
		 * When rule table is empty, default DSCP based
		 * prioritization should be followed
		 */
		goto set_output;
	}
	rcu_read_unlock();

	/*
	 * The iteration loop goes backward because
	 * rules should be matched in the precedence
	 * descending order.
	 */

	/* Traverse for SAWF rule type */
	for (i = SP_MAPDB_RULE_MAX_PRECEDENCENUM - 1; i >= 0; i--) {
		list_for_each_entry_rcu(curnode, &(rule_manager.prec_map[i].rule_list), rule_list) {
			DEBUG_INFO("Matching with rule id = %d (sawf case)\n", curnode->rule.id);
			if (curnode->rule.classifier_type == SP_RULE_TYPE_SAWF) {
				if (sp_mapdb_rule_match_sawf(&curnode->rule, params)) {
					output = curnode->rule.inner.rule_output;
					dscp_remark = curnode->rule.inner.dscp_remark;
					vlan_pcp_remark = curnode->rule.inner.vlan_pcp_remark;
					service_class_id = curnode->rule.inner.service_class_id;
					rule_id = curnode->rule.id;
					goto set_output;
				}
			}
		}
	}

	/* Traverse for SAWF-SCS rule type */
	for (i = SP_MAPDB_RULE_MAX_PRECEDENCENUM - 1; i >= 0; i--) {
		list_for_each_entry_rcu(curnode, &(rule_manager.prec_map[i].rule_list), rule_list) {
			DEBUG_INFO("Matching with rule id = %d (sawf-scs case)\n", curnode->rule.id);
			if (curnode->rule.classifier_type == SP_RULE_TYPE_SAWF_SCS) {
				if (sp_mapdb_rule_match_sawf(&curnode->rule, params)) {
					output = curnode->rule.inner.rule_output;
					dscp_remark = curnode->rule.inner.dscp_remark;
					vlan_pcp_remark = curnode->rule.inner.vlan_pcp_remark;
					service_class_id = curnode->rule.inner.service_class_id;
					rule_id = curnode->rule.id;
					goto set_output;
				}
			}
		}
	}


set_output:
	rule_output->service_class_id = service_class_id;
	rule_output->rule_id = rule_id;
	rule_output->priority = output;
	rule_output->dscp_remark = dscp_remark;
	rule_output->vlan_pcp_remark = vlan_pcp_remark;
}
EXPORT_SYMBOL(sp_mapdb_rule_apply_sawf);

/*
 * sp_mapdb_apply_scs()
 * 	Assign the user priority value into skb->priority on rule match.
 */
void sp_mapdb_apply_scs(struct sk_buff *skb, struct sp_rule_input_params *params, struct sp_rule_output_params *output)
{
	int i;
	struct sp_mapdb_rule_node *curnode;
	uint8_t priority = SP_RULE_INVALID_PRIORITY;
	uint32_t rule_id = SP_RULE_INVALID_RULE_ID;
	rcu_read_lock();
	if (rule_manager.rule_count == 0) {
		rcu_read_unlock();
		DEBUG_WARN("rule table is empty\n");
		/*
		 * Rule table is empty.
		 */
		goto set_output;
	}
	rcu_read_unlock();

	/*
	 * The iteration loop goes backward because
	 * rules should be matched in the precedence
	 * descending order.
	 */
	for (i = SP_MAPDB_RULE_MAX_PRECEDENCENUM - 1; i >= 0; i--) {
		list_for_each_entry_rcu(curnode, &(rule_manager.prec_map[i].rule_list), rule_list) {
			DEBUG_INFO("Matching with rule id = %d (scs case)\n", curnode->rule.id);
			if (curnode->rule.classifier_type == SP_RULE_TYPE_SCS) {
				if (sp_mapdb_rule_match_sawf(&curnode->rule, params)) {
					priority = curnode->rule.inner.rule_output;
					rule_id = curnode->rule.id;
					goto set_output;
				}
			}
		}
	}

set_output:
	output->rule_id = rule_id;
	output->priority = priority;
}
EXPORT_SYMBOL(sp_mapdb_apply_scs);

/*
 * sp_mapdb_apply_mscs()
 *      Assign the user priority value into skb->priority on rule match.
 */
void sp_mapdb_apply_mscs(struct sk_buff *skb, struct sp_rule_input_params *params, struct sp_rule_output_params *output)
{
	int i;
	struct sp_mapdb_rule_node *curnode;
	uint8_t priority = SP_RULE_INVALID_PRIORITY;
	uint8_t mscs_tid_bitmap = SP_RULE_INVALID_MSCS_TID_BITMAP;
	uint32_t rule_id = SP_RULE_INVALID_RULE_ID;
	rcu_read_lock();
	if (rule_manager.rule_count == 0) {
		rcu_read_unlock();
		DEBUG_WARN("rule table is empty\n");
		/*
		 * Rule table is empty.
		 */
		goto set_output;
	}

	rcu_read_unlock();

	/*
	 * The iteration loop goes backward because
	 * rules should be matched in the precedence
	 * descending order.
	 */
	for (i = SP_MAPDB_RULE_MAX_PRECEDENCENUM - 1; i >= 0; i--) {
		list_for_each_entry_rcu(curnode, &(rule_manager.prec_map[i].rule_list), rule_list) {
			DEBUG_INFO("Matching with rule id = %d (mscs case)\n", curnode->rule.id);
			if (curnode->rule.classifier_type == SP_RULE_TYPE_MSCS) {
				if (sp_mapdb_rule_match_sawf(&curnode->rule, params)) {
					mscs_tid_bitmap = curnode->rule.inner.mscs_tid_bitmap;
					/*
					 * Check the priority of the tid bit map received from rule.
					 */
					if (mscs_tid_bitmap != SP_RULE_INVALID_MSCS_TID_BITMAP) {
						if ((1 << skb->priority) & mscs_tid_bitmap) {
							priority = skb->priority;
							rule_id = curnode->rule.id;
							goto set_output;
						}
					}
				}
			}
		}
	}

set_output:
	output->rule_id = rule_id;
	output->priority = priority;
}
EXPORT_SYMBOL(sp_mapdb_apply_mscs);

/*
 * sp_mapdb_rule_receive_status_notify()
 * 	Message reply to userspace about the status of rule addition or failure.
 */
int sp_mapdb_rule_receive_status_notify(struct sk_buff **msg, void **hdr, struct genl_info *info, uint32_t rule_id, uint8_t rule_result)
{
	*msg = nlmsg_new(NLMSG_DEFAULT_SIZE, GFP_KERNEL);
	if (!*msg) {
		DEBUG_WARN("Failed to allocate netlink message to accomodate rule\n");
		return -ENOMEM;
	}

	*hdr = genlmsg_put(*msg, info->snd_portid, info->snd_seq,
			&sp_genl_family, 0, SPM_CMD_RULE_ACTION);

	if (!*hdr) {
		DEBUG_WARN("Failed to put hdr in netlink buffer\n");
		nlmsg_free(*msg);
		return -ENOMEM;
	}

	if (nla_put_u32(*msg, SP_GNL_ATTR_ID, rule_id) ||
		nla_put_u8(*msg, SP_GNL_ATTR_ADD_DELETE_RULE, rule_result)) {
		goto put_failure;
	}

	return 0;

put_failure:
	genlmsg_cancel(*msg, *hdr);
	nlmsg_free(*msg);
	return -EMSGSIZE;
}

/*
 * sp_mapdb_rule_receive()
 * 	Handles a netlink message from userspace for rule add/delete/update
 */
static inline int sp_mapdb_rule_receive(struct sk_buff *skb, struct genl_info *info)
{
	struct sp_rule to_sawf_sp = {0};
	int rule_cmd;
	uint32_t mask = 0;
	sp_mapdb_update_result_t err;
	int rule_result;
	void *hdr = NULL;
	struct sk_buff *msg = NULL;

	/*
	 * Set the invalid output values in rule to avoid these values to be set as 0's in
	 * EMESH-SAWF classifier. If valid values are received from userspace we set the flags
	 * and update the parameters accordingly.
	 */
	to_sawf_sp.inner.service_class_id = SP_RULE_INVALID_SERVICE_CLASS_ID;
	to_sawf_sp.inner.dscp_remark = SP_RULE_INVALID_DSCP_REMARK;
	to_sawf_sp.inner.vlan_pcp_remark = SP_RULE_INVALID_VLAN_PCP_REMARK;
	to_sawf_sp.inner.mscs_tid_bitmap = SP_RULE_INVALID_MSCS_TID_BITMAP;

	rcu_read_lock();
	DEBUG_INFO("Recieved rule...\n");

	if (info->attrs[SP_GNL_ATTR_ID]) {
		to_sawf_sp.id = nla_get_u32(info->attrs[SP_GNL_ATTR_ID]);
		DEBUG_INFO("Rule id:  0x%x \n", to_sawf_sp.id);
	}

	if (info->attrs[SP_GNL_ATTR_ADD_DELETE_RULE]) {
		rule_cmd = nla_get_u8(info->attrs[SP_GNL_ATTR_ADD_DELETE_RULE]);
		if (rule_cmd == SP_MAPDB_ADD_REMOVE_FILTER_DELETE) {
			to_sawf_sp.cmd = rule_cmd;
			DEBUG_INFO("Deleting rule \n");
		} else if (rule_cmd == SP_MAPDB_ADD_REMOVE_FILTER_ADD) {
			to_sawf_sp.cmd = rule_cmd;
			DEBUG_INFO("Adding rule \n");
		} else {
			rcu_read_unlock();
			DEBUG_ERROR("Invalid rule cmd\n");
			rule_result = SP_MAPDB_UPDATE_RESULT_ERR_INVALIDENTRY;
			goto status_notify;
		}
	}

	if (info->attrs[SP_GNL_ATTR_RULE_PRECEDENCE]) {
		to_sawf_sp.rule_precedence = nla_get_u8(info->attrs[SP_GNL_ATTR_RULE_PRECEDENCE]);
		DEBUG_INFO("Rule precedence: 0x%x\n", to_sawf_sp.rule_precedence);
	}

	if (info->attrs[SP_GNL_ATTR_RULE_OUTPUT]) {
		to_sawf_sp.inner.rule_output = nla_get_u8(info->attrs[SP_GNL_ATTR_RULE_OUTPUT]);
		DEBUG_INFO("Rule output: 0x%x\n", to_sawf_sp.inner.rule_output);
	}

	if (info->attrs[SP_GNL_ATTR_USER_PRIORITY]) {
		to_sawf_sp.inner.user_priority = nla_get_u8(info->attrs[SP_GNL_ATTR_USER_PRIORITY]);
		DEBUG_INFO("User priority: 0x%x\n", to_sawf_sp.inner.user_priority);
	}

	if (info->attrs[SP_GNL_ATTR_SERVICE_CLASS_ID]) {
		to_sawf_sp.inner.service_class_id = nla_get_u8(info->attrs[SP_GNL_ATTR_SERVICE_CLASS_ID]);
		DEBUG_INFO("Service_class_id: 0x%x\n", to_sawf_sp.inner.service_class_id);
	}

	if (info->attrs[SP_GNL_ATTR_SRC_PORT]) {
		to_sawf_sp.inner.src_port = nla_get_u16(info->attrs[SP_GNL_ATTR_SRC_PORT]);
		mask |= SP_RULE_FLAG_MATCH_SAWF_SRC_PORT;
		DEBUG_INFO("Source port: 0x%x\n", to_sawf_sp.inner.src_port);
	}

	if (info->attrs[SP_GNL_ATTR_DST_PORT]) {
		to_sawf_sp.inner.dst_port = nla_get_u16(info->attrs[SP_GNL_ATTR_DST_PORT]);
		mask |= SP_RULE_FLAG_MATCH_SAWF_DST_PORT;
		DEBUG_INFO("Destination port: 0x%x\n", to_sawf_sp.inner.dst_port);
	}

	if (info->attrs[SP_GNL_ATTR_SRC_MAC]) {
		memcpy(to_sawf_sp.inner.sa, nla_data(info->attrs[SP_GNL_ATTR_SRC_MAC]), ETH_ALEN);
		mask |= SP_RULE_FLAG_MATCH_SAWF_SOURCE_MAC;
		DEBUG_INFO("sa = %pM \n", to_sawf_sp.inner.sa);
	}

	if (info->attrs[SP_GNL_ATTR_DST_MAC]) {
		memcpy(to_sawf_sp.inner.da, nla_data(info->attrs[SP_GNL_ATTR_DST_MAC]), ETH_ALEN);
		mask |= SP_RULE_FLAG_MATCH_SAWF_DST_MAC;
		DEBUG_INFO("da = %pM \n", to_sawf_sp.inner.da);
	}

	if (info->attrs[SP_GNL_ATTR_IP_VERSION_TYPE]) {
		to_sawf_sp.inner.ip_version_type = nla_get_u8(info->attrs[SP_GNL_ATTR_IP_VERSION_TYPE]);
		mask |= SP_RULE_FLAG_MATCH_SAWF_IP_VERSION_TYPE;
		DEBUG_INFO("IP Version type: 0x%x\n", to_sawf_sp.inner.ip_version_type);
	}

	if (info->attrs[SP_GNL_ATTR_SRC_IPV4_ADDR]) {
		to_sawf_sp.inner.src_ipv4_addr = nla_get_in_addr(info->attrs[SP_GNL_ATTR_SRC_IPV4_ADDR]);
		mask |= SP_RULE_FLAG_MATCH_SAWF_SRC_IPV4;
		DEBUG_INFO("src_ipv4 = %pI4 \n", &to_sawf_sp.inner.src_ipv4_addr);
	}

	if (info->attrs[SP_GNL_ATTR_SRC_IPV4_ADDR_MASK]) {
		to_sawf_sp.inner.src_ipv4_addr_mask = nla_get_in_addr(info->attrs[SP_GNL_ATTR_SRC_IPV4_ADDR_MASK]);
		DEBUG_INFO("src_ipv4_mask = %pI4 \n", &to_sawf_sp.inner.src_ipv4_addr_mask);
		to_sawf_sp.inner.src_ipv4_addr &= to_sawf_sp.inner.src_ipv4_addr_mask;
		mask |= SP_RULE_FLAG_MATCH_SAWF_SRC_IPV4_MASK;
	}

	if (info->attrs[SP_GNL_ATTR_DST_IPV4_ADDR]) {
		to_sawf_sp.inner.dst_ipv4_addr = nla_get_in_addr(info->attrs[SP_GNL_ATTR_DST_IPV4_ADDR]);
		mask |= SP_RULE_FLAG_MATCH_SAWF_DST_IPV4;
		DEBUG_INFO("dst_ipv4 = %pI4 \n", &to_sawf_sp.inner.dst_ipv4_addr);
	}

	if (info->attrs[SP_GNL_ATTR_DST_IPV4_ADDR_MASK]) {
		to_sawf_sp.inner.dst_ipv4_addr_mask = nla_get_in_addr(info->attrs[SP_GNL_ATTR_DST_IPV4_ADDR_MASK]);
		DEBUG_INFO("dst_ipv4_mask = %pI4 \n", &to_sawf_sp.inner.dst_ipv4_addr_mask);
		to_sawf_sp.inner.dst_ipv4_addr &= to_sawf_sp.inner.dst_ipv4_addr_mask;
		mask |= SP_RULE_FLAG_MATCH_SAWF_DST_IPV4_MASK;
	}

	if (info->attrs[SP_GNL_ATTR_SRC_IPV6_ADDR]) {
		struct in6_addr saddr;
		saddr = nla_get_in6_addr(info->attrs[SP_GNL_ATTR_SRC_IPV6_ADDR]);
		memcpy(to_sawf_sp.inner.src_ipv6_addr, saddr.s6_addr32, sizeof(struct in6_addr));
		mask |= SP_RULE_FLAG_MATCH_SAWF_SRC_IPV6;
		DEBUG_INFO("src_ipv6 = %pI6 \n", &to_sawf_sp.inner.src_ipv6_addr);
	}

	if (info->attrs[SP_GNL_ATTR_SRC_IPV6_ADDR_MASK]) {
		struct in6_addr saddr_mask;
		int i;

		saddr_mask = nla_get_in6_addr(info->attrs[SP_GNL_ATTR_SRC_IPV6_ADDR_MASK]);
		memcpy(to_sawf_sp.inner.src_ipv6_addr_mask, saddr_mask.s6_addr32, sizeof(struct in6_addr));
		DEBUG_INFO("src_ipv6_mask = %pI6 \n", &to_sawf_sp.inner.src_ipv6_addr_mask);

		for (i = 0; i < IPV6_ADDR_LEN; i++) {
			to_sawf_sp.inner.src_ipv6_addr[i] &= to_sawf_sp.inner.src_ipv6_addr_mask[i];
		}
		mask |= SP_RULE_FLAG_MATCH_SAWF_SRC_IPV6_MASK;
	}

	if (info->attrs[SP_GNL_ATTR_DST_IPV6_ADDR]) {
		struct in6_addr daddr;
		daddr = nla_get_in6_addr(info->attrs[SP_GNL_ATTR_DST_IPV6_ADDR]);
		memcpy(to_sawf_sp.inner.dst_ipv6_addr, daddr.s6_addr32, sizeof(struct in6_addr));
		mask |= SP_RULE_FLAG_MATCH_SAWF_DST_IPV6;
		DEBUG_INFO("dst_ipv6 = %pI6\n", &to_sawf_sp.inner.dst_ipv6_addr);
	}

	if (info->attrs[SP_GNL_ATTR_DST_IPV6_ADDR_MASK]) {
		struct in6_addr daddr_mask;
		int i;

		daddr_mask = nla_get_in6_addr(info->attrs[SP_GNL_ATTR_DST_IPV6_ADDR_MASK]);
		memcpy(to_sawf_sp.inner.dst_ipv6_addr_mask, daddr_mask.s6_addr32, sizeof(struct in6_addr));
		DEBUG_INFO("dst_ipv6_mask = %pI6 \n", &to_sawf_sp.inner.dst_ipv6_addr_mask);

		for (i = 0; i < IPV6_ADDR_LEN; i++) {
			to_sawf_sp.inner.dst_ipv6_addr[i] &= to_sawf_sp.inner.dst_ipv6_addr_mask[i];
		}

		mask |= SP_RULE_FLAG_MATCH_SAWF_DST_IPV6_MASK;
	}

	if (info->attrs[SP_GNL_ATTR_PROTOCOL_NUMBER]) {
		to_sawf_sp.inner.protocol_number = nla_get_u8(info->attrs[SP_GNL_ATTR_PROTOCOL_NUMBER]);
		mask |= SP_RULE_FLAG_MATCH_SAWF_PROTOCOL;
		DEBUG_INFO("protocol_number: 0x%x\n", to_sawf_sp.inner.protocol_number);
	}

	if (info->attrs[SP_GNL_ATTR_VLAN_ID]) {
		to_sawf_sp.inner.vlan_id = nla_get_u16(info->attrs[SP_GNL_ATTR_VLAN_ID]);
		mask |= SP_RULE_FLAG_MATCH_SAWF_VLAN_ID;
		DEBUG_INFO("vlan_id: 0x%x\n", to_sawf_sp.inner.vlan_id);
	}

	if (info->attrs[SP_GNL_ATTR_DSCP]) {
		to_sawf_sp.inner.dscp = nla_get_u8(info->attrs[SP_GNL_ATTR_DSCP]);
		mask |= SP_RULE_FLAG_MATCH_SAWF_DSCP;
		DEBUG_INFO("dscp: 0x%x\n", to_sawf_sp.inner.dscp);
	}

	if (info->attrs[SP_GNL_ATTR_DSCP_REMARK]) {
		to_sawf_sp.inner.dscp_remark = nla_get_u8(info->attrs[SP_GNL_ATTR_DSCP_REMARK]);
		mask |= SP_RULE_FLAG_MATCH_SAWF_DSCP_REMARK;
		DEBUG_INFO("dscp remark: 0x%x\n", to_sawf_sp.inner.dscp_remark);
	}

	if (info->attrs[SP_GNL_ATTR_VLAN_PCP]) {
		to_sawf_sp.inner.vlan_pcp = nla_get_u8(info->attrs[SP_GNL_ATTR_VLAN_PCP]);
		mask |= SP_RULE_FLAG_MATCH_SAWF_VLAN_PCP;
		DEBUG_INFO("vlan_pcp: 0x%x\n", to_sawf_sp.inner.vlan_pcp);
	}

	if (info->attrs[SP_GNL_ATTR_VLAN_PCP_REMARK]) {
		to_sawf_sp.inner.vlan_pcp_remark = nla_get_u8(info->attrs[SP_GNL_ATTR_VLAN_PCP_REMARK]);
		mask |= SP_RULE_FLAG_MATCH_SAWF_VLAN_PCP_REMARK;
		DEBUG_INFO("vlan_pcp_remark: 0x%x\n", to_sawf_sp.inner.vlan_pcp_remark);
	}

	if (info->attrs[SP_GNL_ATTR_MATCH_PATTERN_VALUE]) {
		mask |= SP_RULE_FLAG_MATCH_SCS_SPI;
		to_sawf_sp.inner.match_pattern_value = nla_get_u32(info->attrs[SP_GNL_ATTR_MATCH_PATTERN_VALUE]);
	}

	if (info->attrs[SP_GNL_ATTR_MATCH_PATTERN_MASK]) {
		mask |= SP_RULE_FLAG_MATCH_SCS_SPI;
		to_sawf_sp.inner.match_pattern_mask = nla_get_u32(info->attrs[SP_GNL_ATTR_MATCH_PATTERN_MASK]);
	}

	if (info->attrs[SP_GNL_ATTR_IFINDEX]) {
		to_sawf_sp.inner.ifindex = nla_get_u8(info->attrs[SP_GNL_ATTR_IFINDEX]);
		DEBUG_INFO("Interface Index: 0x%x\n", to_sawf_sp.inner.ifindex);
	}

	if (info->attrs[SP_GNL_ATTR_TID_BITMAP]) {
		to_sawf_sp.inner.mscs_tid_bitmap = nla_get_u8(info->attrs[SP_GNL_ATTR_TID_BITMAP]);
		DEBUG_INFO("MSCS priority bitmap: 0x%x\n", to_sawf_sp.inner.mscs_tid_bitmap);
	}

	if (info->attrs[SP_GNL_ATTR_PRIORITY_LIMIT]) {
		to_sawf_sp.inner.priority_limit = nla_get_u8(info->attrs[SP_GNL_ATTR_PRIORITY_LIMIT]);
		DEBUG_INFO("Priority limit: 0x%x\n", to_sawf_sp.inner.priority_limit);
	}

	if ((info->attrs[SP_GNL_ATTR_SRC_PORT_RANGE_START] && !(info->attrs[SP_GNL_ATTR_SRC_PORT_RANGE_END])) ||
			(info->attrs[SP_GNL_ATTR_SRC_PORT_RANGE_END] && !(info->attrs[SP_GNL_ATTR_SRC_PORT_RANGE_START]))) {
		rcu_read_unlock();
		DEBUG_ERROR("Invalid input, please enter both start and end value for source port range\n");
		rule_result = SP_MAPDB_UPDATE_RESULT_ERR_INVALIDENTRY;
		goto status_notify;
	}

	if (info->attrs[SP_GNL_ATTR_SRC_PORT_RANGE_START] && info->attrs[SP_GNL_ATTR_SRC_PORT_RANGE_END]) {
		to_sawf_sp.inner.src_port_range_start = nla_get_u16(info->attrs[SP_GNL_ATTR_SRC_PORT_RANGE_START]);
		mask |= SP_RULE_FLAG_MATCH_SAWF_SRC_PORT_RANGE_START;
		DEBUG_INFO("Source port range start: 0x%x\n", to_sawf_sp.inner.src_port_range_start);

		to_sawf_sp.inner.src_port_range_end = nla_get_u16(info->attrs[SP_GNL_ATTR_SRC_PORT_RANGE_END]);
		mask |= SP_RULE_FLAG_MATCH_SAWF_SRC_PORT_RANGE_END;
		DEBUG_INFO("Source port range end: 0x%x\n", to_sawf_sp.inner.src_port_range_end);
	}

	if ((info->attrs[SP_GNL_ATTR_DST_PORT_RANGE_START] && !(info->attrs[SP_GNL_ATTR_DST_PORT_RANGE_END])) ||
			(info->attrs[SP_GNL_ATTR_DST_PORT_RANGE_END] && !(info->attrs[SP_GNL_ATTR_DST_PORT_RANGE_START]))) {
		rcu_read_unlock();
		DEBUG_ERROR("Invalid input, please enter both start and end value for destination port range\n");
		rule_result = SP_MAPDB_UPDATE_RESULT_ERR_INVALIDENTRY;
		goto status_notify;
	}

	if (info->attrs[SP_GNL_ATTR_DST_PORT_RANGE_START] && info->attrs[SP_GNL_ATTR_DST_PORT_RANGE_END]) {
		to_sawf_sp.inner.dst_port_range_start = nla_get_u16(info->attrs[SP_GNL_ATTR_DST_PORT_RANGE_START]);
		mask |= SP_RULE_FLAG_MATCH_SAWF_DST_PORT_RANGE_START;
		DEBUG_INFO("Destination port range start: 0x%x\n", to_sawf_sp.inner.dst_port_range_start);

		to_sawf_sp.inner.dst_port_range_end = nla_get_u16(info->attrs[SP_GNL_ATTR_DST_PORT_RANGE_END]);
		mask |= SP_RULE_FLAG_MATCH_SAWF_DST_PORT_RANGE_END;
		DEBUG_INFO("Destination port range end: 0x%x\n", to_sawf_sp.inner.dst_port_range_end);
	}


	/*
	 * Default classifier is SAWF, but if SCS rule is received, then classifier type will be
	 * overwritten by SCS.
	 */
	to_sawf_sp.classifier_type = SP_RULE_TYPE_SAWF;
	if (info->attrs[SP_GNL_ATTR_CLASSIFIER_TYPE]) {
		to_sawf_sp.classifier_type = nla_get_u8(info->attrs[SP_GNL_ATTR_CLASSIFIER_TYPE]);
	}

	rcu_read_unlock();

	/*
	 * Update flag mask for valid rules
	 */
	to_sawf_sp.inner.flags_sawf = mask;

	/*
	 * Update rules in database
	 */
	rule_result = sp_mapdb_rule_update(&to_sawf_sp);

status_notify:
	err = sp_mapdb_rule_receive_status_notify(&msg, &hdr, info, to_sawf_sp.id, rule_result);
	if (err)
		return err;

	genlmsg_end(msg, hdr);
	return genlmsg_unicast(genl_info_net(info), msg, info->snd_portid);
}

/*
 * sp_mapdb_rule_query()
 * 	Handles a netlink message from userspace for rule query
 */
static inline int sp_mapdb_rule_query(struct sk_buff *skb, struct genl_info *info)
{
	uint32_t rule_id;
	struct sp_rule rule;
	struct sp_mapdb_rule_id_hashentry *cur_hashentry = NULL;
	void *hdr;
	struct sk_buff *msg = NULL;
	struct in6_addr saddr;
	struct in6_addr daddr;
	struct in6_addr saddr_mask;
	struct in6_addr daddr_mask;

	msg = nlmsg_new(NLMSG_DEFAULT_SIZE, GFP_KERNEL);
	if (!msg) {
		DEBUG_WARN("Failed to allocate netlink message to accomodate rule\n");
		return -ENOMEM;
	}

	hdr = genlmsg_put(msg, info->snd_portid, info->snd_seq,
			  &sp_genl_family, 0, SPM_CMD_RULE_QUERY);
	if (!hdr) {
		DEBUG_WARN("Failed to put hdr in netlink buffer\n");
		nlmsg_free(msg);
		return -ENOMEM;
	}

	rcu_read_lock();
	rule_id = nla_get_u32(info->attrs[SP_GNL_ATTR_ID]);
	DEBUG_INFO("User requested rule with rule_id: 0x%x \n", rule_id);
	rcu_read_unlock();

	spin_lock(&sp_mapdb_lock);
	if (!rule_manager.rule_count) {
		spin_unlock(&sp_mapdb_lock);
		DEBUG_WARN("Requested rule table is empty\n");
		goto put_failure;
	}

	cur_hashentry = sp_mapdb_search_hashentry(rule_id, SP_RULE_TYPE_SAWF);
	if (!cur_hashentry) {
		spin_unlock(&sp_mapdb_lock);
		DEBUG_WARN("Invalid rule with ruleID = %d, rule_type: %d\n", rule_id, SP_RULE_TYPE_SAWF);
		goto put_failure;
	}

	rule = cur_hashentry->rule_node->rule;
	spin_unlock(&sp_mapdb_lock);

	if (nla_put_u32(msg, SP_GNL_ATTR_ID, rule.id) ||
	    nla_put_u8(msg, SP_GNL_ATTR_RULE_PRECEDENCE, rule.rule_precedence) ||
	    nla_put_u8(msg, SP_GNL_ATTR_RULE_OUTPUT, rule.inner.rule_output) ||
	    nla_put_u8(msg, SP_GNL_ATTR_CLASSIFIER_TYPE, rule.classifier_type) ||
	    nla_put(msg, SP_GNL_ATTR_SRC_MAC, ETH_ALEN, rule.inner.sa) ||
	    nla_put(msg, SP_GNL_ATTR_DST_MAC, ETH_ALEN, rule.inner.da)) {
		goto put_failure;
	}

	if (nla_put_in_addr(msg, SP_GNL_ATTR_SRC_IPV4_ADDR, rule.inner.src_ipv4_addr) ||
	    nla_put_in_addr(msg, SP_GNL_ATTR_DST_IPV4_ADDR, rule.inner.dst_ipv4_addr)) {
		goto put_failure;
	}

	memcpy(&saddr, rule.inner.src_ipv6_addr, sizeof(struct in6_addr));
	memcpy(&daddr, rule.inner.dst_ipv6_addr, sizeof(struct in6_addr));

	if (nla_put_in6_addr(msg, SP_GNL_ATTR_DST_IPV6_ADDR, &daddr) ||
	    nla_put_in6_addr(msg, SP_GNL_ATTR_SRC_IPV6_ADDR, &saddr)) {
		goto put_failure;
	}
	if (nla_put_in_addr(msg, SP_GNL_ATTR_SRC_IPV4_ADDR_MASK, rule.inner.src_ipv4_addr_mask) ||
	    nla_put_in_addr(msg, SP_GNL_ATTR_DST_IPV4_ADDR_MASK, rule.inner.dst_ipv4_addr_mask)) {
		goto put_failure;
	}

	memcpy(&saddr_mask, rule.inner.src_ipv6_addr_mask, sizeof(struct in6_addr));
	memcpy(&daddr_mask, rule.inner.dst_ipv6_addr_mask, sizeof(struct in6_addr));

	if (nla_put_in6_addr(msg, SP_GNL_ATTR_DST_IPV6_ADDR_MASK, &daddr_mask) ||
	    nla_put_in6_addr(msg, SP_GNL_ATTR_SRC_IPV6_ADDR_MASK, &saddr_mask)) {
		goto put_failure;
	}

	if (nla_put_u16(msg, SP_GNL_ATTR_SRC_PORT, rule.inner.src_port) ||
	    nla_put_u16(msg, SP_GNL_ATTR_DST_PORT, rule.inner.dst_port) ||
	    nla_put_u8(msg, SP_GNL_ATTR_PROTOCOL_NUMBER, rule.inner.protocol_number) ||
	    nla_put_u16(msg, SP_GNL_ATTR_VLAN_ID, rule.inner.vlan_id) ||
	    nla_put_u8(msg, SP_GNL_ATTR_DSCP, rule.inner.dscp) ||
	    nla_put_u8(msg, SP_GNL_ATTR_DSCP_REMARK, rule.inner.dscp_remark) ||
	    nla_put_u8(msg, SP_GNL_ATTR_VLAN_PCP, rule.inner.vlan_pcp) ||
	    nla_put_u8(msg, SP_GNL_ATTR_VLAN_PCP_REMARK, rule.inner.vlan_pcp_remark) ||
	    nla_put_u8(msg, SP_GNL_ATTR_SERVICE_CLASS_ID, rule.inner.service_class_id) ||
	    nla_put_u8(msg, SP_GNL_ATTR_IP_VERSION_TYPE, rule.inner.ip_version_type) ||
	    nla_put_u32(msg, SP_GNL_ATTR_MATCH_PATTERN_VALUE, rule.inner.match_pattern_value) ||
	    nla_put_u32(msg, SP_GNL_ATTR_MATCH_PATTERN_MASK, rule.inner.match_pattern_mask) ||
	    nla_put_u8(msg, SP_RULE_FLAG_MATCH_MSCS_TID_BITMAP, rule.inner.mscs_tid_bitmap) ||
	    nla_put_u8(msg,  SP_RULE_FLAG_MATCH_PRIORITY_LIMIT, rule.inner.priority_limit) ||
	    nla_put_u8(msg,  SP_RULE_FLAG_MATCH_IFINDEX, rule.inner.ifindex) ||
	    nla_put_u16(msg, SP_GNL_ATTR_SRC_PORT_RANGE_START, rule.inner.src_port_range_start) ||
	    nla_put_u16(msg, SP_GNL_ATTR_SRC_PORT_RANGE_END, rule.inner.src_port_range_end) ||
	    nla_put_u16(msg, SP_GNL_ATTR_DST_PORT_RANGE_START, rule.inner.dst_port_range_start) ||
	    nla_put_u16(msg, SP_GNL_ATTR_DST_PORT_RANGE_END, rule.inner.dst_port_range_end)) {
		goto put_failure;
	}

	genlmsg_end(msg, hdr);
	return genlmsg_reply(msg, info);

put_failure:
	genlmsg_cancel(msg, hdr);
	nlmsg_free(msg);
	return -EMSGSIZE;
}


/*
 * sp_genl_policy
 * 	Policy attributes
 */
static struct nla_policy sp_genl_policy[SP_GNL_MAX + 1] = {
	[SP_GNL_ATTR_ID]		= { .type = NLA_U32, },
	[SP_GNL_ATTR_ADD_DELETE_RULE]		= { .type = NLA_U8, },
	[SP_GNL_ATTR_RULE_PRECEDENCE]		= { .type = NLA_U8, },
	[SP_GNL_ATTR_RULE_OUTPUT]		= { .type = NLA_U8, },
	[SP_GNL_ATTR_USER_PRIORITY]		= { .type = NLA_U8, },
	[SP_GNL_ATTR_SRC_MAC]		= { .len = ETH_ALEN, },
	[SP_GNL_ATTR_DST_MAC]		= { .len = ETH_ALEN, },
	[SP_GNL_ATTR_SRC_IPV4_ADDR]		= { .type = NLA_U32, },
	[SP_GNL_ATTR_SRC_IPV4_ADDR_MASK]	= { .type = NLA_U32, },
	[SP_GNL_ATTR_DST_IPV4_ADDR]		= { .type = NLA_U32, },
	[SP_GNL_ATTR_DST_IPV4_ADDR_MASK]	= { .type = NLA_U32, },
	[SP_GNL_ATTR_SRC_IPV6_ADDR]		= { .len = sizeof(struct in6_addr) },
	[SP_GNL_ATTR_SRC_IPV6_ADDR_MASK]	= { .len = sizeof(struct in6_addr) },
	[SP_GNL_ATTR_DST_IPV6_ADDR]		= { .len = sizeof(struct in6_addr) },
	[SP_GNL_ATTR_DST_IPV6_ADDR_MASK]	= { .len = sizeof(struct in6_addr) },
	[SP_GNL_ATTR_SRC_PORT]		= { .type = NLA_U16, },
	[SP_GNL_ATTR_DST_PORT]		= { .type = NLA_U16, },
	[SP_GNL_ATTR_PROTOCOL_NUMBER]		= { .type = NLA_U8, },
	[SP_GNL_ATTR_VLAN_ID]		= { .type = NLA_U16, },
	[SP_GNL_ATTR_DSCP]		= { .type = NLA_U8, },
	[SP_GNL_ATTR_DSCP_REMARK]		= { .type = NLA_U8, },
	[SP_GNL_ATTR_VLAN_PCP]		= { .type = NLA_U8, },
	[SP_GNL_ATTR_VLAN_PCP_REMARK]		= { .type = NLA_U8, },
	[SP_GNL_ATTR_SERVICE_CLASS_ID]		= { .type = NLA_U8, },
	[SP_GNL_ATTR_IP_VERSION_TYPE]		= { .type = NLA_U8, },
	[SP_GNL_ATTR_CLASSIFIER_TYPE]		= { .type = NLA_U8, },
	[SP_GNL_ATTR_MATCH_PATTERN_VALUE]		= { .type = NLA_U32, },
	[SP_GNL_ATTR_MATCH_PATTERN_MASK]		= { .type = NLA_U32, },
	[SP_GNL_ATTR_TID_BITMAP]		= { .type = NLA_U8, },
	[SP_GNL_ATTR_PRIORITY_LIMIT]		= { .type = NLA_U8, },
	[SP_GNL_ATTR_IFINDEX]			= { .type = NLA_U8, },
	[SP_GNL_ATTR_SRC_PORT_RANGE_START]	= { .type = NLA_U16, },
	[SP_GNL_ATTR_SRC_PORT_RANGE_END]	= { .type = NLA_U16, },
	[SP_GNL_ATTR_DST_PORT_RANGE_START]	= { .type = NLA_U16, },
	[SP_GNL_ATTR_DST_PORT_RANGE_END]	= { .type = NLA_U16, },
};

/* Spm generic netlink operations */
static const struct genl_ops sp_genl_ops[] = {
	{
		.cmd = SPM_CMD_RULE_ACTION,
		.doit = sp_mapdb_rule_receive,
		.validate = GENL_DONT_VALIDATE_STRICT | GENL_DONT_VALIDATE_DUMP,
		.flags = GENL_ADMIN_PERM,
	},
	{
		.cmd = SPM_CMD_RULE_QUERY,
		.doit = sp_mapdb_rule_query,
		.validate = GENL_DONT_VALIDATE_STRICT | GENL_DONT_VALIDATE_DUMP,
		.flags = GENL_ADMIN_PERM,
	},
};

/* Spm generic family */
static struct genl_family sp_genl_family = {
	.name           = "spm",
	.version        = 0,
	.hdrsize        = 0,
	.maxattr        = SP_GNL_MAX,
	.policy 	= sp_genl_policy,
	.netnsok        = true,
	.module         = THIS_MODULE,
	.ops            = sp_genl_ops,
	.n_ops          = ARRAY_SIZE(sp_genl_ops),
};

/*
 * sp_mapdb_fini()
 * 	This is the function called when SPM is unloaded.
 */
void sp_mapdb_fini(void)
{
	sp_mapdb_ruletable_flush();
}

/*
 * sp_netlink_init()
 * 	Initialize generic netlink
 */
bool sp_netlink_init(void)
{
	int err;
	err = genl_register_family(&sp_genl_family);
	if (err) {
		DEBUG_ERROR("Failed to register sp generic netlink family with error: %d\n", err);
		return false;
	}
	return true;
}

/*
 * sp_netlink_exit()
 * 	Netlink exit
 */
bool sp_netlink_exit(void)
{
	int err;
	err = genl_unregister_family(&sp_genl_family);
	if (err) {
		DEBUG_ERROR("Failed to unregister sp generic netlink family with error: %d\n", err);
		return false;
	}
	return true;
}
