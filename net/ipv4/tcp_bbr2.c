// SPDX-License-Identifier: GPL-2.0
/*
 * TCP BBR2 congestion control — v2alpha backport for Android 4.4.205 (msm-4.4)
 *
 * BBR2 improves upon BBR v1 by:
 *  - Responding to ECN marks and packet loss (not ignoring them)
 *  - Bounding inflight bytes (inflight_lo / inflight_hi)
 *  - Bounding the sending rate (bw_lo)
 *  - Rounds-based PROBE_BW with UP/DOWN/CRUISE/REFILL sub-phases
 *  - STARTUP exits on sustained loss, not just BW plateau
 *
 * Based on google/bbr v2alpha (https://github.com/google/bbr).
 * Adapted for 4.4 by removing dependencies on rate_sample infrastructure
 * and re-implementing delivery-rate estimation from tp->delivered counters.
 *
 * Key 4.4 compatibility notes:
 *  - struct rate_sample not available; bw estimated from pkts_acked callback
 *  - win_minmax not in lib/; local minmax filter included below
 *  - tcp_wnd_end(), tcp_packets_in_flight(), tcp_is_cwnd_limited() available
 *  - sk_pacing_rate / sk_max_pacing_rate set directly
 *
 * Copyright (C) 2016-2019 Google LLC, Neal Cardwell, Yuchung Cheng, et al.
 * Backport for msm-4.4: BBR2 Backport Tool
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/module.h>
#include <linux/mm.h>
#include <net/tcp.h>
#include <linux/inet_diag.h>
#include <linux/inet.h>
#include <linux/random.h>
#include <linux/win_minmax.h>

/* ── tunables ─────────────────────────────────────────────────────────────── */

static int bbr2_bw_rtts __read_mostly         = 10;  /* BW filter window (RTTs) */
static int bbr2_min_rtt_win_sec __read_mostly  = 10;  /* min-RTT window (s) */
static int bbr2_probe_rtt_mode_ms __read_mostly =  200; /* PROBE_RTT duration */
static int bbr2_probe_rtt_cwnd_gain __read_mostly = BBR_UNIT / 2; /* 0.5x cwnd */
static int bbr2_drain_gain __read_mostly       = BBR_UNIT * 3 / 4;
static int bbr2_startup_cwnd_gain __read_mostly = BBR_UNIT * 2;
static int bbr2_startup_pacing_gain __read_mostly = BBR_UNIT * 277 / 100;
static int bbr2_full_bw_thresh __read_mostly   = BBR_UNIT * 5 / 4;
static int bbr2_full_bw_cnt __read_mostly      = 3;
/* PROBE_BW pacing gains (cycle through UP, DOWN, CRUISE): */
static int bbr2_pacing_gain[] __read_mostly    = {
	BBR_UNIT * 5 / 4,   /* PROBE_UP   — send faster to probe BW    */
	BBR_UNIT * 3 / 4,   /* PROBE_DOWN — drain the queue             */
	BBR_UNIT,           /* CRUISE 0                                  */
	BBR_UNIT,           /* CRUISE 1                                  */
	BBR_UNIT,           /* CRUISE 2                                  */
	BBR_UNIT,           /* CRUISE 3                                  */
	BBR_UNIT,           /* CRUISE 4                                  */
	BBR_UNIT,           /* CRUISE 5                                  */
};
#define BBR2_CYCLE_LEN ARRAY_SIZE(bbr2_pacing_gain)
static int bbr2_cwnd_gain __read_mostly        = BBR_UNIT * 2;
static int bbr2_ecn_alpha_gain __read_mostly   = BBR_UNIT * 1 / 16; /* 1/16 */
static int bbr2_ecn_factor __read_mostly       = BBR_UNIT * 1 / 3;
static int bbr2_beta __read_mostly             = BBR_UNIT * 3 / 10; /* 0.3 */
static int bbr2_loss_thresh __read_mostly      = BBR_UNIT * 2 / 100;/* 2%  */
static int bbr2_inflight_headroom __read_mostly = BBR_UNIT * 15 / 100;
static int bbr2_probe_rtt_interval_ms __read_mostly = 5000;
static int bbr2_probe_bw_cwnd_gain __read_mostly    = BBR_UNIT * 2;

