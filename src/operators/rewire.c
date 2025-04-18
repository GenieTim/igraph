/* -*- mode: C -*-  */
/* vim:set ts=4 sw=4 sts=4 et: */
/*
   IGraph library.
   Copyright (C) 2005-2021 The igraph development team

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
   02110-1301 USA

*/

#include "igraph_operators.h"

#include "igraph_adjlist.h"
#include "igraph_conversion.h"
#include "igraph_interface.h"
#include "igraph_iterators.h"
#include "igraph_progress.h"
#include "igraph_random.h"
#include "igraph_structural.h"

#include "core/interruption.h"
#include "operators/rewire_internal.h"

/* Threshold that defines when to switch over to using adjacency lists during
 * rewiring */
#define REWIRE_ADJLIST_THRESHOLD 10

/* Not declared static so that the testsuite can use it, but not part of the public API. */
igraph_error_t igraph_i_rewire(igraph_t *graph, igraph_integer_t n, igraph_rewiring_t mode, igraph_bool_t use_adjlist) {
    const igraph_integer_t no_of_edges = igraph_ecount(graph);
    char message[256];
    igraph_integer_t a, b, c, d, dummy, num_swaps, num_successful_swaps;
    igraph_vector_int_t eids;
    igraph_vector_int_t edgevec, alledges;
    const igraph_bool_t directed = igraph_is_directed(graph);
    const igraph_bool_t loops = (mode & IGRAPH_REWIRING_SIMPLE_LOOPS);
    igraph_bool_t ok;
    igraph_es_t es;
    igraph_adjlist_t al;

    if (no_of_edges < 2) {
        /* There are no possible rewirings, return with the same graph. */
        return IGRAPH_SUCCESS;
    }

    RNG_BEGIN();

    IGRAPH_VECTOR_INT_INIT_FINALLY(&eids, 2);

    if (use_adjlist) {
        /* As well as the sorted adjacency list, we maintain an unordered
         * list of edges for picking a random edge in constant time.
         */
        IGRAPH_CHECK(igraph_adjlist_init(graph, &al, IGRAPH_OUT, IGRAPH_LOOPS_ONCE, IGRAPH_MULTIPLE));
        IGRAPH_FINALLY(igraph_adjlist_destroy, &al);
        IGRAPH_VECTOR_INT_INIT_FINALLY(&alledges, no_of_edges * 2);
        igraph_get_edgelist(graph, &alledges, /*bycol=*/ false);
    } else {
        IGRAPH_VECTOR_INT_INIT_FINALLY(&edgevec, 4);
        es = igraph_ess_vector(&eids);
    }

    /* We count both successful and unsuccessful rewiring trials.
     * This is necessary for uniform sampling. */

    num_swaps = num_successful_swaps = 0;
    while (num_swaps < n) {

        IGRAPH_ALLOW_INTERRUPTION();
        if (num_swaps % 1000 == 0) {
            snprintf(message, sizeof(message),
                     "Random rewiring (%.2f%% of the trials were successful)",
                     num_swaps > 0 ? ((100.0 * num_successful_swaps) / num_swaps) : 0.0);
            IGRAPH_PROGRESS(message, (100.0 * num_swaps) / n, 0);
        }

        switch (mode) {
        case IGRAPH_REWIRING_SIMPLE:
        case IGRAPH_REWIRING_SIMPLE_LOOPS:
            ok = true;

            /* Choose two edges randomly */
            VECTOR(eids)[0] = RNG_INTEGER(0, no_of_edges - 1);
            do {
                VECTOR(eids)[1] = RNG_INTEGER(0, no_of_edges - 1);
            } while (VECTOR(eids)[0] == VECTOR(eids)[1]);

            /* Get the endpoints */
            if (use_adjlist) {
                a = VECTOR(alledges)[VECTOR(eids)[0] * 2];
                b = VECTOR(alledges)[VECTOR(eids)[0] * 2 + 1];
                c = VECTOR(alledges)[VECTOR(eids)[1] * 2];
                d = VECTOR(alledges)[VECTOR(eids)[1] * 2 + 1];
            } else {
                IGRAPH_CHECK(igraph_edge(graph, VECTOR(eids)[0], &a, &b));
                IGRAPH_CHECK(igraph_edge(graph, VECTOR(eids)[1], &c, &d));
            }

            /* For an undirected graph, we have two "variants" of each edge, i.e.
             * a -- b and b -- a. Since some rewirings can be performed only when we
             * "swap" the endpoints, we do it now with probability 0.5 */
            if (!directed && RNG_BOOL()) {
                dummy = c; c = d; d = dummy;
                if (use_adjlist) {
                    /* Flip the edge in the unordered edge-list, so the update later on
                     * hits the correct end. */
                    VECTOR(alledges)[VECTOR(eids)[1] * 2] = c;
                    VECTOR(alledges)[VECTOR(eids)[1] * 2 + 1] = d;
                }
            }

            /* If we do not touch loops, check whether a == b or c == d and disallow
             * the swap if needed */
            if (!loops && (a == b || c == d)) {
                ok = false;
            } else {
                /* Check whether they are suitable for rewiring */
                if (a == c || b == d) {
                    /* Swapping would have no effect */
                    ok = false;
                } else {
                    /* a != c && b != d */
                    /* If a == d or b == c, the swap would generate at least one loop, so
                     * we disallow them unless we want to have loops */
                    ok = loops || (a != d && b != c);
                    /* Also, if a == b and c == d and we allow loops, doing the swap
                     * would result in a multiple edge if the graph is undirected */
                    ok = ok && (directed || a != b || c != d);
                }
            }

            /* All good so far. Now check for the existence of a --> d and c --> b to
             * disallow the creation of multiple edges */
            if (ok) {
                if (use_adjlist) {
                    if (igraph_adjlist_has_edge(&al, a, d, directed)) {
                        ok = false;
                    }
                } else {
                    IGRAPH_CHECK(igraph_are_adjacent(graph, a, d, &ok));
                    ok = !ok;
                }
            }
            if (ok) {
                if (use_adjlist) {
                    if (igraph_adjlist_has_edge(&al, c, b, directed)) {
                        ok = false;
                    }
                } else {
                    IGRAPH_CHECK(igraph_are_adjacent(graph, c, b, &ok));
                    ok = !ok;
                }
            }

            /* If we are still okay, we can perform the rewiring */
            if (ok) {
                /* printf("Deleting: %" IGRAPH_PRId " -> %" IGRAPH_PRId ", %" IGRAPH_PRId " -> %" IGRAPH_PRId "\n",
                              a, b, c, d); */
                if (use_adjlist) {
                    /* Replace entry in sorted adjlist: */
                    IGRAPH_CHECK(igraph_adjlist_replace_edge(&al, a, b, d, directed));
                    IGRAPH_CHECK(igraph_adjlist_replace_edge(&al, c, d, b, directed));
                    /* Also replace in unsorted edgelist: */
                    VECTOR(alledges)[VECTOR(eids)[0] * 2 + 1] = d;
                    VECTOR(alledges)[VECTOR(eids)[1] * 2 + 1] = b;
                } else {
                    IGRAPH_CHECK(igraph_delete_edges(graph, es));
                    VECTOR(edgevec)[0] = a; VECTOR(edgevec)[1] = d;
                    VECTOR(edgevec)[2] = c; VECTOR(edgevec)[3] = b;
                    /* printf("Adding: %" IGRAPH_PRId " -> %" IGRAPH_PRId ", %" IGRAPH_PRId " -> %" IGRAPH_PRId "\n",
                                a, d, c, b); */
                    IGRAPH_CHECK(igraph_add_edges(graph, &edgevec, 0));
                }
                num_successful_swaps++;
            }
            break;
        default:
            RNG_END();
            IGRAPH_ERROR("Invalid rewiring mode.", IGRAPH_EINVAL);
        }
        num_swaps++;
    }

    if (use_adjlist) {
        /* Replace graph edges with the adjlist current state */
        IGRAPH_CHECK(igraph_delete_edges(graph, igraph_ess_all(IGRAPH_EDGEORDER_ID)));
        IGRAPH_CHECK(igraph_add_edges(graph, &alledges, 0));
    }

    IGRAPH_PROGRESS("Random rewiring: ", 100.0, 0);

    if (use_adjlist) {
        igraph_vector_int_destroy(&alledges);
        igraph_adjlist_destroy(&al);
    } else {
        igraph_vector_int_destroy(&edgevec);
    }

    igraph_vector_int_destroy(&eids);
    IGRAPH_FINALLY_CLEAN(use_adjlist ? 3 : 2);

    RNG_END();

    return IGRAPH_SUCCESS;
}

