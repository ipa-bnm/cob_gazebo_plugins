/*********************************************************************
 * Software License Agreement (BSD License)
 *
 *  Copyright (c) 2013, Open Source Robotics Foundation
 *  Copyright (c) 2013, The Johns Hopkins University
 *  Copyright (c) 2014, Fraunhofer IPA
 *  All rights reserved.
 *
 *  Redistribution and use in source and binary forms, with or without
 *  modification, are permitted provided that the following conditions
 *  are met:
 *
 *   * Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above
 *     copyright notice, this list of conditions and the following
 *     disclaimer in the documentation and/or other materials provided
 *     with the distribution.
 *   * Neither the name of the Open Source Robotics Foundation
 *     nor the names of its contributors may be
 *     used to endorse or promote products derived
 *     from this software without specific prior written permission.
 *
 *  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *  "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *  LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 *  FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 *  COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 *  INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 *  BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 *  LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 *  CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 *  LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 *  ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 *  POSSIBILITY OF SUCH DAMAGE.
 *********************************************************************/

/* Author: Dave Coleman, Jonathan Bohren, Felix Messmer
   Desc:   Gazebo plugin for ros_control that allows 'hardware_interfaces' to be plugged in
           using pluginlib. It extends gazebo_ros_control_plugin with harware_interface switching capability.
*/

#include <cob_gazebo_ros_control/hwi_switch_gazebo_ros_control_plugin.h>
#include <urdf/model.h>