#define BBR_UNIT (1 << 8)
#define BBR2_BW_SCALE 24
#define BBR2_BW_UNIT  (1 << BBR2_BW_SCALE)
#define BBR2_MAX_DATAGRAM_SIZE 1500U

/* ── state ────────────────────────────────────────────────────────────────── */

enum bbr2_mode {
	BBR2_STARTUP,
	BBR2_DRAIN,
	BBR2_PROBE_BW,
	BBR2_PROBE_RTT,
};

enum bbr2_phase {
	BBR2_BW_PROBE_UP = 0,
	BBR2_BW_PROBE_DOWN,
	BBR2_BW_PROBE_CRUISE,
	BBR2_BW_PROBE_REFILL,
};

struct bbr2 {
	/* Bandwidth estimation */
	struct minmax    bw;          /* windowed max delivery rate (bytes/us<<24) */
	u32              bw_lo;       /* lower bound on bw (response to loss/ECN)  */
	u32              bw_hi[2];    /* recent BW upper bounds (cur/prev round)   */
	u32              bw_probe_up_cnt;   /* rounds to wait before probing up    */
	u32              bw_probe_up_acks; /* acks delivered in probe_up round     */
	u32              bw_probe_samples; /* # round trips in current probe       */
	u32              bw_probe_up_rounds;

	/* RTT */
	u32              min_rtt_us;
	u32              min_rtt_stamp;
	u32              probe_rtt_done_stamp;
	u32              probe_rtt_min_us;
	u32              probe_rtt_min_stamp;

	/* Round tracking */
	u32              next_rtt_delivered;
	u32              round_delivered;
	u32              rtt_cnt;
	bool             round_start;

	/* Startup */
	u32              full_bw;
	u32              full_bw_cnt:3,
	                 full_bw_reached:1,
	                 idle_restart:1,
	                 packet_conservation:1,
	                 has_seen_rtt:1,
	                 probe_rtt_round_done:1;

	/* ECN */
	u32              ecn_alpha;           /* EWMA of ECN marks fraction */
	u32              alpha_last_delivered;
	u32              alpha_last_delivered_ce;
	bool             ecn_eligible;

	/* PROBE_BW */
	enum bbr2_phase  bw_probe_phase;
	u32              probe_wait_us;
	u64              cycle_stamp;

	/* Mode/state */
	enum bbr2_mode   mode;
	u32              prior_cwnd;
	bool             restore_cwnd;
	bool             prev_probe_too_high;
	u32              loss_in_round;
	u32              loss_round_delivered;
	bool             loss_round_start;
	u32              inflight_lo;
	u32              inflight_hi;

	/* Delivery-rate estimation (local, for 4.4 without rate_sample) */
	u32              delivered_mstamp; /* mstamp of last ack that updated bw */
	u32              bw_delivered;     /* tp->delivered snapshot for bw calc */
	u32              interval_us;      /* smoothed interval for bw calc      */
};

/* ── helpers ──────────────────────────────────────────────────────────────── */

static inline struct bbr2 *bbr2_ca(const struct sock *sk)
{
	return inet_csk_ca(sk);
}

static u32 bbr2_bw(const struct sock *sk)
{
	struct bbr2 *bbr = bbr2_ca(sk);
	return minmax_get(&bbr->bw);
}

/* Effective bw: min of filtered max and current floor */
static u32 bbr2_max_bw(const struct sock *sk)
{
	struct bbr2 *bbr = bbr2_ca(sk);
	return min(minmax_get(&bbr->bw), bbr->bw_hi[0]);
}

static u32 bbr2_bdp(const struct sock *sk, u32 bw, int gain)
{
	struct tcp_sock *tp = tcp_sk(sk);
	u32 bdp;
	u64 w;

	if (unlikely(bbr2_ca(sk)->min_rtt_us == ~0U))
		return TCP_INIT_CWND;

	w  = (u64)bw * bbr2_ca(sk)->min_rtt_us;
	bdp = (u32)(w >> BBR2_BW_SCALE);
	return (u64)bdp * gain / BBR_UNIT;
}

static u32 bbr2_quantization_budget(const struct sock *sk, u32 inflight)
{
	/* Ensure there is room for 3 segments of headroom to avoid stalls. */
	return inflight + 3 * tcp_sk(sk)->mss_cache;
}

