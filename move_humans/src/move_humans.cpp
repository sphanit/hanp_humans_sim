#define NODE_NAME "move_humans"

#include <geometry_msgs/PoseArray.h>
#include <boost/thread.hpp>

#include "move_humans/move_humans.h"

namespace move_humans {

MoveHumans::MoveHumans(tf::TransformListener &tf)
    : tf_(tf), mhas_(NULL), planner_costmap_ros_(NULL),
      controller_costmap_ros_(NULL),
      planner_loader_("move_humans", "move_humans::PlannerInterface"),
      controller_loader_("move_humans", "move_humans::ControllerInterface"),
      planner_plans_(NULL), latest_plans_(NULL), controller_plans_(NULL),
      run_planner_(false), setup_(false), new_global_plans_(false),
      publish_feedback_(false), p_freq_change_(false), c_freq_change_(false) {
  ros::NodeHandle private_nh("~");

  mhas_ = new MoveHumansActionServer(
      private_nh, "action_server", boost::bind(&MoveHumans::actionCB, this, _1),
      false);

  std::string planner_name, controller_name;
  private_nh.param("planner", planner_name,
                   std::string("move_humans/PlannerInterface"));
  private_nh.param("controller", controller_name,
                   std::string("move_humans/ControllerInterface"));
  private_nh.param("planner_frequency", planner_frequency_, 0.0);
  private_nh.param("controller_frequency", controller_frequency_, 20.0);
  private_nh.param("shutdown_costmaps", shutdown_costmaps_, false);
  private_nh.param("publish_feedback", publish_feedback_, true);

  current_goals_pub_ =
      private_nh.advertise<geometry_msgs::PoseArray>("current_goals", 0);
  clear_costmaps_srv_ = private_nh.advertiseService(
      "clear_costmaps", &MoveHumans::clearCostmapsService, this);

  planner_plans_ = new move_humans::map_pose_vectors();
  latest_plans_ = new move_humans::map_pose_vectors();
  controller_plans_ = new move_humans::map_pose_vectors();

  planner_costmap_ros_ = new costmap_2d::Costmap2DROS("planner_costmap", tf_);
  planner_costmap_ros_->pause();
  if (!loadPlugin(planner_name, planner_, planner_loader_,
                  planner_costmap_ros_)) {
    exit(1);
  }
  controller_costmap_ros_ =
      new costmap_2d::Costmap2DROS("controller_costmap", tf_);
  controller_costmap_ros_->pause();
  if (!loadPlugin(controller_name, controller_, controller_loader_,
                  controller_costmap_ros_)) {
    exit(1);
  }
  planner_costmap_ros_->start();
  controller_costmap_ros_->start();

  if (shutdown_costmaps_) {
    ROS_DEBUG_NAMED(NODE_NAME, "Stopping costmaps initially");
    planner_costmap_ros_->stop();
    controller_costmap_ros_->stop();
  }

  dsrv_ = new dynamic_reconfigure::Server<move_humans::MoveHumansConfig>(
      ros::NodeHandle("~"));
  dynamic_reconfigure::Server<move_humans::MoveHumansConfig>::CallbackType cb =
      boost::bind(&MoveHumans::reconfigureCB, this, _1, _2);
  dsrv_->setCallback(cb);

  planner_thread_ =
      new boost::thread(boost::bind(&MoveHumans::planThread, this));

  mhas_->start();
  ROS_INFO_NAMED(NODE_NAME, "move_humans server started");

  state_ = move_humans::MoveHumansState::IDLE;
}

MoveHumans::~MoveHumans() {
  delete dsrv_;

  if (mhas_ != NULL) {
    delete mhas_;
  }

  if (planner_costmap_ros_ != NULL) {
    delete planner_costmap_ros_;
  }

  if (controller_costmap_ros_ != NULL) {
    delete controller_costmap_ros_;
  }

  planner_thread_->interrupt();
  planner_thread_->join();
  delete planner_thread_;

  delete planner_plans_;
  delete latest_plans_;
  delete controller_plans_;

  planner_.reset();
  controller_.reset();
}

void MoveHumans::reconfigureCB(move_humans::MoveHumansConfig &config,
                               uint32_t level) {
  boost::recursive_mutex::scoped_lock lock(configuration_mutex_);

  if (!setup_) {
    last_config_ = config;
    default_config_ = config;
    setup_ = true;
    return;
  }

  if (config.restore_defaults) {
    config = default_config_;
    config.restore_defaults = false;
  }

  if (planner_frequency_ != config.planner_frequency) {
    planner_frequency_ = config.planner_frequency;
    p_freq_change_ = true;
  }

  if (controller_frequency_ != config.controller_frequency) {
    controller_frequency_ = config.controller_frequency;
    c_freq_change_ = true;
  }

  if (config.planner != last_config_.planner) {
    if (!loadPlugin<move_humans::PlannerInterface>(
            config.planner, planner_, planner_loader_, planner_costmap_ros_)) {
      config.planner = last_config_.planner;
    }
  }

  if (config.controller != last_config_.controller) {
    if (!loadPlugin<move_humans::ControllerInterface>(
            config.controller, controller_, controller_loader_,
            planner_costmap_ros_)) {
      config.controller = last_config_.controller;
    }
  }

  last_config_ = config;
}

void MoveHumans::planThread() {
  ROS_DEBUG_NAMED(NODE_NAME "_plan_thread", "Starting planner thread");
  ros::NodeHandle nh;
  ros::Timer timer;
  bool wait_for_wake = false;
  boost::unique_lock<boost::mutex> lock(planner_mutex_);
  while (nh.ok()) {
    while (wait_for_wake || !run_planner_) {
      ROS_INFO_NAMED(NODE_NAME "_plan_thread", "Planner thread is suspending");
      planner_cond_.wait(lock);
      wait_for_wake = false;
    }
    ros::Time start_time = ros::Time::now();
    auto planner_starts = planner_starts_;
    auto planner_goals = planner_goals_;
    auto planner_sub_goals = planner_sub_goals_;
    lock.unlock();

    ROS_DEBUG_NAMED(NODE_NAME "_plan_thread", "Planning");
    planner_plans_->clear();
    if (nh.ok()) {
      boost::unique_lock<costmap_2d::Costmap2D::mutex_t> costmap_lock(
          *(planner_costmap_ros_->getCostmap()->getMutex()));

      if (planner_costmap_ros_ == NULL) {
        ROS_ERROR_NAMED(NODE_NAME "_plan_thread",
                        "Planner costmap NULL, unable to create plan");
      } else {
        bool planning_success = false;
        if (planner_sub_goals_.size() > 0) {
          planning_success =
              planner_->makePlans(planner_starts, planner_sub_goals_,
                                  planner_goals, *planner_plans_);
        } else {
          planning_success = planner_->makePlans(planner_starts, planner_goals,
                                                 *planner_plans_);
        }
        if (!planning_success) {
          ROS_DEBUG_NAMED(NODE_NAME "_plan_thread",
                          "Planner plugin failed to find plans");
        }
      }
    }

    lock.lock();

    if (planner_plans_->size() > 0) {
      ROS_INFO_NAMED(NODE_NAME "_plan_thread", "Got %lu new plans",
                     planner_plans_->size());
      auto planner_plans = planner_plans_;
      planner_plans_ = latest_plans_;
      latest_plans_ = planner_plans;
      new_global_plans_ = true;

      if (run_planner_) {
        state_ = move_humans::MoveHumansState::CONTROLLING;
        ROS_INFO_NAMED(NODE_NAME, "Changing to CONTROLLING state");
      }
      if (planner_frequency_ <= 0) {
        run_planner_ = false;
      }
    } else {
      if (state_ == move_humans::MoveHumansState::PLANNING) {
        ROS_INFO_NAMED(NODE_NAME "_plan_thread",
                       "No plans calculated, Stopping");
        run_planner_ = false;
        state_ = move_humans::MoveHumansState::IDLE;
      }
    }

    if (planner_frequency_ > 0) {
      ros::Duration sleep_time =
          (start_time + ros::Duration(1.0 / planner_frequency_)) -
          ros::Time::now();
      if (sleep_time > ros::Duration(0.0)) {
        wait_for_wake = true;
        timer = nh.createTimer(sleep_time, &MoveHumans::wakePlanner, this);
      }
    }
  }
  lock.unlock();
}

void MoveHumans::wakePlanner(const ros::TimerEvent &event) {
  planner_cond_.notify_one();
}

void MoveHumans::actionCB(
    const move_humans::MoveHumansGoalConstPtr &move_humans_goal) {
  ROS_INFO_NAMED(NODE_NAME, "Received new planning request");
  move_humans::map_pose starts, goals;
  move_humans::map_pose_vector sub_goals;
  if (!validateGoals(*move_humans_goal, starts, sub_goals, goals)) {
    mhas_->setAborted(move_humans::MoveHumansResult(),
                      "Aborting on invalid request");
    return;
  }
  starts = toGlobaolFrame(starts);
  goals = toGlobaolFrame(goals);
  sub_goals = toGlobaolFrame(sub_goals);

  geometry_msgs::PoseArray current_goals;
  if (goals.size() > 0) {
    current_goals.header.frame_id = goals.begin()->second.header.frame_id;
    for (auto &goal_kv : goals) {
      current_goals.poses.push_back(goal_kv.second.pose);
    }
    current_goals_pub_.publish(current_goals);
  }

  if (shutdown_costmaps_) {
    ROS_DEBUG_NAMED(NODE_NAME,
                    "Starting up costmaps that were shut down previously");
    planner_costmap_ros_->start();
    controller_costmap_ros_->start();
  }

  boost::unique_lock<boost::mutex> lock(planner_mutex_);
  planner_starts_ = starts;
  planner_goals_ = goals;
  planner_sub_goals_ = sub_goals;
  state_ = move_humans::MoveHumansState::PLANNING;
  ROS_INFO_NAMED(NODE_NAME, "Changed to  PLANNING state");
  run_planner_ = true;
  planner_cond_.notify_one();
  lock.unlock();

  ros::NodeHandle nh;
  ros::Rate r(controller_frequency_);
  move_humans::map_pose_vector global_plans;
  while (nh.ok()) {
    if ((mhas_->isPreemptRequested())) {
      if (mhas_->isNewGoalAvailable()) {
        move_humans::map_pose start_poses, goal_poses;
        move_humans::map_pose_vector sub_goal_poses;
        if (!validateGoals(*mhas_->acceptNewGoal(), start_poses, sub_goal_poses,
                           goal_poses)) {
          mhas_->setAborted(move_humans::MoveHumansResult(),
                            "Aborting on invalid request");
          resetState();
          return;
        }

        starts = toGlobaolFrame(start_poses);
        goals = toGlobaolFrame(goal_poses);
        sub_goals = toGlobaolFrame(sub_goal_poses);

        lock.lock();
        planner_starts_ = starts;
        planner_goals_ = goals;
        planner_sub_goals_ = sub_goals;
        state_ = move_humans::MoveHumansState::PLANNING;
        ROS_INFO_NAMED(NODE_NAME, "Changed to  PLANNING state");
        run_planner_ = true;
        planner_cond_.notify_one();
        lock.unlock();
      } else {
        resetState();
        ROS_DEBUG_NAMED(NODE_NAME, "Preempting the current goal");
        mhas_->setPreempted();
        return;
      }
    }

    if (c_freq_change_) {
      ROS_INFO_NAMED(NODE_NAME, "Setting controller frequency to %.2f",
                     controller_frequency_);
      r = ros::Rate(controller_frequency_);
      c_freq_change_ = false;
    }

    if (goals.begin()->second.header.frame_id !=
        planner_costmap_ros_->getGlobalFrameID()) {
      starts = toGlobaolFrame(starts);
      goals = toGlobaolFrame(goals);
      sub_goals = toGlobaolFrame(sub_goals);
      ROS_DEBUG_NAMED(NODE_NAME, "Replanning as the global frame for "
                                 "move_humans has changed, new frame: %s",
                      planner_costmap_ros_->getGlobalFrameID().c_str());

      lock.lock();
      planner_starts_ = starts;
      planner_goals_ = goals;
      planner_sub_goals_ = sub_goals;
      state_ = move_humans::MoveHumansState::PLANNING;
      ROS_INFO_NAMED(NODE_NAME, "Changed to  PLANNING state");
      run_planner_ = true;
      planner_cond_.notify_one();
      lock.unlock();
    }

    if (new_global_plans_) {
      auto controller_plans = controller_plans_;
      boost::unique_lock<boost::mutex> lock(planner_mutex_);
      new_global_plans_ = false;
      controller_plans_ = latest_plans_;
      latest_plans_ = controller_plans;
      lock.unlock();

      current_controller_plans_.clear();
      for (auto plan_vector_kv : *controller_plans_) {
        auto human_id = plan_vector_kv.first;
        auto plan_vector = plan_vector_kv.second;
        ROS_INFO_NAMED(NODE_NAME, "Got %ld plans for %ld human",
                       plan_vector.size(), human_id);
        if (plan_vector.size() > 0) {
          current_controller_plans_[human_id] = plan_vector.front();
        }
      }
      if (!controller_->setPlans(current_controller_plans_)) {
        ROS_ERROR_NAMED(NODE_NAME,
                        "Failed to pass the plans to the controller, aborting");
        mhas_->setAborted(move_humans::MoveHumansResult(),
                          "Failed to pass the plans to the controller");
        resetState();
        return;
      }
    }

    ros::WallTime start = ros::WallTime::now();

    if (executeCycle(goals, global_plans)) {
      return;
    }

    ros::WallDuration t_diff = ros::WallTime::now() - start;
    ROS_DEBUG_NAMED(NODE_NAME, "Full control cycle time: %.9f\n",
                    t_diff.toSec());

    r.sleep();
    if (r.cycleTime() > ros::Duration(1 / controller_frequency_) &&
        state_ == move_humans::MoveHumansState::CONTROLLING) {
      ROS_WARN_NAMED(NODE_NAME, "Control loop missed its desired rate of "
                                "%.4fHz, the loop actually took %.4f seconds",
                     controller_frequency_, r.cycleTime().toSec());
    }
  }

  lock.lock();
  run_planner_ = true;
  planner_cond_.notify_one();
  lock.unlock();

  mhas_->setAborted(move_humans::MoveHumansResult(),
                    "Aborting on the goal because the node has been killed");
}

bool MoveHumans::executeCycle(move_humans::map_pose &goals,
                              move_humans::map_pose_vector &global_plans) {
  boost::recursive_mutex::scoped_lock ecl(configuration_mutex_);

  switch (state_) {
  case move_humans::MoveHumansState::PLANNING: {
    ROS_INFO_NAMED(NODE_NAME, "Waiting for plan, in the planning state");
    break;
  }

  case move_humans::MoveHumansState::CONTROLLING: {
    move_humans::id_vector reached_humans;
    if (!controller_->areGoalsReached(reached_humans)) {
      ROS_INFO_NAMED(NODE_NAME, "Controller failure");
      mhas_->setAborted(move_humans::MoveHumansResult(), "Controller failure");
      resetState();
      return true;
    }

    if (reached_humans.size() > 0) {
      current_controller_plans_.clear();
      for (auto human_id : reached_humans) {
        auto plan_vector_it = controller_plans_->find(human_id);
        if (plan_vector_it != controller_plans_->end()) {
          auto &plan_vector = plan_vector_it->second;
          if (plan_vector.size() > 0) {
            plan_vector.erase(plan_vector.begin());
            if (plan_vector.size() > 0) {
              current_controller_plans_[human_id] = plan_vector.front();
            }
          }
        }
      }
      if (current_controller_plans_.size() > 0) {
        if (!controller_->setPlans(current_controller_plans_)) {
          ROS_ERROR_NAMED(
              NODE_NAME,
              "Failed to pass the plans to the controller, aborting");
        }
      }
    }

    bool all_human_goals_reached = true;
    for (auto &plan_vector_kv : *controller_plans_) {
      if (plan_vector_kv.second.size() > 0) {
        all_human_goals_reached = false;
      }
    }
    if (all_human_goals_reached) {
      ROS_INFO_NAMED(NODE_NAME, "All goals reached!");
      mhas_->setSucceeded(move_humans::MoveHumansResult(), "Goals reached");
      resetState();
      return true;
    }

    boost::unique_lock<costmap_2d::Costmap2D::mutex_t> costmap_lock(
        *(controller_costmap_ros_->getCostmap()->getMutex()));
    move_humans::map_pose humans;
    if (controller_->computeHumansStates(humans)) {
      ROS_DEBUG_NAMED(NODE_NAME,
                      "Got valid human positions from the controller");
      if (publish_feedback_) {
        move_humans::MoveHumansFeedback feedback;
        for (auto &pose_kv : humans) {
          move_humans::HumanPose human_pose;
          human_pose.human_id = pose_kv.first;
          human_pose.pose = pose_kv.second;
          feedback.current_poses.push_back(human_pose);
        }
        mhas_->publishFeedback(feedback);
      }
    } else {
      ROS_DEBUG_NAMED(NODE_NAME,
                      "The controller could not calculate new human positions");
      mhas_->setAborted(
          move_humans::MoveHumansResult(),
          "The controller could not calculate new human positions");
      resetState();
    }
    costmap_lock.unlock();
    break;
  }

  case move_humans::MoveHumansState::IDLE: {
    ROS_INFO_NAMED(NODE_NAME, "In IDLE state");
    ROS_DEBUG_NAMED(NODE_NAME, "The planner could not calculate plans");
    mhas_->setAborted(move_humans::MoveHumansResult(),
                      "The planner could not calculate plans");
    resetState();
    return true;
  }

  default: {
    ROS_ERROR_NAMED(NODE_NAME, "This case should never be reached, aborting");
    mhas_->setAborted(move_humans::MoveHumansResult(),
                      "This case should never be reached, aborting");
    resetState();
    return true;
  }
  }
  return false;
}

void MoveHumans::resetState() {
  boost::unique_lock<boost::mutex> lock(planner_mutex_);
  run_planner_ = false;
  state_ = move_humans::MoveHumansState::IDLE;
  lock.unlock();

  if (shutdown_costmaps_) {
    ROS_DEBUG_NAMED(NODE_NAME, "Stopping costmaps");
    planner_costmap_ros_->stop();
    controller_costmap_ros_->stop();
  }
}

template <typename T>
bool MoveHumans::loadPlugin(const std::string plugin_name,
                            boost::shared_ptr<T> &plugin,
                            pluginlib::ClassLoader<T> &plugin_loader,
                            costmap_2d::Costmap2DROS *plugin_costmap) {
  boost::shared_ptr<T> old_plugin = plugin;
  ROS_INFO_NAMED(NODE_NAME, "Loading plugin %s", plugin_name.c_str());
  try {
    plugin = plugin_loader.createInstance(plugin_name);

    boost::unique_lock<boost::mutex> lock(planner_mutex_);
    planner_plans_->clear();
    controller_plans_->clear();
    latest_plans_->clear();
    plugin->initialize(plugin_loader.getName(plugin_name), &tf_,
                       plugin_costmap);
    lock.unlock();

    resetState();
  } catch (const pluginlib::PluginlibException &ex) {
    ROS_FATAL_NAMED(NODE_NAME, "Failed to create plugin %s. Exception: %s",
                    plugin_name.c_str(), ex.what());
    plugin = old_plugin;
    return false;
  }
  return true;
}

bool MoveHumans::validateGoals(const move_humans::MoveHumansGoal &mh_goal,
                               move_humans::map_pose &starts,
                               move_humans::map_pose_vector &sub_goals,
                               move_humans::map_pose &goals) {
  if (mh_goal.start_poses.size() == 0 || mh_goal.goal_poses.size() == 0 ||
      mh_goal.start_poses.size() != mh_goal.goal_poses.size()) {
    ROS_ERROR_NAMED(NODE_NAME, "Number of start and goals poses are not equal, "
                               "aborting on planning request");
    return false;
  }

  auto frame_id = mh_goal.start_poses[0].pose.header.frame_id;
  for (auto &s_pose : mh_goal.start_poses) {
    if (s_pose.pose.header.frame_id != frame_id) {
      ROS_ERROR_NAMED(
          NODE_NAME,
          "All start, goal and sub-goal positions must be in same frame");
      return false;
    }
  }
  for (auto &g_pose : mh_goal.goal_poses) {
    if (g_pose.pose.header.frame_id != frame_id) {
      ROS_ERROR_NAMED(
          NODE_NAME,
          "All start, goal and sub-goal positions must be in same frame");
      return false;
    }
  }
  for (auto &sg_poses : mh_goal.sub_goal_poses) {
    for (auto &sg_pose : sg_poses.poses) {
      if (sg_pose.header.frame_id != frame_id) {
        ROS_ERROR_NAMED(
            NODE_NAME,
            "All start, goal and sub-goal positions must be in same frame");
        return false;
      }
    }
  }

  for (auto &start : mh_goal.start_poses) {
    if (!isQuaternionValid(start.pose.pose.orientation)) {
      ROS_ERROR_NAMED(NODE_NAME, "Not planning for human %lu, start pose was "
                                 "sent with an invalid quaternion",
                      start.human_id);
      continue;
    }
    starts[start.human_id] = start.pose;
  }
  for (auto &goal : mh_goal.goal_poses) {
    if (!isQuaternionValid(goal.pose.pose.orientation)) {
      ROS_ERROR_NAMED(NODE_NAME, "Not planning for human %lu, goal pose was "
                                 "sent with an invalid quaternion",
                      goal.human_id);
      continue;
    }
    goals[goal.human_id] = goal.pose;
  }
  for (auto &sub_goal_poses : mh_goal.sub_goal_poses) {
    move_humans::pose_vector valid_sub_goals;
    for (auto &sub_goal : sub_goal_poses.poses) {
      if (!isQuaternionValid(sub_goal.pose.orientation)) {
        ROS_ERROR_NAMED(NODE_NAME, "Removing a sub-goals for human %lu, it was "
                                   "sent with an invalid quaternion",
                        sub_goal_poses.human_id);
      } else {
        valid_sub_goals.push_back(sub_goal);
      }
    }
    if (valid_sub_goals.size() > 0) {
      sub_goals[sub_goal_poses.human_id] = valid_sub_goals;
    }
  }

  auto its = starts.begin();
  while (its != starts.end()) {
    if (goals.find(its->first) == goals.end()) {
      sub_goals.erase(its->first);
      its = starts.erase(its);
    } else {
      ++its;
    }
  }
  auto itg = goals.begin();
  while (itg != goals.end()) {
    if (starts.find(itg->first) == starts.end()) {
      sub_goals.erase(itg->first);
      itg = goals.erase(itg);
    } else {
      ++itg;
    }
  }

  if (starts.size() == 0 || goals.size() == 0) {
    ROS_ERROR_NAMED(NODE_NAME,
                    "Aborting on request as not valid start-goal pair found");
    return false;
  }

  return true;
}

bool MoveHumans::isQuaternionValid(const geometry_msgs::Quaternion &q) {
  if (!std::isfinite(q.x) || !std::isfinite(q.y) || !std::isfinite(q.z) ||
      !std::isfinite(q.w)) {
    ROS_ERROR_NAMED(NODE_NAME, "Quaternion has nans or infs");
    return false;
  }

  tf::Quaternion tf_q(q.x, q.y, q.z, q.w);
  if (tf_q.length2() < 1e-6) {
    ROS_ERROR_NAMED(NODE_NAME, "Quaternion has length close to zero");
    return false;
  }
  tf_q.normalize();

  tf::Vector3 up(0, 0, 1);
  double dot = up.dot(up.rotate(tf_q.getAxis(), tf_q.getAngle()));
  if (fabs(dot - 1) > 1e-3) {
    ROS_ERROR("Quaternion is invalid, the z-axis of the quaternion must be "
              "close to vertical");
    return false;
  }

  return true;
}

move_humans::map_pose
MoveHumans::toGlobaolFrame(const move_humans::map_pose &pose_map) {
  move_humans::map_pose global_pose_map;
  std::string global_frame = planner_costmap_ros_->getGlobalFrameID();
  for (auto &pose_kv : pose_map) {
    tf::Stamped<tf::Pose> tf_pose, global_tf_pose;
    poseStampedMsgToTF(pose_kv.second, tf_pose);

    tf_pose.stamp_ = ros::Time();
    try {
      tf_.transformPose(global_frame, tf_pose, global_tf_pose);
    } catch (tf::TransformException &ex) {
      ROS_WARN("Failed to transform pose from %s into the %s frame: %s",
               tf_pose.frame_id_.c_str(), global_frame.c_str(), ex.what());
      global_tf_pose = tf_pose;
    }

    geometry_msgs::PoseStamped global_pose;
    tf::poseStampedTFToMsg(global_tf_pose, global_pose);
    global_pose_map[pose_kv.first] = global_pose;
  }
  return global_pose_map;
}

move_humans::map_pose_vector MoveHumans::toGlobaolFrame(
    const move_humans::map_pose_vector &pose_vector_map) {
  move_humans::map_pose_vector global_pose_vector_map;
  std::string global_frame = planner_costmap_ros_->getGlobalFrameID();
  for (auto &pose_vector_kv : pose_vector_map) {
    move_humans::pose_vector global_pose_vector;
    for (auto &pose : pose_vector_kv.second) {
      tf::Stamped<tf::Pose> tf_pose, global_tf_pose;
      poseStampedMsgToTF(pose, tf_pose);

      tf_pose.stamp_ = ros::Time();
      try {
        tf_.transformPose(global_frame, tf_pose, global_tf_pose);
      } catch (tf::TransformException &ex) {
        ROS_WARN("Failed to transform pose from %s into the %s frame: %s",
                 tf_pose.frame_id_.c_str(), global_frame.c_str(), ex.what());
        global_tf_pose = tf_pose;
      }

      geometry_msgs::PoseStamped global_pose;
      tf::poseStampedTFToMsg(global_tf_pose, global_pose);
      global_pose_vector.push_back(global_pose);
    }
    global_pose_vector_map[pose_vector_kv.first] = global_pose_vector;
  }
  return global_pose_vector_map;
}

bool MoveHumans::clearCostmapsService(std_srvs::Empty::Request &req,
                                      std_srvs::Empty::Response &resp) {
  planner_costmap_ros_->resetLayers();
  controller_costmap_ros_->resetLayers();
  return true;
}
};
