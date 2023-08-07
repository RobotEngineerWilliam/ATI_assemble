#include "ros/ros.h"
#include "std_msgs/String.h"

#include "Eigen/Dense"
#include "Eigen/Core"
#include "Eigen/Geometry"
#include "Eigen/StdVector"

#include "robot_msgs/Move.h"
#include "std_srvs/Empty.h"
#include "std_srvs/SetBool.h"
#include "robot_msgs/RobotMsg.h"
#include "robot_msgs/SetUserFrame.h"
#include "robot_msgs/SetTcp.h"
#include "robot_msgs/SetLoad.h"
#include "robot_msgs/ServoL.h"
#include "robot_msgs/ClearErr.h"
#include "robot_msgs/SetCollision.h"
#include "robot_msgs/SetAxis.h"

#include "admittance_control/Plot.h"

#include "sensor_msgs/JointState.h"
#include "geometry_msgs/Twist.h"
#include "geometry_msgs/TwistStamped.h"

#include <pthread.h>
#include <unistd.h>
#include <sstream>

#include <vector>

#include "libs/robot.h"
#include "libs/conversion.h"
#include "time.h"
#include <map>
#include <string>

using namespace std;
using namespace Eigen;

const double PI = 3.1415926;

int index_initial = 100;

std::map<int, string> mapErr = {
    {2, "ERR_FUCTION_CALL_ERROR"},
    {-1, "ERR_INVALID_HANDLER"},
    {-2, "ERR_INVALID_PARAMETER"},
    {-3, "ERR_COMMUNICATION_ERR"},
    {-4, "ERR_KINE_INVERSE_ERR"},
    {-5, "ERR_EMERGENCY_PRESSED"},
    {-6, "ERR_NOT_POWERED"},
    {-7, "ERR_NOT_ENABLED"},
    {-8, "ERR_DISABLE_SERVOMODE"},
    {-9, "ERR_NOT_OFF_ENABLE"},
    {-10, "ERR_PROGRAM_IS_RUNNING"},
    {-11, "ERR_CANNOT_OPEN_FILE"},
    {-12, "ERR_MOTION_ABNORMAL"}};

/* global variables */
JAKAZuRobot robot;
VectorXd expected_pose_servo(6);
VectorXd current_joint_servo(6);
bool servo_mode = false;

admittance_control::Plot plot_data;

// 1.1 service move line -
bool movel_callback(robot_msgs::Move::Request &req,
                    robot_msgs::Move::Response &res)
{
    servo_mode = false;

    CartesianPose cart;
    cart.tran.x = req.pose[0] * 1000;
    cart.tran.y = req.pose[1] * 1000;
    cart.tran.z = req.pose[2] * 1000;

    cart.rpy.rx = req.pose[3];
    cart.rpy.ry = req.pose[4];
    cart.rpy.rz = req.pose[5];

    double speed = (double)req.mvvelo * 1000;
    double accel = (double)req.mvacc * 1000;

    OptionalCond *p = nullptr;

    int sdk_res = robot.linear_move(&cart, MoveMode::ABS, req.is_block, speed, accel, 0.3, p);

    switch (sdk_res)
    {
    case 0:
        res.ret = 1;
        res.message = "cmd has been executed!";
        break;
    default:
        res.ret = sdk_res;
        res.message = mapErr[sdk_res] + "\n";
        break;
    }

    return true;
}

// 1.2 service move joint -
bool movej_callback(robot_msgs::Move::Request &req,
                    robot_msgs::Move::Response &res)
{
    servo_mode = false;

    JointValue joint_pose;
    joint_pose.jVal[0] = (req.pose[0]);
    joint_pose.jVal[1] = (req.pose[1]);
    joint_pose.jVal[2] = (req.pose[2]);
    joint_pose.jVal[3] = (req.pose[3]);
    joint_pose.jVal[4] = (req.pose[4]);
    joint_pose.jVal[5] = (req.pose[5]);

    double speed = (double)req.mvvelo;
    double accel = (double)req.mvacc;

    OptionalCond *p = nullptr;

    int sdk_res = robot.joint_move(&joint_pose, MoveMode::ABS, req.is_block, speed, accel, 0.2, p);

    switch (sdk_res)
    {
    case 0:
        res.ret = 1;
        res.message = "cmd has been executed!";
        break;
    default:
        res.ret = sdk_res;
        res.message = mapErr[sdk_res] + "\n";
        break;
    }

    return true;
}