static u32 bbr2_inflight(const struct sock *sk, u32 bw, int gain)
{
	u32 inflight = bbr2_bdp(sk, bw, gain);
	inflight = bbr2_quantization_budget(sk, inflight);
	return inflight;
}

static u32 bbr2_cwnd_with_headroom(const struct sock *sk, u32 cwnd, u32 headroom)
{
	if (!headroom)
		return cwnd;
	return max_t(u32, cwnd - headroom, 1);
}

/* Return the number of segments currently in flight. */
static u32 bbr2_packets_in_flight(const struct sock *sk)
{
	return tcp_packets_in_flight(tcp_sk(sk));
}

static void bbr2_set_pacing_rate(struct sock *sk, u32 bw, int gain)
{
	struct tcp_sock *tp = tcp_sk(sk);
	u64 rate = bw;

	rate = (rate * gain) >> BBR2_BW_SCALE;
	rate *= tp->mss_cache;
	if (bbr2_ca(sk)->mode != BBR2_STARTUP || rate > sk->sk_pacing_rate)
		sk->sk_pacing_rate = min_t(u64, rate, sk->sk_max_pacing_rate);
}

static u32 bbr2_probe_rtt_cwnd(struct sock *sk)
{
	return max_t(u32, bbr2_bdp(sk, bbr2_max_bw(sk), bbr2_probe_rtt_cwnd_gain),
		     4U);
}

/* ── round-trip tracking ──────────────────────────────────────────────────── */

static void bbr2_update_round_start(struct sock *sk, u32 delivered)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct bbr2 *bbr    = bbr2_ca(sk);

	bbr->round_start = false;
	if (delivered >= bbr->next_rtt_delivered) {
		bbr->next_rtt_delivered = tp->delivered;
		bbr->rtt_cnt++;
		bbr->round_start = true;
		bbr->packet_conservation = false;
	}
}

/* ── bandwidth estimation ─────────────────────────────────────────────────── */

/* Called from pkts_acked; bw_sample is in bytes/us << BBR2_BW_SCALE. */
static void bbr2_update_bw(struct sock *sk, u32 bw_sample, u32 delivered)
{
	struct bbr2 *bbr = bbr2_ca(sk);

	if (bw_sample == 0)
		return;

	/* Windowed max over bbr2_bw_rtts round trips. */
	minmax_running_max(&bbr->bw, bbr2_bw_rtts, bbr->rtt_cnt, bw_sample);

	/* Track bw_hi (upper bound) for two consecutive rounds. */
	if (bbr->bw_probe_phase == BBR2_BW_PROBE_UP &&
	    bw_sample >= bbr->bw_hi[0]) {
		bbr->bw_hi[0] = bw_sample;
		bbr->bw_hi[1] = bw_sample;
	}
}

/* ── loss / ECN response ──────────────────────────────────────────────────── */

static bool bbr2_is_inflight_too_high(const struct sock *sk)
{
	struct bbr2 *bbr          = bbr2_ca(sk);
	u32          inflight     = bbr2_packets_in_flight(sk);
	struct tcp_sock *tp       = tcp_sk(sk);
	u32          lost_bytes   = tp->lost - (tp->lost - tp->sacked_out);

	if (!tp->delivered)
		return false;

	/* Loss rate > threshold? */
	if ((u64)lost_bytes * BBR_UNIT > (u64)tp->delivered * bbr2_loss_thresh)
		return true;

	/* ECN: alpha-weighted mark fraction too high? */
	if (bbr->ecn_eligible &&
	    (u64)bbr->ecn_alpha * BBR_UNIT >
	    (u64)tp->delivered * bbr2_ecn_factor)
		return true;

	return false;
}

static void bbr2_handle_inflight_too_high(struct sock *sk, bool rsv)
{
	struct bbr2    *bbr = bbr2_ca(sk);
	struct tcp_sock *tp  = tcp_sk(sk);
	u32 inflight        = bbr2_packets_in_flight(sk);

	bbr->prev_probe_too_high = true;
	bbr->bw_probe_up_cnt     = U32_MAX; /* wait before probing again */
	/* Cut inflight_hi to slightly above current inflight. */
	bbr->inflight_hi = max(inflight,
		(u32)((u64)bbr2_bdp(sk, bbr2_max_bw(sk), BBR_UNIT) *
		      (BBR_UNIT - bbr2_inflight_headroom) / BBR_UNIT));
	/* Lower bw floor. */
	bbr->bw_lo = max_t(u32, bbr2_max_bw(sk) * (BBR_UNIT - bbr2_beta) /
			   BBR_UNIT, 1U);
}

