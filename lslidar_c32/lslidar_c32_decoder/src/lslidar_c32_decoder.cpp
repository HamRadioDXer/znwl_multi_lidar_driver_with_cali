/*
 * This file is part of lslidar_c32 driver.
 *
 * The driver is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * The driver is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with the driver.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <lslidar_c32_decoder/lslidar_c32_decoder.h>
#include <std_msgs/Int8.h>

using namespace std;

namespace lslidar_c32_decoder {
LslidarC32Decoder::LslidarC32Decoder(
        ros::NodeHandle& n, ros::NodeHandle& pn):
    nh(n),
    pnh(pn),
    publish_point_cloud(true),
    is_first_sweep(true),
    last_azimuth(0.0),
    sweep_start_time(0.0),
    // layer_num(8),
    packet_start_time(0.0),
    sweep_data(new lslidar_c32_msgs::LslidarC32Sweep()),
    multi_scan(new lslidar_c32_msgs::LslidarC32Layer())
    {
    return;
}

bool LslidarC32Decoder::loadParameters() {
    pnh.param<int>("point_num", point_num, 2000);
    pnh.param<int>("channel_num", layer_num, 8);
    pnh.param<double>("min_range", min_range, 0.5);
    pnh.param<double>("max_range", max_range, 100.0);
    pnh.param<double>("angle_disable_min", angle_disable_min,-1);
    pnh.param<double>("angle_disable_max", angle_disable_max, -1);

    pnh.param<double>("frequency", frequency, 10.0);
    pnh.param<bool>("publish_point_cloud", publish_point_cloud, true);
    pnh.param<bool>("publish_channels", publish_channels, true);
    pnh.param<string>("fixed_frame_id", fixed_frame_id, "map");
    pnh.param<string>("child_frame_id", child_frame_id, "lslidar");

    angle_base = M_PI*2 / point_num;
    return true;
}

bool LslidarC32Decoder::createRosIO() {
    packet_sub = nh.subscribe<lslidar_c32_msgs::LslidarC32Packet>(
                "lslidar_packet", 100, &LslidarC32Decoder::packetCallback, this);
    layer_sub = nh.subscribe(
                "layer_num", 100, &LslidarC32Decoder::layerCallback, this);
    sweep_pub = nh.advertise<lslidar_c32_msgs::LslidarC32Sweep>(
                "lslidar_sweep", 10);
    point_cloud_pub = nh.advertise<sensor_msgs::PointCloud2>(
                "lslidar_point_cloud", 10);
    scan_pub = nh.advertise<sensor_msgs::LaserScan>(
                "scan", 100);
    channel_scan_pub = nh.advertise<lslidar_c32_msgs::LslidarC32Layer>(
                "scan_channel", 100);
    return true;
}

bool LslidarC32Decoder::initialize() {
    if (!loadParameters()) {
        ROS_ERROR("Cannot load all required parameters...");
        return false;
    }

    if (!createRosIO()) {
        ROS_ERROR("Cannot create ROS I/O...");
        return false;
    }

    // Fill in the altitude for each scan.
    for (size_t scan_idx = 0; scan_idx < 32; ++scan_idx) {
        size_t remapped_scan_idx = scan_idx%2 == 0 ? scan_idx/2 : scan_idx/2+16;
        sweep_data->scans[remapped_scan_idx].altitude = scan_altitude[scan_idx];
    }

    // Create the sin and cos table for different azimuth values.
    for (size_t i = 0; i < 6300; ++i) {
        double angle = static_cast<double>(i) / 1000.0;
        cos_azimuth_table[i] = cos(angle);
        sin_azimuth_table[i] = sin(angle);
    }

    return true;
}

bool LslidarC32Decoder::checkPacketValidity(const RawPacket* packet) {
    for (size_t blk_idx = 0; blk_idx < BLOCKS_PER_PACKET; ++blk_idx) {
        if (packet->blocks[blk_idx].header != UPPER_BANK) {
            //ROS_WARN("Skip invalid LS-16 packet: block %lu header is %x",
                    //blk_idx, packet->blocks[blk_idx].header);
            return false;
        }
    }
    return true;
}

void LslidarC32Decoder::publishPointCloud() {
//    pcl::PointCloud<pcl::PointXYZI>::Ptr point_cloud(
//                new pcl::PointCloud<pcl::PointXYZI>());
    VPointCloud::Ptr point_cloud(new VPointCloud());

    //point_cloud->header.stamp =
            //pcl_conversions::toPCL(sweep_data->header).stamp;
    point_cloud->header.frame_id = child_frame_id;
    point_cloud->height = 1;

    for (size_t i = 0; i < 32; ++i) {
        const lslidar_c32_msgs::LslidarC32Scan& scan = sweep_data->scans[i];
        // The first and last point in each scan is ignored, which
        // seems to be corrupted based on the received data.
        // TODO: The two end points should be removed directly
        //    in the scans.
        // point_time unit is sec
        double timestamp = point_time;
        point_cloud->header.stamp = static_cast<uint64_t>(timestamp * 1e6);
        if (scan.points.size() == 0) continue;
        size_t j;
        VPoint point;
        for (j = 1; j < scan.points.size()-1; ++j) {
			point.timestamp = timestamp - (scan.points.size()-1 - j)*0.00005;  // time interval for each point is 50us
            point.x = scan.points[j].x;
            point.y = scan.points[j].y;
            point.z = scan.points[j].z;
            point.intensity = scan.points[j].intensity;
            point.range = scan.points[j].distance;
            point.h_angle = scan.points[j].azimuth;
            point.v_angle = layer_altitude[i];
            point.laserid = layer_id[i];

            point_cloud->points.push_back(point);
            ++point_cloud->width;
        }

    }

    //  	if(point_cloud->width > 2000)
    {
        point_cloud_pub.publish(point_cloud);
    }


    return;
}

void LslidarC32Decoder::publishChannelScan()
{
    multi_scan = lslidar_c32_msgs::LslidarC32LayerPtr(
                    new lslidar_c32_msgs::LslidarC32Layer());
    // lslidar_c32_msgs::LslidarC32Layer multi_scan(new lslidar_c32_msgs::LslidarC32Layer);
    sensor_msgs::LaserScan scan;

    int layer_num_local = layer_num;
    ROS_INFO_ONCE("default channel is %d", layer_num_local );
    if(sweep_data->scans[layer_num_local].points.size() <= 1)
        return;

    for (uint16_t j=0; j<32; j++)
    {
    scan.header.frame_id = child_frame_id;
    scan.header.stamp = sweep_data->header.stamp;

    scan.angle_min = 0.0;
    scan.angle_max = 2.0*M_PI;
    scan.angle_increment = (scan.angle_max - scan.angle_min)/point_num;

    //	scan.time_increment = motor_speed_/1e8;
    scan.range_min = min_range;
    scan.range_max = max_range;
    scan.ranges.reserve(point_num);
    scan.ranges.assign(point_num, std::numeric_limits<float>::infinity());

    scan.intensities.reserve(point_num);
    scan.intensities.assign(point_num, std::numeric_limits<float>::infinity());

    for(uint16_t i = 0; i < sweep_data->scans[j].points.size(); i++)
    {
        int point_idx = sweep_data->scans[j].points[i].azimuth / angle_base;

        if (point_idx >= point_num)
            point_idx = 0;
        if (point_idx < 0)
            point_idx = point_num - 1;

        scan.ranges[point_num - 1-point_idx] = sweep_data->scans[j].points[i].distance;
        scan.intensities[point_num - 1-point_idx] = sweep_data->scans[j].points[i].intensity;
    }

    for (int i = point_num - 1; i >= 0; i--)
	{
		if((i >= angle_disable_min*point_num/360) && (i < angle_disable_max*point_num/360))
			scan.ranges[i] = std::numeric_limits<float>::infinity();
	}

        multi_scan->scan_channel[j] = scan;
        if (j == layer_num_local)
            scan_pub.publish(scan);
    }

    channel_scan_pub.publish(multi_scan);  

}


void LslidarC32Decoder::publishScan()
{
    sensor_msgs::LaserScan::Ptr scan(new sensor_msgs::LaserScan);
    int layer_num_local = layer_num;
    ROS_INFO_ONCE("default channel is %d", layer_num_local);
    if(sweep_data->scans[layer_num_local].points.size() <= 1)
        return;

    scan->header.frame_id = child_frame_id;
    scan->header.stamp = sweep_data->header.stamp;

    scan->angle_min = 0.0;
    scan->angle_max = 2.0*M_PI;
    scan->angle_increment = (scan->angle_max - scan->angle_min)/point_num;

    //	scan->time_increment = motor_speed_/1e8;
    scan->range_min = min_range;
    scan->range_max = max_range;
    scan->ranges.reserve(point_num);
    scan->ranges.assign(point_num, std::numeric_limits<float>::infinity());

    scan->intensities.reserve(point_num);
    scan->intensities.assign(point_num, std::numeric_limits<float>::infinity());

    for(uint16_t i = 0; i < sweep_data->scans[layer_num_local].points.size(); i++)
    {
        int point_idx = sweep_data->scans[layer_num_local].points[i].azimuth / angle_base;

        if (point_idx >= point_num)
            point_idx = 0;
        if (point_idx < 0)
            point_idx = point_num - 1;

        scan->ranges[point_num - 1-point_idx] = sweep_data->scans[layer_num_local].points[i].distance;
        scan->intensities[point_num - 1-point_idx] = sweep_data->scans[layer_num_local].points[i].intensity;
    }

    for (int i = point_num - 1; i >= 0; i--)
	{
		if((i >= angle_disable_min*point_num/360) && (i < angle_disable_max*point_num/360))
			scan->ranges[i] = std::numeric_limits<float>::infinity();
	}

    scan_pub.publish(scan);

}


point_struct LslidarC32Decoder::getMeans(std::vector<point_struct> clusters)
{
    point_struct tmp;
    int num = clusters.size();
    if (num == 0)
    {
        tmp.distance = std::numeric_limits<float>::infinity();
        tmp.intensity = std::numeric_limits<float>::infinity();

    }
    else
    {
        double mean_distance = 0, mean_intensity = 0;

        for (int i = 0; i < num; i++)
        {
            mean_distance += clusters[i].distance;
            mean_intensity += clusters[i].intensity;
        }

        tmp.distance = mean_distance / num;
        tmp.intensity = mean_intensity / num;
    }
    return tmp;
}

void LslidarC32Decoder::decodePacket(const RawPacket* packet) {

    // Compute the azimuth angle for each firing
    for (size_t fir_idx = 0; fir_idx < FIRINGS_PER_PACKET; fir_idx++) {
        
             firings[fir_idx].firing_azimuth = rawAzimuthToDouble(packet->blocks[fir_idx].rotation);
       
				
    }
    /*
    for (size_t fir_idx = 0; fir_idx < FIRINGS_PER_PACKET; fir_idx+=2) {
        size_t blk_idx = fir_idx / 2;
        firings[fir_idx].firing_azimuth = rawAzimuthToDouble(
                    packet->blocks[blk_idx].rotation);
    }

    // Interpolate the azimuth values
    for (size_t fir_idx = 1; fir_idx < FIRINGS_PER_PACKET; fir_idx+=2) {
        size_t lfir_idx = fir_idx - 1;
        size_t rfir_idx = fir_idx + 1;

        double azimuth_diff;
        if (fir_idx == FIRINGS_PER_PACKET - 1) {
            lfir_idx = fir_idx - 3;
            rfir_idx = fir_idx - 1;
        }

        azimuth_diff = firings[rfir_idx].firing_azimuth -
                firings[lfir_idx].firing_azimuth;
        azimuth_diff = azimuth_diff < 0 ? azimuth_diff + 2*M_PI : azimuth_diff;

        firings[fir_idx].firing_azimuth =
                firings[fir_idx-1].firing_azimuth + azimuth_diff/2.0;


        firings[fir_idx].firing_azimuth  =
                firings[fir_idx].firing_azimuth > 2*M_PI ?
                    firings[fir_idx].firing_azimuth-2*M_PI : firings[fir_idx].firing_azimuth;
    }
     */
    // Fill in the distance and intensity for each firing.
    for (size_t blk_idx = 0; blk_idx < BLOCKS_PER_PACKET; ++blk_idx) {
        const RawBlock& raw_block = packet->blocks[blk_idx];

        for (size_t blk_fir_idx = 0; blk_fir_idx < FIRINGS_PER_BLOCK; ++blk_fir_idx){
            size_t fir_idx = blk_idx*FIRINGS_PER_BLOCK + blk_fir_idx;
             /*
            double azimuth_diff = 0.0;
            if (fir_idx < FIRINGS_PER_PACKET - 1)
                azimuth_diff = firings[fir_idx+1].firing_azimuth -
                        firings[fir_idx].firing_azimuth;
            else
                azimuth_diff = firings[fir_idx].firing_azimuth -
                        firings[fir_idx-1].firing_azimuth;
             */
            for (size_t scan_fir_idx = 0; scan_fir_idx < SCANS_PER_FIRING; ++scan_fir_idx){
                size_t byte_idx = RAW_SCAN_SIZE * (
                            SCANS_PER_FIRING*blk_fir_idx + scan_fir_idx);

                // Azimuth
               // firings[fir_idx].azimuth[scan_fir_idx] = firings[fir_idx].firing_azimuth +
               //         (scan_fir_idx*DSR_TOFFSET/FIRING_TOFFSET) * azimuth_diff;
                 if(scan_fir_idx%2==0)
                 {
				  //if(abs(firings[fir_idx].firing_azimuth-360*DEG_TO_RAD)>4*DEG_TO_RAD)
                   firings[fir_idx].azimuth[scan_fir_idx] = firings[fir_idx].firing_azimuth+4*DEG_TO_RAD;
			     }
			     else
			     {
					 firings[fir_idx].azimuth[scan_fir_idx] = firings[fir_idx].firing_azimuth;
					 //firings[fir_idx].azimuth[scan_fir_idx] = firings[fir_idx].firing_azimuth-4*3.1415926/180;
			     }
                // Distance
                TwoBytes raw_distance;
                raw_distance.bytes[0] = raw_block.data[byte_idx];
                raw_distance.bytes[1] = raw_block.data[byte_idx+1];
                firings[fir_idx].distance[scan_fir_idx] = static_cast<double>(
                            raw_distance.distance) * DISTANCE_RESOLUTION;
               
                // Intensity
                firings[fir_idx].intensity[scan_fir_idx] = static_cast<double>(
                            raw_block.data[byte_idx+2]);
            }
        }
    }
      //for (size_t idx = 0; idx < SCANS_PER_FIRING; ++idx)
    //{
     //ROS_WARN("[%f %f %f]", firings[1].azimuth[idx], firings[1].distance[idx], firings[1].intensity[idx]);
    //}
    return;
}

