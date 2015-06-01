/* Copyright (c) 2001 Matej Pfajfar.
 * Copyright (c) 2001-2004, Roger Dingledine.
 * Copyright (c) 2004-2006, Roger Dingledine, Nick Mathewson.
 * Copyright (c) 2007-2011, The Tor Project, Inc. */
/* See LICENSE for licensing information */

/**
 * \file circuituse.c
 * \brief Launch the right sort of circuits and attach streams to them.
 **/

#include "or.h"
#include "circuitbuild.h"
#include "circuitlist.h"
#include "circuituse.h"
#include "config.h"
#include "connection.h"
#include "connection_edge.h"
#include "control.h"
#include "policies.h"
#include "rendclient.h"
#include "rendcommon.h"
#include "rendservice.h"
#include "rephist.h"
#include "router.h"
#include "routerlist.h"

/********* START VARIABLES **********/

extern circuit_t *global_circuitlist; /* from circuitlist.c */

/********* END VARIABLES ************/

static void circuit_expire_old_circuits_clientside(void);
static void circuit_increment_failure_count(void);

/** Return 1 if <b>circ</b> could be returned by circuit_get_best().
 * Else return 0.
 */
static int
circuit_is_acceptable(circuit_t *circ, edge_connection_t *conn,
                      int must_be_open, uint8_t purpose,
                      int need_uptime, int need_internal,
                      time_t now)
{
  routerinfo_t *exitrouter;
  cpath_build_state_t *build_state;
  tor_assert(circ);
  tor_assert(conn);
  tor_assert(conn->socks_request);

  if (!CIRCUIT_IS_ORIGIN(circ))
    return 0; /* this circ doesn't start at us */
  if (must_be_open && (circ->state != CIRCUIT_STATE_OPEN || !circ->n_conn))
    return 0; /* ignore non-open circs */
  if (circ->marked_for_close)
    return 0;

  /* if this circ isn't our purpose, skip. */
  if (purpose == CIRCUIT_PURPOSE_C_REND_JOINED && !must_be_open) {
    if (circ->purpose != CIRCUIT_PURPOSE_C_ESTABLISH_REND &&
        circ->purpose != CIRCUIT_PURPOSE_C_REND_READY &&
        circ->purpose != CIRCUIT_PURPOSE_C_REND_READY_INTRO_ACKED &&
        circ->purpose != CIRCUIT_PURPOSE_C_REND_JOINED)
      return 0;
  } else if (purpose == CIRCUIT_PURPOSE_C_INTRODUCE_ACK_WAIT &&
             !must_be_open) {
    if (circ->purpose != CIRCUIT_PURPOSE_C_INTRODUCING &&
        circ->purpose != CIRCUIT_PURPOSE_C_INTRODUCE_ACK_WAIT)
      return 0;
  } else {
    if (purpose != circ->purpose)
      return 0;
  }

  if (purpose == CIRCUIT_PURPOSE_C_GENERAL ||
      purpose == CIRCUIT_PURPOSE_C_REND_JOINED)
    if (circ->timestamp_dirty &&
       circ->timestamp_dirty+get_options()->MaxCircuitDirtiness <= now)
      return 0;

  /* decide if this circ is suitable for this conn */

  /* for rend circs, circ->cpath->prev is not the last router in the
   * circuit, it's the magical extra bob hop. so just check the nickname
   * of the one we meant to finish at.
   */
  build_state = TO_ORIGIN_CIRCUIT(circ)->build_state;
  exitrouter = build_state_get_exit_router(build_state);

  if (need_uptime && !build_state->need_uptime)
    return 0;
  if (need_internal != build_state->is_internal)
    return 0;

  if (purpose == CIRCUIT_PURPOSE_C_GENERAL) {
    if (!exitrouter && !build_state->onehop_tunnel) {
      log_debug(LD_CIRC,"Not considering circuit with unknown router.");
      return 0; /* this circuit is screwed and doesn't know it yet,
                 * or is a rendezvous circuit. */
    }
    if (build_state->onehop_tunnel) {
      if (!conn->want_onehop) {
        log_debug(LD_CIRC,"Skipping one-hop circuit.");
        return 0;
      }
      tor_assert(conn->chosen_exit_name);
      if (build_state->chosen_exit) {
        char digest[DIGEST_LEN];
        if (hexdigest_to_digest(conn->chosen_exit_name, digest) < 0)
          return 0; /* broken digest, we don't want it */
        if (tor_memneq(digest, build_state->chosen_exit->identity_digest,
                          DIGEST_LEN))
          return 0; /* this is a circuit to somewhere else */
        if (tor_digest_is_zero(digest)) {
          /* we don't know the digest; have to compare addr:port */
          tor_addr_t addr;
          int r = tor_addr_from_str(&addr, conn->socks_request->address);
          if (r < 0 ||
              !tor_addr_eq(&build_state->chosen_exit->addr, &addr) ||
              build_state->chosen_exit->port != conn->socks_request->port)
            return 0;
        }
      }
    } else {
      if (conn->want_onehop) {
        /* don't use three-hop circuits -- that could hurt our anonymity. */
        return 0;
      }
    }
    if (exitrouter && !connection_ap_can_use_exit(conn, exitrouter)) {
      /* can't exit from this router */
      return 0;
    }
  } else { /* not general */
    origin_circuit_t *ocirc = TO_ORIGIN_CIRCUIT(circ);
    if ((conn->rend_data && !ocirc->rend_data) ||
        (!conn->rend_data && ocirc->rend_data) ||
        (conn->rend_data && ocirc->rend_data &&
         rend_cmp_service_ids(conn->rend_data->onion_address,
                              ocirc->rend_data->onion_address))) {
      /* this circ is not for this conn */
      return 0;
    }
  }
  return 1;
}

/** Return 1 if circuit <b>a</b> is better than circuit <b>b</b> for
 * <b>purpose</b>, and return 0 otherwise. Used by circuit_get_best.
 */
static int
circuit_is_better(circuit_t *a, circuit_t *b, uint8_t purpose)
{
  switch (purpose) {
    case CIRCUIT_PURPOSE_C_GENERAL:
      /* if it's used but less dirty it's best;
       * else if it's more recently created it's best
       */
      if (b->timestamp_dirty) {
        if (a->timestamp_dirty &&
            a->timestamp_dirty > b->timestamp_dirty)
          return 1;
      } else {
        if (a->timestamp_dirty ||
            timercmp(&a->timestamp_created, &b->timestamp_created, >))
          return 1;
        if (CIRCUIT_IS_ORIGIN(b) &&
            TO_ORIGIN_CIRCUIT(b)->build_state->is_internal)
          /* XXX023 what the heck is this internal thing doing here. I
           * think we can get rid of it. circuit_is_acceptable() already
           * makes sure that is_internal is exactly what we need it to
           * be. -RD */
          return 1;
      }
      break;
    case CIRCUIT_PURPOSE_C_INTRODUCE_ACK_WAIT:
      /* the closer it is to ack_wait the better it is */
      if (a->purpose > b->purpose)
        return 1;
      break;
    case CIRCUIT_PURPOSE_C_REND_JOINED:
      /* the closer it is to rend_joined the better it is */
      if (a->purpose > b->purpose)
        return 1;
      break;
  }
  return 0;
}

/** Find the best circ that conn can use, preferably one which is
 * dirty. Circ must not be too old.
 *
 * Conn must be defined.
 *
 * If must_be_open, ignore circs not in CIRCUIT_STATE_OPEN.
 *
 * circ_purpose specifies what sort of circuit we must have.
 * It can be C_GENERAL, C_INTRODUCE_ACK_WAIT, or C_REND_JOINED.
 *
 * If it's REND_JOINED and must_be_open==0, then return the closest
 * rendezvous-purposed circuit that you can find.
 *
 * If it's INTRODUCE_ACK_WAIT and must_be_open==0, then return the
 * closest introduce-purposed circuit that you can find.
 */
static origin_circuit_t *
circuit_get_best(edge_connection_t *conn, int must_be_open, uint8_t purpose,
                 int need_uptime, int need_internal)
{
  circuit_t *circ, *best=NULL;
  struct timeval now;
  int intro_going_on_but_too_old = 0;

  tor_assert(conn);

  tor_assert(purpose == CIRCUIT_PURPOSE_C_GENERAL ||
             purpose == CIRCUIT_PURPOSE_C_INTRODUCE_ACK_WAIT ||
             purpose == CIRCUIT_PURPOSE_C_REND_JOINED);

  tor_gettimeofday(&now);

  for (circ=global_circuitlist;circ;circ = circ->next) {
    if (!circuit_is_acceptable(circ,conn,must_be_open,purpose,
                               need_uptime,need_internal,now.tv_sec))
      continue;

    if (purpose == CIRCUIT_PURPOSE_C_INTRODUCE_ACK_WAIT &&
        !must_be_open && circ->state != CIRCUIT_STATE_OPEN &&
        tv_mdiff(&now, &circ->timestamp_created) > circ_times.timeout_ms) {
      intro_going_on_but_too_old = 1;
      continue;
    }

    /* now this is an acceptable circ to hand back. but that doesn't
     * mean it's the *best* circ to hand back. try to decide.
     */
    if (!best || circuit_is_better(circ,best,purpose))
      best = circ;
  }

  if (!best && intro_going_on_but_too_old)
    log_info(LD_REND|LD_CIRC, "There is an intro circuit being created "
             "right now, but it has already taken quite a while. Starting "
             "one in parallel.");

  return best ? TO_ORIGIN_CIRCUIT(best) : NULL;
}