// 1.3 service jog move -
bool move_jog_callback(robot_msgs::Move::Request &req,
                       robot_msgs::Move::Response &res)
{
    servo_mode = false;

    // 1 赋初始值
    double move_velocity = 0;
    CoordType coord_type = COORD_JOINT;

    // 2 选择index   mapping the index and velocity
    // req.index 如果是关节空间   就是  0+,0-,1+,1-,2+,2-,3+,3-,4+,4-,5+,5-
    // req.index 如果是笛卡尔空间 就是x+,x-,y+,y-,z+,z-,rx+,rx-,ry+,ry-,rz+,rz-
    float index_f = static_cast<float>(req.index) / 2 + 0.1;
    int index = static_cast<int>(index_f);

    // 3 选择坐标
    switch (req.coord_mode)
    {
    case 0:
        coord_type = COORD_JOINT;       // 关节坐标 关节空间
        move_velocity = PI / 180.0 * 2; // 关节运动速度 (可修改)
        break;
    case 1:
        coord_type = COORD_BASE; // 基坐标 笛卡尔空间
        if (index >= 3)
        {
            move_velocity = PI / 180.0 * 2; // rx,ry,rz，旋转运动速度 (可修改)
        }
        else
        {
            move_velocity = 2; // 沿x或y或z轴方向运动速度  (可修改)
        }

        break;
    default:
        coord_type = COORD_TOOL; // TCP末端坐标 笛卡尔空间
        if (index >= 3)
        {
            move_velocity = PI / 180.0 * 2; // rx,ry,rz，旋转运动速度 (可修改)
        }
        else
        {
            move_velocity = 2; // 沿x或y或z轴方向运动速度  (可修改)
        }
    }
    // 4 确定速度方向 //判断速度是正方向还是负方向
    if (req.index & 1)
    {
        move_velocity = -move_velocity;
    }
    // 5 进行jog运动
    //  cout << index << ";" << coord_type << ";" << move_velocity << ";"<< pos_cmd << endl;
    if (index_initial != req.index)
    {
        int jog_stop_res = robot.jog_stop(-1);
        if (jog_stop_res == 0)
        {
            sleep(0.2);
            int sdk_res = robot.jog(index, CONTINUE, coord_type, move_velocity, 0);
            switch (sdk_res)
            {
            case 0:
                res.ret = 1;
                res.message = "Position is reached";
                break;
            default:
                res.ret = sdk_res;
                res.message = mapErr[sdk_res] + "\n";
                break;
            }
        }
        else
        {
            res.ret = jog_stop_res;
            res.message = mapErr[jog_stop_res] + "\n";
        }
        index_initial = req.index;
    }
    else
    {
        res.ret = 1;
        res.message = "Moving on";
        ROS_INFO("Moving on");
    }
    return true;
    /* move_jog */
    /* bool move_jog_callback(robot_msgs::Move::Request &req,
            robot_msgs::Move::Response &res)
    {

        //1 赋初始值
        double move_velocity = req.mvvelo;
        CoordType coord_type = COORD_BASE;
        double pos_cmd = 0.0;

        //2 选择index   mapping the index and velocity
        //req.index 如果是关节空间   就是  0+,0-,1+,1-,2+,2-,3+,3-,4+,4-,5+,5-
        //req.index 如果是笛卡尔空间 就是x+,x-,y+,y-,z+,z-,rx+,rx-,ry+,ry-,rz+,rz-
        float index_f = static_cast<float>(req.index) / 2 + 0.1;
        int index = static_cast<int>(index_f);

        //3 选择坐标
        switch (req.coord_mode)
        {
            case 0:
                coord_type = COORD_JOINT; //关节坐标 关节空间
                pos_cmd = PI / 180.0;       //某个关节运动(PI/180.0)弧度 (可修改)
                break;
            case 1:
                coord_type = COORD_BASE;   //基坐标 笛卡尔空间
                if (index >= 3)
                {                         //沿rx,ry,rz，旋转(PI/180.0)弧度 (可修改)
                    pos_cmd = PI / 180.0;
                }
                else
                {
                    pos_cmd = 10;         //沿x或y或z轴方向，运行10mm  (可修改)
                }

                break;
            default:
                coord_type = COORD_TOOL;   //TCP末端坐标 笛卡尔空间
                if (index >= 3)
                {                         //沿rx,ry,rz，旋转(PI/180.0)弧度(可修改)
                    pos_cmd = PI / 180.0;
                }
                else
                {
                    pos_cmd = 10;         //沿x或y或z轴方向，运行10mm  (可修改)
                }
        }
        // 4 确定速度方向 //判断速度是正方向还是负方向
        if(req.index&1)
        {
            move_velocity = -move_velocity;
        }
        //5 进行jog运动
        // cout << index << ";" << coord_type << ";" << move_velocity << ";"<< pos_cmd << endl;
        int jog_stop_res = robot.jog_stop(-1);
        if (jog_stop_res == 0)
        {
            sleep(0.2);
            int sdk_res = robot.jog(index, INCR, coord_type, move_velocity, pos_cmd);
            switch(sdk_res)
            {
                case 0:
                    res.ret = 1;
                    res.message = "Position is reached";
                    break;
                default:
                    res.ret = sdk_res;
                    res.message = mapErr[sdk_res]+"\n";
                    break;
            }
        }
        else
        {
            res.ret = jog_stop_res;
            res.message = mapErr[jog_stop_res]+"\n";
        }
        return true;

    } */
}

