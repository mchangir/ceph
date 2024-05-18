// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#include "Finisher.h"

#define dout_subsys ceph_subsys_finisher
#undef dout_prefix
#define dout_prefix *_dout << "finisher(" << this << ") "

void Finisher::start()
{
  ldout(cct, 10) << __func__ << dendl;
  finisher_thread.create(thread_name.c_str());
}

void Finisher::stop()
{
  ldout(cct, 10) << __func__ << dendl;
  finisher_lock.lock();
  finisher_stop = true;
  // we don't have any new work to do, but we want the worker to wake up anyway
  // to process the stop condition.
  finisher_cond.notify_all();
  finisher_lock.unlock();
  finisher_thread.join(); // wait until the worker exits completely
  ldout(cct, 10) << __func__ << " finish" << dendl;
}

void Finisher::wait_for_empty()
{
  std::unique_lock ul(finisher_lock);
  while (!finisher_queue.empty() || finisher_running) {
    ldout(cct, 10) << "wait_for_empty waiting" << dendl;
    finisher_empty_wait = true;
    finisher_empty_cond.wait(ul);
  }
  ldout(cct, 10) << "wait_for_empty empty" << dendl;
  finisher_empty_wait = false;
}

bool Finisher::is_empty()
{
  std::unique_lock ul(finisher_lock);
  return finisher_queue.empty();
}

void Finisher::queue(Context *c, int r /*= 0*/) {
  ldout(cct, 10) << "Finisher::queue(" << thread_name << ") locking finisher_lock" << dendl;
  std::unique_lock ul(finisher_lock);
  ldout(cct, 10) << "Finisher::queue(" << thread_name << ") locked finisher_lock" << dendl;
  bool was_empty = finisher_queue.empty();
  ldout(cct, 10) << "Finisher::queue(" << thread_name << ") finisher_queue was_empty:" << (was_empty ? "yes" : "no") << dendl;
  finisher_queue.push_back(std::make_pair(c, r));
  ldout(cct, 10) << "Finisher::queue(" << thread_name << ") finisher_queue:" << finisher_queue << dendl;
  if (was_empty) {
    ldout(cct, 10) << "Finisher::queue(" << thread_name << ") notifying to finisher_cond" << finisher_queue << dendl;
    finisher_cond.notify_one();
  } else {
    ldout(cct, 10) << "Finisher::queue(" << thread_name << ") not notifying to finisher_cond" << finisher_queue << dendl;
  }
  ldout(cct, 10) << "Finisher::queue(" << thread_name << ") notify done to finisher_cond" << finisher_queue << dendl;
  if (logger)
    logger->inc(l_finisher_queue_len);
}

void *Finisher::finisher_thread_entry()
{
  std::unique_lock ul(finisher_lock);
  ldout(cct, 10) << "finisher_thread(" << thread_name << ") start" << dendl;

  utime_t start;
  uint64_t count = 0;
  while (!finisher_stop) {
    /// Every time we are woken up, we process the queue until it is empty.
    while (!finisher_queue.empty()) {
      // To reduce lock contention, we swap out the queue to process.
      // This way other threads can submit new contexts to complete
      // while we are working.
      in_progress_queue.swap(finisher_queue);
      finisher_running = true;
      ul.unlock();
      ldout(cct, 10) << "finisher_thread(" << thread_name << ") doing " << in_progress_queue << dendl;

      if (logger) {
	start = ceph_clock_now();
	count = in_progress_queue.size();
      }

      // Now actually process the contexts.
      for (auto p : in_progress_queue) {
	ldout(cct, 10) << "finisher_thread(" << thread_name << ") completing context:"
		       << p.first << " with r:" << p.second
		       << dendl;

	p.first->complete(p.second);

	ldout(cct, 10) << "finisher_thread(" << thread_name << ") context completed"
		       << dendl;
      }
      ldout(cct, 10) << "finisher_thread(" << thread_name << ") done with " << in_progress_queue
                     << dendl;
      in_progress_queue.clear();
      if (logger) {
	logger->dec(l_finisher_queue_len, count);
	logger->tinc(l_finisher_complete_lat, ceph_clock_now() - start);
      }

      ul.lock();
      finisher_running = false;
    }
    ldout(cct, 10) << "finisher_thread(" << thread_name << ") empty" << dendl;
    if (unlikely(finisher_empty_wait))
      finisher_empty_cond.notify_all();
    if (finisher_stop)
      break;
    
    ldout(cct, 10) << "finisher_thread(" << thread_name << ") sleeping" << dendl;
    finisher_cond.wait(ul);
  }
  // If we are exiting, we signal the thread waiting in stop(),
  // otherwise it would never unblock
  finisher_empty_cond.notify_all();

  ldout(cct, 10) << "finisher_thread(" << thread_name << ") stop" << dendl;
  finisher_stop = false;
  return 0;
}