#if 0
/** Check whether, according to the policies in <b>options</b>, the
 * circuit <b>circ</b> makes sense. */
/* XXXX currently only checks Exclude{Exit}Nodes; it should check more.
 * Also, it doesn't have the right definition of an exit circuit. Also,
 * it's never called. */
int
circuit_conforms_to_options(const origin_circuit_t *circ,
                            const or_options_t *options)
{
  const crypt_path_t *cpath, *cpath_next = NULL;

  /* first check if it includes any excluded nodes */
  for (cpath = circ->cpath; cpath_next != circ->cpath; cpath = cpath_next) {
    cpath_next = cpath->next;
    if (routerset_contains_extendinfo(options->ExcludeNodes,
                                      cpath->extend_info))
      return 0;
  }

  /* then consider the final hop */
  if (routerset_contains_extendinfo(options->ExcludeExitNodes,
                                    circ->cpath->prev->extend_info))
    return 0;

  return 1;
}
#endif

/** Close all circuits that start at us, aren't open, and were born
 * at least CircuitBuildTimeout seconds ago.
 */
void
circuit_expire_building(void)
{
  circuit_t *victim, *next_circ = global_circuitlist;
  /* circ_times.timeout_ms and circ_times.close_ms are from
   * circuit_build_times_get_initial_timeout() if we haven't computed
   * custom timeouts yet */
  struct timeval general_cutoff, begindir_cutoff, fourhop_cutoff,
    cannibalize_cutoff, close_cutoff, extremely_old_cutoff;
  struct timeval now;
  cpath_build_state_t *build_state;

  tor_gettimeofday(&now);
#define SET_CUTOFF(target, msec) do {                       \
    long ms = tor_lround(msec);                             \
    struct timeval diff;                                    \
    diff.tv_sec = ms / 1000;                                \
    diff.tv_usec = (int)((ms % 1000) * 1000);               \
    timersub(&now, &diff, &target);                         \
  } while (0)

  SET_CUTOFF(general_cutoff, circ_times.timeout_ms);
  SET_CUTOFF(begindir_cutoff, circ_times.timeout_ms / 2.0);
  SET_CUTOFF(fourhop_cutoff, circ_times.timeout_ms * (4/3.0));
  SET_CUTOFF(cannibalize_cutoff, circ_times.timeout_ms / 2.0);
  SET_CUTOFF(close_cutoff, circ_times.close_ms);
  SET_CUTOFF(extremely_old_cutoff, circ_times.close_ms*2 + 1000);

  while (next_circ) {
    struct timeval cutoff;
    victim = next_circ;
    next_circ = next_circ->next;
    if (!CIRCUIT_IS_ORIGIN(victim) || /* didn't originate here */
        victim->marked_for_close) /* don't mess with marked circs */
      continue;

    build_state = TO_ORIGIN_CIRCUIT(victim)->build_state;
    if (build_state && build_state->onehop_tunnel)
      cutoff = begindir_cutoff;
    else if (build_state && build_state->desired_path_len == 4
             && !TO_ORIGIN_CIRCUIT(victim)->has_opened)
      cutoff = fourhop_cutoff;
    else if (TO_ORIGIN_CIRCUIT(victim)->has_opened)
      cutoff = cannibalize_cutoff;
    else if (victim->purpose == CIRCUIT_PURPOSE_C_MEASURE_TIMEOUT)
      cutoff = close_cutoff;
    else
      cutoff = general_cutoff;

    if (timercmp(&victim->timestamp_created, &cutoff, >))
      continue; /* it's still young, leave it alone */

#if 0
    /* some debug logs, to help track bugs */
    if (victim->purpose >= CIRCUIT_PURPOSE_C_INTRODUCING &&
        victim->purpose <= CIRCUIT_PURPOSE_C_REND_READY_INTRO_ACKED) {
      if (!victim->timestamp_dirty)
        log_fn(LOG_DEBUG,"Considering %sopen purpose %d to %s (circid %d)."
               "(clean).",
               victim->state == CIRCUIT_STATE_OPEN ? "" : "non",
               victim->purpose, victim->build_state->chosen_exit_name,
               victim->n_circ_id);
      else
        log_fn(LOG_DEBUG,"Considering %sopen purpose %d to %s (circid %d). "
               "%d secs since dirty.",
               victim->state == CIRCUIT_STATE_OPEN ? "" : "non",
               victim->purpose, victim->build_state->chosen_exit_name,
               victim->n_circ_id,
               (int)(now - victim->timestamp_dirty));
    }
#endif

    /* if circ is !open, or if it's open but purpose is a non-finished
     * intro or rend, then mark it for close */
    if (victim->state == CIRCUIT_STATE_OPEN) {
      switch (victim->purpose) {
        default: /* most open circuits can be left alone. */
          continue; /* yes, continue inside a switch refers to the nearest
                     * enclosing loop. C is smart. */
        case CIRCUIT_PURPOSE_C_ESTABLISH_REND:
        case CIRCUIT_PURPOSE_C_INTRODUCING:
        case CIRCUIT_PURPOSE_S_ESTABLISH_INTRO:
          break; /* too old, need to die */
        case CIRCUIT_PURPOSE_C_REND_READY:
          /* it's a rend_ready circ -- has it already picked a query? */
          /* c_rend_ready circs measure age since timestamp_dirty,
           * because that's set when they switch purposes
           */
          if (TO_ORIGIN_CIRCUIT(victim)->rend_data ||
              victim->timestamp_dirty > cutoff.tv_sec)
            continue;
          break;
        case CIRCUIT_PURPOSE_C_REND_READY_INTRO_ACKED:
        case CIRCUIT_PURPOSE_C_INTRODUCE_ACK_WAIT:
          /* rend and intro circs become dirty each time they
           * make an introduction attempt. so timestamp_dirty
           * will reflect the time since the last attempt.
           */
          if (victim->timestamp_dirty > cutoff.tv_sec)
            continue;
          break;
      }
    } else { /* circuit not open, consider recording failure as timeout */
      int first_hop_succeeded = TO_ORIGIN_CIRCUIT(victim)->cpath &&
            TO_ORIGIN_CIRCUIT(victim)->cpath->state == CPATH_STATE_OPEN;

      if (TO_ORIGIN_CIRCUIT(victim)->p_streams != NULL) {
        log_warn(LD_BUG, "Circuit %d (purpose %d, %s) has timed out, "
                 "yet has attached streams!",
                 TO_ORIGIN_CIRCUIT(victim)->global_identifier,
                 victim->purpose,
                 circuit_purpose_to_string(victim->purpose));
        tor_fragile_assert();
        continue;
      }

      if (circuit_timeout_want_to_count_circ(TO_ORIGIN_CIRCUIT(victim)) &&
          circuit_build_times_enough_to_compute(&circ_times)) {
        /* Circuits are allowed to last longer for measurement.
         * Switch their purpose and wait. */
        if (victim->purpose != CIRCUIT_PURPOSE_C_MEASURE_TIMEOUT) {
          control_event_circuit_status(TO_ORIGIN_CIRCUIT(victim),
                                       CIRC_EVENT_FAILED,
                                       END_CIRC_REASON_TIMEOUT);
          victim->purpose = CIRCUIT_PURPOSE_C_MEASURE_TIMEOUT;
          /* Record this failure to check for too many timeouts
           * in a row. This function does not record a time value yet
           * (we do that later); it only counts the fact that we did
           * have a timeout. */
          circuit_build_times_count_timeout(&circ_times,
                                            first_hop_succeeded);
          continue;
        }

        /*
         * If the circuit build time is much greater than we would have cut
         * it off at, we probably had a suspend event along this codepath,
         * and we should discard the value.
         */
        if (timercmp(&victim->timestamp_created, &extremely_old_cutoff, <)) {
          log_notice(LD_CIRC,
                     "Extremely large value for circuit build timeout: %lds. "
                     "Assuming clock jump. Purpose %d (%s)",
                     (long)(now.tv_sec - victim->timestamp_created.tv_sec),
                     victim->purpose,
                     circuit_purpose_to_string(victim->purpose));
        } else if (circuit_build_times_count_close(&circ_times,
                                         first_hop_succeeded,
                                         victim->timestamp_created.tv_sec)) {
          circuit_build_times_set_timeout(&circ_times);
        }
      }
    }

    if (victim->n_conn)
      log_info(LD_CIRC,"Abandoning circ %s:%d:%d (state %d:%s, purpose %d)",
               victim->n_conn->_base.address, victim->n_conn->_base.port,
               victim->n_circ_id,
               victim->state, circuit_state_to_string(victim->state),
               victim->purpose);
    else
      log_info(LD_CIRC,"Abandoning circ %d (state %d:%s, purpose %d)",
               victim->n_circ_id, victim->state,
               circuit_state_to_string(victim->state), victim->purpose);

    circuit_log_path(LOG_INFO,LD_CIRC,TO_ORIGIN_CIRCUIT(victim));
    if (victim->purpose == CIRCUIT_PURPOSE_C_MEASURE_TIMEOUT)
      circuit_mark_for_close(victim, END_CIRC_REASON_MEASUREMENT_EXPIRED);
    else
      circuit_mark_for_close(victim, END_CIRC_REASON_TIMEOUT);
  }
}

