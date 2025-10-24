/*
 * Ceph - scalable distributed file system
 *
 * Copyright (C) 2025 IBM, Red Hat
 *
 * This is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License version 2.1, as published by the Free Software
 * Foundation.  See file COPYING.
 *
 */
#pragma once

#include <chrono>
#include <atomic>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <condition_variable>

#include "include/ceph_assert.h"
#include "include/cephfs/types.h" // for mds_rank_t
#include "include/Context.h"
#include "MDCache.h"

class QuarantineStats {
public:
  QuarantineStats() = default;
  ~QuarantineStats() = default;

  void inode_quarantine_started() {
    ++inodes_started;
  }

  void inode_quarantine_finished() {
    ++inodes_finished;
  }

  void rank_quarantine_started() {
    rank_started = true;
  }

  void rank_quarantine_finished() {
    rank_finished = true;
  }

  uint64_t get_inodes_started() {
    return inodes_started;
  }

  uint64_t get_inodes_finished() {
    return inodes_finished;
  }

  bool is_quarantine_completed() const {
    if (inodes_started == inodes_finished) {
      if (rank_started && rank_finished) {
	return true;
      }
    }
    return false;
  }

private:
  std::atomic_uint64_t inodes_started = 0;
  std::atomic_uint64_t inodes_finished = 0;
  std::atomic_bool rank_started = false;
  std::atomic_bool rank_finished = false;
};

class QuarantineManager {
public:
  QuarantineManager() { }

  ~QuarantineManager() { }

  void get() {
    std::lock_guard<ceph::mutex> lck(m);
    ++ref;
  }

  void put() {
    std::unique_lock<ceph::mutex> lck(m);
    --ref;
  }

  int get_ref() const {
    std::unique_lock<ceph::mutex> lck(m);
    return ref;
  }

  bool is_passed() const {
    std::lock_guard<ceph::mutex> lck(m);
    return passed;
  }

  bool is_cancelled() const {
    std::lock_guard<ceph::mutex> lck(m);
    return cancelled;
  }

  void set_cancelled() {
    std::lock_guard<ceph::mutex> lck(m);
    cancelled = true;
  }

  bool wait_for(std::chrono::seconds secs) {
    std::unique_lock<ceph::mutex> lck(m);

    return qtine_completion.wait_for(lck, secs,
		  		     [this]() -> bool {
				      return is_quarantine_completed_locked();
				    });
  }

  bool is_quarantine_completed() {
    std::lock_guard<ceph::mutex> lck(m);
    return is_quarantine_completed_locked();
  }

  void mds_quarantine_started(mds_rank_t rank) {
    std::lock_guard<ceph::mutex> lck(m);
    ceph_assert(qtine_stats.find(rank) == qtine_stats.end());
    qtine_stats.emplace(std::piecewise_construct,
			std::forward_as_tuple(rank),
			std::forward_as_tuple());
    qtine_stats[rank].rank_quarantine_started();
  }

  void mds_quarantine_finished(mds_rank_t rank) {
    std::unique_lock<ceph::mutex> lck(m);
    ceph_assert(qtine_stats.find(rank) != qtine_stats.end());
    qtine_stats[rank].rank_quarantine_finished();

    if (qtine_stats[rank].is_quarantine_completed()) {
      notify_completion_locked(rank, (cancelled ? -ETIMEDOUT : 0));
    }
  }

  void inode_quarantine_started(mds_rank_t rank) {
    std::lock_guard<ceph::mutex> lck(m);
    ceph_assert(qtine_stats.find(rank) != qtine_stats.end());
    qtine_stats[rank].inode_quarantine_started();
  }

  void inode_quarantine_finished(mds_rank_t rank) {
    std::unique_lock<ceph::mutex> lck(m);
    ceph_assert(qtine_stats.find(rank) != qtine_stats.end());
    qtine_stats[rank].inode_quarantine_finished();

    if (qtine_stats[rank].is_quarantine_completed()) {
      notify_completion_locked(rank, (cancelled ? -ETIMEDOUT : 0));
    }
  }

  uint64_t get_inodes_started(mds_rank_t rank) {
    std::lock_guard<ceph::mutex> lck(m);
    return qtine_stats[rank].get_inodes_started();
  }

  uint64_t get_inodes_finished(mds_rank_t rank) {
    std::lock_guard<ceph::mutex> lck(m);
    return qtine_stats[rank].get_inodes_finished();
  }

  void register_rank_finisher(mds_rank_t rank, Context *fin) {
    std::lock_guard<ceph::mutex> lck(m);
    ceph_assert(qtine_finisher.find(rank) == qtine_finisher.end());
    qtine_finisher[rank] = fin;
  }

  void notify_completion(mds_rank_t rank, int r) {
    std::unique_lock<ceph::mutex> lck(m);
    notify_completion_locked(rank, r);
  }

protected:
  void notify_completion_locked(mds_rank_t rank, int r) {
    ceph_assert(qtine_finisher.find(rank) != qtine_finisher.end());

    auto c = qtine_finisher[rank];
    qtine_finisher.erase(rank);
    c->complete(r);
  }

  bool is_quarantine_completed_locked() {
    for (auto& [rank, rank_stats] : qtine_stats) {
      if (!rank_stats.is_quarantine_completed()) {
	return false;
      }
    }
    return true;
  }

  void notify_all_completion(int r) {
    std::unique_lock<ceph::mutex> lck(m);
    std::unordered_set<mds_rank_t> ranks;

    for (auto& [rank, stats] : qtine_stats) {
      ranks.insert(rank);
    }

    lck.unlock();
    for (auto rank : ranks) {
      notify_completion(rank, r);
    }
  }

  mutable ceph::mutex m = ceph::make_mutex("QuarantineManager::m");
  std::atomic_int ref = 0;
  std::unordered_map<mds_rank_t, QuarantineStats> qtine_stats;
  std::unordered_map<mds_rank_t, Context*> qtine_finisher;
  ceph::condition_variable qtine_completion;
  std::atomic_bool cancelled = false;
  std::atomic_bool passed = false;
};

using QtineMgrRef = std::shared_ptr<QuarantineManager>;
