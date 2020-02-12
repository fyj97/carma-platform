/*
 * Copyright (C) 2020 LEIDOS.
 *
 * Licensed under the Apache License, Version 2.0 (the "License"); you may not
 * use this file except in compliance with the License. You may obtain a copy of
 * the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. See the
 * License for the specific language governing permissions and limitations under
 * the License.
 */

#include "route_generator_worker.h"

namespace route {

    RouteGeneratorWorker::RouteGeneratorWorker(tf2_ros::Buffer& tf_buffer, carma_wm::WorldModelConstPtr wm) :
                                               tf_tree_(tf_buffer), world_model_(wm), new_route_msg_generated_(false) { }
    
    lanelet::Optional<lanelet::routing::Route> RouteGeneratorWorker::routing(const lanelet::BasicPoint2d start,
                                                                             const std::vector<lanelet::BasicPoint2d>& via,
                                                                             const lanelet::BasicPoint2d end,
                                                                             const lanelet::LaneletMapConstPtr map_pointer,
                                                                             const carma_wm::LaneletRoutingGraphConstPtr graph_pointer) const
    {
        // find start lanelet
        auto start_lanelet_vector = lanelet::geometry::findNearest(map_pointer->laneletLayer, start, 1);
        // check if there are any lanelets in the map
        if(start_lanelet_vector.size() == 0)
        {
            ROS_ERROR_STREAM("Found no lanelets in the map. Routing cannot be done.");
            return lanelet::Optional<lanelet::routing::Route>();
        }
        // extract starting lanelet
        auto start_lanelet = lanelet::ConstLanelet(start_lanelet_vector[0].second.constData());
        // find end lanelet
        auto end_lanelet_vector = lanelet::geometry::findNearest(map_pointer->laneletLayer, end, 1);
        // extract end lanelet
        auto end_lanelet = lanelet::ConstLanelet(end_lanelet_vector[0].second.constData());
        // find all via lanelets
        lanelet::ConstLanelets via_lanelets_vector;
        for(lanelet::BasicPoint2d point : via)
        {
            auto via_lanelet_vector = lanelet::geometry::findNearest(map_pointer->laneletLayer, point, 1);
            via_lanelets_vector.push_back(lanelet::ConstLanelet(via_lanelet_vector[0].second.constData()));
        }
        // routing
        return graph_pointer->getRouteVia(start_lanelet, via_lanelets_vector, end_lanelet);
    }

    bool RouteGeneratorWorker::get_available_route_cb(cav_srvs::GetAvailableRoutesRequest& req, cav_srvs::GetAvailableRoutesResponse& resp)
    {
        // only allow to get route names after entering route selection state
        if(this->rs_worker_.get_route_state() == RouteStateWorker::RouteState::ROUTE_SELECTION)
        {
            boost::filesystem::path route_path_object(this->route_file_path_);
            if(boost::filesystem::exists(route_path_object))
            {
                boost::filesystem::directory_iterator end_point;
                // read all route files in the given directory
                for(boost::filesystem::directory_iterator itr(route_path_object); itr != end_point; ++itr)
                {
                    if(!boost::filesystem::is_directory(itr->status()))
                    {
                        auto full_file_name = itr->path().filename().generic_string();
                        cav_msgs::Route route_msg;
                        // assume route files ending with ".csv", before that is the actual route name
                        route_msg.route_name = full_file_name.substr(0, full_file_name.find(".csv"));
                        resp.availableRoutes.push_back(route_msg);
                    }
                }
            }
            return true;
        }
        // calling service fails because the state is not ready
        return false;
    }

    void RouteGeneratorWorker::set_route_file_path(const std::string& path)
    {
        this->route_file_path_ = path;
        // after route path is set, worker will able to transit state and provide route selection service
        this->rs_worker_.on_route_event(RouteStateWorker::RouteEvent::LOAD_ROUTE_FILES);
        publish_route_event(cav_msgs::RouteEvent::LOAD_ROUTE_FILES);
    }