/** Remove any elements in <b>needed_ports</b> that are handled by an
 * open or in-progress circuit.
 */
void
circuit_remove_handled_ports(smartlist_t *needed_ports)
{
  int i;
  uint16_t *port;

    // Kevin
    if (get_options()->Simulate) {
        return;
    }

  for (i = 0; i < smartlist_len(needed_ports); ++i) {
    port = smartlist_get(needed_ports, i);
    tor_assert(*port);
    if (circuit_stream_is_being_handled(NULL, *port,
                                        MIN_CIRCUITS_HANDLING_STREAM)) {
//      log_debug(LD_CIRC,"Port %d is already being handled; removing.", port);
      smartlist_del(needed_ports, i--);
      tor_free(port);
    } else {
      log_debug(LD_CIRC,"Port %d is not handled.", *port);
    }
  }
}

/** Return 1 if at least <b>min</b> general-purpose non-internal circuits
 * will have an acceptable exit node for exit stream <b>conn</b> if it
 * is defined, else for "*:port".
 * Else return 0.
 */
int
circuit_stream_is_being_handled(edge_connection_t *conn,
                                uint16_t port, int min)
{
  circuit_t *circ;
  routerinfo_t *exitrouter;
  int num=0;
  time_t now = time(NULL);
  int need_uptime = smartlist_string_num_isin(get_options()->LongLivedPorts,
                                   conn ? conn->socks_request->port : port);

  for (circ=global_circuitlist;circ;circ = circ->next) {
    if (CIRCUIT_IS_ORIGIN(circ) &&
        !circ->marked_for_close &&
        circ->purpose == CIRCUIT_PURPOSE_C_GENERAL &&
        (!circ->timestamp_dirty ||
         circ->timestamp_dirty + get_options()->MaxCircuitDirtiness > now)) {
      cpath_build_state_t *build_state = TO_ORIGIN_CIRCUIT(circ)->build_state;
      if (build_state->is_internal || build_state->onehop_tunnel)
        continue;

      exitrouter = build_state_get_exit_router(build_state);
      if (exitrouter && (!need_uptime || build_state->need_uptime)) {
        int ok;
        if (conn) {
          ok = connection_ap_can_use_exit(conn, exitrouter);
        } else {
          addr_policy_result_t r = compare_addr_to_addr_policy(
              0, port, exitrouter->exit_policy);
          ok = r != ADDR_POLICY_REJECTED && r != ADDR_POLICY_PROBABLY_REJECTED;
        }
        if (ok) {
          if (++num >= min)
            return 1;
        }
      }
    }
  }
  return 0;
}

/** Don't keep more than this many unused open circuits around. */
#define MAX_UNUSED_OPEN_CIRCUITS 14

/** Figure out how many circuits we have open that are clean. Make
 * sure it's enough for all the upcoming behaviors we predict we'll have.
 * But put an upper bound on the total number of circuits.
 */
static void
circuit_predict_and_launch_new(void)
{
  circuit_t *circ;
  int num=0, num_internal=0, num_uptime_internal=0;
  int hidserv_needs_uptime=0, hidserv_needs_capacity=1;
  int port_needs_uptime=0, port_needs_capacity=1;
  time_t now = time(NULL);
  int flags = 0;

  /* First, count how many of each type of circuit we have already. */
  for (circ=global_circuitlist;circ;circ = circ->next) {
    cpath_build_state_t *build_state;
    if (!CIRCUIT_IS_ORIGIN(circ))
      continue;
    if (circ->marked_for_close)
      continue; /* don't mess with marked circs */
    if (circ->timestamp_dirty)
      continue; /* only count clean circs */
    if (circ->purpose != CIRCUIT_PURPOSE_C_GENERAL)
      continue; /* only pay attention to general-purpose circs */
    build_state = TO_ORIGIN_CIRCUIT(circ)->build_state;
    if (build_state->onehop_tunnel)
      continue;
    num++;
    if (build_state->is_internal)
      num_internal++;
    if (build_state->need_uptime && build_state->is_internal)
      num_uptime_internal++;
  }

  /* If that's enough, then stop now. */
  if (num >= MAX_UNUSED_OPEN_CIRCUITS)
    return; /* we already have many, making more probably will hurt */

  /* Second, see if we need any more exit circuits. */
  /* check if we know of a port that's been requested recently
   * and no circuit is currently available that can handle it. */
  if (!circuit_all_predicted_ports_handled(now, &port_needs_uptime,
                                           &port_needs_capacity)) {
    if (port_needs_uptime)
      flags |= CIRCLAUNCH_NEED_UPTIME;
    if (port_needs_capacity)
      flags |= CIRCLAUNCH_NEED_CAPACITY;
    log_info(LD_CIRC,
             "Have %d clean circs (%d internal), need another exit circ.",
             num, num_internal);
    circuit_launch_by_router(CIRCUIT_PURPOSE_C_GENERAL, NULL, flags);
    return;
  }

  /* Third, see if we need any more hidden service (server) circuits. */
  if (num_rend_services() && num_uptime_internal < 3) {
    flags = (CIRCLAUNCH_NEED_CAPACITY | CIRCLAUNCH_NEED_UPTIME |
             CIRCLAUNCH_IS_INTERNAL);
    log_info(LD_CIRC,
             "Have %d clean circs (%d internal), need another internal "
             "circ for my hidden service.",
             num, num_internal);
    circuit_launch_by_router(CIRCUIT_PURPOSE_C_GENERAL, NULL, flags);
    return;
  }

  /* Fourth, see if we need any more hidden service (client) circuits. */
  if (rep_hist_get_predicted_internal(now, &hidserv_needs_uptime,
                                      &hidserv_needs_capacity) &&
      ((num_uptime_internal<2 && hidserv_needs_uptime) ||
        num_internal<2)) {
    if (hidserv_needs_uptime)
      flags |= CIRCLAUNCH_NEED_UPTIME;
    if (hidserv_needs_capacity)
      flags |= CIRCLAUNCH_NEED_CAPACITY;
    flags |= CIRCLAUNCH_IS_INTERNAL;
    log_info(LD_CIRC,
             "Have %d clean circs (%d uptime-internal, %d internal), need"
             " another hidden service circ.",
             num, num_uptime_internal, num_internal);
    circuit_launch_by_router(CIRCUIT_PURPOSE_C_GENERAL, NULL, flags);
    return;
  }

  /* Finally, check to see if we still need more circuits to learn
   * a good build timeout. But if we're close to our max number we
   * want, don't do another -- we want to leave a few slots open so
   * we can still build circuits preemptively as needed. */
  if (num < MAX_UNUSED_OPEN_CIRCUITS-2 &&
      circuit_build_times_needs_circuits_now(&circ_times)) {
    flags = CIRCLAUNCH_NEED_CAPACITY;
    log_info(LD_CIRC,
             "Have %d clean circs need another buildtime test circ.", num);
    circuit_launch_by_router(CIRCUIT_PURPOSE_C_GENERAL, NULL, flags);
    return;
  }
}

/** Build a new test circuit every 5 minutes */
#define TESTING_CIRCUIT_INTERVAL 300

/** This function is called once a second, if router_have_min_dir_info() is
 * true. Its job is to make sure all services we offer have enough circuits
 * available. Some services just want enough circuits for current tasks,
 * whereas others want a minimum set of idle circuits hanging around.
 */
void
circuit_build_needed_circs(time_t now)
{
  static time_t time_to_new_circuit = 0;
  or_options_t *options = get_options();

  /* launch a new circ for any pending streams that need one */
  connection_ap_attach_pending();

  /* make sure any hidden services have enough intro points */
  rend_services_introduce();

  if (time_to_new_circuit < now) {
    circuit_reset_failure_count(1);
    time_to_new_circuit = now + options->NewCircuitPeriod;
    if (proxy_mode(get_options()))
      addressmap_clean(now);
    circuit_expire_old_circuits_clientside();

#if 0 /* disable for now, until predict-and-launch-new can cull leftovers */
    circ = circuit_get_youngest_clean_open(CIRCUIT_PURPOSE_C_GENERAL);
    if (get_options()->RunTesting &&
        circ &&
        circ->timestamp_created + TESTING_CIRCUIT_INTERVAL < now) {
      log_fn(LOG_INFO,"Creating a new testing circuit.");
      circuit_launch_by_router(CIRCUIT_PURPOSE_C_GENERAL, NULL, 0);
    }
#endif
  }
  if (!options->DisablePredictedCircuits)
    circuit_predict_and_launch_new();
}

