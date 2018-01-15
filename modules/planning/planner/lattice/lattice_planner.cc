/******************************************************************************
 * Copyright 2017 The Apollo Authors. All Rights Reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *****************************************************************************/

#include "modules/planning/planner/lattice/lattice_planner.h"

#include <memory>
#include <vector>

#include "modules/common/log.h"
#include "modules/common/macro.h"
#include "modules/common/time/time.h"
#include "modules/common/adapters/adapter_manager.h"
#include "modules/planning/common/planning_gflags.h"
#include "modules/planning/math/frame_conversion/cartesian_frenet_conversion.h"
#include "modules/planning/lattice/util/lattice_params.h"
#include "modules/planning/lattice/behavior_decider/path_time_neighborhood.h"
#include "modules/planning/lattice/util/reference_line_matcher.h"
#include "modules/planning/lattice/trajectory1d_generator/trajectory1d_generator.h"
#include "modules/planning/lattice/trajectory1d_generator/trajectory_evaluator.h"
#include "modules/planning/lattice/util/lattice_trajectory1d.h"
#include "modules/planning/lattice/util/collision_checker.h"
#include "modules/planning/lattice/util/lattice_constraint_checker.h"
#include "modules/planning/lattice/util/lattice_util.h"

namespace apollo {
namespace planning {

using apollo::common::adapter::AdapterManager;
using apollo::common::Status;
using apollo::common::ErrorCode;
using apollo::common::PathPoint;
using apollo::common::TrajectoryPoint;
using apollo::common::time::Clock;

LatticePlanner::LatticePlanner() {}

Status LatticePlanner::Init(const PlanningConfig& config) {
  return Status::OK();
}

Status LatticePlanner::Plan(const common::TrajectoryPoint& planning_init_point,
                            Frame* frame,
                            ReferenceLineInfo* reference_line_info) {
  static std::size_t num_planning_cycles = 0;
  static std::size_t num_planning_succeeded_cycles = 0;

  AINFO << "";
  AINFO << "[BEGIN]-------------------------------------------------";
  double start_time = Clock::NowInSeconds();
  double current_time = start_time;

  AINFO << "Number of planning cycles:\t" << num_planning_cycles << "\t"
        << num_planning_succeeded_cycles;
  ++num_planning_cycles;

  // 1. obtain a reference line and transform it to the PathPoint format.
  auto discretized_reference_line = ToDiscretizedReferenceLine(
      reference_line_info->reference_line().reference_points());

  // 2. compute the matched point of the init planning point on the reference
  // line.
  PathPoint matched_point = ReferenceLineMatcher::MatchToReferenceLine(
      discretized_reference_line, planning_init_point.path_point().x(),
      planning_init_point.path_point().y());

  // 3. according to the matched point, compute the init state in Frenet frame.
  std::array<double, 3> init_s;
  std::array<double, 3> init_d;
  ComputeInitFrenetState(matched_point, planning_init_point, &init_s, &init_d);

  AINFO << "Step 1,2,3 Succeeded "
        << "ReferenceLine and Frenet Conversion Time = "
        << (Clock::NowInSeconds() - current_time) * 1000;
  current_time = Clock::NowInSeconds();

  // 4. parse the decision and get the planning target.
  std::shared_ptr<PathTimeNeighborhood> path_time_neighborhood_ptr(
      new PathTimeNeighborhood(frame->obstacles(), init_s[0],
                               discretized_reference_line));

  decider_.UpdatePathTimeNeighborhood(path_time_neighborhood_ptr);
  PlanningTarget planning_target =
      decider_.Analyze(frame, reference_line_info, planning_init_point, init_s,
                       discretized_reference_line);

  AINFO << "Decision_Time = " << (Clock::NowInSeconds() - current_time) * 1000;
  current_time = Clock::NowInSeconds();

  // 5. generate 1d trajectory bundle for longitudinal and lateral respectively.
  Trajectory1dGenerator trajectory1d_generator(init_s, init_d);
  std::vector<std::shared_ptr<Curve1d>> lon_trajectory1d_bundle;
  std::vector<std::shared_ptr<Curve1d>> lat_trajectory1d_bundle;
  trajectory1d_generator.GenerateTrajectoryBundles(
      planning_target, &lon_trajectory1d_bundle, &lat_trajectory1d_bundle);

  AINFO << "Trajectory_Generation_Time="
        << (Clock::NowInSeconds() - current_time) * 1000;
  current_time = Clock::NowInSeconds();

  // 6. first, evaluate the feasibility of the 1d trajectories according to
  // dynamic constraints.
  //   second, evaluate the feasible longitudinal and lateral trajectory pairs
  //   and sort them according to the cost.
  TrajectoryEvaluator trajectory_evaluator(
      planning_target, lon_trajectory1d_bundle, lat_trajectory1d_bundle,
      true, path_time_neighborhood_ptr);

  AINFO << "Trajectory_Evaluator_Construction_Time="
        << (Clock::NowInSeconds() - current_time) * 1000;
  current_time = Clock::NowInSeconds();

  AINFO << "number of trajectory pairs = "
        << trajectory_evaluator.num_of_trajectory_pairs()
        << "  number_lon_traj=" << lon_trajectory1d_bundle.size()
        << "  number_lat_traj=" << lat_trajectory1d_bundle.size();
  AINFO << "";

  AINFO << "Step 4,5,6 Succeeded";

  // Get instance of collision checker and constraint checker
  const std::vector<const Obstacle*>& obstacles = frame->obstacles();

  CollisionChecker collision_checker(obstacles);

  // 7. always get the best pair of trajectories to combine; return the first
  // collision-free trajectory.
  std::size_t constraint_failure_count = 0;
  std::size_t collision_failure_count = 0;
  std::size_t combined_constraint_failure_count = 0;

  planning_internal::Debug* ptr_debug = reference_line_info->mutable_debug();

  /*
  // put obstacles into debug data
  // Note : create prediction_obstacles since there is no longer original
  // data exposed. WTF
  // Hence, there might be obstacles with same id but different trajectory

  for (uint i = 0; i < obstacles.size(); ++i) {
    const Obstacle* obstacle_ptr = obstacles[i];
    apollo::prediction::PredictionObstacle* prediction_obstacle =
        ptr_debug->mutable_planning_data()->add_prediction_obstacle();
    prediction_obstacle->mutable_perception_obstacle()->CopyFrom(
        obstacle_ptr->Perception());
    prediction_obstacle->add_trajectory()->CopyFrom(obstacle_ptr->Trajectory());
  }

  AINFO << "Step Debug Succeeded";
  */
  int num_lattice_traj = 0;
  while (trajectory_evaluator.has_more_trajectory_pairs()) {
    double trajectory_pair_cost = 0.0;
    trajectory_pair_cost = trajectory_evaluator.top_trajectory_pair_cost();
    // For auto tuning
    std::vector<double> trajectory_pair_cost_components =
      trajectory_evaluator.top_trajectory_pair_component_cost();

    auto trajectory_pair = trajectory_evaluator.next_top_trajectory_pair();

    // check the validity of 1d trajectories
    if (!LatticeConstraintChecker::IsValidTrajectoryPair(
            *trajectory_pair.second, *trajectory_pair.first)) {
      ++constraint_failure_count;
      continue;
    }

    // combine two 1d trajectories to one 2d trajectory
    auto combined_trajectory = CombineTrajectory(
        discretized_reference_line, *trajectory_pair.first,
        *trajectory_pair.second, planning_init_point.relative_time());

    // check longitudinal and lateral acceleration
    // considering trajectory curvatures
    if (!LatticeConstraintChecker::IsValidTrajectory(combined_trajectory)) {
      ++combined_constraint_failure_count;
      continue;
    }

    // check collision with other obstacles
    if (collision_checker.InCollision(combined_trajectory)) {
      ++collision_failure_count;
      continue;
    }

    // put combine trajectory into debug data
    const std::vector<common::TrajectoryPoint>& combined_trajectory_points =
        combined_trajectory.trajectory_points();
    num_lattice_traj += 1;
    reference_line_info->SetTrajectory(combined_trajectory);
    reference_line_info->SetCost(reference_line_info->PriorityCost() +
      trajectory_pair_cost);
    reference_line_info->SetDrivable(true);

    // Auto Tuning

    bool tuning_success = true;
    if (AdapterManager::GetLocalization() == nullptr) {
      AINFO << "Auto tuning failed since no localization avaiable";
      tuning_success = false;
    } else if (FLAGS_enable_auto_tuning) {
      // 1. Get future trajectory from localization
      DiscretizedTrajectory future_trajectory = GetFutureTrajectory();
      // 2. Map future trajectory to lon-lat trajectory pair
      std::vector<apollo::common::SpeedPoint> lon_future_trajectory;
      std::vector<apollo::common::FrenetFramePoint> lat_future_trajectory;
      if (!MapFutureTrajectoryToSL(future_trajectory,
               &lon_future_trajectory, &lat_future_trajectory,
               reference_line_info)) {
        AINFO << "Auto tuning failed since no mapping "
              << "from future trajectory to lon-lat";
        tuning_success = false;
      }
      // 3. evalutate cost
      std::vector<double> future_trajectory_component_cost =
        trajectory_evaluator.evaluate_per_lonlat_trajectory(
          planning_target, lon_future_trajectory, lat_future_trajectory);

      // 4. emit
      ///////////////////////
    }

    // Print the chosen end condition and start condition
    AINFO << "   --- Starting Pose: s=" << init_s[0] << " ds=" << init_s[1]
          << " dds=" << init_s[2];
    // cast
    auto lattice_traj_ptr =
        std::dynamic_pointer_cast<LatticeTrajectory1d>(trajectory_pair.first);
    if (!lattice_traj_ptr) {
      AINFO << "Not lattice traj";
    }
    AINFO << "   --- Ending Pose:   s=" << lattice_traj_ptr->target_position()
          << " ds=" << lattice_traj_ptr->target_velocity()
          << " t=" << lattice_traj_ptr->target_time();

    AINFO << "   --- InputPose";
    AINFO << "          XY: " << planning_init_point.ShortDebugString();
    AINFO << "           S: (" << init_s[0] << " " << init_s[1] << ","
          << init_s[2] << ")";
    AINFO << "           L: (" << init_d[0] << " " << init_d[1] << ","
          << init_d[2] << ")";
    AINFO << "   --- TrajectoryPairComponentCost";
    AINFO << "       travel_cost = " << trajectory_pair_cost_components[0];
    AINFO << "       jerk_cost = " << trajectory_pair_cost_components[1];
    AINFO << "       obstacle_cost = " << trajectory_pair_cost_components[2];
    AINFO << "       lateral_cost = " << trajectory_pair_cost_components[3];
    AINFO << "       reference_line_priority_cost = "
          << reference_line_info->PriorityCost();
    AINFO << "   --- Total_Trajectory_Cost = " << trajectory_pair_cost;
    ADEBUG << "   --- OutputTrajectory";
    for (uint i = 0; i < 10; ++i) {
      ADEBUG << combined_trajectory_points[i].ShortDebugString();
    }

    break;
    /*
    auto combined_trajectory_path =
        ptr_debug->mutable_planning_data()->add_trajectory_path();
    for (uint i = 0; i < combined_trajectory_points.size(); ++i) {
      combined_trajectory_path->add_trajectory_point()->CopyFrom(
          combined_trajectory_points[i]);
    }
    combined_trajectory_path->set_lattice_trajectory_cost(trajectory_pair_cost);
    */
  }

  AINFO << "Trajectory_Evaluation_Time="
        << (Clock::NowInSeconds() - current_time) * 1000;
  current_time = Clock::NowInSeconds();

  AINFO << "Step CombineTrajectory Succeeded";

  AINFO << "1d trajectory not valid for constraint ["
        << constraint_failure_count << "] times";
  AINFO << "combined trajectory not valid for ["
        << combined_constraint_failure_count << "] times";
  AINFO << "trajectory not valid for collision [" << collision_failure_count
        << "] times";
  AINFO << "Total_Lattice_Planning_Frame_Time="
        << (Clock::NowInSeconds() - start_time) * 1000;

  if (num_lattice_traj > 0) {
    AINFO << "Planning succeeded";
    num_planning_succeeded_cycles += 1;
    AINFO << "[END]-------------------------------------------------";
    AINFO << "";
    reference_line_info->SetDrivable(true);
    return Status::OK();
  } else {
    AINFO << "Planning failed";
    AINFO << "[END]-------------------------------------------------";
    AINFO << "";
    return Status(ErrorCode::PLANNING_ERROR, "No feasible trajectories");
  }
}

DiscretizedTrajectory LatticePlanner::CombineTrajectory(
    const std::vector<PathPoint>& reference_line, const Curve1d& lon_trajectory,
    const Curve1d& lat_trajectory, const double init_relative_time) const {
  DiscretizedTrajectory combined_trajectory;

  double s0 = lon_trajectory.Evaluate(0, 0.0);
  double s_ref_max = reference_line.back().s();
  double t_param_max = lon_trajectory.ParamLength();
  double s_param_max = lat_trajectory.ParamLength();

  double t_param = 0.0;
  while (t_param < planned_trajectory_time) {
    // linear extrapolation is handled internally in LatticeTrajectory1d;
    // no worry about t_param > lon_trajectory.ParamLength() situation
    double s = lon_trajectory.Evaluate(0, t_param);
    double s_dot = lon_trajectory.Evaluate(1, t_param);
    double s_ddot = lon_trajectory.Evaluate(2, t_param);
    if (s > s_ref_max) {
      break;
    }

    double s_param = s - s0;
    // linear extrapolation is handled internally in LatticeTrajectory1d;
    // no worry about s_param > lat_trajectory.ParamLength() situation
    double d = lat_trajectory.Evaluate(0, s_param);
    double d_prime = lat_trajectory.Evaluate(1, s_param);
    double d_pprime = lat_trajectory.Evaluate(2, s_param);

    PathPoint matched_ref_point =
        ReferenceLineMatcher::MatchToReferenceLine(reference_line, s);

    double x = 0.0;
    double y = 0.0;
    double theta = 0.0;
    double kappa = 0.0;
    double v = 0.0;
    double a = 0.0;

    const double rs = matched_ref_point.s();
    const double rx = matched_ref_point.x();
    const double ry = matched_ref_point.y();
    const double rtheta = matched_ref_point.theta();
    const double rkappa = matched_ref_point.kappa();
    const double rdkappa = matched_ref_point.dkappa();

    std::array<double, 3> s_conditions = {rs, s_dot, s_ddot};
    std::array<double, 3> d_conditions = {d, d_prime, d_pprime};
    CartesianFrenetConverter::frenet_to_cartesian(
        rs, rx, ry, rtheta, rkappa, rdkappa, s_conditions, d_conditions,
        &x, &y, &theta, &kappa, &v, &a);

    TrajectoryPoint trajectory_point;
    trajectory_point.mutable_path_point()->set_x(x);
    trajectory_point.mutable_path_point()->set_y(y);
    trajectory_point.mutable_path_point()->set_theta(theta);
    trajectory_point.mutable_path_point()->set_kappa(kappa);
    trajectory_point.set_v(v);
    trajectory_point.set_a(a);
    trajectory_point.set_relative_time(t_param + init_relative_time);

    combined_trajectory.AppendTrajectoryPoint(trajectory_point);

    t_param = t_param + trajectory_time_resolution;
  }
  return combined_trajectory;
}

DiscretizedTrajectory LatticePlanner::GetFutureTrajectory() const {
  // localization
  const auto& localization =
      AdapterManager::GetLocalization()->GetLatestObserved();
  ADEBUG << "Get localization:" << localization.DebugString();
  std::vector<common::TrajectoryPoint> traj_pts;
  for (const auto& traj_pt : localization.trajectory_point()) {
    traj_pts.emplace_back(traj_pt);
  }
  DiscretizedTrajectory ret(traj_pts);
  return ret;
}

bool LatticePlanner::MapFutureTrajectoryToSL(
    const DiscretizedTrajectory& future_trajectory,
    std::vector<apollo::common::SpeedPoint>* st_points,
    std::vector<apollo::common::FrenetFramePoint>* sl_points,
    ReferenceLineInfo* reference_line_info) {
  return false;
}


}  // namespace planning
}  // namespace apollo
