#include "recroute.h"
#include "fingerroutepns.h"
#include <location.h>
#include <locationtable.h>
#include <misc_utils.h>
#include <modlogger.h>

#define rwarning modlogger ("recroute", modlogger::WARNING)
#define rinfo    modlogger ("recroute", modlogger::INFO)
#define rtrace   modlogger ("recroute", modlogger::TRACE)

template<class T>
ref<vnode>
recroute<T>::produce_vnode (ref<chord> _chordnode,
			    ref<rpc_manager> _rpcm,
			    ref<location> _l)
{
  return New refcounted<recroute<T> > (_chordnode, _rpcm, _l);
}

template<class T>
recroute<T>::recroute (ref<chord> _chord,
		       ref<rpc_manager> _rpcm,
		       ref<location> _l)
  : T (_chord, _rpcm, _l)
{
  addHandler (recroute_program_1, wrap (this, &recroute<T>::dispatch));
}

template<class T>
recroute<T>::~recroute ()
{
  route_recchord *r = routers.first ();
  route_recchord *rn = NULL;
  while (r != NULL) {
    rn = routers.next (r);
    routers.remove (r);
    delete r;
    r = rn;
  }
}

template<class T>
void
recroute<T>::dispatch (user_args *a)
{
  if (a->prog->progno != recroute_program_1.progno) {
    T::dispatch (a);
    return;
  }

  switch (a->procno) {
  case RECROUTEPROC_NULL:
    a->reply (NULL);
    break;
  case RECROUTEPROC_ROUTE:
    {
      // go off and do recursive next hop and pass it on.
      recroute_route_arg *ra = a->template getarg<recroute_route_arg> ();
      dorecroute (a, ra);
    }
    break;
  case RECROUTEPROC_COMPLETE:
    {
      // Try to see if this is one of ours.
      // If so, we'll need to pass things back up to whoever called us.
      recroute_complete_arg *ca =
	a->template getarg<recroute_complete_arg> ();
      docomplete (a, ca);
    }
    break;
  default:
    a->reject (PROC_UNAVAIL);
    break;
  }
}

template<class T>
void
recroute<T>::dorecroute (user_args *sbp, recroute_route_arg *ra)
{
  recroute_route_stat rstat (RECROUTE_ACCEPTED);
  bool complete = false;

  // XXX check to see if I am actually the successor?
  //     an optimization that may be wrong because of lagging
  //     predecessor updates...
  
  vec<ptr<location> > cs = succs ();
  u_long m = ra->succs_desired;
  // duplicate some code from chord.C's vnode_impl::doroute
  // as well as some of server.C's find_succlist_hop_cb.
  if (betweenrightincl (my_ID (), cs[0]->id (), ra->x)) {
    complete = true;
  } else {
    // XXX should we check chord.find_succlist_shaving?
    size_t left = 0;
    size_t i = 1;
    if (cs.size () < m)
      left = cs.size ();
    else
    left = cs.size () - m;
    for (i = 1; i < left; i++) {
      if (betweenrightincl (cs[i-1]->id (), cs[i]->id (), ra->x)) {
	rtrace << my_ID () << ": dorecroute (" << ra->routeid << ", "
	       << ra->x << "): skipping " << i << " nodes.\n";
	cs.popn_front (i);
	complete = true;
	break;
      }
    }
  }
  chord_node_wire me;
  my_location ()->fill_node (me);

  if (complete) {
    // If complete (i.e. we have enough here to satisfy request),
    // send off a complete RPC.
    rtrace << my_ID () << ": dorecroute (" << ra->routeid << ", " << ra->x
	   << "): complete.\n";
    ptr<recroute_complete_arg> ca = New refcounted<recroute_complete_arg> ();
    ca->body.set_status (RECROUTE_ROUTE_OK);
    ca->routeid = ra->routeid;
    
    ca->path.setsize (ra->path.size () + 1);
    for (size_t i = 0; i < ra->path.size (); i++)
      ca->path[i] = ra->path[i];
    ca->path[ra->path.size ()] = me;
    
    u_long tofill = (cs.size () < m) ? cs.size () : m;
    ca->body.robody->successors.setsize (tofill);
    for (size_t i = 0; i < tofill; i++)
      cs[i]->fill_node (ca->body.robody->successors[i]);

    ca->retries = ra->retries;

#if 0
    const rpcgen_table *rtp;
    rtp = &recroute_program_1.tbl[RECROUTEPROC_COMPLETE];
    assert (rtp);

    if (rtp->print_arg)
      rtp->print_arg (ca, NULL, 6, "ARGS", "");
#endif /* 0 */    
    
    ptr<location> l = locations->lookup_or_create (make_chord_node (ra->origin));
    doRPC (l, recroute_program_1, RECROUTEPROC_COMPLETE,
	   ca, NULL,
	   wrap (this, &recroute<T>::recroute_sent_complete_cb));
    // We don't really care if this is lost beyond the RPC system's
    // retransmits.
  } else {
    // Otherwise... construct a new recroute_route_arg.
    // - Fill out new path
    ptr<recroute_route_arg> nra = New refcounted<recroute_route_arg> ();
    *nra = *ra;
    nra->path.setsize (ra->path.size () + 1);
    for (size_t i = 0; i < ra->path.size (); i++)
      nra->path[i] = ra->path[i];
    nra->path[ra->path.size ()] = me;
    
    vec<chordID> failed;
    ptr<location> p = closestpred (ra->x, failed);
    recroute_route_stat *res = New recroute_route_stat (RECROUTE_ACCEPTED);
    doRPC (p, recroute_program_1, RECROUTEPROC_ROUTE,
	   nra, res,
	   wrap (this, &recroute<T>::recroute_hop_cb, nra, p, failed, res));
  }

#if 0
  // XXX I think will need to have customized up call handling, if we
  //     want to support this.
  // XXX will also need to forward the RPC later on, or decide what to
  //     do about CHORD_STOP type behavior.
  if (ra->upcall_prog)  {
    do_upcall (ra->upcall_prog, ra->upcall_proc,
	       ra->upcall_args.base (), ra->upcall_args.size (),
	       wrap (this, &vnode_impl::upcall_done, fa, res, sbp));
  } else {
    sbp->replyref (rstat);
  }
#endif /* 0 */
  sbp->replyref (rstat);
}