/** If the stream <b>conn</b> is a member of any of the linked
 * lists of <b>circ</b>, then remove it from the list.
 */
void
circuit_detach_stream(circuit_t *circ, edge_connection_t *conn)
{
  edge_connection_t *prevconn;

  tor_assert(circ);
  tor_assert(conn);

  conn->cpath_layer = NULL; /* make sure we don't keep a stale pointer */
  conn->on_circuit = NULL;

  if (CIRCUIT_IS_ORIGIN(circ)) {
    origin_circuit_t *origin_circ = TO_ORIGIN_CIRCUIT(circ);
    if (conn == origin_circ->p_streams) {
      origin_circ->p_streams = conn->next_stream;
      return;
    }

    for (prevconn = origin_circ->p_streams;
         prevconn && prevconn->next_stream && prevconn->next_stream != conn;
         prevconn = prevconn->next_stream)
      ;
    if (prevconn && prevconn->next_stream) {
      prevconn->next_stream = conn->next_stream;
      return;
    }
  } else {
    or_circuit_t *or_circ = TO_OR_CIRCUIT(circ);
    if (conn == or_circ->n_streams) {
      or_circ->n_streams = conn->next_stream;
      return;
    }
    if (conn == or_circ->resolving_streams) {
      or_circ->resolving_streams = conn->next_stream;
      return;
    }

    for (prevconn = or_circ->n_streams;
         prevconn && prevconn->next_stream && prevconn->next_stream != conn;
         prevconn = prevconn->next_stream)
      ;
    if (prevconn && prevconn->next_stream) {
      prevconn->next_stream = conn->next_stream;
      return;
    }

    for (prevconn = or_circ->resolving_streams;
         prevconn && prevconn->next_stream && prevconn->next_stream != conn;
         prevconn = prevconn->next_stream)
      ;
    if (prevconn && prevconn->next_stream) {
      prevconn->next_stream = conn->next_stream;
      return;
    }
  }

  log_warn(LD_BUG,"Edge connection not in circuit's list.");
  /* Don't give an error here; it's harmless. */
  tor_fragile_assert();
}

/** If we haven't yet decided on a good timeout value for circuit
 * building, we close idles circuits aggressively so we can get more
 * data points. */
#define IDLE_TIMEOUT_WHILE_LEARNING (10*60)

/** Find each circuit that has been unused for too long, or dirty
 * for too long and has no streams on it: mark it for close.
 */
static void
circuit_expire_old_circuits_clientside(void)
{
  circuit_t *circ;
  struct timeval cutoff, now;

  tor_gettimeofday(&now);
  cutoff = now;

  if (circuit_build_times_needs_circuits(&circ_times)) {
    /* Circuits should be shorter lived if we need more of them
     * for learning a good build timeout */
    cutoff.tv_sec -= IDLE_TIMEOUT_WHILE_LEARNING;
  } else {
    cutoff.tv_sec -= get_options()->CircuitIdleTimeout;
  }

  for (circ = global_circuitlist; circ; circ = circ->next) {
    if (circ->marked_for_close || !CIRCUIT_IS_ORIGIN(circ))
      continue;
    /* If the circuit has been dirty for too long, and there are no streams
     * on it, mark it for close.
     */
    if (circ->timestamp_dirty &&
        circ->timestamp_dirty + get_options()->MaxCircuitDirtiness <
          now.tv_sec &&
        !TO_ORIGIN_CIRCUIT(circ)->p_streams /* nothing attached */ ) {
      log_debug(LD_CIRC, "Closing n_circ_id %d (dirty %ld sec ago, "
                "purpose %d)",
                circ->n_circ_id, (long)(now.tv_sec - circ->timestamp_dirty),
                circ->purpose);
      circuit_mark_for_close(circ, END_CIRC_REASON_FINISHED);
    } else if (!circ->timestamp_dirty && circ->state == CIRCUIT_STATE_OPEN) {
      if (timercmp(&circ->timestamp_created, &cutoff, <)) {
        if (circ->purpose == CIRCUIT_PURPOSE_C_GENERAL ||
                circ->purpose == CIRCUIT_PURPOSE_C_MEASURE_TIMEOUT ||
                circ->purpose == CIRCUIT_PURPOSE_S_ESTABLISH_INTRO ||
                circ->purpose == CIRCUIT_PURPOSE_TESTING ||
                (circ->purpose >= CIRCUIT_PURPOSE_C_INTRODUCING &&
                circ->purpose <= CIRCUIT_PURPOSE_C_REND_READY_INTRO_ACKED) ||
                circ->purpose == CIRCUIT_PURPOSE_S_CONNECT_REND) {
          log_debug(LD_CIRC,
                    "Closing circuit that has been unused for %ld msec.",
                    tv_mdiff(&circ->timestamp_created, &now));
          circuit_mark_for_close(circ, END_CIRC_REASON_FINISHED);
        } else if (!TO_ORIGIN_CIRCUIT(circ)->is_ancient) {
          /* Server-side rend joined circuits can end up really old, because
           * they are reused by clients for longer than normal. The client
           * controls their lifespan. (They never become dirty, because
           * connection_exit_begin_conn() never marks anything as dirty.)
           * Similarly, server-side intro circuits last a long time. */
          if (circ->purpose != CIRCUIT_PURPOSE_S_REND_JOINED &&
              circ->purpose != CIRCUIT_PURPOSE_S_INTRO) {
            log_notice(LD_CIRC,
                       "Ancient non-dirty circuit %d is still around after "
                       "%ld milliseconds. Purpose: %d (%s)",
                       TO_ORIGIN_CIRCUIT(circ)->global_identifier,
                       tv_mdiff(&circ->timestamp_created, &now),
                       circ->purpose,
                       circuit_purpose_to_string(circ->purpose));
            TO_ORIGIN_CIRCUIT(circ)->is_ancient = 1;
          }
        }
      }
    }
  }
}

/** How long do we wait before killing circuits with the properties
 * described below?
 *
 * Probably we could choose a number here as low as 5 to 10 seconds,
 * since these circs are used for begindir, and a) generally you either
 * ask another begindir question right after or you don't for a long time,
 * b) clients at least through 0.2.1.x choose from the whole set of
 * directory mirrors at each choice, and c) re-establishing a one-hop
 * circuit via create-fast is a light operation assuming the TLS conn is
 * still there.
 *
 * I expect "b" to go away one day when we move to using directory
 * guards, but I think "a" and "c" are good enough reasons that a low
 * number is safe even then.
 */
#define IDLE_ONE_HOP_CIRC_TIMEOUT 60

/** Find each non-origin circuit that has been unused for too long,
 * has no streams on it, used a create_fast, and ends here: mark it
 * for close.
 */
void
circuit_expire_old_circuits_serverside(time_t now)
{
  circuit_t *circ;
  or_circuit_t *or_circ;
  time_t cutoff = now - IDLE_ONE_HOP_CIRC_TIMEOUT;

  for (circ = global_circuitlist; circ; circ = circ->next) {
    if (circ->marked_for_close || CIRCUIT_IS_ORIGIN(circ))
      continue;
    or_circ = TO_OR_CIRCUIT(circ);
    /* If the circuit has been idle for too long, and there are no streams
     * on it, and it ends here, and it used a create_fast, mark it for close.
     */
    if (or_circ->is_first_hop && !circ->n_conn &&
        !or_circ->n_streams && !or_circ->resolving_streams &&
        or_circ->p_conn &&
        or_circ->p_conn->timestamp_last_added_nonpadding <= cutoff) {
      log_info(LD_CIRC, "Closing circ_id %d (empty %d secs ago)",
               or_circ->p_circ_id,
               (int)(now - or_circ->p_conn->timestamp_last_added_nonpadding));
      circuit_mark_for_close(circ, END_CIRC_REASON_FINISHED);
    }
  }
}

/** Number of testing circuits we want open before testing our bandwidth. */
#define NUM_PARALLEL_TESTING_CIRCS 4

/** True iff we've ever had enough testing circuits open to test our
 * bandwidth. */
static int have_performed_bandwidth_test = 0;

/** Reset have_performed_bandwidth_test, so we'll start building
 * testing circuits again so we can exercise our bandwidth. */
void
reset_bandwidth_test(void)
{
  have_performed_bandwidth_test = 0;
}

/** Return 1 if we've already exercised our bandwidth, or if we
 * have fewer than NUM_PARALLEL_TESTING_CIRCS testing circuits
 * established or on the way. Else return 0.
 */
