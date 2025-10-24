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

#include "MDSRank.h"
#include "MDCache.h"
#include "QuarantineManager.h"

#include "mon/MonClient.h"
#include "common/cmdparse.h"
#include "common/errno.h"

#include "messages/MMDSQuarantine.h"

#define dout_context g_ceph_context
#define dout_subsys ceph_subsys_mds_quiesce
#undef dout_prefix
#define dout_prefix *_dout << "quarantine.mds." << whoami << " <" << __func__ << "> "

#undef dout
#define dout(lvl)                                                        \
  do {                                                                   \
    auto subsys = ceph_subsys_mds;                                       \
    if ((dout_context)->_conf->subsys.should_gather(dout_subsys, lvl)) { \
      subsys = dout_subsys;                                              \
    }                                                                    \
  dout_impl(dout_context, ceph::dout::need_dynamic(subsys), lvl) dout_prefix

#undef dendl
#define dendl \
  dendl_impl; \
  }           \
  while (0)

using TOPNSPC::common::cmd_getval;

// asok command handler
void MDSRank::command_quarantine_dir(const cmdmap_t& cmdmap, asok_finisher on_finish, const unsigned op)
{
  std::string path;

  cmd_getval(cmdmap, "path", path);

  MDRequestRef mdr = mdcache->request_start_internal(CEPH_MDS_OP_QUARANTINEDIR_AUTH);
  mdr->no_early_reply = true;
  mdr->set_filepath(filepath(path));
  mdr->internal_op_finish = new LambdaContext([](int r){});
  mdr->qtine_op = op;
  mdr->qtine_mgr = std::make_shared<QuarantineManager>();
  mdr->qtine_mgr->register_rank_finisher(get_nodeid(),
      new LambdaContext([on_finish=on_finish, op=op](int r) {
	std::string_view op_str{(op == QUARANTINE_ADD ? "enable" : "disable")};
	ceph::bufferlist bl;
	std::ostringstream oss;
	if (r) {
	  oss << "Quarantine " << op_str << " failed - " << cpp_strerror(r) << ".";
	} else {
	  oss << "Quarantine " << op_str << " successful.";
	}
	on_finish(r, oss.str(), bl);
      }));
  std::lock_guard l(mds_lock);
  mdcache->dispatch_request(mdr);
}

bool MDSRank::register_quarantine_mgr(inodeno_t ino, std::shared_ptr<QuarantineManager> qtine_mgr)
{
  std::lock_guard lck(qtine_mutex);
  if (qtine_mgrs.find(ino) != qtine_mgrs.end()) {
    return false;
  }
  qtine_mgrs.emplace(std::piecewise_construct,
		     std::forward_as_tuple(ino),
		     std::forward_as_tuple(qtine_mgr));
  return true;
}

void MDSRank::unregister_quarantine_mgr(inodeno_t ino)
{
  std::lock_guard lck(qtine_mutex);
  ceph_assert(qtine_mgrs.find(ino) != qtine_mgrs.end());
  if (qtine_mgrs[ino]->get_ref() == 0) {
    qtine_mgrs.erase(ino);
  }
}

// we can have multiple quarantines running for different subvols at the same time
std::shared_ptr<QuarantineManager>& MDSRank::get_quarantine_mgr(inodeno_t ino)
{
  static std::shared_ptr<QuarantineManager> null_qtine_mgr(nullptr);

  std::lock_guard lck(qtine_mutex);
  try {
    return qtine_mgrs.at(ino);
  } catch (std::out_of_range& ex) {
    // this implies that there's something amiss elsewhere that led to this
    // condition or this method has been called erroneously
    return null_qtine_mgr;
  }
}