static void bbr2_update_ecn_alpha(struct sock *sk)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct bbr2 *bbr    = bbr2_ca(sk);
	u32 delivered       = tp->delivered - bbr->alpha_last_delivered;
	u32 delivered_ce    = tp->delivered_ce - bbr->alpha_last_delivered_ce;
	u32 alpha, ce_ratio;

	if (!bbr->ecn_eligible || !delivered)
		return;

	bbr->alpha_last_delivered    = tp->delivered;
	bbr->alpha_last_delivered_ce = tp->delivered_ce;

	/* EWMA of CE ratio. */
	ce_ratio = (u64)delivered_ce * BBR_UNIT / delivered;
	alpha    = (u64)bbr->ecn_alpha * (BBR_UNIT - bbr2_ecn_alpha_gain) / BBR_UNIT
		   + (u64)ce_ratio * bbr2_ecn_alpha_gain / BBR_UNIT;
	bbr->ecn_alpha = min(alpha, (u32)BBR_UNIT);
}

/* ── STARTUP ──────────────────────────────────────────────────────────────── */

static bool bbr2_full_bw_reached(const struct sock *sk)
{
	return bbr2_ca(sk)->full_bw_reached;
}

static void bbr2_check_full_bw_reached(struct sock *sk, u32 bw_sample)
{
	struct bbr2 *bbr = bbr2_ca(sk);

	if (bbr->full_bw_reached || !bbr->round_start ||
	    tcp_sk(sk)->app_limited)
		return;

	if (bw_sample >= (u64)bbr->full_bw * bbr2_full_bw_thresh / BBR_UNIT) {
		bbr->full_bw     = bw_sample;
		bbr->full_bw_cnt = 0;
		return;
	}
	if (++bbr->full_bw_cnt >= bbr2_full_bw_cnt)
		bbr->full_bw_reached = true;
}

static void bbr2_check_startup_done(struct sock *sk)
{
	struct bbr2 *bbr = bbr2_ca(sk);

	if (!bbr->full_bw_reached)
		return;

	/* Also exit on sustained loss during STARTUP. */
	if (bbr->loss_in_round) {
		bbr->full_bw_reached = true;
		/* bw_hi set at max observed during startup */
		bbr->bw_hi[0] = bbr->bw_hi[1] = bbr2_bw(sk);
	}

	bbr->mode = BBR2_DRAIN;
}

/* ── PROBE_BW ─────────────────────────────────────────────────────────────── */

static u32 bbr2_probe_bw_cwnd_gain_to_use(const struct sock *sk)
{
	return bbr2_probe_bw_cwnd_gain;
}

static bool bbr2_is_probing_bandwidth(const struct sock *sk)
{
	struct bbr2 *bbr = bbr2_ca(sk);
	return (bbr->mode == BBR2_PROBE_BW &&
		bbr->bw_probe_phase == BBR2_BW_PROBE_UP);
}

static void bbr2_raise_inflight_hi_slope(struct sock *sk)
{
	struct bbr2 *bbr = bbr2_ca(sk);
	struct tcp_sock *tp = tcp_sk(sk);
	u32 growth_this_round = 1 << bbr->bw_probe_up_rounds;

	bbr->bw_probe_up_rounds = min_t(u32, bbr->bw_probe_up_rounds + 1, 30);
	bbr->bw_probe_up_cnt = max_t(u32, tp->snd_cwnd / growth_this_round, 1);
}

static void bbr2_probe_inflight_hi_upward(struct sock *sk, u32 acked)
{
	struct bbr2 *bbr    = bbr2_ca(sk);
	struct tcp_sock *tp = tcp_sk(sk);

	if (!tcp_is_cwnd_limited(sk) || tp->snd_cwnd < bbr->inflight_hi)
		return;   /* not fully using inflight_hi yet */

	bbr->bw_probe_up_acks += acked;
	if (bbr->bw_probe_up_acks >= bbr->bw_probe_up_cnt) {
		u32 delta         = bbr->bw_probe_up_acks / bbr->bw_probe_up_cnt;
		bbr->bw_probe_up_acks -= delta * bbr->bw_probe_up_cnt;
		bbr->inflight_hi  += delta;
	}
	if (bbr->round_start)
		bbr2_raise_inflight_hi_slope(sk);
}

