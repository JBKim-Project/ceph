// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#pragma once

#include <boost/intrusive_ptr.hpp>
#include <boost/smart_ptr/intrusive_ref_counter.hpp>

#include <seastar/core/file.hh>
#include <seastar/core/future.hh>
#include <seastar/core/reactor.hh>

#include "crimson/common/layout.h"

#include "crimson/os/seastore/segment_manager.h"

namespace crimson::os::seastore::segment_manager::block {

using write_ertr = crimson::errorator<
  crimson::ct_error::input_output_error>;
using read_ertr = crimson::errorator<
  crimson::ct_error::input_output_error>;

/**
 * SegmentStateTracker
 *
 * Tracks lifecycle state of each segment using space at the beginning
 * of the drive.
 */
class SegmentStateTracker {
  using segment_state_t = Segment::segment_state_t;

  bufferptr bptr;

  using L = absl::container_internal::Layout<uint8_t>;
  const L layout;

public:
  static size_t get_raw_size(size_t segments, size_t block_size) {
    return p2roundup(segments, block_size);
  }

  SegmentStateTracker(size_t segments, size_t block_size)
    : bptr(ceph::buffer::create_page_aligned(
	     get_raw_size(segments, block_size))),
      layout(bptr.length())
  {
    ::memset(
      bptr.c_str(),
      static_cast<char>(segment_state_t::EMPTY),
      bptr.length());
  }

  size_t get_size() const {
    return bptr.length();
  }

  size_t get_capacity() const {
    return bptr.length();
  }

  segment_state_t get(device_segment_id_t offset) const {
    assert(offset < get_capacity());
    return static_cast<segment_state_t>(
      layout.template Pointer<0>(
	bptr.c_str())[offset]);
  }

  void set(device_segment_id_t offset, segment_state_t state) {
    assert(offset < get_capacity());
    layout.template Pointer<0>(bptr.c_str())[offset] =
      static_cast<uint8_t>(state);
  }

  write_ertr::future<> write_out(
    seastar::file &device,
    uint64_t offset);

  read_ertr::future<> read_in(
    seastar::file &device,
    uint64_t offset);
};

class BlockSegmentManager;
class BlockSegment final : public Segment {
  friend class BlockSegmentManager;
  BlockSegmentManager &manager;
  const segment_id_t id;
  segment_off_t write_pointer = 0;
public:
  BlockSegment(BlockSegmentManager &manager, segment_id_t id);

  segment_id_t get_segment_id() const final { return id; }
  segment_off_t get_write_capacity() const final;
  segment_off_t get_write_ptr() const final { return write_pointer; }
  close_ertr::future<> close() final;
  write_ertr::future<> write(segment_off_t offset, ceph::bufferlist bl) final;

  ~BlockSegment() {}
};

/**
 * BlockSegmentManager
 *
 * Implements SegmentManager on a conventional block device.
 * SegmentStateTracker uses space at the start of the device to store
 * state analagous to that of the segments of a zns device.
 */
class BlockSegmentManager final : public SegmentManager {
public:
  mount_ret mount() final;

  mkfs_ret mkfs(segment_manager_config_t) final;

  close_ertr::future<> close();

  BlockSegmentManager(
    const std::string &path)
  : device_path(path) {}

  ~BlockSegmentManager();

  open_ertr::future<SegmentRef> open(segment_id_t id) final;

  release_ertr::future<> release(segment_id_t id) final;

  read_ertr::future<> read(
    paddr_t addr,
    size_t len,
    ceph::bufferptr &out) final;

  size_t get_size() const final {
    return superblock.size;
  }
  segment_off_t get_block_size() const {
    return superblock.block_size;
  }
  segment_off_t get_segment_size() const {
    return superblock.segment_size;
  }

  device_id_t get_device_id() const final {
    return superblock.device_id;
  }
  secondary_device_set_t& get_secondary_devices() final {
    return superblock.secondary_devices;
  }
  // public so tests can bypass segment interface when simpler
  Segment::write_ertr::future<> segment_write(
    paddr_t addr,
    ceph::bufferlist bl,
    bool ignore_check=false);

  device_spec_t get_device_spec() const final {
    return {superblock.magic,
	    superblock.dtype,
	    superblock.device_id};
  }

  magic_t get_magic() const final {
    return superblock.magic;
  }

private:
  friend class BlockSegment;
  using segment_state_t = Segment::segment_state_t;

  struct effort_t {
    uint64_t num = 0;
    uint64_t bytes = 0;

    void increment(uint64_t read_bytes) {
      ++num;
      bytes += read_bytes;
    }
  };

  struct {
    effort_t data_read;
    effort_t data_write;
    effort_t metadata_write;
    uint64_t opened_segments;
    uint64_t closed_segments;
    uint64_t closed_segments_unused_bytes;
    uint64_t released_segments;

    void reset() {
      data_read = {};
      data_write = {};
      metadata_write = {};
      opened_segments = 0;
      closed_segments = 0;
      closed_segments_unused_bytes = 0;
      released_segments = 0;
    }
  } stats;

  void register_metrics();
  seastar::metrics::metric_group metrics;

  std::string device_path;
  std::unique_ptr<SegmentStateTracker> tracker;
  block_sm_superblock_t superblock;
  seastar::file device;

  device_id_t device_id = 0;

  size_t get_offset(paddr_t addr) {
    return superblock.first_segment_offset +
      (addr.segment.device_segment_id() * superblock.segment_size) +
      addr.offset;
  }

  const seastore_meta_t &get_meta() const {
    return superblock.meta;
  }

  std::vector<segment_state_t> segment_state;

  char *buffer = nullptr;

  Segment::close_ertr::future<> segment_close(
      segment_id_t id, segment_off_t write_pointer);
};

}