void LslidarC32Decoder::layerCallback(const std_msgs::Int8Ptr& msg){
    int num = msg->data;
    if (num < 0)
    {
        num = 0;
        ROS_WARN("layer num outside of the index, select layer 0 instead!");
    }
    else if (num > 31)
    {
        num = 31;
        ROS_WARN("layer num outside of the index, select layer 31 instead!");
    }
    ROS_INFO("select layer num: %d", msg->data);
    layer_num = num;
    return;
}

void LslidarC32Decoder::packetCallback(
        const lslidar_c32_msgs::LslidarC32PacketConstPtr& msg) {
    //  ROS_WARN("packetCallBack");
    // Convert the msg to the raw packet type.
    const RawPacket* raw_packet = (const RawPacket*) (&(msg->data[0]));

    // Check if the packet is valid
    if (!checkPacketValidity(raw_packet)) return;

    // Decode the packet
    decodePacket(raw_packet);
    point_time = msg->stamp.toSec();
    // Find the start of a new revolution
    //    If there is one, new_sweep_start will be the index of the start firing,
    //    otherwise, new_sweep_start will be FIRINGS_PER_PACKET.
    size_t new_sweep_start = 0;
    do {
        //    if (firings[new_sweep_start].firing_azimuth < last_azimuth) break;
        if (fabs(firings[new_sweep_start].firing_azimuth - last_azimuth) > M_PI) break;
        else {
            last_azimuth = firings[new_sweep_start].firing_azimuth;
            ++new_sweep_start;
        }
    } while (new_sweep_start < FIRINGS_PER_PACKET);
    //  ROS_WARN("new_sweep_start %d", new_sweep_start);

    // The first sweep may not be complete. So, the firings with
    // the first sweep will be discarded. We will wait for the
    // second sweep in order to find the 0 azimuth angle.
    size_t start_fir_idx = 0;
    size_t end_fir_idx = new_sweep_start;
    if (is_first_sweep &&
            new_sweep_start == FIRINGS_PER_PACKET) {
        // The first sweep has not ended yet.
        return;
    } else {
        if (is_first_sweep) {
            is_first_sweep = false;
            start_fir_idx = new_sweep_start;
            end_fir_idx = FIRINGS_PER_PACKET;
            sweep_start_time = msg->stamp.toSec() +
                    FIRING_TOFFSET * (end_fir_idx-start_fir_idx) * 1e-6;
        }
    }

    for (size_t fir_idx = start_fir_idx; fir_idx < end_fir_idx; ++fir_idx) {
        for (size_t scan_idx = 0; scan_idx < SCANS_PER_FIRING; ++scan_idx) {
            // Check if the point is valid.
            if (!isPointInRange(firings[fir_idx].distance[scan_idx])) continue;

            // Convert the point to xyz coordinate
            size_t table_idx = floor(firings[fir_idx].azimuth[scan_idx]*1000.0+0.5);
            //cout << table_idx << endl;
            if(table_idx>6280) table_idx-=6280;
            double cos_azimuth = cos_azimuth_table[table_idx];
            double sin_azimuth = sin_azimuth_table[table_idx];

            //double x = firings[fir_idx].distance[scan_idx] *
            //  cos_scan_altitude[scan_idx] * sin(firings[fir_idx].azimuth[scan_idx]);
            //double y = firings[fir_idx].distance[scan_idx] *
            //  cos_scan_altitude[scan_idx] * cos(firings[fir_idx].azimuth[scan_idx]);
            //double z = firings[fir_idx].distance[scan_idx] *
            //  sin_scan_altitude[scan_idx];

            double x = firings[fir_idx].distance[scan_idx] *
                    cos_scan_altitude[scan_idx] * sin_azimuth;
            double y = firings[fir_idx].distance[scan_idx] *
                    cos_scan_altitude[scan_idx] * cos_azimuth;
            double z = firings[fir_idx].distance[scan_idx] *
                    sin_scan_altitude[scan_idx];

            double x_coord = y;
            double y_coord = -x;
            double z_coord = z;

            // Compute the time of the point
            double time = packet_start_time +
                    FIRING_TOFFSET*fir_idx + DSR_TOFFSET*scan_idx;

            // Remap the index of the scan
            int remapped_scan_idx = scan_idx%2 == 0 ? scan_idx/2 : scan_idx/2+16;
            sweep_data->scans[remapped_scan_idx].points.push_back(
                        lslidar_c32_msgs::LslidarC32Point());

            lslidar_c32_msgs::LslidarC32Point& new_point =		// new_point ???push_back?????????????????????
                    sweep_data->scans[remapped_scan_idx].points[
                    sweep_data->scans[remapped_scan_idx].points.size()-1];

            // Pack the data into point msg
            new_point.time = time;
            new_point.x = x_coord;
            new_point.y = y_coord;
            new_point.z = z_coord;
            new_point.azimuth = firings[fir_idx].azimuth[scan_idx];
            new_point.distance = firings[fir_idx].distance[scan_idx];
            new_point.intensity = firings[fir_idx].intensity[scan_idx];
        }
    }

    packet_start_time += FIRING_TOFFSET * (end_fir_idx-start_fir_idx);

    // A new sweep begins
    if (end_fir_idx != FIRINGS_PER_PACKET) {
        //	ROS_WARN("A new sweep begins");
        // Publish the last revolution
        sweep_data->header.frame_id = "sweep";
        sweep_data->header.stamp = ros::Time(sweep_start_time);

        sweep_pub.publish(sweep_data);

        if (publish_point_cloud) publishPointCloud();
        
        if (publish_channels)
            publishChannelScan();
        else{
            publishScan();
        }

        sweep_data = lslidar_c32_msgs::LslidarC32SweepPtr(
                    new lslidar_c32_msgs::LslidarC32Sweep());

        // Prepare the next revolution
        sweep_start_time = msg->stamp.toSec() +
                FIRING_TOFFSET * (end_fir_idx-start_fir_idx) * 1e-6;

        packet_start_time = 0.0;
        last_azimuth = firings[FIRINGS_PER_PACKET-1].firing_azimuth;

        start_fir_idx = end_fir_idx;
        end_fir_idx = FIRINGS_PER_PACKET;

        for (size_t fir_idx = start_fir_idx; fir_idx < end_fir_idx; ++fir_idx) {
            for (size_t scan_idx = 0; scan_idx < SCANS_PER_FIRING; ++scan_idx) {
                // Check if the point is valid.
                if (!isPointInRange(firings[fir_idx].distance[scan_idx])) continue;

                // Convert the point to xyz coordinate
                size_t table_idx = floor(firings[fir_idx].azimuth[scan_idx]*1000.0+0.5);
                //cout << table_idx << endl;
                if(table_idx>6280) table_idx-=6280;
                double cos_azimuth = cos_azimuth_table[table_idx];
                double sin_azimuth = sin_azimuth_table[table_idx];

                //double x = firings[fir_idx].distance[scan_idx] *
                //  cos_scan_altitude[scan_idx] * sin(firings[fir_idx].azimuth[scan_idx]);
                //double y = firings[fir_idx].distance[scan_idx] *
                //  cos_scan_altitude[scan_idx] * cos(firings[fir_idx].azimuth[scan_idx]);
                //double z = firings[fir_idx].distance[scan_idx] *
                //  sin_scan_altitude[scan_idx];

                double x = firings[fir_idx].distance[scan_idx] *
                        cos_scan_altitude[scan_idx] * sin_azimuth;
                double y = firings[fir_idx].distance[scan_idx] *
                        cos_scan_altitude[scan_idx] * cos_azimuth;
                double z = firings[fir_idx].distance[scan_idx] *
                        sin_scan_altitude[scan_idx];

                double x_coord = y;
                double y_coord = -x;
                double z_coord = z;

                // Compute the time of the point
                double time = packet_start_time +
                        FIRING_TOFFSET*(fir_idx-start_fir_idx) + DSR_TOFFSET*scan_idx;

                // Remap the index of the scan
                int remapped_scan_idx = scan_idx%2 == 0 ? scan_idx/2 : scan_idx/2+16;
                sweep_data->scans[remapped_scan_idx].points.push_back(
                            lslidar_c32_msgs::LslidarC32Point());
                lslidar_c32_msgs::LslidarC32Point& new_point =
                        sweep_data->scans[remapped_scan_idx].points[
                        sweep_data->scans[remapped_scan_idx].points.size()-1];

                // Pack the data into point msg
                new_point.time = time;
                new_point.x = x_coord;
                new_point.y = y_coord;
                new_point.z = z_coord;
                new_point.azimuth = firings[fir_idx].azimuth[scan_idx];
                new_point.distance = firings[fir_idx].distance[scan_idx];
                new_point.intensity = firings[fir_idx].intensity[scan_idx];
            }
        }

        packet_start_time += FIRING_TOFFSET * (end_fir_idx-start_fir_idx);
    }
    //  ROS_WARN("pack end");
    return;
}

} // end namespace lslidar_c32_decoder

