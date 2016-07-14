/* -*- mode: c; c-basic-offset: 2; indent-tabs-mode: nil; -*-
 * (c) 2015 H Hartley Sweeten <hsweeten@visionengravers.com>
 *    and author who implemented Smoothieware Robot::append_arc()
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

#include <math.h>
#include <stdio.h>

#include "arc-gen.h"
#include "logging.h"

// Arc generation based on smoothieware implementation
// https://github.com/Smoothieware/Smoothieware.git
// src/modules/robot/Robot.cpp - Robot::append_arc()
//
// Simplified: we don't care about using trigonometric optimization
// tricks (CPU fast enough) but rather have readable code.
//
// Normal axis is the axis perpendicular to the plane the arc is
// created in.

// Small arc segments increase accuracy, though might create some
// CPU load. With 0.1mm arc segments, we can generate about 90 meter/second
// of arcs with the BeagleBone Black CPU. Sufficient :)
#define MM_PER_ARC_SEGMENT      0.1
#define MAX_FEEDRATE 5000
#define STEPS_RAMP 20

struct ArcCallbackData {
  void *callbacks;
  float feedrate;
};

// Generate an arc. Input is the
void arc_gen(enum GCodeParserAxis normal_axis,  // Normal axis of the arc-plane
             bool is_cw,                        // 0 CCW, 1 CW
             AxesRegister *position_out,   // start position. Will be updated.
             const AxesRegister &offset,     // Offset to center.
             const AxesRegister &target,     // Target position.
             void (*segment_output)(void *, const AxesRegister&),
             void *segment_output_user_data) {
  // Depending on the normal vector, pre-calc plane
  enum GCodeParserAxis plane[3];
  switch (normal_axis) {
  case AXIS_Z: plane[0] = AXIS_X; plane[1] = AXIS_Y; plane[2] = AXIS_Z; break;
  case AXIS_X: plane[0] = AXIS_Y; plane[1] = AXIS_Z; plane[2] = AXIS_X; break;
  case AXIS_Y: plane[0] = AXIS_X; plane[1] = AXIS_Z; plane[2] = AXIS_Y; break;
  default:
    return;   // Invalid axis.
  }

  // Alias reference for readable use with [] operator
  AxesRegister &position = *position_out;

  const float radius = sqrtf(offset[plane[0]] * offset[plane[0]] +
                             offset[plane[1]] * offset[plane[1]]);
  const float center_0 = position[plane[0]] + offset[plane[0]];
  const float center_1 = position[plane[1]] + offset[plane[1]];
   // Allow multiple axes to be linearly interpolated
  const float linear_travel[3] = {
    target[plane[2]] - position[plane[2]],
    target[AXIS_A] - position[AXIS_A],
    target[AXIS_B] - position[AXIS_B],
   };
  const float rt_0 = target[plane[0]] - center_0;
  const float rt_1 = target[plane[1]] - center_1;

  float r_0 = -offset[plane[0]]; // Radius vector from center to current location
  float r_1 = -offset[plane[1]];

#if 0
  Log_debug("arc from %c,%c: %.3f,%.3f to %.3f,%.3f (radius:%.3f) helix %c:%.3f\n",
            gcodep_axis2letter(plane[0]), gcodep_axis2letter(plane[1]),
            position[plane[0]], position[plane[1]],
            target[plane[0]], target[plane[1]], radius,
            gcodep_axis2letter(plane[2]),  linear_travel[0]);
#endif

  struct ArcCallbackData *cb_arc_data
    = (struct ArcCallbackData *) segment_output_user_data;
  const float max_feedrate
    = MAX_FEEDRATE > cb_arc_data->feedrate ? cb_arc_data->feedrate : MAX_FEEDRATE;

  // CCW angle between position and target from circle center.
  float angular_travel = atan2(r_0*rt_1 - r_1*rt_0, r_0*rt_0 + r_1*rt_1);
  if (is_cw) {
    if (angular_travel >= -1e-6) angular_travel -= 2*M_PI;
  } else {
    if (angular_travel <=  1e-6) angular_travel += 2*M_PI;
  }

  // Find the distance for this gcode in the axes we care.
  const float mm_of_travel = hypotf(angular_travel * radius, fabs(linear_travel[0]));

  // We don't care about non-XYZ moves (e.g. extruder)
  if (mm_of_travel < 0.00001)
    return;

  // Figure out how many segments for this gcode
  const int segments = floorf(mm_of_travel / MM_PER_ARC_SEGMENT);

  const float theta_per_segment = angular_travel / segments;
  const float linear_per_segment = linear_travel[0] / segments;

  // Evaluate non "positional" linear segment partition
  const float linear_per_segment_A = linear_travel[1] / segments;
  const float linear_per_segment_B = linear_travel[2] / segments;

  for (int i = 1; i < segments; i++) { // Increment (segments-1)
    // Hack to slowly accelerate thourgh the circle, work in progress to remove this
    if (i < STEPS_RAMP) {
        cb_arc_data->feedrate = max_feedrate * i / STEPS_RAMP;
    } else if (segments - i < STEPS_RAMP) {
        cb_arc_data->feedrate = max_feedrate * (segments - i) / STEPS_RAMP;
    }

    const float cos_Ti = cosf(i * theta_per_segment);
    const float sin_Ti = sinf(i * theta_per_segment);
    r_0 = -offset[plane[0]] * cos_Ti + offset[plane[1]] * sin_Ti;
    r_1 = -offset[plane[0]] * sin_Ti - offset[plane[1]] * cos_Ti;

    // Update arc_target location
    position[plane[0]] = center_0 + r_0;
    position[plane[1]] = center_1 + r_1;
    position[plane[2]] += linear_per_segment;

    // And components target location
    position[AXIS_A] += linear_per_segment_A;
    position[AXIS_B] += linear_per_segment_B;

    // Emit
    segment_output(segment_output_user_data, position);
  }

  // Ensure last segment arrives at target location.
  for (int axis = AXIS_X; axis < GCODE_NUM_AXES; axis++) {
    position[axis] = target[axis];
  }
  segment_output(segment_output_user_data, position);
}
