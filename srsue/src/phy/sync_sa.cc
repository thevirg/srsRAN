/**
 *
 * \section COPYRIGHT
 *
 * Copyright 2013-2021 Software Radio Systems Limited
 *
 * By using this file, you agree to the terms and conditions set
 * forth in the LICENSE file which can be found at the top level of
 * the distribution.
 *
 */
#include "srsue/hdr/phy/nr/sync_sa.h"
#include "srsran/radio/rf_buffer.h"

namespace srsue {
namespace nr {
sync_sa::sync_sa(srslog::basic_logger& logger_, worker_pool& workers_) :
  logger(logger_), workers(workers_), slot_synchronizer(logger_), searcher(logger_), srsran::thread("SYNC")
{}

sync_sa::~sync_sa() {}

bool sync_sa::init(const args_t& args, stack_interface_phy_nr* stack_, srsran::radio_interface_phy* radio_)
{
  stack = stack_;
  radio = radio_;
  sf_sz = (uint32_t)(args.srate_hz / 1000.0f);

  // Initialise cell search internal object
  if (not searcher.init(args.get_cell_search(), stack, radio)) {
    logger.error("Error initialising cell searcher");
    return false;
  }

  // Initialise slot synchronizer object
  if (not slot_synchronizer.init(args.get_slot_sync(), stack, radio)) {
    logger.error("Error initialising slot synchronizer");
    return false;
  }

  // Thread control
  running = true;
  start(args.thread_priority);

  // If reached here it was successful
  return true;
}

bool sync_sa::start_cell_search(const cell_search::cfg_t& cfg)
{
  // Make sure current state is IDLE
  std::unique_lock<std::mutex> lock(state_mutex);
  if (state != STATE_IDLE or next_state != STATE_IDLE) {
    logger.error("Sync: trying to start cell search but state is not IDLE");
    return false;
  }

  // Configure searcher without locking state for avoiding stalling the Rx stream
  lock.unlock();
  if (not searcher.start(cfg)) {
    logger.error("Sync: failed to start cell search");
    return false;
  }
  lock.lock();

  // Transition to search
  next_state = STATE_CELL_SEARCH;

  return true;
}

bool sync_sa::start_cell_select()
{
  return true;
}

bool sync_sa::go_idle()
{
  std::unique_lock<std::mutex> lock(state_mutex);

  // Force transition to IDLE
  while (state != STATE_IDLE) {
    next_state = STATE_IDLE;
    state_cvar.wait(lock);
  }

  // Wait worker pool to finish any processing
  tti_semaphore.wait_all();

  return true;
}

void sync_sa::stop()
{
  running = false;
  wait_thread_finish();
}

sync_sa::state_t sync_sa::get_state() const
{
  std::unique_lock<std::mutex> lock(state_mutex);
  return state;
}

void sync_sa::run_state_idle()
{
  srsran::rf_buffer_t rf_buffer = {};
  rf_buffer.set_nof_samples(sf_sz);

  srsran::rf_timestamp_t ts = {};

  // Receives from radio 1 slot
  radio->rx_now(rf_buffer, ts);

  stack->run_tti(slot_cfg.idx);
}

void sync_sa::run_state_cell_search()
{
  // Run Searcher
  if (not searcher.run()) {
    logger.error("Failed to run searcher. Transitioning to IDLE...");

    // Transition to IDLE if fails to run
    state_mutex.lock();
    next_state = STATE_IDLE;
    state_mutex.unlock();
  }
}

void sync_sa::run_thread()
{
  while (running) {
    state_mutex.lock();
    // Detect state transition
    if (next_state != state) {
      state = next_state;
      state_cvar.notify_all();
    }
    state_t current_state = state;
    state_mutex.unlock();

    switch (current_state) {
      case STATE_IDLE:
        run_state_idle();
        break;
      case STATE_CELL_SEARCH:
        run_state_cell_search();
        break;
      case STATE_CELL_SELECT:
        break;
    }
  }
}
void sync_sa::worker_end(const srsran::phy_common_interface::worker_context_t& w_ctx,
                         const bool&                                           tx_enable,
                         srsran::rf_buffer_t&                                  buffer)
{}

} // namespace nr
} // namespace srsue