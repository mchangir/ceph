// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab
#include "./osd_scrub_sched.h"

#include <string_view>
#include "osd/OSD.h"

#include "pg_scrubber.h"

using namespace ::std::chrono;
using namespace ::std::chrono_literals;
using namespace ::std::literals;

using must_scrub_t = Scrub::must_scrub_t;
using ScrubQContainer = Scrub::ScrubQContainer;
using sched_params_t = Scrub::sched_params_t;
using OSDRestrictions = Scrub::OSDRestrictions;
using ScrubJob = Scrub::ScrubJob;



// ////////////////////////////////////////////////////////////////////////// //
// ScrubQueue

#define dout_subsys ceph_subsys_osd
#undef dout_context
#define dout_context (cct)
#undef dout_prefix
#define dout_prefix _prefix_fn(_dout, this, __func__)

template <class T>
static std::ostream& _prefix_fn(std::ostream* _dout, T* t, std::string fn = "")
{
  return t->gen_prefix(*_dout, fn);
}

ScrubQueue::ScrubQueue(CephContext* cct, Scrub::ScrubSchedListener& osds)
    : cct{cct}
    , osd_service{osds}
{}

std::ostream& ScrubQueue::gen_prefix(std::ostream& out, std::string_view fn)
    const
{
  return out << fmt::format(
	     "osd.{} scrub-queue:{}: ", osd_service.get_nodeid(), fn);
}

/*
 * Remove the scrub job from the OSD scrub queue.
 * Caller should mark the Scrubber-owned job as 'not_registered'.
 */
void ScrubQueue::remove_from_osd_queue(spg_t pgid)
{
  dout(10) << fmt::format(
		  "removing pg[{}] from OSD scrub queue", pgid)
	   << dendl;

  std::unique_lock lck{jobs_lock};
  std::erase_if(to_scrub, [pgid](const auto& job) {
    return job->pgid == pgid;
  });
}


void ScrubQueue::enqueue_target(const Scrub::ScrubJob& sjob)
{
  std::unique_lock lck{jobs_lock};
  // the costly copying is only for this stage
  to_scrub.push_back(std::make_unique<ScrubJob>(sjob));
}


std::unique_ptr<ScrubJob> ScrubQueue::pop_ready_pg(
    OSDRestrictions restrictions,  // note: 4B in size! (thus - copy)
    utime_t time_now)
{
  std::unique_lock lck{jobs_lock};

  const auto eligible_filtr = [time_now, rst = restrictions](
				  const std::unique_ptr<ScrubJob>& jb) -> bool {
    // look for jobs that have their n.b. in the past, and are not
    // blocked by restrictions
    return jb->get_sched_time() <= time_now &&
	   (jb->high_priority ||
	    (!rst.high_priority_only &&
	     (!rst.only_deadlined || (!jb->schedule.deadline.is_zero() &&
				      jb->schedule.deadline <= time_now))));
  };

  auto not_ripes =
      std::partition(to_scrub.begin(), to_scrub.end(), eligible_filtr);
  if (not_ripes == to_scrub.begin()) {
    return nullptr;
  }
  auto top = std::min_element(
      to_scrub.begin(), not_ripes,
      [](const std::unique_ptr<ScrubJob>& lhs,
	 const std::unique_ptr<ScrubJob>& rhs) -> bool {
	return lhs->get_sched_time() < rhs->get_sched_time();
      });

  if (top == not_ripes) {
    return nullptr;
  }

  auto top_job = std::move(*top);
  to_scrub.erase(top);
  return top_job;
}


namespace {
struct cmp_time_n_priority_t {
  bool operator()(const Scrub::ScrubJob& lhs, const Scrub::ScrubJob& rhs)
      const
  {
    return lhs.is_high_priority() > rhs.is_high_priority() ||
	   (lhs.is_high_priority() == rhs.is_high_priority() &&
	    lhs.schedule.scheduled_at < rhs.schedule.scheduled_at);
  }
};
}  // namespace


/**
 * the set of all PGs named by the entries in the queue (but only those
 * entries that satisfy the predicate)
 */
std::set<spg_t> ScrubQueue::get_pgs(const ScrubQueue::EntryPred& cond) const
{
  std::lock_guard lck{jobs_lock};
  std::set<spg_t> pgs_w_matching_entries;
  for (const auto& job : to_scrub) {
    if (cond(*job)) {
      pgs_w_matching_entries.insert(job->pgid);
    }
  }
  return pgs_w_matching_entries;
}


void ScrubQueue::for_each_job(
    std::function<void(const Scrub::ScrubJob&)> fn,
    int max_jobs) const
{
  std::lock_guard lck(jobs_lock);
  // implementation note: not using 'for_each_n()', as it is UB
  // if there aren't enough elements
  std::for_each(
      to_scrub.begin(),
      to_scrub.begin() + std::min(max_jobs, static_cast<int>(to_scrub.size())),
      [fn](const auto& job) { fn(*job); });
}


void ScrubQueue::dump_scrubs(ceph::Formatter* f) const
{
  ceph_assert(f != nullptr);
  f->open_array_section("scrubs");
  for_each_job(
      [&f](const Scrub::ScrubJob& j) { j.dump(f); },
      std::numeric_limits<int>::max());
  f->close_section();
}


// ////////////////////////////////////////////////////////////////////////// //
// ScrubQueue - maintaining the 'blocked on a locked object' count

void ScrubQueue::clear_pg_scrub_blocked(spg_t blocked_pg)
{
  dout(5) << fmt::format(": pg {} is unblocked", blocked_pg) << dendl;
  --blocked_scrubs_cnt;
  ceph_assert(blocked_scrubs_cnt >= 0);
}

void ScrubQueue::mark_pg_scrub_blocked(spg_t blocked_pg)
{
  dout(5) << fmt::format(": pg {} is blocked on an object", blocked_pg)
	  << dendl;
  ++blocked_scrubs_cnt;
}

int ScrubQueue::get_blocked_pgs_count() const
{
  return blocked_scrubs_cnt;
}