int
circuit_enough_testing_circs(void)
{
  circuit_t *circ;
  int num = 0;

  if (have_performed_bandwidth_test)
    return 1;

  for (circ = global_circuitlist; circ; circ = circ->next) {
    if (!circ->marked_for_close && CIRCUIT_IS_ORIGIN(circ) &&
        circ->purpose == CIRCUIT_PURPOSE_TESTING &&
        circ->state == CIRCUIT_STATE_OPEN)
      num++;
  }
  return num >= NUM_PARALLEL_TESTING_CIRCS;
}

/** A testing circuit has completed. Take whatever stats we want.
 * Noticing reachability is taken care of in onionskin_answer(),
 * so there's no need to record anything here. But if we still want
 * to do the bandwidth test, and we now have enough testing circuits
 * open, do it.
 */
static void
circuit_testing_opened(origin_circuit_t *circ)
{
  if (have_performed_bandwidth_test ||
      !check_whether_orport_reachable()) {
    /* either we've already done everything we want with testing circuits,
     * or this testing circuit became open due to a fluke, e.g. we picked
     * a last hop where we already had the connection open due to an
     * outgoing local circuit. */
    circuit_mark_for_close(TO_CIRCUIT(circ), END_CIRC_AT_ORIGIN);
  } else if (circuit_enough_testing_circs()) {
    router_perform_bandwidth_test(NUM_PARALLEL_TESTING_CIRCS, time(NULL));
    have_performed_bandwidth_test = 1;
  } else
    consider_testing_reachability(1, 0);
}

/** A testing circuit has failed to build. Take whatever stats we want. */
static void
circuit_testing_failed(origin_circuit_t *circ, int at_last_hop)
{
  if (server_mode(get_options()) && check_whether_orport_reachable())
    return;

  log_info(LD_GENERAL,
           "Our testing circuit (to see if your ORPort is reachable) "
           "has failed. I'll try again later.");

  /* These aren't used yet. */
  (void)circ;
  (void)at_last_hop;
}

/** The circuit <b>circ</b> has just become open. Take the next
 * step: for rendezvous circuits, we pass circ to the appropriate
 * function in rendclient or rendservice. For general circuits, we
 * call connection_ap_attach_pending, which looks for pending streams
 * that could use circ.
 */
void
circuit_has_opened(origin_circuit_t *circ)
{
  control_event_circuit_status(circ, CIRC_EVENT_BUILT, 0);

  /* Remember that this circuit has finished building. Now if we start
   * it building again later (e.g. by extending it), we will know not
   * to consider its build time. */
  circ->has_opened = 1;

  switch (TO_CIRCUIT(circ)->purpose) {
    case CIRCUIT_PURPOSE_C_ESTABLISH_REND:
      rend_client_rendcirc_has_opened(circ);
      connection_ap_attach_pending();
      break;
    case CIRCUIT_PURPOSE_C_INTRODUCING:
      rend_client_introcirc_has_opened(circ);
      break;
    case CIRCUIT_PURPOSE_C_GENERAL:
      /* Tell any AP connections that have been waiting for a new
       * circuit that one is ready. */
      connection_ap_attach_pending();
      break;
    case CIRCUIT_PURPOSE_S_ESTABLISH_INTRO:
      /* at Bob, waiting for introductions */
      rend_service_intro_has_opened(circ);
      break;
    case CIRCUIT_PURPOSE_S_CONNECT_REND:
      /* at Bob, connecting to rend point */
      rend_service_rendezvous_has_opened(circ);
      break;
    case CIRCUIT_PURPOSE_TESTING:
      circuit_testing_opened(circ);
      break;
    /* default:
     * This won't happen in normal operation, but might happen if the
     * controller did it. Just let it slide. */
  }
}

/** Called whenever a circuit could not be successfully built.
 */
void
circuit_build_failed(origin_circuit_t *circ)
{
  /* we should examine circ and see if it failed because of
   * the last hop or an earlier hop. then use this info below.
   */
  int failed_at_last_hop = 0;
  /* If the last hop isn't open, and the second-to-last is, we failed
   * at the last hop. */
  if (circ->cpath &&
      circ->cpath->prev->state != CPATH_STATE_OPEN &&
      circ->cpath->prev->prev->state == CPATH_STATE_OPEN) {
    failed_at_last_hop = 1;
  }
  if (circ->cpath &&
      circ->cpath->state != CPATH_STATE_OPEN) {
    /* We failed at the first hop. If there's an OR connection
     * to blame, blame it. Also, avoid this relay for a while, and
     * fail any one-hop directory fetches destined for it. */
    const char *n_conn_id = circ->cpath->extend_info->identity_digest;
    int already_marked = 0;
    if (circ->_base.n_conn) {
      or_connection_t *n_conn = circ->_base.n_conn;
      if (n_conn->is_bad_for_new_circs) {
        /* We only want to blame this router when a fresh healthy
         * connection fails. So don't mark this router as newly failed,
         * since maybe this was just an old circuit attempt that's
         * finally timing out now. Also, there's no need to blow away
         * circuits/streams/etc, since the failure of an unhealthy conn
         * doesn't tell us much about whether a healthy conn would
         * succeed. */
        already_marked = 1;
      }
      log_info(LD_OR,
               "Our circuit failed to get a response from the first hop "
               "(%s:%d). I'm going to try to rotate to a better connection.",
               n_conn->_base.address, n_conn->_base.port);
      n_conn->is_bad_for_new_circs = 1;
    } else {
      log_info(LD_OR,
               "Our circuit died before the first hop with no connection");
    }
    if (n_conn_id && !already_marked) {
      entry_guard_register_connect_status(n_conn_id, 0, 1, time(NULL));
      /* if there are any one-hop streams waiting on this circuit, fail
       * them now so they can retry elsewhere. */
      connection_ap_fail_onehop(n_conn_id, circ->build_state);
    }
  }

  switch (circ->_base.purpose) {
    case CIRCUIT_PURPOSE_C_GENERAL:
      /* If we never built the circuit, note it as a failure. */
      circuit_increment_failure_count();
      if (failed_at_last_hop) {
        /* Make sure any streams that demand our last hop as their exit
         * know that it's unlikely to happen. */
        circuit_discard_optional_exit_enclaves(circ->cpath->prev->extend_info);
      }
      break;
    case CIRCUIT_PURPOSE_TESTING:
      circuit_testing_failed(circ, failed_at_last_hop);
      break;
    case CIRCUIT_PURPOSE_S_ESTABLISH_INTRO:
      /* at Bob, waiting for introductions */
      if (circ->_base.state != CIRCUIT_STATE_OPEN) {
        circuit_increment_failure_count();
      }
      /* no need to care here, because bob will rebuild intro
       * points periodically. */
      break;
    case CIRCUIT_PURPOSE_C_INTRODUCING:
      /* at Alice, connecting to intro point */
      /* Don't increment failure count, since Bob may have picked
       * the introduction point maliciously */
      /* Alice will pick a new intro point when this one dies, if
       * the stream in question still cares. No need to act here. */
      break;
    case CIRCUIT_PURPOSE_C_ESTABLISH_REND:
      /* at Alice, waiting for Bob */
      circuit_increment_failure_count();
      /* Alice will pick a new rend point when this one dies, if
       * the stream in question still cares. No need to act here. */
      break;
    case CIRCUIT_PURPOSE_S_CONNECT_REND:
      /* at Bob, connecting to rend point */
      /* Don't increment failure count, since Alice may have picked
       * the rendezvous point maliciously */
      log_info(LD_REND,
               "Couldn't connect to Alice's chosen rend point %s "
               "(%s hop failed).",
               escaped(build_state_get_exit_nickname(circ->build_state)),
               failed_at_last_hop?"last":"non-last");
      rend_service_relaunch_rendezvous(circ);
      break;
    /* default:
     * This won't happen in normal operation, but might happen if the
     * controller did it. Just let it slide. */
  }
}

/** Number of consecutive failures so far; should only be touched by
 * circuit_launch_new and circuit_*_failure_count.
 */
static int n_circuit_failures = 0;
/** Before the last time we called circuit_reset_failure_count(), were
 * there a lot of failures? */
static int did_circs_fail_last_period = 0;

/** Don't retry launching a new circuit if we try this many times with no
 * success. */
#define MAX_CIRCUIT_FAILURES 5

/** Launch a new circuit; see circuit_launch_by_extend_info() for
 * details on arguments. */
origin_circuit_t *
circuit_launch_by_router(uint8_t purpose,
                         routerinfo_t *exit, int flags)
{
  origin_circuit_t *circ;
  extend_info_t *info = NULL;
  if (exit)
    info = extend_info_from_router(exit);
  circ = circuit_launch_by_extend_info(purpose, info, flags);

  extend_info_free(info);
  return circ;
}