    bool RouteGeneratorWorker::set_active_route_cb(cav_srvs::SetActiveRouteRequest &req, cav_srvs::SetActiveRouteResponse &resp)
    {
        // only allow activate a new route in route selection state
        if(this->rs_worker_.get_route_state() == RouteStateWorker::RouteState::ROUTE_SELECTION)
        {
            // entering to routing state once destinations are picked
            this->rs_worker_.on_route_event(RouteStateWorker::RouteEvent::ROUTE_SELECTED);
            publish_route_event(cav_msgs::RouteEvent::ROUTE_SELECTED);
            // load destination points in ECEF frame
            auto destination_points = load_route_destinations_in_ecef(req.routeID);
            // Check if route file are valid with at least one starting points and one destination points
            if(destination_points.size() < 2)
            {
                ROS_ERROR_STREAM("Selected route file conatins 1 or less points. Routing cannot be completed.");
                resp.errorStatus = cav_srvs::SetActiveRouteResponse::ROUTE_FILE_ERROR;
                this->rs_worker_.on_route_event(RouteStateWorker::RouteEvent::ROUTING_FAILURE);
                publish_route_event(cav_msgs::RouteEvent::ROUTING_FAILURE);
                return false;
            }
            // get transform from ECEF(earth) to local map frame
            tf2::Transform map_in_earth;
            try
            {
                tf2::convert(tf_tree_.lookupTransform("earth", "map", ros::Time(0)).transform, map_in_earth);
            }
            catch (tf2::TransformException &ex)
            {
                ROS_ERROR_STREAM("Could not lookup transform with exception " << ex.what());
                resp.errorStatus = cav_srvs::SetActiveRouteResponse::TRANSFORM_ERROR;
                this->rs_worker_.on_route_event(RouteStateWorker::RouteEvent::ROUTING_FAILURE);
                publish_route_event(cav_msgs::RouteEvent::ROUTING_FAILURE);
                return false;
            }
            // convert points in ECEF to map frame
            auto destination_points_in_map = transform_to_map_frame(destination_points, map_in_earth);
            // get route graph from world model object
            auto p = world_model_->getMapRoutingGraph();
            // generate a route
            auto route = routing(destination_points_in_map.front(),
                                std::vector<lanelet::BasicPoint2d>(destination_points_in_map.begin() + 1, destination_points_in_map.end() - 1),
                                destination_points_in_map.back(),
                                world_model_->getMap(), world_model_->getMapRoutingGraph());
            // check if route successed
            if(!route)
            {
                ROS_ERROR_STREAM("Cannot find a route passing all destinations.");
                resp.errorStatus = cav_srvs::SetActiveRouteResponse::ROUTING_FAILURE;
                this->rs_worker_.on_route_event(RouteStateWorker::RouteEvent::ROUTING_FAILURE);
                publish_route_event(cav_msgs::RouteEvent::ROUTING_FAILURE);
                return false;
            }
            // update route message
            route_msg_ = compose_route_msg(route);
            route_msg_.header.stamp = ros::Time::now();
            route_msg_.header.frame_id = "map";
            route_msg_.route_name = req.routeID;
            // since routing is done correctly, transit to route following state
            this->rs_worker_.on_route_event(RouteStateWorker::RouteEvent::ROUTING_SUCCESS);
            publish_route_event(cav_msgs::RouteEvent::ROUTE_STARTED);
            // set publish flag such that updated msg will be published in the next spin
            new_route_msg_generated_ = true;
            return true;
        }
        return false;
    }

    std::vector<tf2::Vector3> RouteGeneratorWorker::load_route_destinations_in_ecef(const std::string& route_id) const
    {
        // compose full path of the route file
        std::string route_file_name = route_file_path_ + route_id + ".csv";
        std::ifstream fs(route_file_name);
        std::string line;
        std::vector<tf2::Vector3> destination_points;
        // read each line if any
        while(std::getline(fs, line))
        {
            wgs84_utils::wgs84_coordinate coordinate;
            // lat lon and elev is seperated by comma
            auto comma = line.find(",");
            // convert lon value in the range of [0, 360.0] degree and then into rad
            coordinate.lon =
                (std::stod(line.substr(0, comma)) < 0 ? std::stod(line.substr(0, comma)) + 360.0 : std::stod(line.substr(0, comma))) * DEG_TO_RAD;
            line.erase(0, comma + 1);
            comma = line.find(",");
            // convert lon value in the range of [0, 360.0] degree and then into rad
            coordinate.lat =
                (std::stod(line.substr(0, comma)) < 0 ? std::stod(line.substr(0, comma)) + 360.0 : std::stod(line.substr(0, comma))) * DEG_TO_RAD;
            // elevation is in meters
            coordinate.elevation = std::stod(line.substr(comma + 1));
            // no rotation needed since it only represents a point
            tf2::Quaternion no_rotation(0, 0, 0, 1);
            destination_points.emplace_back(wgs84_utils::geodesic_to_ecef(coordinate, tf2::Transform(no_rotation)));
        }
        return destination_points;
    }

