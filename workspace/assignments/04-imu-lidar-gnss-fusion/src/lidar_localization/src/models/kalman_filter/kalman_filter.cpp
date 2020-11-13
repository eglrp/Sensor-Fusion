/*
 * @Description: IMU-lidar-GNSS fusion using Kalman filter for localization
 * @Author: Ge Yao
 * @Date: 2020-11-12 15:14:07
 */
#include <limits>

#include <cmath>
#include <iostream>
#include <fstream>
#include <ostream>

// use sophus to handle so3 hat & SO3 log operations:
#include <sophus/so3.hpp>

#include "lidar_localization/models/kalman_filter/kalman_filter.hpp"

#include "glog/logging.h"

namespace lidar_localization {

KalmanFilter::KalmanFilter(const YAML::Node& node) {
    //
    // parse config:
    // 
    // a. earth constants:
    EARTH.GRAVITY_MAGNITUDE = node["earth"]["gravity_magnitude"].as<double>();
    EARTH.ROTATION_SPEED = node["earth"]["rotation_speed"].as<double>();
    EARTH.LATITUDE = node["earth"]["latitude"].as<double>();
    EARTH.LATITUDE *= M_PI / 180.0;
    // b. prior state covariance:
    COV.PRIOR.POS = node["covariance"]["prior"]["pos"].as<double>();
    COV.PRIOR.VEL = node["covariance"]["prior"]["vel"].as<double>();
    COV.PRIOR.ORIENTATION = node["covariance"]["prior"]["orientation"].as<double>();
    COV.PRIOR.EPSILON = node["covariance"]["prior"]["epsilon"].as<double>();
    COV.PRIOR.DELTA = node["covariance"]["prior"]["delta"].as<double>();
    // c. process noise:
    COV.PROCESS.GYRO = node["covariance"]["process"]["gyro"].as<double>();
    COV.PROCESS.ACCEL = node["covariance"]["process"]["accel"].as<double>();
    // d. measurement noise:
    COV.MEASUREMENT.POS = node["covariance"]["measurement"]["pos"].as<double>();
    COV.MEASUREMENT.ORIENTATION = node["covariance"]["measurement"]["orientation"].as<double>();

    // prompt:
    LOG(INFO) << std::endl 
              << "IMU-Lidar Kalman Filter params:" << std::endl
              << "\tgravity magnitude: " << EARTH.GRAVITY_MAGNITUDE << std::endl
              << "\tearth rotation speed: " << EARTH.ROTATION_SPEED << std::endl
              << "\tlatitude: " << EARTH.LATITUDE << std::endl
              << std::endl
              << "\tprior cov. pos.: " << COV.PRIOR.POS  << std::endl
              << "\tprior cov. vel.: " << COV.PRIOR.VEL << std::endl
              << "\tprior cov. ori: " << COV.PRIOR.ORIENTATION << std::endl
              << "\tprior cov. epsilon.: " << COV.PRIOR.EPSILON  << std::endl
              << "\tprior cov. delta.: " << COV.PRIOR.DELTA << std::endl
              << std::endl
              << "\tprocess noise gyro.: " << COV.PROCESS.GYRO << std::endl
              << "\tprocess noise accel.: " << COV.PROCESS.ACCEL << std::endl
              << std::endl
              << "\tmeasurement noise pos.: " << COV.MEASUREMENT.POS << std::endl
              << "\tmeasurement noise orientation.: " << COV.MEASUREMENT.ORIENTATION << std::endl
              << std::endl;
    
    //
    // init filter:
    //
    // a. earth constants:
    g_ = Eigen::Vector3d(
        0.0, 
        0.0, 
        EARTH.GRAVITY_MAGNITUDE
    );
    w_ = Eigen::Vector3d(
        0.0,
        EARTH.ROTATION_SPEED*cos(EARTH.LATITUDE),
        EARTH.ROTATION_SPEED*sin(EARTH.LATITUDE)
    );
    // b. prior state covariance:
    P_.block<3, 3>(  INDEX_ERROR_POS,   INDEX_ERROR_POS) = COV.PRIOR.POS*Eigen::Matrix3d::Identity();
    P_.block<3, 3>(  INDEX_ERROR_VEL,   INDEX_ERROR_VEL) = COV.PRIOR.VEL*Eigen::Matrix3d::Identity();
    P_.block<3, 3>(  INDEX_ERROR_ORI,   INDEX_ERROR_ORI) = COV.PRIOR.ORIENTATION*Eigen::Matrix3d::Identity();
    P_.block<3, 3>( INDEX_ERROR_GYRO,  INDEX_ERROR_GYRO) = COV.PRIOR.EPSILON*Eigen::Matrix3d::Identity();
    P_.block<3, 3>(INDEX_ERROR_ACCEL, INDEX_ERROR_ACCEL) = COV.PRIOR.DELTA*Eigen::Matrix3d::Identity();
    // c. process noise:
    Q_.block<3, 3>(0, 0) = COV.PROCESS.GYRO*Eigen::Matrix3d::Identity();
    Q_.block<3, 3>(3, 3) = COV.PROCESS.ACCEL*Eigen::Matrix3d::Identity();
    // d. measurement noise:
    R_.block<3, 3>(0, 0) = COV.MEASUREMENT.POS*Eigen::Matrix3d::Identity();
    R_.block<3, 3>(3, 3) = COV.MEASUREMENT.ORIENTATION*Eigen::Matrix3d::Identity();
    // e. process equation:
    F_.block<3, 3>(  INDEX_ERROR_POS,   INDEX_ERROR_VEL) = Eigen::Matrix3d::Identity();
    F_.block<3, 3>(  INDEX_ERROR_ORI,   INDEX_ERROR_ORI) = Sophus::SO3d::hat(-w_).matrix();
    // f. measurement equation:
    G_.block<3, 3>(0, INDEX_ERROR_POS) = Eigen::Matrix3d::Identity();
    G_.block<3, 3>(3, INDEX_ERROR_ORI) = Eigen::Matrix3d::Identity();
    C_.block<3, 3>(0, 0) = C_.block<3, 3>(3, 3) = Eigen::Matrix3d::Identity();
}

/**
 * @brief  init filter
 * @param  pose, init pose
 * @param  vel, init vel
 * @param  imu_data, init IMU measurements
 * @return true if success false otherwise
 */
void KalmanFilter::Init(
    const Eigen::Matrix4d &pose,
    const Eigen::Vector3d &vel,
    const IMUData &imu_data
) {
    // init odometry:
    pose_ = pose;
    vel_ = vel;

    // init IMU data buffer:
    imu_data_buff_.clear();
    imu_data_buff_.push_back(imu_data);

    // init filter time:
    time_ = imu_data.time;

    // set process equation in case of one step prediction & correction:
    double T;
    UpdateProcessEquation(imu_data, T);

    LOG(INFO) << std::endl 
              << "Kalman Filter Inited at " << static_cast<int>(time_) << std::endl
              << "Init Position: " 
              << pose_(0, 3) << ", "
              << pose_(1, 3) << ", "
              << pose_(2, 3) << std::endl
              << "Init Velocity: "
              << vel_.x() << ", "
              << vel_.y() << ", "
              << vel_.z() << std::endl;
}

/**
 * @brief  Kalman update
 * @param  imu_data, input IMU measurements
 * @return true if success false otherwise
 */
bool KalmanFilter::Update(const IMUData &imu_data) {
    // update IMU buff:
    if (time_ < imu_data.time) {
        // update IMU odometry:
        imu_data_buff_.push_back(imu_data);
        UpdateOdomEstimation();
        imu_data_buff_.pop_front();

        // update error estimation:
        UpdateErrorEstimation(imu_data);

        // update filter time:
        time_ = imu_data.time;

        LOG(INFO) << std::endl 
                << "Kalman Filter Updated at " << static_cast<int>(time_) << std::endl
                << "Curr Position: " 
                << pose_(0, 3) << ", "
                << pose_(1, 3) << ", "
                << pose_(2, 3) << std::endl
                << "Curr Velocity: "
                << vel_.x() << ", "
                << vel_.y() << ", "
                << vel_.z() << std::endl;

        return true;
    }

    return false;
}

/**
 * @brief  Kalman correction
 * @param  T_nb, odometry delta from lidar frontend
 * @return void
 */
bool KalmanFilter::Correct(
    const IMUData &imu_data,
    const double &time,
    const Eigen::Matrix4f &T_nb_lidar
) {
    // get discretized process equations:
    if (time_ < time) {
        double T = time - time_;
        MatrixF F = MatrixF::Identity() + T*F_;
        MatrixB B = T*B_;

        // perform Kalman prediction:
        X_ = F*X_;
        P_ = F*P_*F.transpose() + B*Q_*B.transpose();

        // get observation:
        Eigen::Matrix4d T_nb_lidar_double = T_nb_lidar.cast<double>();

        Eigen::Vector3d P_nn_obs = pose_.block<3, 1>(0,3) - T_nb_lidar_double.block<3, 1>(0,3);

        Eigen::Matrix3d C_nn_obs = pose_.block<3, 3>(0,0) * T_nb_lidar_double.block<3, 3>(0,0).transpose();

        Y_.block<3, 1>(0, 0) = P_nn_obs;
        Y_.block<3, 1>(3, 0) = Sophus::SO3d::vee(Eigen::Matrix3d::Identity() - C_nn_obs);

        // perform Kalman correction:
        MatrixR R = G_*P_*G_.transpose() + C_*R_*C_.transpose();
        MatrixK K = P_*G_.transpose()*R.inverse();

        P_ = (MatrixP::Identity() - K*G_)*P_;
        X_ = X_ + K*(Y_ - G_*X_);

        // eliminate error:
        // a. position:
        pose_.block<3, 1>(0, 3) = pose_.block<3, 1>(0, 3) - X_.block<3, 1>(INDEX_ERROR_POS, 0);
        // b. velocity:
        vel_ = vel_ - X_.block<3, 1>(INDEX_ERROR_VEL, 0);
        // c. orientation:
        Eigen::Matrix3d C_nn = Eigen::Matrix3d::Identity() - Sophus::SO3d::hat(
            X_.block<3, 1>(INDEX_ERROR_ORI, 0)
        );
        pose_.block<3, 3>(0, 0) = C_nn.transpose()*pose_.block<3, 3>(0, 0);

        // reset error state:
        X_.block<3, 1>(INDEX_ERROR_POS, 0) = X_.block<3, 1>(INDEX_ERROR_VEL, 0) = X_.block<3, 1>(INDEX_ERROR_ORI, 0) = Eigen::Vector3d::Zero();

        return true;
    }

    return false;
}
 
/**
 * @brief  get odometry estimation
 * @param  pose, init pose
 * @param  vel, init vel
 * @return void
 */
void KalmanFilter::GetOdometry(
    Eigen::Matrix4f &pose, Eigen::Vector3f &vel
) {
    // init:
    Eigen::Matrix4d pose_double = pose_;
    Eigen::Vector3d vel_double = vel_;

    // eliminate error:
    // a. position:
    pose_double.block<3, 1>(0, 3) = pose_double.block<3, 1>(0, 3) - X_.block<3, 1>(INDEX_ERROR_POS, 0);
    // b. velocity:
    vel_double = vel_double - X_.block<3, 1>(INDEX_ERROR_VEL, 0);
    // c. orientation:
    Eigen::Matrix3d C_nn = Eigen::Matrix3d::Identity() - Sophus::SO3d::hat(
        X_.block<3, 1>(INDEX_ERROR_ORI, 0)
    );
    pose_.block<3, 3>(0, 0) = C_nn.transpose()*pose_.block<3, 3>(0, 0);

    // finally:
    pose = pose_double.cast<float>();
    vel = vel_double.cast<float>();
}

/**
 * @brief  get unbiased angular velocity in body frame
 * @param  angular_vel, angular velocity measurement
 * @return unbiased angular velocity in body frame
 */
inline Eigen::Vector3d KalmanFilter::GetUnbiasedAngularVel(
    const Eigen::Vector3d &angular_vel
) {
    return angular_vel - X_.block<3, 1>(INDEX_ERROR_GYRO, 0);
}

/**
 * @brief  get unbiased linear acceleration in navigation frame
 * @param  linear_acc, linear acceleration measurement
 * @param  R, corresponding orientation of measurement
 * @return unbiased linear acceleration in navigation frame
 */
inline Eigen::Vector3d KalmanFilter::GetUnbiasedLinearAcc(
    const Eigen::Vector3d &linear_acc,
    const Eigen::Matrix3d &R
) {
    return R*(linear_acc - X_.block<3, 1>(INDEX_ERROR_ACCEL, 0)) - g_;
}

/**
 * @brief  get angular delta
 * @param  index_curr, current imu measurement buffer index
 * @param  index_prev, previous imu measurement buffer index
 * @param  angular_delta, angular delta output
 * @return true if success false otherwise
 */
bool KalmanFilter::GetAngularDelta(
    const size_t index_curr, const size_t index_prev,
    Eigen::Vector3d &angular_delta
) {
    if (
        index_curr <= index_prev ||
        imu_data_buff_.size() <= index_curr
    ) {
        return false;
    }

    const IMUData &imu_data_curr = imu_data_buff_.at(index_curr);
    const IMUData &imu_data_prev = imu_data_buff_.at(index_prev);

    double delta_t = imu_data_curr.time - imu_data_prev.time;

    Eigen::Vector3d angular_vel_curr = Eigen::Vector3d(
        imu_data_curr.angular_velocity.x,
        imu_data_curr.angular_velocity.y,
        imu_data_curr.angular_velocity.z
    );
    angular_vel_curr = GetUnbiasedAngularVel(angular_vel_curr);

    Eigen::Vector3d angular_vel_prev = Eigen::Vector3d(
        imu_data_prev.angular_velocity.x,
        imu_data_prev.angular_velocity.y,
        imu_data_prev.angular_velocity.z
    );
    angular_vel_prev = GetUnbiasedAngularVel(angular_vel_prev);

    angular_delta = 0.5*delta_t*(angular_vel_curr + angular_vel_prev);

    return true;
}

/**
 * @brief  get velocity delta
 * @param  index_curr, current imu measurement buffer index
 * @param  index_prev, previous imu measurement buffer index
 * @param  R_curr, corresponding orientation of current imu measurement
 * @param  R_prev, corresponding orientation of previous imu measurement
 * @param  velocity_delta, velocity delta output
 * @return true if success false otherwise
 */
bool KalmanFilter::GetVelocityDelta(
    const size_t index_curr, const size_t index_prev,
    const Eigen::Matrix3d &R_curr, const Eigen::Matrix3d &R_prev, 
    double &T, Eigen::Vector3d &velocity_delta
) {
    if (
        index_curr <= index_prev ||
        imu_data_buff_.size() <= index_curr
    ) {
        return false;
    }

    const IMUData &imu_data_curr = imu_data_buff_.at(index_curr);
    const IMUData &imu_data_prev = imu_data_buff_.at(index_prev);

    T = imu_data_curr.time - imu_data_prev.time;

    Eigen::Vector3d linear_acc_curr = Eigen::Vector3d(
        imu_data_curr.linear_acceleration.x,
        imu_data_curr.linear_acceleration.y,
        imu_data_curr.linear_acceleration.z
    );
    linear_acc_curr = GetUnbiasedLinearAcc(linear_acc_curr, R_curr);
    Eigen::Vector3d linear_acc_prev = Eigen::Vector3d(
        imu_data_prev.linear_acceleration.x,
        imu_data_prev.linear_acceleration.y,
        imu_data_prev.linear_acceleration.z
    );
    linear_acc_prev = GetUnbiasedLinearAcc(linear_acc_prev, R_prev);
    
    velocity_delta = 0.5*T*(linear_acc_curr + linear_acc_prev);

    return true;
}

/**
 * @brief  update orientation with effective rotation angular_delta
 * @param  angular_delta, effective rotation
 * @param  R_curr, current orientation
 * @param  R_prev, previous orientation
 * @return void
 */
void KalmanFilter::UpdateOrientation(
    const Eigen::Vector3d &angular_delta,
    Eigen::Matrix3d &R_curr, Eigen::Matrix3d &R_prev
) {
    // magnitude:
    double angular_delta_mag = angular_delta.norm();
    // direction:
    Eigen::Vector3d angular_delta_dir = angular_delta.normalized();

    // build delta q:
    double angular_delta_cos = cos(angular_delta_mag/2.0);
    double angular_delta_sin = sin(angular_delta_mag/2.0);
    Eigen::Quaterniond dq(
        angular_delta_cos, 
        angular_delta_sin*angular_delta_dir.x(), 
        angular_delta_sin*angular_delta_dir.y(), 
        angular_delta_sin*angular_delta_dir.z()
    );
    Eigen::Quaterniond q(pose_.block<3, 3>(0, 0));
    
    // update:
    q = q*dq;
    
    // write back:
    R_prev = pose_.block<3, 3>(0, 0);
    pose_.block<3, 3>(0, 0) = q.normalized().toRotationMatrix();
    R_curr = pose_.block<3, 3>(0, 0);
}

/**
 * @brief  update orientation with effective velocity change velocity_delta
 * @param  T, timestamp delta 
 * @param  velocity_delta, effective velocity change
 * @return void
 */
void KalmanFilter::UpdatePosition(const double &T, const Eigen::Vector3d &velocity_delta) {
    pose_.block<3, 1>(0, 3) += T*vel_ + 0.5*T*velocity_delta;
    vel_ += velocity_delta;
}

/**
 * @brief  update IMU odometry estimation
 * @param  void
 * @return void
 */
void KalmanFilter::UpdateOdomEstimation(void) {
    // get deltas:
    Eigen::Vector3d angular_delta; 
    GetAngularDelta(1, 0, angular_delta);

    // update orientation:
    Eigen::Matrix3d R_curr, R_prev;
    UpdateOrientation(angular_delta, R_curr, R_prev);

    // get velocity delta:
    double T;
    Eigen::Vector3d velocity_delta;
    GetVelocityDelta(1, 0, R_curr, R_prev, T, velocity_delta);

    // update position:
    UpdatePosition(T, velocity_delta);
}

/**
 * @brief  get process input, C_nb & f_n, from IMU measurement
 * @param  imu_data, input IMU measurement
 * @param  T, output time delta
 * @param  C_nb, output rotation matrix, body frame -> navigation frame
 * @param  f_n, output accel measurement in navigation frame
 * @return void
 */
void KalmanFilter::GetProcessInput(
    const IMUData &imu_data,
    double &T, Eigen::Matrix3d &C_nb, Eigen::Vector3d &f_n
) {
    // get time delta:
    T = imu_data.time - time_;

    // get rotation matrix, body frame -> navigation frame:
    Eigen::Quaterniond q_nb(
        imu_data.orientation.w,
        imu_data.orientation.x,
        imu_data.orientation.y,
        imu_data.orientation.z
    );
    C_nb = q_nb.toRotationMatrix();

    // get accel measurement in navigation frame:
    Eigen::Vector3d f_b(
        imu_data.linear_acceleration.x,
        imu_data.linear_acceleration.y,
        imu_data.linear_acceleration.z
    );
    f_n = C_nb*f_b;
}

/**
 * @brief  set process equation
 * @param  C_nb, rotation matrix, body frame -> navigation frame
 * @param  f_n, accel measurement in navigation frame
 * @return void
 */
void KalmanFilter::SetProcessEquation(
    const Eigen::Matrix3d &C_nb, const Eigen::Vector3d &f_n
) {
    // a. set process equation for delta vel:
    F_.block<3, 3>(INDEX_ERROR_VEL,  INDEX_ERROR_ORI) = Sophus::SO3d::hat(f_n).matrix();
    F_.block<3, 3>(INDEX_ERROR_VEL,INDEX_ERROR_ACCEL) =  C_nb;
    B_.block<3, 3>(INDEX_ERROR_VEL, 3) =  C_nb;
    // b. set process equation for delta ori:
    // TODO: update the block influenced by earth rotation speed:
    F_.block<3, 3>(INDEX_ERROR_ORI, INDEX_ERROR_GYRO) = -C_nb;
    B_.block<3, 3>(INDEX_ERROR_ORI, 0) = -C_nb;
}

/**
 * @brief  update process equation
 * @param  imu_data, input IMU measurement
 * @param  T, output time delta
 * @return void
 */
void KalmanFilter::UpdateProcessEquation(
    const IMUData &imu_data, double &T
) { 
    // get process input:
    Eigen::Matrix3d C_nb;
    Eigen::Vector3d f_n;
    GetProcessInput(imu_data, T, C_nb, f_n);

    // set process equation:
    SetProcessEquation(C_nb, f_n);
}

/**
 * @brief  update error estimation
 * @param  imu_data, input IMU measurement
 * @return void
 */
void KalmanFilter::UpdateErrorEstimation(
    const IMUData &imu_data
) {
    // update process equation:
    double T;
    UpdateProcessEquation(imu_data, T);

    // get discretized process equations:
    MatrixF F = MatrixF::Identity() + T*F_;
    MatrixB B = T*B_;

    // perform Kalman prediction:
    X_ = F*X_;
    P_ = F*P_*F.transpose() + B*Q_*B.transpose();
}

} // namespace lidar_localization