/** Launch a new circuit with purpose <b>purpose</b> and exit node
 * <b>extend_info</b> (or NULL to select a random exit node).  If flags
 * contains CIRCLAUNCH_NEED_UPTIME, choose among routers with high uptime.  If
 * CIRCLAUNCH_NEED_CAPACITY is set, choose among routers with high bandwidth.
 * If CIRCLAUNCH_IS_INTERNAL is true, the last hop need not be an exit node.
 * If CIRCLAUNCH_ONEHOP_TUNNEL is set, the circuit will have only one hop.
 * Return the newly allocated circuit on success, or NULL on failure. */
origin_circuit_t *
circuit_launch_by_extend_info(uint8_t purpose,
                              extend_info_t *extend_info,
                              int flags)
{
  origin_circuit_t *circ;
  int onehop_tunnel = (flags & CIRCLAUNCH_ONEHOP_TUNNEL) != 0;

  if (!onehop_tunnel && !router_have_minimum_dir_info()) {
    log_debug(LD_CIRC,"Haven't fetched enough directory info yet; canceling "
              "circuit launch.");
    return NULL;
  }

  if ((extend_info || purpose != CIRCUIT_PURPOSE_C_GENERAL) &&
      purpose != CIRCUIT_PURPOSE_TESTING && !onehop_tunnel) {
    /* see if there are appropriate circs available to cannibalize. */
    /* XXX if we're planning to add a hop, perhaps we want to look for
     * internal circs rather than exit circs? -RD */
    circ = circuit_find_to_cannibalize(purpose, extend_info, flags);
    if (circ) {
      log_info(LD_CIRC,"Cannibalizing circ '%s' for purpose %d (%s)",
               build_state_get_exit_nickname(circ->build_state), purpose,
               circuit_purpose_to_string(purpose));
      circ->_base.purpose = purpose;
      /* reset the birth date of this circ, else expire_building
       * will see it and think it's been trying to build since it
       * began. */
      tor_gettimeofday(&circ->_base.timestamp_created);
      switch (purpose) {
        case CIRCUIT_PURPOSE_C_ESTABLISH_REND:
        case CIRCUIT_PURPOSE_S_ESTABLISH_INTRO:
          /* it's ready right now */
          break;
        case CIRCUIT_PURPOSE_C_INTRODUCING:
        case CIRCUIT_PURPOSE_S_CONNECT_REND:
        case CIRCUIT_PURPOSE_C_GENERAL:
          /* need to add a new hop */
          tor_assert(extend_info);
          if (circuit_extend_to_new_exit(circ, extend_info) < 0)
            return NULL;
          break;
        default:
          log_warn(LD_BUG,
                   "unexpected purpose %d when cannibalizing a circ.",
                   purpose);
          tor_fragile_assert();
          return NULL;
      }
      return circ;
    }
  }

  if (did_circs_fail_last_period &&
      n_circuit_failures > MAX_CIRCUIT_FAILURES) {
    /* too many failed circs in a row. don't try. */
//    log_fn(LOG_INFO,"%d failures so far, not trying.",n_circuit_failures);
    return NULL;
  }

  /* try a circ. if it fails, circuit_mark_for_close will increment
   * n_circuit_failures */
  return circuit_establish_circuit(purpose, extend_info, flags);
}

/** Record another failure at opening a general circuit. When we have
 * too many, we'll stop trying for the remainder of this minute.
 */
static void
circuit_increment_failure_count(void)
{
  ++n_circuit_failures;
  log_debug(LD_CIRC,"n_circuit_failures now %d.",n_circuit_failures);
}

/** Reset the failure count for opening general circuits. This means
 * we will try MAX_CIRCUIT_FAILURES times more (if necessary) before
 * stopping again.
 */
void
circuit_reset_failure_count(int timeout)
{
  if (timeout && n_circuit_failures > MAX_CIRCUIT_FAILURES)
    did_circs_fail_last_period = 1;
  else
    did_circs_fail_last_period = 0;
  n_circuit_failures = 0;
}

/** Find an open circ that we're happy to use for <b>conn</b> and return 1. If
 * there isn't one, and there isn't one on the way, launch one and return
 * 0. If it will never work, return -1.
 *
 * Write the found or in-progress or launched circ into *circp.
 */
static int
circuit_get_open_circ_or_launch(edge_connection_t *conn,
                                uint8_t desired_circuit_purpose,
                                origin_circuit_t **circp)
{
  origin_circuit_t *circ;
  int check_exit_policy;
  int need_uptime, need_internal;
  int want_onehop;
  or_options_t *options = get_options();

  tor_assert(conn);
  tor_assert(circp);
  tor_assert(conn->_base.state == AP_CONN_STATE_CIRCUIT_WAIT);
  check_exit_policy =
      conn->socks_request->command == SOCKS_COMMAND_CONNECT &&
      !conn->use_begindir &&
      !connection_edge_is_rendezvous_stream(conn);
  want_onehop = conn->want_onehop;

  need_uptime = !conn->want_onehop && !conn->use_begindir &&
                smartlist_string_num_isin(options->LongLivedPorts,
                                          conn->socks_request->port);
  need_internal = desired_circuit_purpose != CIRCUIT_PURPOSE_C_GENERAL;

  circ = circuit_get_best(conn, 1, desired_circuit_purpose,
                          need_uptime, need_internal);

  if (circ) {
    *circp = circ;
    return 1; /* we're happy */
  }

  if (!want_onehop && !router_have_minimum_dir_info()) {
    if (!connection_get_by_type(CONN_TYPE_DIR)) {
      int severity = LOG_NOTICE;
      /* FFFF if this is a tunneled directory fetch, don't yell
       * as loudly. the user doesn't even know it's happening. */
      if (entry_list_is_constrained(options) &&
          entries_known_but_down(options)) {
        log_fn(severity, LD_APP|LD_DIR,
               "Application request when we haven't used client functionality "
               "lately. Optimistically trying known %s again.",
               options->UseBridges ? "bridges" : "entrynodes");
        entries_retry_all(options);
      } else if (!options->UseBridges || any_bridge_descriptors_known()) {
        log_fn(severity, LD_APP|LD_DIR,
               "Application request when we haven't used client functionality "
               "lately. Optimistically trying directory fetches again.");
        routerlist_retry_directory_downloads(time(NULL));
      }
    }
    /* the stream will be dealt with when router_have_minimum_dir_info becomes
     * 1, or when all directory attempts fail and directory_all_unreachable()
     * kills it.
     */
    return 0;
  }

  /* Do we need to check exit policy? */
  if (check_exit_policy) {
    if (!conn->chosen_exit_name) {
      struct in_addr in;
      uint32_t addr = 0;
      if (tor_inet_aton(conn->socks_request->address, &in))
        addr = ntohl(in.s_addr);
      if (router_exit_policy_all_routers_reject(addr,
                                                conn->socks_request->port,
                                                need_uptime)) {
        log_notice(LD_APP,
                   "No Tor server allows exit to %s:%d. Rejecting.",
                   safe_str_client(conn->socks_request->address),
                   conn->socks_request->port);
        return -1;
      }
    } else {
      /* XXXX023 Duplicates checks in connection_ap_handshake_attach_circuit:
       * refactor into a single function? */
      routerinfo_t *router = router_get_by_nickname(conn->chosen_exit_name, 1);
      int opt = conn->chosen_exit_optional;
      if (router && !connection_ap_can_use_exit(conn, router)) {
        log_fn(opt ? LOG_INFO : LOG_WARN, LD_APP,
               "Requested exit point '%s' is excluded or "
               "would refuse request. %s.",
               conn->chosen_exit_name, opt ? "Trying others" : "Closing");
        if (opt) {
          conn->chosen_exit_optional = 0;
          tor_free(conn->chosen_exit_name);
          /* Try again. */
          return circuit_get_open_circ_or_launch(conn,
                                                 desired_circuit_purpose,
                                                 circp);
        }
        return -1;
      }
    }
  }

  /* is one already on the way? */
  circ = circuit_get_best(conn, 0, desired_circuit_purpose,
                          need_uptime, need_internal);
  if (circ)
    log_debug(LD_CIRC, "one on the way!");
  if (!circ) {
    extend_info_t *extend_info=NULL;
    uint8_t new_circ_purpose;

    if (desired_circuit_purpose == CIRCUIT_PURPOSE_C_INTRODUCE_ACK_WAIT) {
      /* need to pick an intro point */
      tor_assert(conn->rend_data);
      extend_info = rend_client_get_random_intro(conn->rend_data);
      if (!extend_info) {
        log_info(LD_REND,
                 "No intro points for '%s': re-fetching service descriptor.",
                 safe_str_client(conn->rend_data->onion_address));
        rend_client_refetch_v2_renddesc(conn->rend_data);
        conn->_base.state = AP_CONN_STATE_RENDDESC_WAIT;
        return 0;
      }
      log_info(LD_REND,"Chose %s as intro point for '%s'.",
               extend_info_describe(extend_info),
               safe_str_client(conn->rend_data->onion_address));
    }

    /* If we have specified a particular exit node for our
     * connection, then be sure to open a circuit to that exit node.
     */
    if (desired_circuit_purpose == CIRCUIT_PURPOSE_C_GENERAL) {
      if (conn->chosen_exit_name) {
        routerinfo_t *r;
        int opt = conn->chosen_exit_optional;
        r = router_get_by_nickname(conn->chosen_exit_name, 1);
        if (r) {
          extend_info = extend_info_from_router(r);
        } else {
          log_debug(LD_DIR, "considering %d, %s",
                    want_onehop, conn->chosen_exit_name);
          if (want_onehop && conn->chosen_exit_name[0] == '$') {
            /* We're asking for a one-hop circuit to a router that
             * we don't have a routerinfo about. Make up an extend_info. */
            char digest[DIGEST_LEN];
            char *hexdigest = conn->chosen_exit_name+1;
            tor_addr_t addr;
            if (strlen(hexdigest) < HEX_DIGEST_LEN ||
                base16_decode(digest,DIGEST_LEN,hexdigest,HEX_DIGEST_LEN)<0) {
              log_info(LD_DIR, "Broken exit digest on tunnel conn. Closing.");
              return -1;
            }
            if (tor_addr_from_str(&addr, conn->socks_request->address) < 0) {
              log_info(LD_DIR, "Broken address %s on tunnel conn. Closing.",
                       escaped_safe_str_client(conn->socks_request->address));
              return -1;
            }
            extend_info = extend_info_alloc(conn->chosen_exit_name+1,
                                            digest, NULL, &addr,
                                            conn->socks_request->port);
          } else {
            /* We will need an onion key for the router, and we
             * don't have one. Refuse or relax requirements. */
            log_fn(opt ? LOG_INFO : LOG_WARN, LD_APP,
                   "Requested exit point '%s' is not known. %s.",
                   conn->chosen_exit_name, opt ? "Trying others" : "Closing");
            if (opt) {
              conn->chosen_exit_optional = 0;
              tor_free(conn->chosen_exit_name);
              /* Try again with no requested exit */
              return circuit_get_open_circ_or_launch(conn,
                                                     desired_circuit_purpose,
                                                     circp);
            }
            return -1;
          }
        }
      }
    }

    if (desired_circuit_purpose == CIRCUIT_PURPOSE_C_REND_JOINED)
      new_circ_purpose = CIRCUIT_PURPOSE_C_ESTABLISH_REND;
    else if (desired_circuit_purpose == CIRCUIT_PURPOSE_C_INTRODUCE_ACK_WAIT)
      new_circ_purpose = CIRCUIT_PURPOSE_C_INTRODUCING;
    else
      new_circ_purpose = desired_circuit_purpose;

    {
      int flags = CIRCLAUNCH_NEED_CAPACITY;
      if (want_onehop) flags |= CIRCLAUNCH_ONEHOP_TUNNEL;
      if (need_uptime) flags |= CIRCLAUNCH_NEED_UPTIME;
      if (need_internal) flags |= CIRCLAUNCH_IS_INTERNAL;
      circ = circuit_launch_by_extend_info(new_circ_purpose, extend_info,
                                           flags);
    }

    extend_info_free(extend_info);

    if (desired_circuit_purpose == CIRCUIT_PURPOSE_C_GENERAL) {
      /* We just caused a circuit to get built because of this stream.
       * If this stream has caused a _lot_ of circuits to be built, that's
       * a bad sign: we should tell the user. */
      if (conn->num_circuits_launched < NUM_CIRCUITS_LAUNCHED_THRESHOLD &&
          ++conn->num_circuits_launched == NUM_CIRCUITS_LAUNCHED_THRESHOLD)
        log_info(LD_CIRC, "The application request to %s:%d has launched "
                 "%d circuits without finding one it likes.",
                 escaped_safe_str_client(conn->socks_request->address),
                 conn->socks_request->port,
                 conn->num_circuits_launched);
    } else {
      /* help predict this next time */
      rep_hist_note_used_internal(time(NULL), need_uptime, 1);
      if (circ) {
        /* write the service_id into circ */
        circ->rend_data = rend_data_dup(conn->rend_data);
        if (circ->_base.purpose == CIRCUIT_PURPOSE_C_ESTABLISH_REND &&
            circ->_base.state == CIRCUIT_STATE_OPEN)
          rend_client_rendcirc_has_opened(circ);
      }
    }
  }
  if (!circ)
    log_info(LD_APP,
             "No safe circuit (purpose %d) ready for edge "
             "connection; delaying.",
             desired_circuit_purpose);
  *circp = circ;
  return 0;
}