// 1.4 service stop move -
bool stop_callback(std_srvs::Empty::Request &req,
                   std_srvs::Empty::Response &res)
{
    servo_mode = false;

    // robot.disable_robot();
    int sdk_res = robot.motion_abort();
    switch (sdk_res)
    {
    case 0:
        ROS_INFO("stop sucess");
        break;
    default:
        cout << "error:" << mapErr[sdk_res] << endl;
        break;
    }

    return true;
}

// 1.5 service servo move -
void ServoMovePoseAccepted(const robot_msgs::ServoL::ConstPtr &msg)
{
    // memcpy(expected_pose_servo.data(), &(msg->pose[0]), 6 * 8);
    for (int i = 0; i < 6; i++)
        expected_pose_servo[i] = msg->pose[i];
    servo_mode = msg->servo_mode;
    cout << expected_pose_servo.transpose() << endl;
    plot_data.data_1 = expected_pose_servo(2);
}

/**
 * @brief    单自由度梯形速度轨迹规划，若距离过短则采用均分规划
 * @param start    起始关节位置
 * @param end      结束关节位置
 * @param start_vel 起始速度
 * @param end_vel 终点速度
 * @param speed_c 巡航速度
 * @param acc_times 加速时间(点数目)
 * @param dec_times 减速时间(点数目)
 * @param total_times 总时间(点数目)
 * @param trajectory 输出轨迹
 */
void Single_Trapezoid_Series(double start, double end, float start_vel, float end_vel,
                             float speed_c, int acc_times, int dec_times, int total_times,
                             std::vector<float> &trajectory)
{
    float path_point;
    float deta;
    float vel;
    int t = 0;
    int dir = 0;

    path_point = start;
    vel = start_vel;
    trajectory.clear();

    dir = (end - start) > 0 ? 1 : -1;

    while (t < total_times) // total times
    {
        if (t < acc_times) // acc times
        {
            vel = vel + (speed_c - start_vel) / acc_times;
        }
        else if (t >= acc_times && t < (total_times - dec_times))
        {
            vel = speed_c;
        }
        else
        {
            vel = vel - (speed_c - end_vel) / dec_times;
        }
        // std::cout << vel << std::endl;

        deta = fabs(end - path_point) > vel ? dir * vel : end - path_point;
        path_point = path_point + deta;

        trajectory.push_back(path_point);

        t++;
    }
    // std::cout << "Trapezoid Series Complete!" << std::endl;
}

/**
 * @brief    单自由度均分规划
 * @param start    起始关节位置
 * @param end      结束关节位置
 * @param total_times 总点数
 * @param trajectory 输出轨迹
 */
void Single_Average_Series(float start, float end, int total_times, std::vector<float> &trajectory)
{
    float path_point;
    float deta;
    int t = 0;

    path_point = start;
    deta = (end - start) / total_times;
    trajectory.clear();

    while (t < total_times)
    {
        path_point = path_point + deta;

        trajectory.push_back(path_point);

        t++;
    }
    // std::cout << "Average Series Complete!" << std::endl;
}

/**
 * @brief 均分规划
 * @param start    起始关节位置
 * @param end      结束关节位置
 * @param total_times 总点数
 * @param trajectory 输出轨迹
 */
