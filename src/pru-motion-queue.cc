/* -*- mode: c++; c-basic-offset: 2; indent-tabs-mode: nil; -*-
 * (c) 2013, 2014 Henner Zeller <h.zeller@acm.org>
 *
 * This file is part of BeagleG. http://github.com/hzeller/beagleg
 *
 * BeagleG is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * BeagleG is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with BeagleG.  If not, see <http://www.gnu.org/licenses/>.
 */

// Implementation of the MotionQueue interfacing the PRU.
// We are using some shared memory between CPU and PRU to communicate.

#include "motion-queue.h"

#include <assert.h>
#include <errno.h>
#include <pruss_intc_mapping.h>
#include <prussdrv.h>
#include <stdio.h>
#include <strings.h>

#include "generic-gpio.h"
#include "pwm-timer.h"
#include "logging.h"
#include "hardware-mapping.h"

// Generated PRU code from motor-interface-pru.p
#include "motor-interface-pru_bin.h"

//#define DEBUG_QUEUE
#define PRU_NUM 0

// The communication with the PRU. We memory map the static RAM in the PRU
// and write stuff into it from here. Mostly this is a ring-buffer with
// commands to execute, but also configuration data, such as what to do when
// an endswitch fires.
struct PRUCommunication {
  volatile struct MotionSegment ring_buffer[QUEUE_LEN];
  volatile struct QueueStatus status;
};

// The queue is a physical singleton, so simply reflect that here.
static volatile struct PRUCommunication *pru_data_;
static unsigned int queue_pos_;

// Represents an already enqueued motion segment.
// pru motion queue is steps-agnostic, It understands only low-level loops.
struct HistorySegment {
  uint32_t total_loops;
  uint32_t fractions[MOTION_MOTOR_COUNT];
  uint8_t direction_bits;
};

// Keeps track of the MotionSegments enqueued.
static struct HistorySegment shadow_queue_[QUEUE_LEN];

// Handle the absolute position in steps
static FixedArray<int32_t, BEAGLEG_NUM_MOTORS> absolute_pos_loops_;

static void update_absolute_position() {
  int i;
  const struct QueueStatus status_register = pru_data_->status;
  const unsigned int last_insert_position = (queue_pos_ - 1) % QUEUE_LEN;
  // Pru_queue_size represent the real pru queue size, so we shrink the history
  // queue until it's exactly the same as the pru one.
  const unsigned pru_queue_size
    = (last_insert_position + QUEUE_LEN - status_register.index)
      % QUEUE_LEN + 1;

  if (pru_queue_size == 0) return;

  while (execution_history_.size() > pru_queue_size) {
    for (i = 0; i < MOTION_MOTOR_COUNT; ++i) {
      (*current_position_steps_)[i] += execution_history_[0]->steps[i];
    }
    execution_history_.pop_front();
  }

  // Now update the current executing slot if it had steps.
  if (execution_history_[0]->max_axis_steps) {
    uint64_t delta;
    for (i = 0; i < MOTION_MOTOR_COUNT; ++i) {
      (*current_position_steps_)[i] -= last_partial[i];

      delta = status_register.counter * abs(execution_history_[0]->steps[i]);
      delta /= LOOPS_PER_STEP;
      delta /= execution_history_[0]->max_axis_steps;

      if (execution_history_[0]->steps[i] < 0) {
        last_partial[i] = execution_history_[0]->steps[i] + delta;
      } else {
        last_partial[i] = execution_history_[0]->steps[i] - delta;
      }
      (*current_position_steps_)[i] += last_partial[i];
    }
  }
}

static void register_history_segment(MotionSegment *element) {
  const unsigned int last_insert_position = (queue_pos_ - 1) % QUEUE_LEN;
  struct HistorySegment *new_slot = &shadow_queue_[last_insert_position];

  bzero(new_slot, sizeof(struct HistorySegment));

  new_slot->total_loops += element->loops_accel;
  new_slot->total_loops += element->loops_travel;
  new_slot->total_loops += element->loops_decel;

  new_slot->direction_bits = element->direction_bits;

  for (int i = 0; i < MOTION_MOTOR_COUNT; ++i) {
    new_slot->fractions[i] = element->fractions[i];
  }
}

static volatile struct PRUCommunication *map_pru_communication() {
  void *result;
  prussdrv_map_prumem(PRUSS0_PRU0_DATARAM, &result);
  bzero(result, sizeof(*pru_data_));
  pru_data_ = (struct PRUCommunication*) result;
  for (int i = 0; i < QUEUE_LEN; ++i) {
    pru_data_->ring_buffer[i].state = STATE_EMPTY;
  }
  queue_pos_ = 0;
  return pru_data_;
}

static volatile struct MotionSegment *next_queue_element() {
  queue_pos_ %= QUEUE_LEN;
  while (pru_data_->ring_buffer[queue_pos_].state != STATE_EMPTY) {
    prussdrv_pru_wait_event(PRU_EVTOUT_0);
    prussdrv_pru_clear_event(PRU_EVTOUT_0, PRU0_ARM_INTERRUPT);
  }
  return &pru_data_->ring_buffer[queue_pos_++];
}