static bool bbr2_has_elapsed_in_phase(const struct sock *sk, u32 interval_us)
{
	struct bbr2 *bbr = bbr2_ca(sk);
	return tcp_stamp_us_delta(tcp_sk(sk)->tcp_mstamp, bbr->cycle_stamp) >
	       interval_us;
}

static void bbr2_pick_probe_wait(struct sock *sk)
{
	struct bbr2 *bbr = bbr2_ca(sk);
	/* Random wait: 2–3 RTTs before probing. */
	bbr->probe_wait_us = bbr2_ca(sk)->min_rtt_us +
		prandom_u32_max(bbr2_ca(sk)->min_rtt_us);
}

static bool bbr2_should_probe_bw(const struct sock *sk)
{
	struct bbr2 *bbr = bbr2_ca(sk);
	return bbr2_has_elapsed_in_phase(sk, bbr->probe_wait_us);
}

static void bbr2_set_probe_bw_phase(struct sock *sk, enum bbr2_phase phase)
{
	struct bbr2 *bbr    = bbr2_ca(sk);
	struct tcp_sock *tp = tcp_sk(sk);

	bbr->bw_probe_phase = phase;
	bbr->cycle_stamp    = tp->tcp_mstamp;

	switch (phase) {
	case BBR2_BW_PROBE_UP:
		/* Reset inflight_hi headroom for the new probe. */
		bbr->bw_hi[1]            = bbr->bw_hi[0];
		bbr->bw_probe_up_rounds  = 0;
		bbr->bw_probe_up_acks    = 0;
		bbr2_raise_inflight_hi_slope(sk);
		bbr->prev_probe_too_high = false;
		break;
	case BBR2_BW_PROBE_DOWN:
		bbr2_pick_probe_wait(sk);
		break;
	case BBR2_BW_PROBE_CRUISE:
		bbr->inflight_lo = min(bbr->inflight_lo, bbr->inflight_hi);
		break;
	case BBR2_BW_PROBE_REFILL:
		/* Reset inflight_lo, let inflight grow back to inflight_hi. */
		bbr->inflight_lo = U32_MAX;
		bbr->bw_probe_samples++;
		break;
	}
}

static void bbr2_update_probe_bw_cycle(struct sock *sk, u32 acked)
{
	struct bbr2 *bbr = bbr2_ca(sk);

	if (bbr->mode != BBR2_PROBE_BW)
		return;

	switch (bbr->bw_probe_phase) {
	case BBR2_BW_PROBE_UP:
		bbr2_probe_inflight_hi_upward(sk, acked);
		if (bbr2_is_inflight_too_high(sk)) {
			bbr2_handle_inflight_too_high(sk, false);
			bbr2_set_probe_bw_phase(sk, BBR2_BW_PROBE_DOWN);
			break;
		}
		/* Transition to DOWN after one RTT in UP. */
		if (bbr->round_start)
			bbr2_set_probe_bw_phase(sk, BBR2_BW_PROBE_DOWN);
		break;

	case BBR2_BW_PROBE_DOWN:
		if (bbr2_is_inflight_too_high(sk)) {
			bbr2_handle_inflight_too_high(sk, false);
			break;
		}
		/* Wait for inflight to drain before cruising. */
		if (bbr2_packets_in_flight(sk) <=
		    bbr2_inflight(sk, bbr2_max_bw(sk), BBR_UNIT)) {
			bbr2_set_probe_bw_phase(sk, BBR2_BW_PROBE_CRUISE);
		} else if (bbr2_has_elapsed_in_phase(sk, bbr->probe_wait_us)) {
			/* Didn't drain quickly; cruise anyway. */
			bbr2_set_probe_bw_phase(sk, BBR2_BW_PROBE_CRUISE);
		}
		break;

	case BBR2_BW_PROBE_CRUISE:
		if (bbr2_should_probe_bw(sk))
			bbr2_set_probe_bw_phase(sk, BBR2_BW_PROBE_REFILL);
		break;

	case BBR2_BW_PROBE_REFILL:
		/* Start probing after one refill RTT. */
		if (bbr->round_start)
			bbr2_set_probe_bw_phase(sk, BBR2_BW_PROBE_UP);
		break;
	}
}