void Average_Series(VectorXd start, VectorXd end, double total_times, std::vector<VectorXd> &trajectory)
{
    VectorXd path_point(6);
    VectorXd deta(6);
    int t = 0;
    int num = round(total_times / 0.008);

    path_point = start;
    deta = (end - start) / num;
    trajectory.clear();

    while (t < num)
    {
        path_point = path_point + deta;

        trajectory.push_back(path_point);

        t++;
    }
    // std::cout << "Average Series Complete!" << std::endl;
}

/**
 * @brief 梯形速度轨迹规划，若距离过短则采用均分规划
 * @param joint_start   起始关节位置
 * @param joint_end     结束关节位置
 * @param trajectory    输出轨迹序列
 * @param vel_lim       关节速度限制
 * @param acc_lim       关节加速度限制
 * @param start_vel     起始速度
 * @param end_vel       终止速度
 */
void Trapezoid_Velocity_Series(JointValue joint_start, JointValue joint_end, std::vector<JointValue> &trajectory)
{
    // trajectory.clear();
    // Joint_Value length, path_point;
    // Joint_Flag joint_trapezoid;

    // // Length of acceleration and deceleration
    // Joint_Value acc_length, dec_length;
    // int longest_index = 0;
    // float time;
    // int Times;
    // int Acc_Times, Dec_Times;
    // float len = 0;

    // std::vector<float> path_series[6];

    // // Find the longest joint
    // for (int i = 0; i < 6; i++)
    // {
    //     // Caculate the length of acceleration and deceleration
    //     acc_length[i] = (vel_lim[i] * vel_lim[i] - start_vel[i] * start_vel[i]) / (2 * acc_lim[i]);
    //     dec_length[i] = (vel_lim[i] * vel_lim[i] - end_vel[i] * end_vel[i]) / (2 * acc_lim[i]);

    //     length[i] = fabs(joint_end.jVal[i] - joint_start.jVal[i]);

    //     if (length[i] > len)
    //     {
    //         longest_index = i;
    //         len = length[i];
    //     }

    //     if (length[i] < acc_length[i] + dec_length[i])
    //     {
    //         joint_trapezoid[i] = 0;
    //     }
    //     else
    //     {
    //         joint_trapezoid[i] = 1;
    //     }
    //     // std::cout << " " << i << " " << joint_trapezoid[i] << " " ;
    //     // std::cout << " l " << length[i] << " a " << acc_length[i] << " d " << dec_length[i] << std::endl;
    // }

    // // Caculate times if the longest is Trapezoid
    // if (joint_trapezoid[longest_index] == 1)
    // { /*
    //      time = (vel_lim[longest_index] - start_vel[longest_index]) / acc_lim[longest_index] +
    //              (vel_lim[longest_index] - end_vel[longest_index]) / acc_lim[longest_index] +
    //              (length[longest_index] - acc_length[longest_index] - dec_length[longest_index]) / vel_lim[longest_index]; */

    //     Acc_Times = static_cast<int>((vel_lim[longest_index] - start_vel[longest_index]) / acc_lim[longest_index] + 1);
    //     Dec_Times = static_cast<int>((vel_lim[longest_index] - end_vel[longest_index]) / acc_lim[longest_index] + 1);
    //     // Acc_Times = static_cast<int>((vel_lim[longest_index] - start_vel[longest_index]) / acc_lim[longest_index]);
    //     // Dec_Times = static_cast<int>((vel_lim[longest_index] - end_vel[longest_index]) / acc_lim[longest_index]);

    //     time = Acc_Times + Dec_Times +
    //            (length[longest_index] - acc_length[longest_index] - dec_length[longest_index]) / vel_lim[longest_index];

    //     // std::cout << " " << i << " " << joint_trapezoid[i] << " " ;
    //     // std::cout << " t " << time << " a " << Acc_Times << " d " << Dec_Times << std::endl;
    // }
    // else
    // {
    //     /* time = (vel_lim[longest_index] - start_vel[longest_index]) / acc_lim[longest_index] +
    //             (vel_lim[longest_index] - end_vel[longest_index]) / acc_lim[longest_index]; */

    //     time = 2 * length[longest_index] / (start_vel[longest_index] + end_vel[longest_index]);
    // }

    // // Times of trajectory
    // // Times = static_cast<int>(time + 1);
    // Times = static_cast<int>(time);

    // // std::cout << " " << Times << std::endl;

    // // Planning the trajectory
    // for (int i = 0; i < 6; i++)
    // {
    //     if (joint_trapezoid[i] == 1)
    //     {

    //         /* if(i == longest_index)
    //         {
    //             Single_Trapezoid_Series(joint_start.jVal[i], joint_end.jVal[i], start_vel[i], end_vel[i], vel_lim[i], Acc_Times, Dec_Times, Times, path_series[i]);
    //         }
    //         else
    //         { */
    //         float cruise_vel;

    //         cruise_vel = fabs((2 * fabs(joint_end.jVal[i] - joint_start.jVal[i]) - start_vel[i] * Acc_Times - end_vel[i] * Dec_Times) / (2 * Times - Acc_Times - Dec_Times));

    //         Single_Trapezoid_Series(joint_start.jVal[i], joint_end.jVal[i], start_vel[i], end_vel[i], cruise_vel, Acc_Times, Dec_Times, Times, path_series[i]);
    //         //}
    //     }
    //     else
    //     {
    //         Single_Average_Series(joint_start.jVal[i], joint_end.jVal[i], Times, path_series[i]);
    //     }
    // }

    // for (int i = 0; i < path_series[longest_index].size(); i++)
    // {
    //     JointValue joint_data;
    //     // joint_data.clear();
    //     for (int j = 0; j < 6; j++)
    //     {
    //         joint_data.jVal[j] = path_series[j][i];
    //     }

    //     trajectory.push_back(joint_data);
    // }

    // // std::cout << "Planning Complete!" << std::endl;
}

