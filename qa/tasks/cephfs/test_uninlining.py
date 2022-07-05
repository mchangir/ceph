
"""
Test that data is uninlined using scrubbing.

The idea is to untar a linux-5.4.0 kernel tarball's Documentation/ dir
consisting of about 8000 files and uninline about 5145 of those which are
less than or equal to client_max_inline_size bytes and can be inlined when
written to while the inline_data config option is enabled.

This test runs across 1 or 2 active MDS, where a subset of the dirs under the
Documentation/ dir are pinned to either of the MDS.
"""

import logging
import threading

from io import StringIO
from tasks.cephfs.cephfs_test_case import CephFSTestCase
from tasks.cephfs.mount import CephFSMount
from datetime import time

log = logging.getLogger(__name__)


def remote_mntpt_cmd(self, mount, cmd):
    final_cmd = f'cd {mount.hostfs_mntpt} && ' + cmd
    out = mount.client_remote.sh(final_cmd, stdout=StringIO())
    return out.strip()


class InlineDataInfo:
    def __init__(self, length: int, version: int):
        self.inline_data_length = length
        self.inline_data_version = version


class SnapshotterThread(threading.Thread):
    def __init__(self, base_dir: str, snap_count: int, mount: CephFSMount):
        super().__init__(self)
        self.base_dir: str = base_dir
        self.snap_count: int = snap_count
        self.mount = mount

    def run(self):
        i = 0
        while i < self.snap_count:
            cmd = f"mkdir {self.base_dir}/.snap/snap_{i}"
            remote_mntpt_cmd(self.mount, cmd)
            time.sleep(2)
            i += 1


