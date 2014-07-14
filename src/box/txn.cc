/*
 * Redistribution and use in source and binary forms, with or
 * without modification, are permitted provided that the following
 * conditions are met:
 *
 * 1. Redistributions of source code must retain the above
 *    copyright notice, this list of conditions and the
 *    following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above
 *    copyright notice, this list of conditions and the following
 *    disclaimer in the documentation and/or other materials
 *    provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY <COPYRIGHT HOLDER> ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 * TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL
 * <COPYRIGHT HOLDER> OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT,
 * INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
 * BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 * LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF
 * THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */
#include "txn.h"
#include "tuple.h"
#include "space.h"
#include <tarantool.h>
#include "cluster.h"
#include "recovery.h"
#include <fiber.h>
#include "request.h" /* for request_name */
#include "session.h"
#include "port.h"

double too_long_threshold;

void
port_send_tuple(struct port *port, struct txn *txn)
{
	struct tuple *tuple;
	if ((tuple = txn->new_tuple) || (tuple = txn->old_tuple))
		port_add_tuple(port, tuple);
}


void
txn_add_redo(struct txn *txn, struct request *request)
{
	txn->row = request->header;
	if (recovery_state->wal_mode == WAL_NONE || request->header != NULL)
		return;

	/* Create a redo log row for Lua requests */
	struct iproto_header *row= (struct iproto_header *)
		region_alloc0(&fiber()->gc, sizeof(struct iproto_header));
	row->type = request->type;
	row->bodycnt = request_encode(request, row->body);
	txn->row = row;
}

void
txn_replace(struct txn *txn, struct space *space,
	    struct tuple *old_tuple, struct tuple *new_tuple,
	    enum dup_replace_mode mode)
{
	assert(old_tuple || new_tuple);
	/*
	 * Remember the old tuple only if we replaced it
	 * successfully, to not remove a tuple inserted by
	 * another transaction in rollback().
	 */
	txn->old_tuple = space_replace(space, old_tuple, new_tuple, mode);
	if (new_tuple) {
		txn->new_tuple = new_tuple;
		tuple_ref(txn->new_tuple, 1);
	}
	txn->space = space;
	/*
	 * Run on_replace triggers. For now, disallow mutation
	 * of tuples in the trigger.
	 */
	if (! rlist_empty(&space->on_replace) && space->run_triggers)
		trigger_run(&space->on_replace, txn);
}

struct txn *
txn_begin()
{
	assert(! in_txn());
	struct txn *txn = (struct txn *)
		region_alloc0(&fiber()->gc, sizeof(*txn));
	rlist_create(&txn->on_commit);
	rlist_create(&txn->on_rollback);
	in_txn() = txn;
	return txn;
}

/**
 * txn_finish() follows txn_commit() on success.
 * It's moved to a separate call to be able to send
 * old tuple to the user before it's deleted.
 */
void
txn_finish(struct txn *txn)
{
	assert(txn == in_txn());
	if (txn->old_tuple)
		tuple_ref(txn->old_tuple, -1);
	if (txn->space)
		txn->space->engine->factory->txnFinish(txn);
	TRASH(txn);
	/** Free volatile txn memory. */
	fiber_gc();
	in_txn() = NULL;
}


void
txn_commit(struct txn *txn, struct port *port)
{
	assert(txn == in_txn());
	if ((txn->old_tuple || txn->new_tuple) &&
	    !space_is_temporary(txn->space)) {
		int res = 0;
		/* txn_commit() must be done after txn_add_redo() */
		assert(recovery_state->wal_mode == WAL_NONE ||
		       txn->row != NULL);
		ev_tstamp start = ev_now(loop()), stop;
		res = wal_write(recovery_state, txn->row);
		stop = ev_now(loop());

		if (stop - start > too_long_threshold && txn->row != NULL) {
			say_warn("too long %s: %.3f sec",
				iproto_type_name(txn->row->type), stop - start);
		}

		if (res)
			tnt_raise(LoggedError, ER_WAL_IO);
	}
	trigger_run(&txn->on_commit, txn); /* must not throw. */
	port_send_tuple(port, txn);
	txn_finish(txn);
}

void
txn_rollback()
{
	struct txn *txn = in_txn();
	if (txn == NULL)
		return;
	if (txn->old_tuple || txn->new_tuple) {
		space_replace(txn->space, txn->new_tuple,
			      txn->old_tuple, DUP_INSERT);
		trigger_run(&txn->on_rollback, txn); /* must not throw. */
		if (txn->new_tuple)
			tuple_ref(txn->new_tuple, -1);
	}
	TRASH(txn);
	/** Free volatile txn memory. */
	fiber_gc();
	in_txn() = NULL;
}