/**
 * @brief 伺服关节运动,使用给定序列
 * @param trajectory 轨迹
 * @param start      起始点列
 * @param end        终止点列
 */
bool servo_move_joint(vector<VectorXd> &trajectory, int start, int end)
{
    JointValue tmp_expected_joint_servo;

    robot.servo_move_use_joint_LPF(4);
    robot.servo_move_enable(true);

    std::cout << "Servo enable!" << std::endl;

    usleep(8 * 1000);
    for (int t = start; t < end; t++)
    {
        memcpy(tmp_expected_joint_servo.jVal, trajectory[t].data(), 6 * 8);
        int sdk_res = robot.servo_j(&tmp_expected_joint_servo, ABS, 4);
        if (sdk_res != 0)
        {
            cout << "error:" << mapErr[sdk_res] << endl;
            return false;
        }
    }
    usleep(8 * 1000);

    robot.servo_move_enable(false);

    return true;
}

// 2.1 robot tcp publish
void tool_point_callback(ros::Publisher tcp_pub)
{
    geometry_msgs::TwistStamped tool_point;
    CartesianPose tool_pose;
    robot.get_tcp_position(&tool_pose);

    tool_point.twist.linear.x = tool_pose.tran.x / 1000;
    tool_point.twist.linear.y = tool_pose.tran.y / 1000;
    tool_point.twist.linear.z = tool_pose.tran.z / 1000;

    tool_point.twist.angular.x = tool_pose.rpy.rx;
    tool_point.twist.angular.y = tool_pose.rpy.ry;
    tool_point.twist.angular.z = tool_pose.rpy.rz;

    plot_data.data_2 = tool_point.twist.linear.z;

    tcp_pub.publish(tool_point);
}

// 2.2 robot joint publish
void joint_states_callback(ros::Publisher joint_states_pub)
{
    // clock_t t1 = clock();

    sensor_msgs::JointState joint_states;
    JointValue joint_pose;

    // joint_states.position.clear(); // clear position vector
    robot.get_joint_position(&joint_pose); // get current joint pose

    for (int i = 0; i < 6; i++)
    {
        joint_states.position.push_back(joint_pose.jVal[i]); // write data into standard ros msg
        int j = i + 1;
        joint_states.name.push_back("joint_" + std::to_string(j));
        joint_states.header.stamp = ros::Time::now();
    }

    joint_states_pub.publish(joint_states); // publish data

    memcpy(current_joint_servo.data(), joint_pose.jVal, 6 * 8);

    plot_data.data_4 = joint_pose.jVal[1];
    plot_data.data_5 = joint_pose.jVal[2];

    // cout << (clock() - t1) * 1.0 / CLOCKS_PER_SEC * 1000 << endl;
}