class TestDataUninlining(CephFSTestCase):
    MDSS_REQUIRED = 2
    CLIENTS_REQUIRED = 2

    # data version number of uninlined inode: ((1 << 64) - 1)
    CEPH_INLINE_NONE = 18446744073709551615

    NUM_SNAPS = 10

    def setUp(self):
        super(TestDataUninlining, self).setUp()
        self.cache_info = dict()
        self.umount_info = dict()
        self.mount_openbg_info = dict()
        self.multimds_info = dict()
        self.snapshot_info = dict()

        self.cache_info[0] = "without clearing cache"
        self.cache_info[1] = "clear cache before scrub"
        self.cache_info[2] = "clear cache after scrub"
        self.unmount_info[0] = "without unmount client"
        self.unmount_info[1] = "unmount client before scrub"
        self.unmount_info[2] = "unmount client after scrub"
        self.mount_openbg_info[0] = "without mount.open_background"
        self.mount_openbg_info[1] = "with mount.open_background"
        self.multimds_info[0] = "without multimds"
        self.multimds_info[1] = "with multimds"
        self.snapshot_info[0] = "without snapshots"
        self.snapshot_info[1] = "with snapshots"

    def tearDown(self):
        super(TestDataUninlining, self).tearDown()

    def extract_inodes(self, files):
        inodes = []
        for fil in files:
            cmd = f'ls -i {fil}'
            o = remote_mntpt_cmd(self.mount_a, cmd)
            inodes.append(o.split(' ')[0])
        return inodes

    def get_inline_data_info(self, inodes):
        info = []
        for inode in inodes:
            json_out = self.fs.rank_asok(['dump', 'inode', inode], 0)
            if json_out is None:
                json_out = self.fs.rank_asok(['dump', 'inode', inode], 1)
            self.assertTrue(json_out is not None)
            self.assertTrue('inline_data_length' in json_out)
            self.assertTrue('inline_data_version' in json_out)
            info.append(InlineDataInfo(json_out['inline_data_length'],
                                       json_out['inline_data_version']))
        return info

    def run_test_worker(self,
                        opt_clear_cache,
                        opt_unmount,
                        opt_mount_openbg,
                        opt_multimds,
                        opt_snapshot):
        log.info("Running Data Uninlining test with: "
                 f"{self.cache_info[opt_clear_cache]}, "
                 f"{self.unmount_info[opt_unmount]}, "
                 f"{self.mount_openbg_info[opt_mount_openbg]}, "
                 f"{self.multimds_info[opt_multimds]}, "
                 f"{self.snapshot_info[opt_snapshot]}")

        # Set max_mds to 1 or 2
        num_mds = 2 if opt_multimds else 1
        self.fs.set_max_mds(num_mds)

        # Enable inline_data
        self.fs.set_var('inline_data', '1', '--yes-i-really-really-mean-it')

        # Get configured max inline data size
        idsize = self.fs.fs_config.get('client_max_inline_size', 4096)
        idsize = int(idsize)

        # Fetch tarball
        cmd = 'wget http://download.ceph.com/qa/linux-5.4.tar.gz'
        remote_mntpt_cmd(self.mount_a, cmd)

        # Extract test data tarball
        # FIXME
        cmd = 'tar -x -z -f linux-5.4.tar.gz linux-5.4/Documentation/'
        # cmd = 'tar -x -z -f linux-5.4.tar.gz'
        remote_mntpt_cmd(self.mount_a, cmd)

        # Get dirs under linux-5.4.0/Documentation/
        # FIXME
        cmd = 'find linux-5.4/ -mindepth 2 -maxdepth 2 -type d'
        # cmd = 'find linux-5.4/ -mindepth 1 -maxdepth 1 -type d'
        o = remote_mntpt_cmd(self.mount_a, cmd)
        dirs = o.split('\n')

        # Pin dirs alternately to available mds
        if num_mds > 1:
            for i in range(len(dirs)):
                self.mount_a.setfattr(dirs[i], 'ceph.dir.pin', str(i % 2))

        bg_proc = None
        # the data uninlining should cause the caps to be revoked and get the
        # data uninlined with any problems
        if opt_mount_openbg:
            # open in read-only mode
            test_file = "linux-5.4/cap_revoke_test_file"
            bg_proc = self.mount_b.open_background(test_file, True)

        # Count files with size <= idsize
        cmd = f'find linux-5.4/ -type f -size -{idsize + 1}c'
        o = remote_mntpt_cmd(self.mount_a, cmd)
        files = o.split('\n')

        # Dump file count
        log.info(f'Found {len(files)} inlined files')

        if opt_unmount == 1:
            self.mount_a.umount(False)

        if opt_clear_cache == 1:
            i = 0
            while i < num_mds:
                self.fs.mds_asok(['cache', 'drop'], i)
                i += 1

        # Start recursive scrub on rank 0
        out_json = self.fs.run_scrub(["start", "/", "recursive"])

        snapshotter = None
        if opt_snapshot:
            snapshotter = SnapshotterThread("linux-5.4",
                                            self.NUM_SNAPS,
                                            self.mount_b)
            snapshotter.start()

        # Wait for scrub completion
        status = self.fs.wait_until_scrub_complete(tag=out_json["scrub_tag"],
                                                   timeout=43200)
        self.assertEqual(status, True)

        if opt_snapshot:
            snapshotter.join()
            i = 0
            while i < self.NUM_SNAPS:
                cmd = f"rmdir linux-5.4/.snap/snap_{i}"
                remote_mntpt_cmd(self.mount_b, cmd)
                i += 1

        if opt_unmount == 2:
            self.mount_a.mount()

        if opt_clear_cache == 2:
            i = 0
            while i < num_mds:
                self.fs.mds_asok(['cache', 'drop'], i)
                i += 1

        # Extract inode numbers of inlined files
        inodes = self.extract_inodes(files)

        # Dump inode info of files with size <= idsize
        self.assertEqual(len(files), len(inodes))

        info = self.get_inline_data_info(inodes)

        # cleanup
        if opt_mount_openbg:
            self.mount_b.kill_background(bg_proc)

        remote_mntpt_cmd(self.mount_a, "rm -rf linux-5.4/")
        remote_mntpt_cmd(self.mount_a, "rm -f linux-5.4.tar.gz")

        self.assertEqual(len(info), len(inodes))

        # Count files with inline_data_length == 0 and validate
        zero_length_count = 0
        for finfo in info:
            if int(finfo.inline_data_length) == 0:
                zero_length_count += 1
        log.info(f'Found {zero_length_count} files with '
                 'inline_data_length == 0')
        self.assertTrue(zero_length_count == len(files))

        # Count files with inline_data_version == 18446744073709551615
        # and validate
        uninlined_version_count = 0
        for finfo in info:
            if int(finfo.inline_data_version) == self.CEPH_INLINE_NONE:
                uninlined_version_count += 1
        log.info(f'Found {uninlined_version_count} files with '
                 'inline_data_version == CEPH_INLINE_NONE')
        self.assertTrue(uninlined_version_count == len(files))

    def test_data_uninlining(self):
        # multimds
        # 0: without multimds
        # 1: with multimds
        for opt_multimds in [0, 1]:
            # unmount
            # 0: do not unmount
            # 1: unmount before scrub
            # 2: unmount after scrub
            for opt_unmount in [0, 1, 2]:
                # mount
                # 0: no mount.open_background
                # 1: mount.open_background
                for opt_mount_openbg in [0, 1]:
                    # clear cache
                    # 0: do not clear cache
                    # 1: clear cache before scrub
                    # 2: clear cache after scrub
                    for opt_clear_cache in [0, 1, 2]:
                        # snapshots
                        # 0: without snapshots
                        # 1: with snapshots
                        for opt_snapshot in [0, 1]:
                            self.run_test_worker(opt_clear_cache,
                                                 opt_unmount,
                                                 opt_mount_openbg,
                                                 opt_multimds,
                                                 opt_snapshot)