#ifdef DEBUG_QUEUE
static void DumpMotionSegment(volatile const struct MotionSegment *e) {
  if (e->state == STATE_EXIT) {
    Log_debug("enqueue[%02td]: EXIT", e - pru_data_->ring_buffer);
  } else {
    MotionSegment copy = (MotionSegment&) *e;
    std::string line;
    line = StringPrintf("enqueue[%02td]: dir:0x%02x s:(%5d + %5d + %5d) = %5d ",
                        e - pru_data_->ring_buffer, copy.direction_bits,
                        copy.loops_accel, copy.loops_travel, copy.loops_decel,
                        copy.loops_accel + copy.loops_travel + copy.loops_decel);

    if (copy.hires_accel_cycles > 0) {
      line += StringPrintf("accel : %5.0fHz (%d loops);",
                           TIMER_FREQUENCY /
                           (2.0*(copy.hires_accel_cycles >> DELAY_CYCLE_SHIFT)),
                           copy.hires_accel_cycles >> DELAY_CYCLE_SHIFT);
    }
    if (copy.travel_delay_cycles > 0) {
      line += StringPrintf("travel: %5.0fHz (%d loops);",
                           TIMER_FREQUENCY / (2.0*copy.travel_delay_cycles),
                           copy.travel_delay_cycles);
    }
#if 0
    // The fractional parts.
    for (int i = 0; i < MOTION_MOTOR_COUNT; ++i) {
      if (copy.fractions[i] == 0) continue;  // not interesting.
      line += StringPrintf("f%d:0x%08x ", i, copy.fractions[i]);
    }
#endif
    Log_debug("%s", line.c_str());
  }
}
#endif

// Stop gap for compiler attempting to be overly clever when copying between
// host and PRU memory.
static void unaligned_memcpy(void *dest, const void *src, size_t size) {
  char *d = (char*) dest;
  const char *s = (char*) src;
  const char *end = d + size;
  while (d < end) {
    *d++ = *s++;
  }
}

void PRUMotionQueue::Enqueue(MotionSegment *element) {
  const uint8_t state_to_send = element->state;
  assert(state_to_send != STATE_EMPTY);  // forgot to set proper state ?
  // Initially, we copy everything with 'STATE_EMPTY', then flip the state
  // to avoid a race condition while copying.
  element->state = STATE_EMPTY;

  // Need to case volatile away, otherwise c++ complains.
  MotionSegment *queue_element = (MotionSegment*) next_queue_element();
  unaligned_memcpy(queue_element, element, sizeof(*queue_element));

  // Fully initialized. Tell busy-waiting PRU by flipping the state.
  queue_element->state = state_to_send;

  // Register the enqueued motion segment into the shadow_queue_.
  register_history_segment(element);

#ifdef DEBUG_QUEUE
  DumpMotionSegment(queue_element);
#endif
}

void PRUMotionQueue::WaitQueueEmpty() {
  const unsigned int last_insert_position = (queue_pos_ - 1) % QUEUE_LEN;
  while (pru_data_->ring_buffer[last_insert_position].state != STATE_EMPTY) {
    prussdrv_pru_wait_event(PRU_EVTOUT_0);
    prussdrv_pru_clear_event(PRU_EVTOUT_0, PRU0_ARM_INTERRUPT);
  }
}

void PRUMotionQueue::MotorEnable(bool on) {
  hardware_mapping_->EnableMotors(on);
}

void PRUMotionQueue::Shutdown(bool flush_queue) {
  if (flush_queue) {
    struct MotionSegment end_element = {0};
    end_element.state = STATE_EXIT;
    Enqueue(&end_element);
    WaitQueueEmpty();
  }
  prussdrv_pru_disable(PRU_NUM);
  prussdrv_exit();
  MotorEnable(false);
}

PRUMotionQueue::PRUMotionQueue(HardwareMapping *hw) : hardware_mapping_(hw) {
  const int init_result = Init();
  // For now, we just assert-fail here, if things fail.
  // Typically hardware-doomed event anyway.
  assert(init_result == 0);
}

int PRUMotionQueue::Init() {
  MotorEnable(false);  // motors off initially.

  tpruss_intc_initdata pruss_intc_initdata = PRUSS_INTC_INITDATA;
  prussdrv_init();

  /* Get the interrupt initialized */
  int ret = prussdrv_open(PRU_EVTOUT_0);  // allow access.
  if (ret) {
    Log_error("prussdrv_open() failed (%d) %s\n", ret, strerror(errno));
    return ret;
  }
  prussdrv_pruintc_init(&pruss_intc_initdata);
  if (map_pru_communication() == NULL) {
    Log_error("Couldn't map PRU memory for queue.\n");
    return 1;
  }

  prussdrv_pru_write_memory(PRUSS0_PRU0_IRAM, 0, PRUcode, sizeof(PRUcode));
  prussdrv_pru_enable(0);

  return 0;
}