// 2.3 robot state publish
void robot_state_callback(ros::Publisher robot_state_pub)
{
    robot_msgs::RobotMsg robot_state;

    RobotState robotstate;
    RobotStatus robotstatus;
    ProgramState programstate;
    BOOL in_pos = true;
    BOOL in_col = false;

    if (robot.is_in_pos(&in_pos))
        ROS_INFO("Failed to get robot pos!");
    if (robot.get_program_state(&programstate))
        ROS_INFO("Failed to get program state!");
    if (robot.get_robot_status(&robotstatus))
        ROS_INFO("Failed to get robot status!");
    if (robot.get_robot_state(&robotstate))
        ROS_INFO("Failed to get robot state!");
    if (robot.is_in_collision(&in_col))
        ROS_INFO("Failed to get robot collision state!");

    if (robotstate.estoped)
    {
        robot_state.state = 2;
        // ROS_INFO("Robot program is running!");
    }
    else if (robotstatus.errcode != 0)
    {
        robot_state.state = 4;
        // ROS_INFO("Robot is in error!")
    }
    else if (in_pos && programstate == PROGRAM_IDLE && robotstatus.drag_status == 0)
    {
        robot_state.state = 0;
        // ROS_INFO("Robot is in pos!");
    }
    else if (programstate == PROGRAM_PAUSED)
    {
        robot_state.state = 1;
        // ROS_INFO("Robot program is paused!");
    }
    else if (!in_pos || programstate == PROGRAM_RUNNING || robotstatus.drag_status == 1)
    {
        robot_state.state = 3;
        // ROS_INFO("Robot program is running!");
    }

    /*
    if(in_pos && programstate == PROGRAM_IDLE)
    {
        robot_state.state = 0;
        //ROS_INFO("Robot is in pos!");
    }
    else if(programstate == PROGRAM_PAUSED)
    {
        robot_state.state = 1;
        //ROS_INFO("Robot program is paused!");
    }
    else if(robotstate.estoped)
    {
        robot_state.state = 2;
        //ROS_INFO("Robot is emergency stopped!");
    }
    else if(!in_pos || programstate == PROGRAM_RUNNING)
    {
        robot_state.state = 3;
        //ROS_INFO("Robot program is running!");
    }
    else if(robotstatus.errcode != 0)
    {
        robot_state.state = 4;
        //ROS_INFO("Robot is in error!");
    }
    */

    robot_state.mode = 2;
    // ROS_INFO("Robot is in distance mode!");

    if (robotstate.poweredOn)
    {
        robot_state.motor_sync = 1;
        // ROS_INFO("Robot is in synchronization!");
    }
    else
    {
        robot_state.motor_sync = 0;
        // ROS_INFO("Robot is in unsynchronization!");
    }

    if (robotstatus.enabled)
    {
        // 客户文档要求servo_enable 代表机器人是否使能，sdk内的servo_enable表示透传模式下是否使能
        robot_state.servo_enable = 1;
        // ROS_INFO("Servo mode is enable!");
    }
    else
    {
        robot_state.servo_enable = 0;
        // ROS_INFO("Servo mode is disable!");
    }

    if (in_col)
    {
        robot_state.collision_state = 1;
        // ROS_INFO("Robot is in collision!");
    }
    else
    {
        robot_state.collision_state = 0;
        // ROS_INFO("Robot is not in collision!");
    }
    // robot_state.state = 0;
    // robot_state.mode = 2;
    // robot_state.servo_enable = 1;
    // robot_state.motor_sync = 1;
    // robot_state.collision_state = 0;

    robot_state_pub.publish(robot_state);
}

// 2.4 servo line publish
void servo_line_callback(ros::Publisher servo_line)
{
    robot_msgs::ServoL set_pose;
    // float pos[6] = {set_pose[0],set_pose[1],set_pose[2],set_pose[3],set_pose[4],set_pose[5]};
    // robot.servo_p(&set_pose,ABS);
    CartesianPose pose;
    pose.tran.x = set_pose.pose[0];
    pose.tran.y = set_pose.pose[1];
    pose.tran.z = set_pose.pose[2];

    pose.rpy.rx = set_pose.pose[0];
    pose.rpy.ry = set_pose.pose[1];
    pose.rpy.rz = set_pose.pose[2];
    // robot.servo_move_enable(true);

    // robot.servo_p(&pose,ABS);
    // robot.servo_move_enable(false);

    servo_line.publish(set_pose);
}