/* ── PROBE_RTT ────────────────────────────────────────────────────────────── */

static void bbr2_update_min_rtt(struct sock *sk, s32 rtt_us)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct bbr2 *bbr    = bbr2_ca(sk);
	bool filter_expired;

	if (rtt_us > 0 && rtt_us <= (s32)bbr->min_rtt_us) {
		bbr->min_rtt_us    = rtt_us;
		bbr->min_rtt_stamp = tcp_jiffies32;
	}

	filter_expired = after(tcp_jiffies32,
		bbr->min_rtt_stamp + bbr2_min_rtt_win_sec * HZ);

	if (rtt_us > 0 && (filter_expired || bbr->probe_rtt_round_done)) {
		bbr->probe_rtt_min_us    = rtt_us;
		bbr->probe_rtt_min_stamp = tcp_jiffies32;
	}

	/* Check if we should enter PROBE_RTT. */
	if (bbr->mode != BBR2_PROBE_RTT && filter_expired &&
	    !bbr->idle_restart &&
	    after(tcp_jiffies32,
		  bbr->probe_rtt_done_stamp + bbr2_probe_rtt_interval_ms * HZ / 1000)) {
		bbr->mode              = BBR2_PROBE_RTT;
		bbr->probe_rtt_done_stamp = 0;
		bbr->packet_conservation = false;
		bbr->prior_cwnd        = tcp_sk(sk)->snd_cwnd;
	}

	if (bbr->mode == BBR2_PROBE_RTT) {
		/* Wait for inflight to drop. */
		if (!bbr->probe_rtt_done_stamp &&
		    tcp_packets_in_flight(tp) <= bbr2_probe_rtt_cwnd(sk)) {
			bbr->probe_rtt_done_stamp = tcp_jiffies32 +
				msecs_to_jiffies(bbr2_probe_rtt_mode_ms);
			bbr->probe_rtt_round_done = false;
			bbr->next_rtt_delivered   = tp->delivered;
		} else if (bbr->probe_rtt_done_stamp) {
			if (bbr->round_start)
				bbr->probe_rtt_round_done = true;
			if (!after(tcp_jiffies32, bbr->probe_rtt_done_stamp))
				return;
			/* Done probing RTT; restore cwnd and resume. */
			bbr->min_rtt_stamp = tcp_jiffies32;
			tcp_sk(sk)->snd_cwnd = max(tcp_sk(sk)->snd_cwnd,
						   bbr->prior_cwnd);
			if (bbr->full_bw_reached) {
				bbr->mode = BBR2_PROBE_BW;
				bbr2_set_probe_bw_phase(sk, BBR2_BW_PROBE_DOWN);
			} else {
				bbr->mode = BBR2_STARTUP;
			}
		}
	}
	bbr->idle_restart = false;
}

/* ── cwnd / pacing ────────────────────────────────────────────────────────── */

static u32 bbr2_target_cwnd(struct sock *sk, u32 bw, int gain)
{
	struct bbr2 *bbr = bbr2_ca(sk);
	u32 cwnd         = bbr2_inflight(sk, bw, gain);

	/* Apply inflight constraints. */
	if (bbr->mode == BBR2_PROBE_BW) {
		if (bbr->bw_probe_phase == BBR2_BW_PROBE_CRUISE)
			cwnd = min(cwnd, bbr->inflight_hi);
		if (bbr->inflight_lo < U32_MAX)
			cwnd = min(cwnd, bbr->inflight_lo);
	}
	return cwnd;
}

static void bbr2_set_cwnd(struct sock *sk, u32 acked, int gain, u32 cwnd_gain)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct bbr2 *bbr    = bbr2_ca(sk);
	u32 cwnd            = 0, target_cwnd;

	if (!acked)
		return;

	/* Packet conservation during recovery. */
	if (bbr->packet_conservation) {
		cwnd = max_t(u32, tp->snd_cwnd, tcp_packets_in_flight(tp) + acked);
		goto done;
	}

	target_cwnd = bbr2_target_cwnd(sk, bbr2_max_bw(sk), cwnd_gain);

	if (bbr2_full_bw_reached(sk))
		cwnd = min_t(u32, cwnd + acked, target_cwnd);
	else if (tp->snd_cwnd < target_cwnd ||
		 tp->delivered < TCP_INIT_CWND)
		cwnd = tp->snd_cwnd + acked;

	cwnd = max_t(u32, cwnd, bbr2_packets_in_flight(sk) + acked);