namespace cob_gazebo_ros_control
{

// Overloaded Gazebo entry point
void HWISwitchGazeboRosControlPlugin::Load(gazebo::physics::ModelPtr parent, sdf::ElementPtr sdf)
{
  ROS_INFO_STREAM_NAMED("cob_gazebo_ros_control","Loading cob_gazebo_ros_control plugin");
  enable_joint_filtering_ = false;

  // Save pointers to the model
  parent_model_ = parent;
  sdf_ = sdf;

  // Error message if the model couldn't be found
  if (!parent_model_)
  {
    ROS_ERROR_STREAM_NAMED("cob_gazebo_ros_control","parent model is NULL");
    return;
  }

  // Check that ROS has been initialized
  if(!ros::isInitialized())
  {
    ROS_FATAL_STREAM_NAMED("cob_gazebo_ros_control","A ROS node for Gazebo has not been initialized, unable to load plugin. "
      << "Load the Gazebo system plugin 'libgazebo_ros_api_plugin.so' in the gazebo_ros package)");
    return;
  }

  // Get namespace for nodehandle
  if(sdf_->HasElement("robotNamespace"))
  {
    robot_namespace_ = sdf_->GetElement("robotNamespace")->Get<std::string>();
  }
  else
  {
    robot_namespace_ = parent_model_->GetName(); // default
  }

  // Get robot_description ROS param name
  if (sdf_->HasElement("robotParam"))
  {
    robot_description_ = sdf_->GetElement("robotParam")->Get<std::string>();
  }
  else
  {
    robot_description_ = "robot_description"; // default
  }

  // Get the robot simulation interface type
  if(sdf_->HasElement("robotSimType"))
  {
    //robot_hw_sim_type_str_ = sdf_->Get<std::string>("robotSimType");
    robot_hw_sim_type_str_ = "cob_gazebo_ros_control/HWISwitchRobotHWSim";
    ROS_WARN_STREAM_NAMED("cob_gazebo_ros_control","Tag 'robotSimType' is currently ignored. Using default plugin for RobotHWSim \""<<robot_hw_sim_type_str_<<"\"");
  }
  else
  {
    robot_hw_sim_type_str_ = "cob_gazebo_ros_control/HWISwitchRobotHWSim";
    ROS_DEBUG_STREAM_NAMED("cob_gazebo_ros_control","Using default plugin for RobotHWSim (none specified in URDF/SDF)\""<<robot_hw_sim_type_str_<<"\"");
  }

  // Get the Gazebo simulation period
  ros::Duration gazebo_period(parent_model_->GetWorld()->GetPhysicsEngine()->GetMaxStepSize());

  // Decide the plugin control period
  if(sdf_->HasElement("controlPeriod"))
  {
    control_period_ = ros::Duration(sdf_->Get<double>("controlPeriod"));

    // Check the period against the simulation period
    if( control_period_ < gazebo_period )
    {
      ROS_ERROR_STREAM_NAMED("cob_gazebo_ros_control","Desired controller update period ("<<control_period_
        <<" s) is faster than the gazebo simulation period ("<<gazebo_period<<" s).");
    }
    else if( control_period_ > gazebo_period )
    {
      ROS_WARN_STREAM_NAMED("cob_gazebo_ros_control","Desired controller update period ("<<control_period_
        <<" s) is slower than the gazebo simulation period ("<<gazebo_period<<" s).");
    }
  }
  else
  {
    control_period_ = gazebo_period;
    ROS_DEBUG_STREAM_NAMED("cob_gazebo_ros_control","Control period not found in URDF/SDF, defaulting to Gazebo period of "
      << control_period_);
  }

  // Determine whether to filter joints
  if(sdf_->HasElement("filterJointsParam"))
  {
    enable_joint_filtering_ = true;
    filterJointsParam_ = sdf_->Get<std::string>("filterJointsParam");
    ROS_INFO_STREAM_NAMED("cob_gazebo_ros_control","Enable joint-filtering. Using joint_names from parameter: \""<<filterJointsParam_<<"\"");
  }
  else
  {
    enable_joint_filtering_ = false;
    filterJointsParam_ = "";
    ROS_INFO_STREAM_NAMED("cob_gazebo_ros_control","Joint-filtering is disabled. The plugin will set up JointHandles for all joints!");
  }


  // Get parameters/settings for controllers from ROS param server
  model_nh_ = ros::NodeHandle(robot_namespace_);
  ROS_INFO_NAMED("cob_gazebo_ros_control", "Starting cob_gazebo_ros_control plugin in namespace: %s", robot_namespace_.c_str());

  // Read urdf from ros parameter server then
  // setup actuators and mechanism control node.
  // This call will block if ROS is not properly initialized.
  const std::string urdf_string = getURDF(robot_description_);
  if (!parseTransmissionsFromURDF(urdf_string))
  {
    ROS_ERROR_NAMED("cob_gazebo_ros_control", "Error parsing URDF in cob_gazebo_ros_control plugin, plugin not active.\n");
    return;
  }

  // Load the RobotHWSim abstraction to interface the controllers with the gazebo model
  try
  {
    //robot_hw_sim_loader_.reset
      //(new pluginlib::ClassLoader<gazebo_ros_control::RobotHWSim>
        //("gazebo_ros_control",
          //"gazebo_ros_control::RobotHWSim"));

    //robot_hw_sim_ = robot_hw_sim_loader_->createInstance(robot_hw_sim_type_str_);

    hwi_switch_robot_hw_sim_.reset(new cob_gazebo_ros_control::HWISwitchRobotHWSim());


    urdf::Model urdf_model;
    const urdf::Model *const urdf_model_ptr = urdf_model.initString(urdf_string) ? &urdf_model : NULL;

    if(enable_joint_filtering_)
    {
      if(!hwi_switch_robot_hw_sim_->enableJointFiltering(model_nh_, filterJointsParam_))
      {
        ROS_FATAL_STREAM_NAMED("cob_gazebo_ros_control","Joint-filtering was enabled but param '"<<filterJointsParam_<<"' was not found");
        return;
      }
    }

    if(!hwi_switch_robot_hw_sim_->initSim(robot_namespace_, model_nh_, parent_model_, urdf_model_ptr, transmissions_))
    {
      ROS_FATAL_NAMED("cob_gazebo_ros_control","Could not initialize robot simulation interface");
      return;
    }

    // Create the controller manager
    ROS_DEBUG_STREAM_NAMED("cob_gazebo_ros_control","Loading controller_manager");
    controller_manager_.reset
      (new controller_manager::ControllerManager(hwi_switch_robot_hw_sim_.get(), model_nh_));

    // Listen to the update event. This event is broadcast every simulation iteration.
    update_connection_ =
      gazebo::event::Events::ConnectWorldUpdateBegin
      (boost::bind(&HWISwitchGazeboRosControlPlugin::Update, this));

  }
  catch(pluginlib::LibraryLoadException &ex)
  {
    ROS_FATAL_STREAM_NAMED("cob_gazebo_ros_control","Failed to create robot simulation interface loader: "<<ex.what());
  }

  ROS_INFO_NAMED("cob_gazebo_ros_control", "Loaded cob_gazebo_ros_control.");
}


// Called by the world update start event
void HWISwitchGazeboRosControlPlugin::Update()
{
  // Get the simulation time and period
  gazebo::common::Time gz_time_now = parent_model_->GetWorld()->GetSimTime();
  ros::Time sim_time_ros(gz_time_now.sec, gz_time_now.nsec);
  ros::Duration sim_period = sim_time_ros - last_update_sim_time_ros_;

  // Check if we should update the controllers
  if(sim_period >= control_period_) {
    // Store this simulation time
    last_update_sim_time_ros_ = sim_time_ros;

    // Update the robot simulation with the state of the gazebo model
    hwi_switch_robot_hw_sim_->readSim(sim_time_ros, sim_period);

    // Compute the controller commands
    controller_manager_->update(sim_time_ros, sim_period);
  }

  // Update the gazebo model with the result of the controller
  // computation
  hwi_switch_robot_hw_sim_->writeSim(sim_time_ros, sim_time_ros - last_write_sim_time_ros_);
  last_write_sim_time_ros_ = sim_time_ros;
}

// Register this plugin with the simulator
GZ_REGISTER_MODEL_PLUGIN(HWISwitchGazeboRosControlPlugin);
} // namespace