/** Return true iff <b>crypt_path</b> is one of the crypt_paths for
 * <b>circ</b>. */
static int
cpath_is_on_circuit(origin_circuit_t *circ, crypt_path_t *crypt_path)
{
  crypt_path_t *cpath, *cpath_next = NULL;
  for (cpath = circ->cpath; cpath_next != circ->cpath; cpath = cpath_next) {
    cpath_next = cpath->next;
    if (crypt_path == cpath)
      return 1;
  }
  return 0;
}

/** Attach the AP stream <b>apconn</b> to circ's linked list of
 * p_streams. Also set apconn's cpath_layer to <b>cpath</b>, or to the last
 * hop in circ's cpath if <b>cpath</b> is NULL.
 */
static void
link_apconn_to_circ(edge_connection_t *apconn, origin_circuit_t *circ,
                    crypt_path_t *cpath)
{
  /* add it into the linked list of streams on this circuit */
  log_debug(LD_APP|LD_CIRC, "attaching new conn to circ. n_circ_id %d.",
            circ->_base.n_circ_id);
  /* reset it, so we can measure circ timeouts */
  apconn->_base.timestamp_lastread = time(NULL);
  apconn->next_stream = circ->p_streams;
  apconn->on_circuit = TO_CIRCUIT(circ);
  /* assert_connection_ok(conn, time(NULL)); */
  circ->p_streams = apconn;

  if (cpath) { /* we were given one; use it */
    tor_assert(cpath_is_on_circuit(circ, cpath));
    apconn->cpath_layer = cpath;
  } else { /* use the last hop in the circuit */
    tor_assert(circ->cpath);
    tor_assert(circ->cpath->prev);
    tor_assert(circ->cpath->prev->state == CPATH_STATE_OPEN);
    apconn->cpath_layer = circ->cpath->prev;
  }
}

/** Return true iff <b>address</b> is matched by one of the entries in
 * TrackHostExits. */
int
hostname_in_track_host_exits(or_options_t *options, const char *address)
{
  if (!options->TrackHostExits)
    return 0;
  SMARTLIST_FOREACH_BEGIN(options->TrackHostExits, const char *, cp) {
    if (cp[0] == '.') { /* match end */
      if (cp[1] == '\0' ||
          !strcasecmpend(address, cp) ||
          !strcasecmp(address, &cp[1]))
        return 1;
    } else if (strcasecmp(cp, address) == 0) {
      return 1;
    }
  } SMARTLIST_FOREACH_END(cp);
  return 0;
}

/** If an exit wasn't explicitly specified for <b>conn</b>, consider saving
 * the exit that we *did* choose for use by future connections to
 * <b>conn</b>'s destination.
 */
static void
consider_recording_trackhost(edge_connection_t *conn, origin_circuit_t *circ)
{
  or_options_t *options = get_options();
  char *new_address = NULL;
  char fp[HEX_DIGEST_LEN+1];

  /* Search the addressmap for this conn's destination. */
  /* If he's not in the address map.. */
  if (!options->TrackHostExits ||
      addressmap_have_mapping(conn->socks_request->address,
                              options->TrackHostExitsExpire))
    return; /* nothing to track, or already mapped */

  if (!hostname_in_track_host_exits(options, conn->socks_request->address) ||
      !circ->build_state->chosen_exit)
    return;

  /* write down the fingerprint of the chosen exit, not the nickname,
   * because the chosen exit might not be named. */
  base16_encode(fp, sizeof(fp),
                circ->build_state->chosen_exit->identity_digest, DIGEST_LEN);

  /* Add this exit/hostname pair to the addressmap. */
  tor_asprintf(&new_address, "%s.%s.exit",
               conn->socks_request->address, fp);

  addressmap_register(conn->socks_request->address, new_address,
                      time(NULL) + options->TrackHostExitsExpire,
                      ADDRMAPSRC_TRACKEXIT);
}

/** Attempt to attach the connection <b>conn</b> to <b>circ</b>, and send a
 * begin or resolve cell as appropriate.  Return values are as for
 * connection_ap_handshake_attach_circuit.  The stream will exit from the hop
 * indicated by <b>cpath</b>, or from the last hop in circ's cpath if
 * <b>cpath</b> is NULL. */