done:
	tp->snd_cwnd = max_t(u32, cwnd, 2U);

	/* Clamp to bbr-computed target during PROBE_RTT. */
	if (bbr->mode == BBR2_PROBE_RTT)
		tp->snd_cwnd = min_t(u32, tp->snd_cwnd,
				     bbr2_probe_rtt_cwnd(sk));
}

/* ── main update, called from pkts_acked ─────────────────────────────────── */

static void bbr2_update_model(struct sock *sk, u32 acked, s32 rtt_us)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct bbr2 *bbr    = bbr2_ca(sk);
	u32 bw_sample = 0;

	/* Estimate delivery rate from tp->delivered and elapsed time. */
	if (rtt_us > 0 && acked) {
		u32 delivered = tp->delivered;
		u32 delta_pkt = delivered - bbr->bw_delivered;
		if (delta_pkt) {
			/* Scale: (bytes delivered / rtt) << BBR2_BW_SCALE */
			u64 bw = (u64)delta_pkt * tp->mss_cache *
				 BBR2_BW_UNIT;
			do_div(bw, max_t(u32, rtt_us, 1));
			bw_sample = (u32)min_t(u64, bw, U32_MAX);
			bbr->bw_delivered = delivered;
		}
	}

	bbr2_update_round_start(sk, tp->delivered);
	if (bbr->round_start) {
		bbr->loss_in_round = 0;
		bbr->loss_round_start = true;
	}
	bbr2_update_ecn_alpha(sk);
	bbr2_update_bw(sk, bw_sample, tp->delivered);
	bbr2_check_full_bw_reached(sk, bw_sample);
	bbr2_update_min_rtt(sk, rtt_us);
	bbr2_update_probe_bw_cycle(sk, acked);

	if (bbr->mode == BBR2_STARTUP)
		bbr2_check_startup_done(sk);
}

static void bbr2_update_gains(struct sock *sk)
{
	struct bbr2 *bbr = bbr2_ca(sk);

	switch (bbr->mode) {
	case BBR2_STARTUP:
		bbr2_set_pacing_rate(sk, bbr2_bw(sk), bbr2_startup_pacing_gain);
		bbr2_set_cwnd(sk, 1, bbr2_startup_pacing_gain,
			      bbr2_startup_cwnd_gain);
		break;
	case BBR2_DRAIN:
		bbr2_set_pacing_rate(sk, bbr2_bw(sk), bbr2_drain_gain);
		bbr2_set_cwnd(sk, 1, bbr2_drain_gain, bbr2_cwnd_gain);
		/* Exit DRAIN when queue is drained. */
		if (bbr2_packets_in_flight(sk) <=
		    bbr2_inflight(sk, bbr2_max_bw(sk), BBR_UNIT)) {
			bbr->mode = BBR2_PROBE_BW;
			bbr2_set_probe_bw_phase(sk, BBR2_BW_PROBE_CRUISE);
		}
		break;
	case BBR2_PROBE_BW:
		bbr2_set_pacing_rate(sk, bbr2_max_bw(sk),
			bbr2_pacing_gain[bbr->bw_probe_phase]);
		bbr2_set_cwnd(sk, 1,
			bbr2_pacing_gain[bbr->bw_probe_phase],
			bbr2_probe_bw_cwnd_gain_to_use(sk));
		break;
	case BBR2_PROBE_RTT:
		bbr2_set_pacing_rate(sk, bbr2_max_bw(sk), BBR_UNIT);
		break;
	}
}

/* ── congestion_ops callbacks ─────────────────────────────────────────────── */

