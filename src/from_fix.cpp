/**
 *
 *  \file
 *  \brief Node receives NavSatFix messages and publishes ENU Odometry messages.
 *  \author Mike Purvis <mpurvis@clearpathrobotics.com>
 *  \author Ryan Gariepy <rgariepy@clearpathrobotics.com>
 *
 *  \copyright Copyright (c) 2013, Clearpath Robotics, Inc. 
 *  All Rights Reserved
 * 
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in the
 *       documentation and/or other materials provided with the distribution.
 *     * Neither the name of Clearpath Robotics, Inc. nor the
 *       names of its contributors may be used to endorse or promote products
 *       derived from this software without specific prior written permission.
 * 
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL CLEARPATH ROBOTICS, INC. BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 * 
 * Please send comments, questions, or patches to skynet@clearpathrobotics.com 
 *
 */

#include <boost/bind.hpp>
#include <string>

#include "ros/ros.h"
#include "sensor_msgs/NavSatFix.h"
#include "nav_msgs/Odometry.h"
#include "enu/ToENU.h"
#include "enu/enu.h"  // ROS wrapper for conversion functions


//msoler {
#include "sensor_msgs/Imu.h"
#include "geometry_msgs/Point.h"
#include <tf/transform_broadcaster.h>
#include <tf/transform_listener.h>

ros::Subscriber imu_sub;
sensor_msgs::ImuConstPtr imu_curr_ptr;
//}

void initialize_datum(const sensor_msgs::NavSatFix& fix,
                      const ros::Publisher& pub_datum,
                      sensor_msgs::NavSatFix* datum_ptr) {
  ros::NodeHandle pnh("~");

  // Local ENU coordinates are with respect to a plane which is
  // perpendicular to a particular lat/lon. This logic decides
  // whether to use a specific passed-in point (typical for
  // repeated tests in a locality) or just an arbitrary starting
  // point (more ad-hoc type scenarios).
  if (pnh.hasParam("datum_latitude") &&
      pnh.hasParam("datum_longitude") &&
      pnh.hasParam("datum_altitude")) {
    pnh.getParam("datum_latitude", datum_ptr->latitude);
    pnh.getParam("datum_longitude", datum_ptr->longitude);
    pnh.getParam("datum_altitude", datum_ptr->altitude);
    ROS_INFO("Using datum provided by node parameters.");
  } else {
    datum_ptr->latitude = fix.latitude;
    datum_ptr->longitude = fix.longitude;
    datum_ptr->altitude = fix.altitude;
    ROS_INFO("Using initial position fix as datum.");
  }
  pub_datum.publish(*datum_ptr);
}


static void imu_callback(const sensor_msgs::ImuConstPtr imu_msg)
{
  imu_curr_ptr = imu_msg;
}