int
connection_ap_handshake_attach_chosen_circuit(edge_connection_t *conn,
                                              origin_circuit_t *circ,
                                              crypt_path_t *cpath)
{
  tor_assert(conn);
  tor_assert(conn->_base.state == AP_CONN_STATE_CIRCUIT_WAIT ||
             conn->_base.state == AP_CONN_STATE_CONTROLLER_WAIT);
  tor_assert(conn->socks_request);
  tor_assert(circ);
  tor_assert(circ->_base.state == CIRCUIT_STATE_OPEN);

  conn->_base.state = AP_CONN_STATE_CIRCUIT_WAIT;

  if (!circ->_base.timestamp_dirty)
    circ->_base.timestamp_dirty = time(NULL);

  link_apconn_to_circ(conn, circ, cpath);
  tor_assert(conn->socks_request);
  if (conn->socks_request->command == SOCKS_COMMAND_CONNECT) {
    if (!conn->use_begindir)
      consider_recording_trackhost(conn, circ);
    if (connection_ap_handshake_send_begin(conn) < 0)
      return -1;
  } else {
    if (connection_ap_handshake_send_resolve(conn) < 0)
      return -1;
  }

  return 1;
}

/** Try to find a safe live circuit for CONN_TYPE_AP connection conn. If
 * we don't find one: if conn cannot be handled by any known nodes,
 * warn and return -1 (conn needs to die, and is maybe already marked);
 * else launch new circuit (if necessary) and return 0.
 * Otherwise, associate conn with a safe live circuit, do the
 * right next step, and return 1.
 */
/* XXXX this function should mark for close whenever it returns -1;
 * its callers shouldn't have to worry about that. */
int
connection_ap_handshake_attach_circuit(edge_connection_t *conn)
{
  int retval;
  int conn_age;
  int want_onehop;

  tor_assert(conn);
  tor_assert(conn->_base.state == AP_CONN_STATE_CIRCUIT_WAIT);
  tor_assert(conn->socks_request);
  want_onehop = conn->want_onehop;

  conn_age = (int)(time(NULL) - conn->_base.timestamp_created);

  if (conn_age >= get_options()->SocksTimeout) {
    int severity = (tor_addr_is_null(&conn->_base.addr) && !conn->_base.port) ?
      LOG_INFO : LOG_NOTICE;
    log_fn(severity, LD_APP,
           "Tried for %d seconds to get a connection to %s:%d. Giving up.",
           conn_age, safe_str_client(conn->socks_request->address),
           conn->socks_request->port);
    return -1;
  }

  if (!connection_edge_is_rendezvous_stream(conn)) { /* we're a general conn */
    origin_circuit_t *circ=NULL;

    if (conn->chosen_exit_name) {
      routerinfo_t *router = router_get_by_nickname(conn->chosen_exit_name, 1);
      int opt = conn->chosen_exit_optional;
      if (!router && !want_onehop) {
        /* We ran into this warning when trying to extend a circuit to a
         * hidden service directory for which we didn't have a router
         * descriptor. See flyspray task 767 for more details. We should
         * keep this in mind when deciding to use BEGIN_DIR cells for other
         * directory requests as well. -KL*/
        log_fn(opt ? LOG_INFO : LOG_WARN, LD_APP,
               "Requested exit point '%s' is not known. %s.",
               conn->chosen_exit_name, opt ? "Trying others" : "Closing");
        if (opt) {
          conn->chosen_exit_optional = 0;
          tor_free(conn->chosen_exit_name);
          return 0;
        }
        return -1;
      }
      if (router && !connection_ap_can_use_exit(conn, router)) {
        log_fn(opt ? LOG_INFO : LOG_WARN, LD_APP,
               "Requested exit point '%s' is excluded or "
               "would refuse request. %s.",
               conn->chosen_exit_name, opt ? "Trying others" : "Closing");
        if (opt) {
          conn->chosen_exit_optional = 0;
          tor_free(conn->chosen_exit_name);
          return 0;
        }
        return -1;
      }
    }

    /* find the circuit that we should use, if there is one. */
    retval = circuit_get_open_circ_or_launch(
        conn, CIRCUIT_PURPOSE_C_GENERAL, &circ);
    if (retval < 1) // XXX022 if we totally fail, this still returns 0 -RD
      return retval;

    log_debug(LD_APP|LD_CIRC,
              "Attaching apconn to circ %d (stream %d sec old).",
              circ->_base.n_circ_id, conn_age);
    /* print the circ's path, so people can figure out which circs are
     * sucking. */
    circuit_log_path(LOG_INFO,LD_APP|LD_CIRC,circ);

    /* We have found a suitable circuit for our conn. Hurray. */
    return connection_ap_handshake_attach_chosen_circuit(conn, circ, NULL);

  } else { /* we're a rendezvous conn */
    origin_circuit_t *rendcirc=NULL, *introcirc=NULL;

    tor_assert(!conn->cpath_layer);

    /* start by finding a rendezvous circuit for us */

    retval = circuit_get_open_circ_or_launch(
       conn, CIRCUIT_PURPOSE_C_REND_JOINED, &rendcirc);
    if (retval < 0) return -1; /* failed */

    if (retval > 0) {
      tor_assert(rendcirc);
      /* one is already established, attach */
      log_info(LD_REND,
               "rend joined circ %d already here. attaching. "
               "(stream %d sec old)",
               rendcirc->_base.n_circ_id, conn_age);
      /* Mark rendezvous circuits as 'newly dirty' every time you use
       * them, since the process of rebuilding a rendezvous circ is so
       * expensive. There is a tradeoff between linkability and
       * feasibility, at this point.
       */
      rendcirc->_base.timestamp_dirty = time(NULL);
      link_apconn_to_circ(conn, rendcirc, NULL);
      if (connection_ap_handshake_send_begin(conn) < 0)
        return 0; /* already marked, let them fade away */
      return 1;
    }

    if (rendcirc && (rendcirc->_base.purpose ==
                     CIRCUIT_PURPOSE_C_REND_READY_INTRO_ACKED)) {
      log_info(LD_REND,
               "pending-join circ %d already here, with intro ack. "
               "Stalling. (stream %d sec old)",
                rendcirc->_base.n_circ_id, conn_age);
      return 0;
    }

    /* it's on its way. find an intro circ. */
    retval = circuit_get_open_circ_or_launch(
      conn, CIRCUIT_PURPOSE_C_INTRODUCE_ACK_WAIT, &introcirc);
    if (retval < 0) return -1; /* failed */

    if (retval > 0) {
      /* one has already sent the intro. keep waiting. */
      circuit_t *c = NULL;
      tor_assert(introcirc);
      log_info(LD_REND, "Intro circ %d present and awaiting ack (rend %d). "
               "Stalling. (stream %d sec old)",
               introcirc->_base.n_circ_id,
               rendcirc ? rendcirc->_base.n_circ_id : 0,
               conn_age);
      /* abort parallel intro circs, if any */
      for (c = global_circuitlist; c; c = c->next) {
        if (c->purpose == CIRCUIT_PURPOSE_C_INTRODUCING &&
            !c->marked_for_close && CIRCUIT_IS_ORIGIN(c)) {
          origin_circuit_t *oc = TO_ORIGIN_CIRCUIT(c);
          if (oc->rend_data &&
              !rend_cmp_service_ids(conn->rend_data->onion_address,
                                    oc->rend_data->onion_address)) {
            log_info(LD_REND|LD_CIRC, "Closing introduction circuit that we "
                     "built in parallel.");
            circuit_mark_for_close(c, END_CIRC_REASON_TIMEOUT);
          }
        }
      }
      return 0;
    }

    /* now rendcirc and introcirc are each either undefined or not finished */

    if (rendcirc && introcirc &&
        rendcirc->_base.purpose == CIRCUIT_PURPOSE_C_REND_READY) {
      log_info(LD_REND,
               "ready rend circ %d already here (no intro-ack yet on "
               "intro %d). (stream %d sec old)",
               rendcirc->_base.n_circ_id,
               introcirc->_base.n_circ_id, conn_age);

      tor_assert(introcirc->_base.purpose == CIRCUIT_PURPOSE_C_INTRODUCING);
      if (introcirc->_base.state == CIRCUIT_STATE_OPEN) {
        log_info(LD_REND,"found open intro circ %d (rend %d); sending "
                 "introduction. (stream %d sec old)",
                 introcirc->_base.n_circ_id, rendcirc->_base.n_circ_id,
                 conn_age);
        switch (rend_client_send_introduction(introcirc, rendcirc)) {
        case 0: /* success */
          rendcirc->_base.timestamp_dirty = time(NULL);
          introcirc->_base.timestamp_dirty = time(NULL);
          assert_circuit_ok(TO_CIRCUIT(rendcirc));
          assert_circuit_ok(TO_CIRCUIT(introcirc));
          return 0;
        case -1: /* transient error */
          return 0;
        case -2: /* permanent error */
          return -1;
        default: /* oops */
          tor_fragile_assert();
          return -1;
        }
      }
    }

    log_info(LD_REND, "Intro (%d) and rend (%d) circs are not both ready. "
             "Stalling conn. (%d sec old)",
             introcirc ? introcirc->_base.n_circ_id : 0,
             rendcirc ? rendcirc->_base.n_circ_id : 0, conn_age);
    return 0;
  }
}