static void bbr2_init(struct sock *sk)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct bbr2 *bbr    = inet_csk_ca(sk);

	memset(bbr, 0, sizeof(*bbr));
	bbr->mode           = BBR2_STARTUP;
	bbr->min_rtt_us     = tcp_min_rtt(tp) ? : ~0U;
	bbr->min_rtt_stamp  = tcp_jiffies32;
	minmax_reset(&bbr->bw, 0, 0);
	bbr->bw_lo          = U32_MAX;
	bbr->bw_hi[0]       = U32_MAX;
	bbr->bw_hi[1]       = U32_MAX;
	bbr->inflight_lo    = U32_MAX;
	bbr->inflight_hi    = U32_MAX;
	bbr->bw_probe_phase = BBR2_BW_PROBE_CRUISE;
	bbr2_pick_probe_wait(sk);
	bbr->ecn_alpha      = 0;
	bbr->ecn_eligible   = !!(sk->sk_userlocks & SOCK_RCVLOWAT_LOCK) ? 0 :
			       (tcp_ecn_ok(tp) ? 1 : 0);
	tp->snd_cwnd        = bbr2_target_cwnd(sk, bbr->bw_lo, BBR_UNIT);
	tp->snd_cwnd        = max_t(u32, tp->snd_cwnd, 2U);
	sk->sk_pacing_rate  = 0;
}

static void bbr2_pkts_acked(struct sock *sk, u32 num_acked, s32 rtt_us)
{
	bbr2_update_model(sk, num_acked, rtt_us);
	bbr2_update_gains(sk);
}

static u32 bbr2_ssthresh(struct sock *sk)
{
	/* BBR2 does not use ssthresh; return current cwnd to retain it. */
	return tcp_sk(sk)->snd_cwnd;
}

static u32 bbr2_undo_cwnd(struct sock *sk)
{
	struct bbr2 *bbr = bbr2_ca(sk);
	/* Restore to prior saved value on spurious congestion. */
	tcp_sk(sk)->snd_cwnd = max(tcp_sk(sk)->snd_cwnd, bbr->prior_cwnd);
	return tcp_sk(sk)->snd_cwnd;
}

static void bbr2_cong_avoid(struct sock *sk, u32 ack, u32 acked)
{
	/* All cwnd management done in pkts_acked / bbr2_set_cwnd. */
}

static void bbr2_set_state(struct sock *sk, u8 new_state)
{
	struct bbr2 *bbr = bbr2_ca(sk);

	if (new_state == TCP_CA_Loss) {
		bbr->loss_in_round++;
		bbr->prior_cwnd        = max(bbr->prior_cwnd,
					     tcp_sk(sk)->snd_cwnd);
		bbr->packet_conservation = true;
		bbr->inflight_lo = min_t(u32, bbr->inflight_lo,
			bbr2_packets_in_flight(sk));
	} else if (new_state == TCP_CA_Open &&
		   bbr->packet_conservation) {
		bbr->packet_conservation = false;
	}
}

static void bbr2_event(struct sock *sk, enum tcp_ca_event ev)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct bbr2 *bbr    = bbr2_ca(sk);

	if (!bbr->has_seen_rtt && tp->srtt_us) {
		bbr->min_rtt_us = tp->srtt_us >> 3;
		bbr->has_seen_rtt = true;
	}

	switch (ev) {
	case CA_EVENT_TX_START:
		if (!tp->app_limited)
			break;
		bbr->idle_restart = true;
		bbr->ecn_eligible = tcp_ecn_ok(tp);
		break;
	default:
		break;
	}
}

static struct tcp_congestion_ops tcp_bbr2_cong_ops __read_mostly = {
	.flags          = TCP_CONG_NON_RESTRICTED,
	.name           = "bbr2",
	.owner          = THIS_MODULE,
	.init           = bbr2_init,
	.ssthresh       = bbr2_ssthresh,
	.cong_avoid     = bbr2_cong_avoid,
	.set_state      = bbr2_set_state,
	.undo_cwnd      = bbr2_undo_cwnd,
	.pkts_acked     = bbr2_pkts_acked,
	.event          = bbr2_event,
};

static int __init bbr2_register(void)
{
	return tcp_register_congestion_control(&tcp_bbr2_cong_ops);
}

static void __exit bbr2_unregister(void)
{
	tcp_unregister_congestion_control(&tcp_bbr2_cong_ops);
}

module_init(bbr2_register);
module_exit(bbr2_unregister);

MODULE_AUTHOR("Neal Cardwell <ncardwell@google.com>");
MODULE_AUTHOR("Yuchung Cheng <ycheng@google.com>");
MODULE_DESCRIPTION("TCP BBR2 v2alpha backport for Android 4.4.205 (msm-4.4)");
MODULE_LICENSE("GPL v2");