/* enable or disable robot */
void robot_state_control_callback()
{
    bool enable_robot, disable_robot; // enable or disable robot
    ros::param::get("/enable_robot", enable_robot);
    ros::param::get("/disable_robot", disable_robot);

    if (enable_robot)
    {
        robot.power_on();
        robot.enable_robot();
        ros::param::set("/enable_robot", false);
    }

    if (disable_robot)
    {
        robot.disable_robot();
        // robot.power_off();
        ros::param::set("/disable_robot", false);
    }
}

/* 多线程监控网络状态 */
void *get_conn_scoket_state(void *args)
{
    int get_count = 0;
    RobotStatus robot_status;
    int is_socket_connect;
    while (ros::ok())
    {
        /* code */
        int ret = robot.get_robot_status(&robot_status);

        // cout<<"ret:"<<ret<<endl;
        is_socket_connect = robot_status.is_socket_connect;
        // cout<<"postition:"<<robot_status.joint_position[0]<<endl;
        // cout<<"is_socket_connect:"<<is_socket_connect<<endl;
        if (!is_socket_connect)
        {
            ROS_ERROR("connect error!!!");
        }
        // cout<<"get_count:"<<get_count++<<endl;

        sleep(1);
    }
}

int main(int argc, char **argv)
{
    // ros::init(argc, argv, "connect_robot");

    // ros::NodeHandle n;

    // robot.login_in("192.168.50.170");
    // robot.power_on();
    // robot.enable_robot();
    // robot.servo_move_use_joint_LPF(4);
    // robot.servo_move_enable(true);

    // JointValue joint_pos = {0, 0 * PI / 180, 0 * PI / 180, 0 * PI / 180, 0 * PI / 180, -0.01};
    // cout << joint_pos.jVal[0] << endl;
    // for (int i = 0; i < 1000; i++)
    // {
    //     robot.servo_j(&joint_pos, INCR);
    //     sleep(2);
    // }

    // robot.servo_move_enable(false);

    RobotStatus ret_status;

    // robot.set_status_data_update_time_interval(40);

    ros::init(argc, argv, "connect_robot");

    ros::NodeHandle n;

    // init params
    string ip = "192.168.50.170";
    ros::param::set("/enable_robot", false);
    ros::param::set("/disable_robot", false);
    ros::param::set("robot_ip", ip);

    /* services and topics */

    // 2.1 robot tcp publisher -
    ros::Publisher tool_point_pub = n.advertise<geometry_msgs::TwistStamped>("/robot_driver/tool_point", 1);

    // 2.2 robot joint angle publisher -
    ros::Publisher joint_states_pub = n.advertise<sensor_msgs::JointState>("/robot_driver/joint_states", 1);

    // 2.3 robot state publisher -
    // ros::Publisher robot_state_pub = n.advertise<robot_msgs::RobotMsg>("/robot_driver/robot_states", 10);

    // 2.4 servo line publisher -
    // ros::Publisher servo_line_pub = n.advertise<robot_msgs::ServoL>("/robot_driver/servo_line", 100);

    ROS_INFO("Try to connect robot");

    // connect robot
    std::cout << "-----------" << endl;
    robot.login_in(ip.c_str());
    robot.set_network_exception_handle(110, MOT_ABORT);
    sleep(1);
    robot.power_on();
    sleep(1);
    robot.enable_robot();
    robot.get_robot_status(&ret_status);
    if (ret_status.enabled == false)
    {
        ROS_INFO("restart......");
        robot.power_off();
        sleep(1);
        robot.power_on();
        sleep(1);
        robot.enable_robot();
    }

    ROS_INFO("Robot:%s enable", ip.c_str());
    ros::Duration(1).sleep();
    ROS_INFO("Robot:%s ready!", ip.c_str());

    // 监控状态
    pthread_t conn_state_thread;
    int ret = pthread_create(&conn_state_thread, NULL, get_conn_scoket_state, NULL);

    if (ret != 0)
    {
        ROS_ERROR("thread creat error!!!!!!!!!!");
    }

    /*方式1：设置定时器来循环发布话题*/
    // robot control timer
    // ros::Timer robot_state_control_timer = n.createTimer(ros::Duration(1), robot_state_control_callback);

    // // 2.4
    // ros::Timer servo_line_timer = n.createTimer(ros::Duration(0.08), boost::bind(servo_line_callback, _1, servo_line_pub));

    // 2.1
    ros::Timer tool_point_timer = n.createTimer(ros::Duration(0.08), boost::bind(tool_point_callback, tool_point_pub));

    // 2.2
    ros::Timer joint_states_timer = n.createTimer(ros::Duration(0.08), boost::bind(joint_states_callback, joint_states_pub));

    // // 2.3
    // ros::Timer robot_states_timer = n.createTimer(ros::Duration(0.08), boost::bind(robot_state_callback, _1, robot_state_pub));

    // ros::spin();

    // 1.1 service move line -
    ros::ServiceServer service_movel = n.advertiseService("/robot_driver/move_line", movel_callback);

    // 1.2 service move joint -
    ros::ServiceServer service_movej = n.advertiseService("/robot_driver/move_joint", movej_callback);

    // 1.3 service jog move -
    // ros::ServiceServer move_jog = n.advertiseService("/robot_driver/move_jog", move_jog_callback);

    // 1.4 service stop move -
    ros::ServiceServer service_stop = n.advertiseService("/robot_driver/stop_move", stop_callback);

    // 1.5 service servo move -
    ros::Subscriber servo_move_sub = n.subscribe("/robot_driver/servo_move", 1, ServoMovePoseAccepted);

    ros::Publisher plot_pub = n.advertise<admittance_control::Plot>("/plot_data_2", 100);

    vector<VectorXd> tra;
    VectorXd expected_joint_servo(6);
    JointValue tmp_expected_joint_servo;
    JointValue tmp_current_joint_servo;
    CartesianPose tmp_expected_pose_servo;

    expected_pose_servo = VectorXd::Zero(6);
    expected_joint_servo = VectorXd::Zero(6);

    /*方式2：通过Rate来循环发布话题*/
    // ros::Rate rate(500);
    while (ros::ok())
    {
        // 2.1
        // tool_point_callback(tool_point_pub);

        // 2.2
        // joint_states_callback(joint_states_pub);

        // 2.3
        // robot_state_callback(robot_state_pub);

        // 2.4
        // servo_line_callback(servo_line_pub);

        ros::spinOnce();

        if (servo_mode)
        {
            expected_pose_servo.block<3, 1>(0, 0) = expected_pose_servo.block<3, 1>(0, 0) * 1000;

            memcpy(tmp_expected_joint_servo.jVal, expected_joint_servo.data(), 6 * 8);
            memcpy(tmp_current_joint_servo.jVal, current_joint_servo.data(), 6 * 8);
            memcpy(&(tmp_expected_pose_servo.tran.x), expected_pose_servo.data(), 6 * 8);

            robot.kine_inverse(&tmp_current_joint_servo, &tmp_expected_pose_servo, &tmp_expected_joint_servo);

            memcpy(expected_joint_servo.data(), tmp_expected_joint_servo.jVal, 6 * 8);

            Average_Series(current_joint_servo, expected_joint_servo, 0.1, tra);

            // Trapezoid_Velocity_Series(expected_joint_servo, tra, vel_lim, acc_lim, start_vel, end_vel);

            bool code = servo_move_joint(tra, 0, tra.size());

            robot.servo_move_use_joint_LPF(4);
            robot.servo_move_enable(true);

            std::cout << "Servo enable!" << std::endl;

            usleep(8 * 1000);
            for (int t = 0; t < tra.size(); t++)
            {
                memcpy(tmp_expected_joint_servo.jVal, tra[t].data(), 6 * 8);
                cout << tra[t].transpose() << endl;
                plot_data.data_6 = tmp_expected_joint_servo.jVal[1];
                plot_data.data_7 = tmp_expected_joint_servo.jVal[1];
                int sdk_res = robot.servo_j(&tmp_expected_joint_servo, ABS, 1);
                if (sdk_res != 0)
                {
                    cout << "error:" << mapErr[sdk_res] << endl;
                    int res = robot.motion_abort();
                    switch (res)
                    {
                    case 0:
                        ROS_INFO("stop sucess");
                        break;
                    default:
                        cout << "error:" << mapErr[res] << endl;
                        break;
                    }
                    return 0;
                }
                plot_pub.publish(plot_data);
                ros::spinOnce();
                // usleep(8 * 1000);
            }
            usleep(8 * 1000);

            robot.servo_move_enable(false);

            servo_mode = false;
        }

        // rate.sleep();
    }
    return 0;
}