static void handle_fix(const sensor_msgs::NavSatFixConstPtr fix_ptr,
                       const ros::Publisher& pub_odom,
                       const ros::Publisher& pub_datum,
                       const std::string& output_tf_frame,
                       const std::string& robot_frame_id,
                       const std::string& sensor_frame_id,
                       const double invalid_covariance_value,
                       const double scale_covariance,
                       const double lock_altitude) {
  static sensor_msgs::NavSatFix datum;
  static bool have_datum = false;

  if (!have_datum) {
    initialize_datum(*fix_ptr, pub_datum, &datum);
    have_datum = true;
  }

  // Convert the input latlon into north-east-down (NED) via an ECEF
  // transformation and an ECEF-formatted datum point
  nav_msgs::Odometry odom;
  geometry_msgs::Point gps_pos_sensor_frame;
  enu::fix_to_point(*fix_ptr, datum, &gps_pos_sensor_frame);
  if (lock_altitude!=-1) 
   gps_pos_sensor_frame.z = lock_altitude;

  // msoler from antenna frame to baselink {
  static bool transform_found = false;
  static tf::TransformListener listener;
  static tf::StampedTransform sensor_to_base, base_to_map_orientation;

  if(!transform_found)
  {
    listener.waitForTransform(robot_frame_id, sensor_frame_id, ros::Time(0), ros::Duration(1.0));
    //  {
    try
    {
      listener.lookupTransform(robot_frame_id, sensor_frame_id, ros::Time(0), sensor_to_base);
      transform_found=true;
    }

    catch (tf::TransformException ex)
    {
      ROS_ERROR("%s",ex.what());
      transform_found=false;
      return;
    }
  }

  double roll, pitch, yaw;
  if(imu_curr_ptr)
  {
    tf::Quaternion q;
    tf::quaternionMsgToTF(imu_curr_ptr->orientation, q);
    tf::Matrix3x3 m(q);
    m.getRPY(roll, pitch, yaw);
  }
  else
  {
    ROS_ERROR("No Imu data skipping....");
    return;
  }

  //transform to map frame
  //tf::Vector3 p( gps_pos_sensor_frame.x, gps_pos_sensor_frame.y, gps_pos_sensor_frame.z );
  //tf::Vector3  gps_tf_robot_frame = sensor_to_base * p;

//  odom.pose.pose.position.x = gps_tf_robot_frame.x();
//  odom.pose.pose.position.y = gps_tf_robot_frame.y();
//  odom.pose.pose.position.z = gps_tf_robot_frame.z();

  tf::Vector3 sensor_off = sensor_to_base.getOrigin();

  ROS_DEBUG_STREAM("sensor_to_base x :"<< sensor_off.x() << " y: " << sensor_off.y() << " Heading: " << yaw*180/M_PI);
  odom.pose.pose.position.x = gps_pos_sensor_frame.x + (sensor_off.x() * cos(yaw) - sensor_off.y() * sin(yaw)) ;
  odom.pose.pose.position.y = gps_pos_sensor_frame.y + (sensor_off.x() * sin(yaw) + sensor_off.y() * cos(yaw)) ;
  odom.pose.pose.position.z = gps_pos_sensor_frame.z;
  // } msoler

  odom.header.stamp = fix_ptr->header.stamp;
  odom.header.frame_id = output_tf_frame;  // Name of output tf frame
  odom.child_frame_id = robot_frame_id;  // robot frame id (already tranformed)

  //Initialize orientation to zero (GPS orientation is N/A)
  odom.pose.pose.orientation.x = 0;
  odom.pose.pose.orientation.y = 0;
  odom.pose.pose.orientation.z = 0;
  odom.pose.pose.orientation.w = 1;

  // We only need to populate the diagonals of the covariance matrix; the
  // rest initialize to zero automatically, which is correct as the
  // dimensions of the state are independent.
  odom.pose.covariance[0] = fix_ptr->position_covariance[0]*scale_covariance;
  odom.pose.covariance[7] = fix_ptr->position_covariance[4]*scale_covariance;
  odom.pose.covariance[14] = fix_ptr->position_covariance[8]*scale_covariance;

  // Do not use orientation dimensions from GPS.
  // (-1 is an invalid covariance and standard ROS practice to set as invalid.)
  odom.pose.covariance[21] = invalid_covariance_value;
  odom.pose.covariance[28] = invalid_covariance_value;
  odom.pose.covariance[35] = invalid_covariance_value;

  pub_odom.publish(odom);
}

bool toENUService(const enu::ToENU::Request& req, enu::ToENU::Response& resp) {
  enu::fix_to_point(req.llh, req.datum, &(resp.enu));
  return true;
}

int main(int argc, char **argv) {
  ros::init(argc, argv, "from_fix");
  ros::NodeHandle n;
  ros::NodeHandle pnh("~");

  std::string output_tf_frame;
  pnh.param<std::string>("output_frame_id", output_tf_frame, "odom");

  // msoler {
  std::string sensor_frame_id, robot_frame_id;
  pnh.param<std::string>("robot_frame_id", robot_frame_id, "base_link");
  pnh.param<std::string>("sensor_frame_id", sensor_frame_id, "gps");
  // }

  double invalid_covariance_value;
  double scale_covariance;
  double lock_altitude;
  pnh.param<double>("invalid_covariance_value", invalid_covariance_value, -1.0);  // -1 is ROS convention.  1e6 is robot_pose_ekf convention
  pnh.param<double>("scale_covariance", scale_covariance,1.0);
  pnh.param<double>("lock_altitude", lock_altitude, -1.0); //if -1 then use ENU altitude, otherwise lock the altitude at the provided value

  // Initialize publishers, and pass them into the handler for
  // the subscriber.
  ros::Publisher pub_odom = n.advertise<nav_msgs::Odometry>("enu", 5);
  ros::Publisher pub_datum = n.advertise<sensor_msgs::NavSatFix>("enu_datum", 5, true);
  ros::Subscriber sub = n.subscribe<sensor_msgs::NavSatFix>("fix", 5,
      boost::bind(handle_fix, _1, pub_odom, pub_datum, output_tf_frame, robot_frame_id, sensor_frame_id, invalid_covariance_value,scale_covariance, lock_altitude));

  imu_sub = n.subscribe<sensor_msgs::Imu>("imu/data", 10, imu_callback);
  ros::ServiceServer srv = n.advertiseService<enu::ToENU::Request, enu::ToENU::Response> ("ToENU", toENUService);

  // modificar a 10 hz o així
  ros::spin();
  return 0;
}