/**
 * \ingroup structural
 * \function igraph_rewire
 * \brief Randomly rewires a graph while preserving its degree sequence.
 *
 * This function generates a new graph based on the original one by randomly
 * "rewriting" edges while preserving the original graph's degree sequence.
 * The rewiring is done "in place", so no new graph will be allocated. If you
 * would like to keep the original graph intact, use \ref igraph_copy()
 * beforehand. All graph attributes will be lost.
 *
 * </para><para>
 * The rewiring is performed with degree-preserving edge switches:
 * Two arbitrary edges are picked uniformly at random, namely
 * <code>(a, b)</code> and <code>(c, d)</code>, then they are replaced
 * by <code>(a, d)</code> and <code>(b, c)</code> if this preserves the
 * constraints specified by \p mode.
 *
 * \param graph The graph object to be rewired.
 * \param n Number of rewiring trials to perform.
 * \param mode The rewiring algorithm to be used. It can be one of the following flags:
 *         \clist
 *           \cli IGRAPH_REWIRING_SIMPLE
 *                This method does not create or destroy self-loops, and does
 *                not create multi-edges.
 *           \cli IGRAPH_REWIRING_SIMPLE_LOOPS
 *                This method allows the creation or destruction of self-loops.
 *                Self-loops are created by switching edges that have a single
 *                common endpoint.
 *         \endclist
 *
 * \return Error code:
 *         \clist
 *           \cli IGRAPH_EINVMODE
 *                Invalid rewiring mode.
 *           \cli IGRAPH_ENOMEM
 *                Not enough memory for temporary data.
 *         \endclist
 *
 * Time complexity: TODO.
 */
igraph_error_t igraph_rewire(igraph_t *graph, igraph_integer_t n, igraph_rewiring_t mode) {
    igraph_bool_t use_adjlist = n >= REWIRE_ADJLIST_THRESHOLD;
    return igraph_i_rewire(graph, n, mode, use_adjlist);
}
