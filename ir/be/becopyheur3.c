
/**
 * More experiments on coalescing.
 * @author Sebastian Hack
 * @date   25.07.2006
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#ifdef WITH_LIBCORE
#include <libcore/lc_opts.h>
#include <libcore/lc_opts_enum.h>
#endif /* WITH_LIBCORE */

#include <stdlib.h>
#include <limits.h>

#include "list.h"
#include "pdeq.h"
#include "bitset.h"

#include "debug.h"
#include "bitfiddle.h"
#include "bitset.h"

#include "irphase_t.h"
#include "irgraph_t.h"
#include "irnode_t.h"
#include "irprintf.h"
#include "irtools.h"

#include "beabi.h"
#include "benode_t.h"
#include "becopyopt.h"
#include "becopyopt_t.h"
#include "bechordal_t.h"
#include "bejavacoal.h"

#include <stdlib.h>
#include <stdio.h>

#define DUMP_BEFORE 1
#define DUMP_AFTER  2
#define DUMP_ALL    2 * DUMP_AFTER - 1

static int dump_flags = 0;
static int dbg_level  = 0;

#ifdef WITH_LIBCORE
static const lc_opt_enum_mask_items_t dump_items[] = {
	{ "before",  DUMP_BEFORE },
	{ "after",   DUMP_AFTER  },
	{ "all",     DUMP_ALL    },
	{ NULL,      0 }
};

static lc_opt_enum_mask_var_t dump_var = {
	&dump_flags, dump_items
};

static const lc_opt_table_entry_t options[] = {
	LC_OPT_ENT_ENUM_MASK("dump", "dump ifg before, after or after each cloud",  &dump_var),
	LC_OPT_ENT_INT      ("dbg",  "debug level for the Java coalescer",          &dbg_level),
	{ NULL }
};

void be_co3_register_options(lc_opt_entry_t *grp)
{
	lc_opt_entry_t *co3_grp = lc_opt_get_grp(grp, "co3");
	lc_opt_add_table(co3_grp, options);
}
#endif


static void set_admissible_regs(java_coal_t *coal, copy_opt_t *co, ir_node *irn, int t_idx, int *col_map)
{
	unsigned i;
	arch_register_req_t req;
	unsigned n_regs = co->cls->n_regs;
	unsigned idx    = get_irn_idx(irn);

	arch_get_register_req(co->aenv, &req, irn, BE_OUT_POS(0));
	if(arch_register_req_is(&req, limited)) {
		bitset_t *adm = bitset_alloca(n_regs);
		req.limited(req.limited_env, adm);
		for(i = 0; i < n_regs; ++i)
			if(!bitset_is_set(adm, i) && col_map[i] >= 0)
				java_coal_forbid_color(coal, t_idx, col_map[i]);
	}
}

int co_solve_heuristic_java(copy_opt_t *co)
{
	be_ifg_t *ifg   = co->cenv->ifg;
	void *nodes_it  = be_ifg_nodes_iter_alloca(ifg);
	void *neigh_it  = be_ifg_neighbours_iter_alloca(ifg);
	bitset_t *nodes = bitset_malloc(get_irg_last_idx(co->irg));
	unsigned n_regs = co->cenv->cls->n_regs;

	unsigned i, j, curr_idx;
	int *col_map;
	int *inv_col_map;

	int *node_map;
	int *inv_node_map;

	java_coal_t *coal;
	ir_node *n, *m;
	int max_idx = 0;

	col_map     = alloca(n_regs * sizeof(col_map[0]));
	inv_col_map = alloca(n_regs * sizeof(inv_col_map[0]));

	memset(inv_col_map, 0, sizeof(inv_col_map[0]) * n_regs);

	for(i = 0, j = 0; i < n_regs; ++i) {
		const arch_register_t *reg = &co->cls->regs[i];
		col_map[i] = i;
		inv_col_map[i] = i;
		if(!arch_register_type_is(reg, ignore)) {
			//col_map[i] = j;
			//inv_col_map[j] = i;
			++j;
		}
	}

	node_map     = malloc((get_irg_last_idx(co->irg) + 1) * sizeof(node_map[0]));
	inv_node_map = malloc((get_irg_last_idx(co->irg) + 1) * sizeof(inv_node_map[0]));

	curr_idx = 0;
	be_ifg_foreach_node(ifg, nodes_it, n) {
		if(!arch_irn_is(co->aenv, n, ignore)) {
			int idx = get_irn_idx(n);
			bitset_set(nodes, idx);
			node_map[idx] = curr_idx;
			inv_node_map[curr_idx] = idx;
			curr_idx++;
		}
	}

	if(curr_idx == 0) {
		free(node_map);
		free(inv_node_map);
		bitset_free(nodes);
		return;
	}

	coal = java_coal_init("test", curr_idx, j, dbg_level);

	/* Check, if all neighbours are indeed connected to the node. */
	be_ifg_foreach_node(ifg, nodes_it, n) {
		int n_idx = get_irn_idx(n);
		int t_idx = node_map[n_idx];

		if(bitset_is_set(nodes, n_idx)) {
			affinity_node_t *an = get_affinity_info(co, n);

			java_coal_set_color(coal, t_idx, col_map[arch_get_irn_register(co->aenv, n)->index]);
			set_admissible_regs(coal, co, n, t_idx, col_map);
			be_ifg_foreach_neighbour(ifg, neigh_it, n, m) {
				int m_idx = get_irn_idx(m);
				int s_idx = node_map[m_idx];

				if(n_idx < m_idx && bitset_is_set(nodes, m_idx)) {
					java_coal_add_int_edge(coal, s_idx, t_idx);
				}
			}

			if(an != NULL) {
				neighb_t *neigh;
				co_gs_foreach_neighb(an, neigh) {
					int m_idx = get_irn_idx(neigh->irn);
					int s_idx = node_map[m_idx];

					if(n_idx < m_idx && bitset_is_set(nodes, m_idx)) {
						java_coal_add_aff_edge(coal, s_idx, t_idx, neigh->costs);
					}
				}
			}
		}
	}

	if(dump_flags & DUMP_BEFORE) {
		char fn[512];
		ir_snprintf(fn, sizeof(fn), "%F-%s-before.dot", co->cenv->irg, co->cenv->cls->name);
		java_coal_dump(coal, fn);
	}

	java_coal_coalesce(coal);

	be_ifg_foreach_node(ifg, nodes_it, n) {
		unsigned idx = get_irn_idx(n);
		if(bitset_is_set(nodes, idx)) {
			unsigned t_idx             = node_map[idx];
			unsigned col               = inv_col_map[java_coal_get_color(coal, t_idx)];
			const arch_register_t *reg = &co->cls->regs[col];
			arch_set_irn_register(co->aenv, n, reg);
		}
	}

	if(dump_flags & DUMP_AFTER) {
		char fn[512];
		ir_snprintf(fn, sizeof(fn), "%F-%s-after.dot", co->cenv->irg, co->cenv->cls->name);
		java_coal_dump(coal, fn);
	}

	java_coal_destroy(coal);
	bitset_free(nodes);
	return 0;
}