    std::vector<lanelet::BasicPoint2d> RouteGeneratorWorker::transform_to_map_frame(const std::vector<tf2::Vector3>& ecef_points, const tf2::Transform& map_in_earth) const
    {
        std::vector<lanelet::BasicPoint2d> map_points;
        for(tf2::Vector3 point : ecef_points)
        {
            tf2::Transform point_in_earth;
            point_in_earth.setOrigin(point);
            // convert to map frame by (T_e_m)^(-1) * T_e_p
            auto point_in_map = map_in_earth.inverse() * point_in_earth;
            // return as 2D points as the API requiremnet of lanelet2 lib
            map_points.push_back(lanelet::BasicPoint2d(point_in_map.getOrigin().getX(), point_in_map.getOrigin().getY()));
        }
        return map_points;
    }

    cav_msgs::Route RouteGeneratorWorker::compose_route_msg(const lanelet::Optional<lanelet::routing::Route>& route) const
    {
        cav_msgs::Route msg;
        // iterate thought the shortest path to populat shortest_path_lanelet_ids
        for(const auto& ll : route.get().shortestPath())
        {
            msg.shortest_path_lanelet_ids.push_back(ll.id());
        }
        // iterate thought the all lanelet in the route to populat route_path_lanelet_ids
        for(const auto& ll : route.get().laneletMap()->laneletLayer)
        {
            msg.route_path_lanelet_ids.push_back(ll.id());
        }
        return msg;
    }

    bool RouteGeneratorWorker::abort_active_route_cb(cav_srvs::AbortActiveRouteRequest &req, cav_srvs::AbortActiveRouteResponse &resp)
    {
        // only make sense to abort when it is in route following state
        if(this->rs_worker_.get_route_state() == RouteStateWorker::RouteState::ROUTE_FOLLOWING)
        {
            this->rs_worker_.on_route_event(RouteStateWorker::RouteEvent::ROUTE_ABORT);
            resp.error_status = cav_srvs::AbortActiveRouteResponse::NO_ERROR;
            publish_route_event(cav_msgs::RouteEvent::ROUTE_ABORTED);
            route_msg_ = cav_msgs::Route{};
        } else {
            // service call successed but there is not active route
            resp.error_status = cav_srvs::AbortActiveRouteResponse::NO_ACTIVE_ROUTE;
        }
        return true;
    }

    void RouteGeneratorWorker::pose_cb(const geometry_msgs::PoseStampedConstPtr& msg)
    {
        // convert from pose stamp into lanelet basic 2D point
        lanelet::BasicPoint2d current_loc(msg->pose.position.x, msg->pose.position.y);
        // get dt ct from world model
        auto track = this->world_model_->routeTrackPos(current_loc);
        current_crosstrack_distance_ = track.crosstrack;
        current_downtrack_distance_ = track.downtrack;
        // check if we left the seleted route by cross track error
        if(std::fabs(current_crosstrack_distance_) > cross_track_max_)
        {
            this->rs_worker_.on_route_event(RouteStateWorker::RouteEvent::LEFT_ROUTE);
            publish_route_event(cav_msgs::RouteEvent::LEFT_ROUTE);
        }
        // check if we reached our destination be remaining down track distance
        if(current_downtrack_distance_ > world_model_->getRoute()->length2d() - down_track_target_range_)
        {
            this->rs_worker_.on_route_event(RouteStateWorker::RouteEvent::ROUTE_COMPLETE);
            publish_route_event(cav_msgs::RouteEvent::ROUTE_COMPLETED);
        }
    }

    void RouteGeneratorWorker::set_publishers(ros::Publisher route_event_pub, ros::Publisher route_state_pub, ros::Publisher route_pub)
    {
        route_event_pub_ = route_event_pub;
        route_state_pub_ = route_state_pub;
        route_pub_ = route_pub;
    }

    void RouteGeneratorWorker::set_ctdt_param(double ct_max_error, double dt_dest_range)
    {
        this->cross_track_max_ = ct_max_error;
        this->down_track_target_range_ = dt_dest_range;
    }

    void RouteGeneratorWorker::publish_route_event(uint8_t event_type)
    {
        route_event_queue.push(event_type);
    }
    
    bool RouteGeneratorWorker::spin_callback()
    {
        // publish new route and set new route flag back to false
        if(new_route_msg_generated_)
        {
            route_pub_.publish(route_msg_);
            new_route_msg_generated_ = false;
        }
        // publish route state messsage if a route is selected
        if(route_msg_.route_name != "")
        {
            cav_msgs::RouteState state_msg;
            state_msg.header.stamp = ros::Time::now();
            state_msg.routeID = route_msg_.route_name;
            state_msg.cross_track = current_crosstrack_distance_;
            state_msg.down_track = current_downtrack_distance_;
            route_state_pub_.publish(state_msg);
        }
        // publish route event in order if any
        while(!route_event_queue.empty())
        {
            route_event_msg_.event = route_event_queue.front();
            route_state_pub_.publish(route_event_msg_);
            route_event_queue.pop();
        }
        return true;
    }
}