template<class T>
void
recroute<T>::recroute_hop_cb (ptr<recroute_route_arg> nra,
			      ptr<location> p,
			      vec<chordID> failed,
			      recroute_route_stat *res,
			      clnt_stat status)
{
  if (!status && *res == RECROUTE_ACCEPTED) {
    delete res; res = NULL;
    return;
  }
  rtrace << my_ID () << ": dorecroute (" << nra->routeid << ", " << nra->x
	 << ") forwarding to "
	 << p->id () << " failed (" << status << "," << *res << ").\n";
  if (failed.size () > 3) {    // XXX hardcoded constant
    rtrace << my_ID () << ": dorecroute (" << nra->routeid << ", " << nra->x
	   << ") failed too often. Discarding.\n";
    
    ptr<recroute_complete_arg> ca = New refcounted<recroute_complete_arg> ();
    ca->body.set_status (RECROUTE_ROUTE_FAILED);
    p->fill_node (ca->body.rfbody->failed_hop);
    ca->body.rfbody->failed_stat = status;
    
    ca->routeid = nra->routeid;
    ca->retries = nra->retries;
    ca->path    = nra->path;

    ptr<location> o = locations->lookup_or_create (make_chord_node (nra->origin));
    doRPC (o, recroute_program_1, RECROUTEPROC_COMPLETE,
	   ca, NULL,
	   wrap (this, &recroute<T>::recroute_sent_complete_cb));
    // We don't really care if this is lost beyond the RPC system's
    // retransmits.
    delete res; res = NULL;
  }
  nra->retries++;
  failed.push_back (p->id ());
  p = closestpred (nra->x, failed);

  doRPC (p, recroute_program_1, RECROUTEPROC_ROUTE,
	 nra, res,
	 wrap (this, &recroute<T>::recroute_hop_cb, nra, p, failed, res));
}

template<class T>
void
recroute<T>::recroute_sent_complete_cb (clnt_stat status)
{
  if (status)
    rwarning << my_ID () << ": recroute_complete lost, status " << status << ".\n";
  // We're not going to do anything more clever about this. It's dropped. Fini.
}

template<class T>
void
recroute<T>::docomplete (user_args *sbp, recroute_complete_arg *ca)
{
  route_recchord *router = routers[ca->routeid];
  if (!router) {
    chord_node src; sbp->fill_from (&src);
    rwarning << my_ID () << ": docomplete: unknown routeid " << ca->routeid
		     << " from host " << src << "\n";
    sbp->reply (NULL);
    return;
  }
  rtrace << "docomplete: routeid " << ca->routeid << " has returned!\n";
  routers.remove (router);
  router->handle_complete (sbp, ca);
  // XXX Print out something about ca->retries?
}

// override produce_iterator*
template<class T>
ptr<route_iterator>
recroute<T>::produce_iterator (chordID xi) 
{
  ptr<route_recchord> ri = New refcounted<route_recchord> (mkref (this), xi);
  routers.insert (ri);
  return ri;
}

template<class T>
ptr<route_iterator>
recroute<T>::produce_iterator (chordID xi,
			       const rpc_program &uc_prog,
			       int uc_procno,
			       ptr<void> uc_args) 
{
  ptr<route_recchord> ri = New refcounted<route_recchord> (mkref (this),
							   xi, uc_prog,
							   uc_procno, uc_args);
  routers.insert (ri);
  return ri;
}

template<class T>
route_iterator *
recroute<T>::produce_iterator_ptr (chordID xi) 
{
  route_recchord *ri = New route_recchord (mkref (this), xi);
  routers.insert (ri);
  return ri;
}

template<class T>
route_iterator *
recroute<T>::produce_iterator_ptr (chordID xi,
				   const rpc_program &uc_prog,
				   int uc_procno,
				   ptr<void> uc_args) 
{
  route_recchord *ri = New route_recchord (mkref (this), xi, uc_prog,
					   uc_procno, uc_args);
  routers.insert (ri);
  return ri;
}