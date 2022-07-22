/**
* This file is part of COVINS.
*
* Copyright (C) 2018-2021 Patrik Schmuck / Vision for Robotics Lab
* (ETH Zurich) <collaborative (dot) slam (at) gmail (dot) com>
* For more information see <https://github.com/VIS4ROB-lab/covins>
*
* COVINS is free software: you can redistribute it and/or modify
* it under the terms of the GNU General Public License as published by
* the Free Software Foundation, either version 3 of the license, or
* (at your option) any later version.
*
* COVINS is distributed to support research and development of
* multi-agent system, but WITHOUT ANY WARRANTY; without even the
* implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
* PURPOSE. In no event will the authors be held liable for any damages
* arising from the use of this software. See the GNU General Public
* License for more details.
*
* You should have received a copy of the GNU General Public License
* along with COVINS. If not, see <http://www.gnu.org/licenses/>.
*/

#include "covins_backend/optimization_be.hpp"

//C++
#include <iostream>
#include <vector>
#include <eigen3/Eigen/Core>
#include <Eigen/QR> 

// COVINS
#include <covins/covins_base/utils_base.hpp>
#include "covins_backend/keyframe_be.hpp"
#include "covins_backend/landmark_be.hpp"
#include "covins_backend/map_be.hpp"

// Thirdparty
#include <ceres/ceres.h>


#include <robopt_open/local-parameterization/pose-quaternion-local-param.h>
#include <robopt_open/posegraph-error/six-dof-between.h>
#include <robopt_open/imu-error/preintegration-factor.h>
#include <robopt_open/reprojection-error/global-euclidean.h>
#include <robopt_open/reprojection-error/relative-euclidean.h>

#include <aslam/cameras/camera.h>
#include <aslam/cameras/camera-pinhole.h>
#include <aslam/cameras/camera-unified-projection.h>
#include <aslam/cameras/distortion.h>
#include <aslam/cameras/distortion-radtan.h>
#include <aslam/cameras/distortion-equidistant.h>
#include <aslam/cameras/distortion-fisheye.h>

namespace covins {


inline double radToDeg(double angle) {
    return angle * 180.0 / M_PI;
}

inline double degToRad(double angle) {
    return angle * M_PI / 180.0;
}


static TypeDefs::Matrix3Type ypr2R(const TypeDefs::Vector3Type &ypr) {
    auto y = ypr(0) / 180.0 * M_PI;
    auto p = ypr(1) / 180.0 * M_PI;
    auto r = ypr(2) / 180.0 * M_PI;

    TypeDefs::Matrix3Type Rz;
    Rz << cos(y), -sin(y), 0,
        sin(y), cos(y), 0,
        0, 0, 1;

    TypeDefs::Matrix3Type Ry;
    Ry << cos(p), 0., sin(p),
        0., 1., 0.,
        -sin(p), 0., cos(p);

    TypeDefs::Matrix3Type Rx;
    Rx << 1., 0., 0.,
        0., cos(r), -sin(r),
        0., sin(r), cos(r);

    return Rz * Ry * Rx;
}

Eigen::MatrixXd transformJacobian(const ceres::CRSMatrix &jacobian)
{
    Eigen::MatrixXd jacobianMat;
    jacobianMat.setZero(jacobian.num_rows, jacobian.num_cols);
    for (int row = 0; row < jacobian.num_rows; ++row)
    {
        int startInd = jacobian.rows[row];
        int endInd = jacobian.rows[row + 1];
        for (int colInd = startInd; colInd < endInd; ++colInd)
        {
            const int col = jacobian.cols[colInd];
            const double val = jacobian.values[colInd];
            jacobianMat(row, col) = val;
        }
    }
    return jacobianMat;
}

const static Eigen::IOFormat CSVFormat(Eigen::StreamPrecision,
                                       Eigen::DontAlignCols, ", ", "\n");
    
void writeToCSVfile(std::string name, const Eigen::MatrixXd& matrix)
{
    std::ofstream file(name.c_str(), std::ios::app);
    file << matrix.format(CSVFormat) << std::endl << std::endl;
    file.close();
}

TypeDefs::QuaternionType eulerAnglesToQuat(double yaw, double pitch, double roll) {
    // Code follows example in https://en.wikipedia.org/wiki/Conversion_between_quaternions_and_Euler_angles

    // transform degre to rad
    yaw = degToRad(yaw);
    pitch = degToRad(pitch);
    roll = degToRad(roll);

    // Abbreviations for the various angular functions
    double cy = std::cos(yaw / 2.0);
    double sy = std::sin(yaw / 2.0);
    double cp = std::cos(pitch / 2.0);
    double sp = std::sin(pitch / 2.0);
    double cr = std::cos(roll / 2.0);
    double sr = std::sin(roll / 2.0);

    TypeDefs::QuaternionType quat;
    quat.w() = static_cast<TypeDefs::precision_t>(cy * cp * cr + sy * sp * sr);
    quat.x() = static_cast<TypeDefs::precision_t>(cy * cp * sr - sy * sp * cr);
    quat.y() = static_cast<TypeDefs::precision_t>(sy * cp * sr + cy * sp * cr);
    quat.z() = static_cast<TypeDefs::precision_t>(sy * cp * cr - cy * sp * sr);

    if (quat.w() < 0) {
        quat.w() = -quat.w();
        quat.x() = -quat.x();
        quat.y() = -quat.y();
        quat.z() = -quat.z();
    }

    return quat;
}


std::array<double, 3> quatToEulerAngles(const TypeDefs::QuaternionType& quat) {
    // Code follows example in https://en.wikipedia.org/wiki/Conversion_between_quaternions_and_Euler_angles

    // yaw (z-axis rotation)
    double siny_cosp = 2 * (quat.w() * quat.z() + quat.x() * quat.y());
    double cosy_cosp = 1 - 2 * (quat.y() * quat.y() + quat.z() * quat.z());
    double yaw = std::atan2(siny_cosp, cosy_cosp);

    // pitch (y-axis rotation)
    double sinp = 2 * (quat.w() * quat.y() - quat.z() * quat.x());
    double pitch;
    if (std::abs(sinp) >= 1)
        pitch = std::copysign(M_PI / 2, sinp); // use 90 degrees if out of range
    else
        pitch = std::asin(sinp);

    // roll (x-axis rotation)
    double sinr_cosp = 2 * (quat.w() * quat.x() + quat.y() * quat.z());
    double cosr_cosp = 1 - 2 * (quat.x() * quat.x() + quat.y() * quat.y());
    double roll = std::atan2(sinr_cosp, cosr_cosp);


    std::array<double, 3> angles = {radToDeg(yaw), radToDeg(pitch), radToDeg(roll)};
    return angles;
}


auto Optimization::GlobalBundleAdjustment(MapPtr map, int interations_limit, double time_limit, bool visual_only, bool outlier_removal, bool estimate_bias)->void {
    std::cout << "+++ GBA: Start +++" << std::endl;

    size_t th_min_observations = 2;

    // First round for outlier removal
    if(outlier_removal) {
        ceres::Problem::Options problem_options;
        problem_options.enable_fast_removal = true;
        ceres::Problem problem(problem_options);

        ceres::LossFunction *loss_function;
        loss_function = new ceres::CauchyLoss(1.0);
        ceres::LocalParameterization *local_pose_param = new robopt::local_param::PoseQuaternionLocalParameterization();

        std::map<idpair, double*> distortion_map;
        std::map<idpair, double*> intrinsics_map;

        KeyframeVector keyframes = map->GetKeyframesVec();
        LandmarkVector landmarks = map->GetLandmarksVec();

        std::vector<bool> not_included_lms;
        not_included_lms.resize(landmarks.size(), false);

        // Add Keyframes
        for(size_t i = 0; i < keyframes.size(); ++i) {
            KeyframePtr kf = keyframes[i];
            if(kf->IsInvalid()) continue;

            // Add the state parameters
            kf->UpdateCeresFromState(kf->ceres_pose_,kf->ceres_velocity_and_bias_,kf->ceres_extrinsics_);
            problem.AddParameterBlock(kf->ceres_pose_, robopt::defs::pose::kPoseBlockSize, local_pose_param);
            if(kf->id_.first == 0 && kf->id_.second == map->id_map_)
                problem.SetParameterBlockConstant(kf->ceres_pose_);
            if(!visual_only) problem.AddParameterBlock(kf->ceres_velocity_and_bias_, robopt::defs::pose::kSpeedBiasBlockSize);
            problem.AddParameterBlock(kf->ceres_extrinsics_, robopt::defs::pose::kPoseBlockSize, local_pose_param);
            problem.SetParameterBlockConstant(kf->ceres_extrinsics_);

            // Add camera parameters
            const aslam::Camera::Type camera_type = kf->camera_->getType();
            if (camera_type == aslam::Camera::Type::kPinhole) {
                std::shared_ptr<aslam::PinholeCamera> camera = std::static_pointer_cast<aslam::PinholeCamera>(kf->camera_);
                distortion_map.insert(std::make_pair(kf->id_, camera->getDistortionMutable()->getParametersMutable()));
                problem.AddParameterBlock(camera->getDistortionMutable()->getParametersMutable(), camera->getDistortion().getParameterSize());
                problem.SetParameterBlockConstant(camera->getDistortionMutable()->getParametersMutable());
                intrinsics_map.insert(std::make_pair(kf->id_,camera->getParametersMutable()));
                problem.AddParameterBlock(camera->getParametersMutable(), camera->getParameterSize());
                problem.SetParameterBlockConstant(camera->getParametersMutable());
            } else if (camera_type == aslam::Camera::Type::kUnifiedProjection) {
                std::shared_ptr<aslam::UnifiedProjectionCamera> camera = std::static_pointer_cast<aslam::UnifiedProjectionCamera>(kf->camera_);
                distortion_map.insert(std::make_pair(kf->id_, camera->getDistortionMutable()->getParametersMutable()));
                problem.AddParameterBlock(camera->getDistortionMutable()->getParametersMutable(), camera->getDistortion().getParameterSize());
                problem.SetParameterBlockConstant(camera->getDistortionMutable()->getParametersMutable());
                intrinsics_map.insert(std::make_pair(kf->id_, camera->getParametersMutable()));
                problem.AddParameterBlock(camera->getParametersMutable(), camera->getParameterSize());
                problem.SetParameterBlockConstant(camera->getParametersMutable());
            } else {
              std::cout << COUTFATAL << "Unknown projection type." << std::endl;
              exit(-1);
            }

            // Add the IMU factors
            if(!visual_only) {
                KeyframePtr pred = kf->GetPredecessor();
                if(!pred || pred->IsInvalid()) {
                    if(kf->id_.first != 0) {
                        std::cout << COUTFATAL << kf << ": no predecessor" << std::endl;
                        exit(-1);
                    }
                    continue;
                }
                if(pred->IsInvalid()) {
                    std::cout << COUTFATAL << kf << ": invalid predecessor" << std::endl;
                    exit(-1);
                }

                Eigen::Vector3d bias_acc(
                    kf->ceres_velocity_and_bias_[3],
                    kf->ceres_velocity_and_bias_[4],
                    kf->ceres_velocity_and_bias_[5]);
                Eigen::Vector3d bias_gyr(
                    kf->ceres_velocity_and_bias_[6],
                    kf->ceres_velocity_and_bias_[7],
                    kf->ceres_velocity_and_bias_[8]);
                kf->preintegrated_imu_->repropagate(bias_acc,bias_gyr);

                ceres::CostFunction* imu_factor = new robopt::imu::PreintegrationFactor(kf->preintegrated_imu_.get());
                problem.AddResidualBlock(imu_factor, NULL, pred->ceres_pose_,pred->ceres_velocity_and_bias_,kf->ceres_pose_,kf->ceres_velocity_and_bias_);
            }
        }

        // Add Landmarks
        std::vector<ceres::ResidualBlockId> residual_ids;
        std::vector<KeypointIdentifier> keypoint_ids;
        residual_ids.reserve(landmarks.size()*20);
        keypoint_ids.reserve(landmarks.size()*20);
        for(size_t i = 0; i < landmarks.size(); ++i) {
            LandmarkPtr lm = landmarks[i];
            if(lm->IsInvalid()) continue;

            const Landmark::KfObservations observations = lm->GetObservations();
            // Do a pre-check to ensure at least 2 proper observations
            if(observations.size() < th_min_observations) {
                not_included_lms[i] = true;
                continue;
            }
            size_t num_edges = 0;
            for(auto mit : observations) {
                KeyframePtr kfx = mit.first;
                if(!kfx || kfx->IsInvalid()) continue;
                num_edges++;
            }
            if(num_edges < th_min_observations) {
                not_included_lms[i] = true;
                continue;
            }
            Vector3Type pos_w = lm->GetWorldPos();
            lm->ceres_pos_[0] = pos_w[0];
            lm->ceres_pos_[1] = pos_w[1];
            lm->ceres_pos_[2] = pos_w[2];
            problem.AddParameterBlock(lm->ceres_pos_,robopt::defs::visual::kPositionBlockSize);

            for(const auto& mit : observations) {
                KeyframePtr kfx = mit.first;
                if(!kfx || kfx->IsInvalid()) continue;
                int feat_id = mit.second;

                Eigen::Vector2d kpx = Utils::FromKeypointType(kfx->keypoints_distorted_[feat_id]);
                const precision_t obs_sigma = (kfx->keypoints_aors_[feat_id][1] + 1) * 2.0;

                ceres::CostFunction* reprojection_error;
                const aslam::Camera::Type camera_type = kfx->camera_->getType();
                const aslam::Distortion::Type distortion_type = kfx->camera_->getDistortion().getType();
                if (camera_type == aslam::Camera::Type::kPinhole) {
                    std::shared_ptr<aslam::PinholeCamera> camera = std::static_pointer_cast<aslam::PinholeCamera>(kfx->camera_);
                    switch (distortion_type) {
                        case aslam::Distortion::Type::kEquidistant :
                            reprojection_error = new robopt::reprojection::GlobalEuclideanReprError<aslam::PinholeCamera, aslam::EquidistantDistortion>(kpx, obs_sigma, camera.get());
                            break;
                        case aslam::Distortion::Type::kRadTan :
                            reprojection_error = new robopt::reprojection::GlobalEuclideanReprError<aslam::PinholeCamera, aslam::RadTanDistortion>(kpx, obs_sigma, camera.get());
                            break;
                        case aslam::Distortion::Type::kFisheye :
                            reprojection_error = new robopt::reprojection::GlobalEuclideanReprError<aslam::PinholeCamera, aslam::FisheyeDistortion>(kpx, obs_sigma, camera.get());
                            break;
                        default:
                            std::cout << COUTFATAL << "Unknown distortion type." << std::endl;
                            exit(-1);
                            break;
                    }
                } else if (camera_type == aslam::Camera::Type::kUnifiedProjection) {
                    std::shared_ptr<aslam::UnifiedProjectionCamera> camera = std::static_pointer_cast<aslam::UnifiedProjectionCamera>(kfx->camera_);
                    switch (distortion_type) {
                        case aslam::Distortion::Type::kEquidistant :
                            reprojection_error = new robopt::reprojection::GlobalEuclideanReprError<aslam::UnifiedProjectionCamera, aslam::EquidistantDistortion>(kpx, obs_sigma, camera.get());
                            break;
                        case aslam::Distortion::Type::kRadTan :
                            reprojection_error = new robopt::reprojection::GlobalEuclideanReprError<aslam::UnifiedProjectionCamera, aslam::RadTanDistortion>(kpx, obs_sigma, camera.get());
                            break;
                        case aslam::Distortion::Type::kFisheye :
                            reprojection_error = new robopt::reprojection::GlobalEuclideanReprError<aslam::UnifiedProjectionCamera, aslam::FisheyeDistortion>(kpx, obs_sigma, camera.get());
                            break;
                        default:
                            std::cout << COUTFATAL << "Unknown distortion type." << std::endl;
                            exit(-1);
                            break;

                    }
                } else {
                    std::cout << COUTFATAL << "Unknown projection type." << std::endl;
                    exit(-1);
                }

                ceres::ResidualBlockId block_id = problem.AddResidualBlock(reprojection_error, loss_function,
                                                                           kfx->ceres_pose_, kfx->ceres_extrinsics_, lm->ceres_pos_,
                                                                           intrinsics_map.at(kfx->id_), distortion_map.at(kfx->id_));
                KeypointIdentifier tmp_ident(kfx, lm, feat_id);
                residual_ids.push_back(block_id);
                keypoint_ids.push_back(tmp_ident);
            }
        }

        Eigen::Matrix<precision_t,6,6> sqrt_info = Eigen::Matrix<precision_t,6,6>::Identity();
        sqrt_info.topLeftCorner<3,3>() *= 100.0;
        sqrt_info.bottomRightCorner<3,3>() *= 1e4;

        // Set Loop Edges
        Map::LoopVector loops = map->GetLoopConstraints();
        for(auto i : loops) {
            KeyframePtr kf1 = i.kf1;
            KeyframePtr kf2 = i.kf2;
            TransformType T_12 = i.T_s1_s2;

            Vector3Type t_12 = T_12.block<3,1>(0,3);
            QuaternionType q_12(T_12.block<3,3>(0,0));

            ceres::CostFunction* loop_edge = new robopt::posegraph::SixDofBetweenError(q_12, t_12, sqrt_info, robopt::defs::pose::PoseErrorType::kImu);
            problem.AddResidualBlock(loop_edge, NULL, kf1->ceres_pose_, kf2->ceres_pose_, kf1->ceres_extrinsics_, kf2->ceres_extrinsics_);
        }

        // Solve
        ceres::Solver::Options solver_options;
        solver_options.linear_solver_type = ceres::SPARSE_SCHUR;
        solver_options.num_threads = covins_params::sys::threads_server;
        solver_options.num_linear_solver_threads = covins_params::sys::threads_server;
        solver_options.trust_region_strategy_type = ceres::DOGLEG;
        solver_options.max_num_iterations = 5;
//        std::cout << "solver_options.max_solver_time_in_seconds: " << solver_options.max_solver_time_in_seconds << std::endl;
        ceres::Solver::Summary summary;
        ceres::Solve(solver_options, &problem, &summary);
    //    std::cout << "GBA - Oulier Rejection" << std::endl;
//        std::cout << summary.FullReport() << std::endl;

        // Remove Outliers
        ceres::Problem::EvaluateOptions eval_opts;
        eval_opts.residual_blocks = residual_ids;
        precision_t total_cost = 0.0;
        std::vector<precision_t> residuals;
        problem.Evaluate(eval_opts, &total_cost, &residuals, NULL, NULL);
        size_t resid_idx = 0;
        size_t num_bad = 0;
        const precision_t threshold = covins_params::opt::th_gba_outlier_global;
        for (size_t i = 0; i < residual_ids.size(); ++i) {
            Vector2Type residual_i(residuals[resid_idx], residuals[resid_idx + 1]);
            resid_idx += 2;
            if(residual_i.norm() > threshold) {
                KeyframePtr kfi = keypoint_ids[i].keyframe;
                LandmarkPtr lmi = keypoint_ids[i].landmark;
                size_t kp_id = keypoint_ids[i].keypoint_id;
                kfi->EraseLandmark(kp_id);
                lmi->EraseObservation(kfi);
                problem.RemoveResidualBlock(residual_ids[i]);
                ++num_bad;
            }
        }
        std::cout << "--> GBA removed " << num_bad << " of " << residuals.size() << " observations" << std::endl;

    }

    // Second Round - 'real' optimization
    {
        ceres::Problem::Options problem_options;
        problem_options.enable_fast_removal = true;
        ceres::Problem problem(problem_options);

        ceres::LossFunction *loss_function;
        loss_function = new ceres::CauchyLoss(1.0);
        ceres::LocalParameterization *local_pose_param = new robopt::local_param::PoseQuaternionLocalParameterization();

        std::map<idpair, double*> distortion_map;
        std::map<idpair, double*> intrinsics_map;

        KeyframeVector keyframes = map->GetKeyframesVec();
        LandmarkVector landmarks = map->GetLandmarksVec();
        std::cout << "--> KFs: " << keyframes.size() << std::endl;
        std::cout << "--> LMs: " << landmarks.size() << std::endl;

        std::vector<bool> not_included_lms;
        not_included_lms.resize(landmarks.size(), false);

        std::vector<ceres::ResidualBlockId> imu_factors;
        std::vector<idpair> imu_factors_kfs;

        // Add Keyframes
        for(size_t i = 0; i < keyframes.size(); ++i) {
            KeyframePtr kf = keyframes[i];
            if(kf->IsInvalid()) {
                continue;
            }
            // Add the state parameters
            kf->UpdateCeresFromState(kf->ceres_pose_,kf->ceres_velocity_and_bias_,kf->ceres_extrinsics_);
            problem.AddParameterBlock(kf->ceres_pose_, robopt::defs::pose::kPoseBlockSize, local_pose_param);
            if(kf->id_.first == 0 && kf->id_.second == map->id_map_) {
                problem.SetParameterBlockConstant(kf->ceres_pose_);
            }
            if(!visual_only) {
                problem.AddParameterBlock(kf->ceres_velocity_and_bias_, robopt::defs::pose::kSpeedBiasBlockSize);
            }
            problem.AddParameterBlock(kf->ceres_extrinsics_, robopt::defs::pose::kPoseBlockSize, local_pose_param);
            problem.SetParameterBlockConstant(kf->ceres_extrinsics_);

            if(kf->is_loaded_ && covins_params::opt::gba_fix_poses_loaded_maps) {
                if(kf->id_.first % 50 == 0) std::cout << COUTNOTICE << "Set GBA KFs constant" << std::endl;
                problem.SetParameterBlockConstant(kf->ceres_pose_);
            }

            // Add camera parameters
            const aslam::Camera::Type camera_type = kf->camera_->getType();
            if (camera_type == aslam::Camera::Type::kPinhole) {
                std::shared_ptr<aslam::PinholeCamera> camera = std::static_pointer_cast<aslam::PinholeCamera>(kf->camera_);
                distortion_map.insert(std::make_pair(kf->id_, camera->getDistortionMutable()->getParametersMutable()));
                problem.AddParameterBlock(camera->getDistortionMutable()->getParametersMutable(), camera->getDistortion().getParameterSize());
                problem.SetParameterBlockConstant(camera->getDistortionMutable()->getParametersMutable());
                intrinsics_map.insert(std::make_pair(kf->id_,camera->getParametersMutable()));
                problem.AddParameterBlock(camera->getParametersMutable(), camera->getParameterSize());
                problem.SetParameterBlockConstant(camera->getParametersMutable());
            } else if (camera_type == aslam::Camera::Type::kUnifiedProjection) {
                std::shared_ptr<aslam::UnifiedProjectionCamera> camera = std::static_pointer_cast<aslam::UnifiedProjectionCamera>(kf->camera_);
                distortion_map.insert(std::make_pair(kf->id_, camera->getDistortionMutable()->getParametersMutable()));
                problem.AddParameterBlock(camera->getDistortionMutable()->getParametersMutable(), camera->getDistortion().getParameterSize());
                problem.SetParameterBlockConstant(camera->getDistortionMutable()->getParametersMutable());
                intrinsics_map.insert(std::make_pair(kf->id_, camera->getParametersMutable()));
                problem.AddParameterBlock(camera->getParametersMutable(), camera->getParameterSize());
                problem.SetParameterBlockConstant(camera->getParametersMutable());
            } else {
              std::cout << COUTFATAL << "Unknown projection type." << std::endl;
              exit(-1);
            }

            // Add the IMU factors
            if(!visual_only) {

                KeyframePtr pred = kf->GetPredecessor();
                if(!pred || pred->IsInvalid()) {
                    if(kf->id_.first != 0) {
                        std::cout << COUTFATAL << kf << ": no predecessor" << std::endl;
                        exit(-1);
                    }
                    continue;
                }
                if(pred->IsInvalid()) {
                    std::cout << COUTFATAL << kf << ": invalid predecessor" << std::endl;
                    exit(-1);
                }

                if(kf->preintegrated_imu_->getNumMeasurements() == 0) {
                    std::cout << kf << " 0 IMU measurements - skip IMU factor" << std::endl;
                    continue;
                }

                Eigen::Vector3d bias_acc(
                    kf->ceres_velocity_and_bias_[3],
                    kf->ceres_velocity_and_bias_[4],
                    kf->ceres_velocity_and_bias_[5]);
                Eigen::Vector3d bias_gyr(
                    kf->ceres_velocity_and_bias_[6],
                    kf->ceres_velocity_and_bias_[7],
                    kf->ceres_velocity_and_bias_[8]);

                kf->preintegrated_imu_->repropagate(bias_acc,bias_gyr);

                if(!problem.HasParameterBlock(pred->ceres_pose_)) {
                    std::cout << COUTERROR << "parameter block missing" << std::endl;
                    exit(-1);
                }
                if(!problem.HasParameterBlock(kf->ceres_pose_)) {
                    std::cout << COUTERROR << "parameter block missing" << std::endl;
                    exit(-1);
                }
                if(!problem.HasParameterBlock(pred->ceres_velocity_and_bias_)) {
                    std::cout << COUTERROR << "parameter block missing" << std::endl;
                    exit(-1);
                }
                if(!problem.HasParameterBlock(kf->ceres_velocity_and_bias_)) {
                    std::cout << COUTERROR << "parameter block missing" << std::endl;
                    exit(-1);
                }

                ceres::CostFunction* imu_factor = new robopt::imu::PreintegrationFactor(kf->preintegrated_imu_.get());
                ceres::ResidualBlockId blockId = problem.AddResidualBlock(imu_factor, NULL, pred->ceres_pose_,pred->ceres_velocity_and_bias_,kf->ceres_pose_,kf->ceres_velocity_and_bias_);

                imu_factors.push_back(blockId);
                imu_factors_kfs.push_back(kf->id_);
            }
        }

        std::map<KeyframePtr,size_t> obs_per_kf;

        // Add Landmarks
        std::vector<ceres::ResidualBlockId> residual_ids;
        std::vector<KeypointIdentifier> keypoint_ids;
        residual_ids.reserve(landmarks.size()*20);
        keypoint_ids.reserve(landmarks.size()*20);
        int cnt_not_included = 0;
        int cnt_included = 0;
        for(size_t i = 0; i < landmarks.size(); ++i) {
            LandmarkPtr lm = landmarks[i];
            if(lm->IsInvalid()) continue;

            const Landmark::KfObservations observations = lm->GetObservations();
            // Do a pre-check to ensure at least 2 proper observations
            if(observations.size() < th_min_observations) {
                not_included_lms[i] = true;
                cnt_not_included++;
                continue;
            }
            size_t num_edges = 0;
            for(auto mit : observations) {
                KeyframePtr kfx = mit.first;
                if(!kfx || kfx->IsInvalid()) continue;
                num_edges++;
            }
            if(num_edges < th_min_observations) {
                not_included_lms[i] = true;
                cnt_not_included++;
                continue;
            }
            Vector3Type pos_w = lm->GetWorldPos();
            lm->ceres_pos_[0] = pos_w[0];
            lm->ceres_pos_[1] = pos_w[1];
            lm->ceres_pos_[2] = pos_w[2];
            problem.AddParameterBlock(lm->ceres_pos_,robopt::defs::visual::kPositionBlockSize);

            cnt_included++;

            for(auto mit : observations) {
                KeyframePtr kfx = mit.first;
                if(!kfx || kfx->IsInvalid()) continue;
                int feat_id = mit.second;

                if(obs_per_kf.find(kfx) != obs_per_kf.end())
                    obs_per_kf[kfx]++;
                else
                  obs_per_kf[kfx]=1;

                if(intrinsics_map.find(kfx->id_) == intrinsics_map.end()) {
                    std::cout << COUTFATAL << "cannot find " << kfx << " in intrinsics_map" << std::endl;
                    exit(-1);
                }

                Eigen::Vector2d kpx = Utils::FromKeypointType(kfx->keypoints_distorted_[feat_id]);
                const precision_t obs_sigma = (kfx->keypoints_aors_[feat_id][1] + 1) * 2.0;

                ceres::CostFunction* reprojection_error;
                const aslam::Camera::Type camera_type = kfx->camera_->getType();
                const aslam::Distortion::Type distortion_type = kfx->camera_->getDistortion().getType();
                if (camera_type == aslam::Camera::Type::kPinhole) {
                    std::shared_ptr<aslam::PinholeCamera> camera = std::static_pointer_cast<aslam::PinholeCamera>(kfx->camera_);
                    switch (distortion_type) {
                        case aslam::Distortion::Type::kEquidistant :
                            reprojection_error = new robopt::reprojection::GlobalEuclideanReprError<aslam::PinholeCamera, aslam::EquidistantDistortion>(kpx, obs_sigma, camera.get());
                            break;
                        case aslam::Distortion::Type::kRadTan :
                            reprojection_error = new robopt::reprojection::GlobalEuclideanReprError<aslam::PinholeCamera, aslam::RadTanDistortion>(kpx, obs_sigma, camera.get());
                            break;
                        case aslam::Distortion::Type::kFisheye :
                            reprojection_error = new robopt::reprojection::GlobalEuclideanReprError<aslam::PinholeCamera, aslam::FisheyeDistortion>(kpx, obs_sigma, camera.get());
                            break;
                        default:
                            std::cout << COUTFATAL << "Unknown distortion type." << std::endl;
                            exit(-1);
                            break;
                    }
                } else if (camera_type == aslam::Camera::Type::kUnifiedProjection) {
                    std::shared_ptr<aslam::UnifiedProjectionCamera> camera = std::static_pointer_cast<aslam::UnifiedProjectionCamera>(kfx->camera_);
                    switch (distortion_type) {
                        case aslam::Distortion::Type::kEquidistant :
                            reprojection_error = new robopt::reprojection::GlobalEuclideanReprError<aslam::UnifiedProjectionCamera, aslam::EquidistantDistortion>(kpx, obs_sigma, camera.get());
                            break;
                        case aslam::Distortion::Type::kRadTan :
                            reprojection_error = new robopt::reprojection::GlobalEuclideanReprError<aslam::UnifiedProjectionCamera, aslam::RadTanDistortion>(kpx, obs_sigma, camera.get());
                            break;
                        case aslam::Distortion::Type::kFisheye :
                            reprojection_error = new robopt::reprojection::GlobalEuclideanReprError<aslam::UnifiedProjectionCamera, aslam::FisheyeDistortion>(kpx, obs_sigma, camera.get());
                            break;
                        default:
                            std::cout << COUTFATAL << "Unknown distortion type." << std::endl;
                            exit(-1);
                            break;

                    }
                } else {
                    std::cout << COUTFATAL << "Unknown projection type." << std::endl;
                    exit(-1);
                }

                ceres::ResidualBlockId block_id = problem.AddResidualBlock(reprojection_error, loss_function,
                                                                           kfx->ceres_pose_, kfx->ceres_extrinsics_, lm->ceres_pos_,
                                                                           intrinsics_map.at(kfx->id_), distortion_map.at(kfx->id_));
                KeypointIdentifier tmp_ident(kfx, lm, feat_id);
                residual_ids.push_back(block_id);
                keypoint_ids.push_back(tmp_ident);
            }
        }

        std::cout << "--> Landmarks included|not: " << cnt_included << " | " << cnt_not_included << std::endl;

        Eigen::Matrix<precision_t,6,6> sqrt_info = Eigen::Matrix<precision_t,6,6>::Identity();
        sqrt_info.topLeftCorner<3,3>() *= 100.0;
        sqrt_info.bottomRightCorner<3,3>() *= 1e4;

        // Set Loop Edges
        if(covins_params::opt::gba_use_map_loop_constraints) {
            Map::LoopVector loops = map->GetLoopConstraints();
            for(auto i : loops) {
                KeyframePtr kf1 = i.kf1;
                KeyframePtr kf2 = i.kf2;
                TransformType T_12 = i.T_s1_s2;

                if(!problem.HasParameterBlock(kf1->ceres_pose_) || !problem.HasParameterBlock(kf2->ceres_pose_)) {
                    std::cout << COUTWARN << "Loop KF missing -- skip loop between " << kf1 << " and " << kf2 << std::endl;
                    continue;
                }

                Vector3Type t_12 = T_12.block<3,1>(0,3);
                QuaternionType q_12(T_12.block<3,3>(0,0));

                ceres::CostFunction* loop_edge = new robopt::posegraph::SixDofBetweenError(q_12, t_12, sqrt_info, robopt::defs::pose::PoseErrorType::kImu);
                problem.AddResidualBlock(loop_edge, loss_function, kf1->ceres_pose_, kf2->ceres_pose_, kf1->ceres_extrinsics_, kf2->ceres_extrinsics_);
            }
        }

        // Solve
        ceres::Solver::Options solver_options;
        solver_options.linear_solver_type = ceres::SPARSE_SCHUR;
        solver_options.num_threads = covins_params::sys::threads_server;
        solver_options.num_linear_solver_threads = covins_params::sys::threads_server;
        solver_options.trust_region_strategy_type = ceres::DOGLEG;
        solver_options.max_num_iterations = interations_limit;
        ceres::Solver::Summary summary;
        ceres::Solve(solver_options, &problem, &summary);
    //    std::cout << "GBA - Optimization" << std::endl;

        // Recover optimized data
        // Keyframes
        for(const auto& i : keyframes) {
            KeyframePtr kf = i;
            if(kf->IsInvalid()) {
                std::cout << COUTWARN << kf << ": invalid" << std::endl;
                continue;
            }

            TransformType T_ws_corrected = Utils::Ceres2Transform(kf->ceres_pose_);
            kf->SetPoseTws(T_ws_corrected);
            kf->SetPoseOptimized();
            Vector3Type vel(kf->ceres_velocity_and_bias_[0],
                            kf->ceres_velocity_and_bias_[1],
                            kf->ceres_velocity_and_bias_[2]);
            Vector3Type bA(kf->ceres_velocity_and_bias_[3],
                           kf->ceres_velocity_and_bias_[4],
                           kf->ceres_velocity_and_bias_[5]);
            Vector3Type bG(kf->ceres_velocity_and_bias_[6],
                           kf->ceres_velocity_and_bias_[7],
                           kf->ceres_velocity_and_bias_[8]);
            if(!visual_only) kf->SetStateBias(bA, bG);
            if(!visual_only) kf->SetStateVelocity(vel);
            if(!visual_only) kf->SetVelBiasOptimized();
            kf->is_gba_optimized_ = true;
        }

        // Landmarks
        for(size_t i = 0; i < landmarks.size(); ++i) {
            if(not_included_lms[i]) continue;
            LandmarkPtr lm = landmarks[i];
            if(lm->IsInvalid()) {
                std::cout << COUTWARN << lm << ": invalid" << std::endl;
                continue;
            }
            Vector3Type pos_w_corrected(lm->ceres_pos_[0],lm->ceres_pos_[1],lm->ceres_pos_[2]);
            lm->SetWorldPos(pos_w_corrected);;
            lm->SetOptimized();
            lm->is_gba_optimized_ = true;
        }
    }

    // Clean map
    std::cout << "--> Clean Map" << std::endl;
    map->Clean();
    std::cout << "--> done." << std::endl;

    std::cout << "+++ GBA: End +++" << std::endl;
}

auto Optimization::LocalBundleAdjustment(std::vector<std::shared_ptr<LocalLM>> lms,
                                         KeyframeVector QKFs,
                                         KeyframeVector CKFs,
                                         TransformType T_s1s2, TypeDefs::Matrix6Type &cov_BA, size_t cnt) -> void {

  //Ts1s2 is T_Qs_Cs (Relative TF between IMU frames of Query and Candidate KFs)
  std::cout << "+++ LBA: Start +++" << std::endl;
  {
    ceres::Problem::Options problem_options;
    problem_options.enable_fast_removal = true;
    ceres::Problem problem(problem_options);

    ceres::LossFunction *loss_function;
    loss_function = new ceres::CauchyLoss(1.0);
    ceres::LocalParameterization *local_pose_param =
        new robopt::local_param::PoseQuaternionLocalParameterization();
    
    std::map<idpair, double*> distortion_map;
    std::map<idpair, double *> intrinsics_map;
    
    KeyframePtr QKF = QKFs[0];
    KeyframePtr CKF = CKFs[0];

    // Add Query KFs
    for (size_t i = 0; i < QKFs.size(); ++i) {
      KeyframePtr kf = QKFs[i];

      if (i == 0) {
        // This is the QKF, set the pose to Identity and constant
        kf->UpdateCeresFromState(kf->ceres_pose_, kf->ceres_velocity_and_bias_,
                                 kf->ceres_extrinsics_);
        Matrix3Type R = Matrix3Type::Identity();
        QuaternionType qws(R);

        kf->ceres_pose_local_[0]= qws.x();
        kf->ceres_pose_local_[1]= qws.y();
        kf->ceres_pose_local_[2]= qws.z();
        kf->ceres_pose_local_[3]= qws.w();
        kf->ceres_pose_local_[4]= 0;
        kf->ceres_pose_local_[5]= 0;
        kf->ceres_pose_local_[6]= 0;

        problem.AddParameterBlock(kf->ceres_pose_local_, robopt::defs::pose::kPoseBlockSize, local_pose_param);
        problem.SetParameterBlockConstant(kf->ceres_pose_local_);
        problem.AddParameterBlock(kf->ceres_extrinsics_, robopt::defs::pose::kPoseBlockSize, local_pose_param);
        problem.SetParameterBlockConstant(kf->ceres_extrinsics_);

        const aslam::Camera::Type camera_type = kf->camera_->getType();
            if (camera_type == aslam::Camera::Type::kPinhole) {
                std::shared_ptr<aslam::PinholeCamera> camera = std::static_pointer_cast<aslam::PinholeCamera>(kf->camera_);
                distortion_map.insert(std::make_pair(kf->id_, camera->getDistortionMutable()->getParametersMutable()));
                problem.AddParameterBlock(camera->getDistortionMutable()->getParametersMutable(), camera->getDistortion().getParameterSize());
                problem.SetParameterBlockConstant(camera->getDistortionMutable()->getParametersMutable());
                intrinsics_map.insert(std::make_pair(kf->id_,camera->getParametersMutable()));
                problem.AddParameterBlock(camera->getParametersMutable(), camera->getParameterSize());
                problem.SetParameterBlockConstant(camera->getParametersMutable());
            } else if (camera_type == aslam::Camera::Type::kUnifiedProjection) {
                std::shared_ptr<aslam::UnifiedProjectionCamera> camera = std::static_pointer_cast<aslam::UnifiedProjectionCamera>(kf->camera_);
                distortion_map.insert(std::make_pair(kf->id_, camera->getDistortionMutable()->getParametersMutable()));
                problem.AddParameterBlock(camera->getDistortionMutable()->getParametersMutable(), camera->getDistortion().getParameterSize());
                problem.SetParameterBlockConstant(camera->getDistortionMutable()->getParametersMutable());
                intrinsics_map.insert(std::make_pair(kf->id_, camera->getParametersMutable()));
                problem.AddParameterBlock(camera->getParametersMutable(), camera->getParameterSize());
                problem.SetParameterBlockConstant(camera->getParametersMutable());
            } else {
              std::cout << COUTFATAL << "Unknown projection type." << std::endl;
              exit(-1);
            }
      } else {
        // This is Neighboring KF of QKF
        kf->UpdateCeresFromState(kf->ceres_pose_, kf->ceres_velocity_and_bias_,
                                 kf->ceres_extrinsics_);
        TransformType T_wsi = QKF->GetPoseTsw() * kf->GetPoseTws();
        Matrix3Type Rwsi = T_wsi.block<3,3>(0,0);
        QuaternionType qwsi(Rwsi);
        kf->ceres_pose_local_[0]= qwsi.x();
        kf->ceres_pose_local_[1]= qwsi.y();
        kf->ceres_pose_local_[2]= qwsi.z();
        kf->ceres_pose_local_[3]= qwsi.w();
        kf->ceres_pose_local_[4]= T_wsi(0,3);
        kf->ceres_pose_local_[5]= T_wsi(1,3);
        kf->ceres_pose_local_[6] = T_wsi(2, 3);
        problem.AddParameterBlock(kf->ceres_pose_local_, robopt::defs::pose::kPoseBlockSize, local_pose_param);
        problem.AddParameterBlock(kf->ceres_extrinsics_, robopt::defs::pose::kPoseBlockSize, local_pose_param);
        problem.SetParameterBlockConstant(kf->ceres_extrinsics_);
        // Add camera parameters
        const aslam::Camera::Type camera_type = kf->camera_->getType();
        if (camera_type == aslam::Camera::Type::kPinhole) {
            std::shared_ptr<aslam::PinholeCamera> camera = std::static_pointer_cast<aslam::PinholeCamera>(kf->camera_);
            distortion_map.insert(std::make_pair(kf->id_, camera->getDistortionMutable()->getParametersMutable()));
            problem.AddParameterBlock(camera->getDistortionMutable()->getParametersMutable(), camera->getDistortion().getParameterSize());
            problem.SetParameterBlockConstant(camera->getDistortionMutable()->getParametersMutable());
            intrinsics_map.insert(std::make_pair(kf->id_,camera->getParametersMutable()));
            problem.AddParameterBlock(camera->getParametersMutable(), camera->getParameterSize());
            problem.SetParameterBlockConstant(camera->getParametersMutable());
        } else if (camera_type == aslam::Camera::Type::kUnifiedProjection) {
            std::shared_ptr<aslam::UnifiedProjectionCamera> camera = std::static_pointer_cast<aslam::UnifiedProjectionCamera>(kf->camera_);
            distortion_map.insert(std::make_pair(kf->id_, camera->getDistortionMutable()->getParametersMutable()));
            problem.AddParameterBlock(camera->getDistortionMutable()->getParametersMutable(), camera->getDistortion().getParameterSize());
            problem.SetParameterBlockConstant(camera->getDistortionMutable()->getParametersMutable());
            intrinsics_map.insert(std::make_pair(kf->id_, camera->getParametersMutable()));
            problem.AddParameterBlock(camera->getParametersMutable(), camera->getParameterSize());
            problem.SetParameterBlockConstant(camera->getParametersMutable());
        } else {
            std::cout << COUTFATAL << "Unknown projection type." << std::endl;
            exit(-1);
        }
      }
    }
    // Add Candidate KFs

    for (size_t i = 0; i < CKFs.size(); ++i) {

        KeyframePtr kf = CKFs[i];
        //Probably Not required to update
        kf->UpdateCeresFromState(kf->ceres_pose_, kf->ceres_velocity_and_bias_,
                                 kf->ceres_extrinsics_);

        // Transformation = T_QC * T_CW * T_WCi
        TransformType T_wsi = T_s1s2 * CKF->GetPoseTsw() * kf->GetPoseTws();
        Matrix3Type Rwsi = T_wsi.block<3,3>(0,0);
        QuaternionType qwsi(Rwsi);
        kf->ceres_pose_local_[0]= qwsi.x();
        kf->ceres_pose_local_[1]= qwsi.y();
        kf->ceres_pose_local_[2]= qwsi.z();
        kf->ceres_pose_local_[3]= qwsi.w();
        kf->ceres_pose_local_[4]= T_wsi(0,3);
        kf->ceres_pose_local_[5]= T_wsi(1,3);
        kf->ceres_pose_local_[6] = T_wsi(2, 3);
        problem.AddParameterBlock(kf->ceres_pose_local_, robopt::defs::pose::kPoseBlockSize, local_pose_param);
        problem.AddParameterBlock(kf->ceres_extrinsics_, robopt::defs::pose::kPoseBlockSize, local_pose_param);
        problem.SetParameterBlockConstant(kf->ceres_extrinsics_);
        // Add camera parameters
        const aslam::Camera::Type camera_type = kf->camera_->getType();
        if (camera_type == aslam::Camera::Type::kPinhole) {
            std::shared_ptr<aslam::PinholeCamera> camera = std::static_pointer_cast<aslam::PinholeCamera>(kf->camera_);
            distortion_map.insert(std::make_pair(kf->id_, camera->getDistortionMutable()->getParametersMutable()));
            problem.AddParameterBlock(camera->getDistortionMutable()->getParametersMutable(), camera->getDistortion().getParameterSize());
            problem.SetParameterBlockConstant(camera->getDistortionMutable()->getParametersMutable());
            intrinsics_map.insert(std::make_pair(kf->id_,camera->getParametersMutable()));
            problem.AddParameterBlock(camera->getParametersMutable(), camera->getParameterSize());
            problem.SetParameterBlockConstant(camera->getParametersMutable());
        } else if (camera_type == aslam::Camera::Type::kUnifiedProjection) {
            std::shared_ptr<aslam::UnifiedProjectionCamera> camera = std::static_pointer_cast<aslam::UnifiedProjectionCamera>(kf->camera_);
            distortion_map.insert(std::make_pair(kf->id_, camera->getDistortionMutable()->getParametersMutable()));
            problem.AddParameterBlock(camera->getDistortionMutable()->getParametersMutable(), camera->getDistortion().getParameterSize());
            problem.SetParameterBlockConstant(camera->getDistortionMutable()->getParametersMutable());
            intrinsics_map.insert(std::make_pair(kf->id_, camera->getParametersMutable()));
            problem.AddParameterBlock(camera->getParametersMutable(), camera->getParameterSize());
            problem.SetParameterBlockConstant(camera->getParametersMutable());
        } else {
            std::cout << COUTFATAL << "Unknown projection type." << std::endl;
            exit(-1);
        }  
    }

        // Add Landmarks


    for (size_t i = 0; i < lms.size(); ++i) {
      
        std::shared_ptr<LocalLM> lm = lms[i];

        Vector3Type pos_w = lm->pos_l_;
        lm->ceres_pos_[0] = pos_w[0];
        lm->ceres_pos_[1] = pos_w[1];
        lm->ceres_pos_[2] = pos_w[2];
        problem.AddParameterBlock(lm->ceres_pos_,robopt::defs::visual::kPositionBlockSize);

        for (auto mit : lm->observations_) {
            KeyframePtr kfx = mit.first;
            if(!kfx || kfx->IsInvalid()) continue;
            int feat_id = mit.second;

            if(intrinsics_map.find(kfx->id_) == intrinsics_map.end()) {
                    std::cout << COUTFATAL << "cannot find " << kfx << " in intrinsics_map" << std::endl;
                    exit(-1);
            }
        
            Eigen::Vector2d kpx = Utils::FromKeypointType(kfx->keypoints_distorted_add_[feat_id]);
            const precision_t obs_sigma = (kfx->keypoints_aors_add_[feat_id][1] + 1) * 2.0;

            ceres::CostFunction* reprojection_error;
            const aslam::Camera::Type camera_type = kfx->camera_->getType();
            const aslam::Distortion::Type distortion_type = kfx->camera_->getDistortion().getType();
            if (camera_type == aslam::Camera::Type::kPinhole) {
                std::shared_ptr<aslam::PinholeCamera> camera = std::static_pointer_cast<aslam::PinholeCamera>(kfx->camera_);
                switch (distortion_type) {
                    case aslam::Distortion::Type::kEquidistant :
                        reprojection_error = new robopt::reprojection::GlobalEuclideanReprError<aslam::PinholeCamera, aslam::EquidistantDistortion>(kpx, obs_sigma, camera.get());
                        break;
                    case aslam::Distortion::Type::kRadTan :
                        reprojection_error = new robopt::reprojection::GlobalEuclideanReprError<aslam::PinholeCamera, aslam::RadTanDistortion>(kpx, obs_sigma, camera.get());
                        break;
                    case aslam::Distortion::Type::kFisheye :
                        reprojection_error = new robopt::reprojection::GlobalEuclideanReprError<aslam::PinholeCamera, aslam::FisheyeDistortion>(kpx, obs_sigma, camera.get());
                        break;
                    default:
                        std::cout << COUTFATAL << "Unknown distortion type." << std::endl;
                        exit(-1);
                        break;
                }
            } else if (camera_type == aslam::Camera::Type::kUnifiedProjection) {
                std::shared_ptr<aslam::UnifiedProjectionCamera> camera = std::static_pointer_cast<aslam::UnifiedProjectionCamera>(kfx->camera_);
                switch (distortion_type) {
                    case aslam::Distortion::Type::kEquidistant :
                        reprojection_error = new robopt::reprojection::GlobalEuclideanReprError<aslam::UnifiedProjectionCamera, aslam::EquidistantDistortion>(kpx, obs_sigma, camera.get());
                        break;
                    case aslam::Distortion::Type::kRadTan :
                        reprojection_error = new robopt::reprojection::GlobalEuclideanReprError<aslam::UnifiedProjectionCamera, aslam::RadTanDistortion>(kpx, obs_sigma, camera.get());
                        break;
                    case aslam::Distortion::Type::kFisheye :
                        reprojection_error = new robopt::reprojection::GlobalEuclideanReprError<aslam::UnifiedProjectionCamera, aslam::FisheyeDistortion>(kpx, obs_sigma, camera.get());
                        break;
                    default:
                        std::cout << COUTFATAL << "Unknown distortion type." << std::endl;
                        exit(-1);
                        break;

                }
            } else {
                std::cout << COUTFATAL << "Unknown projection type." << std::endl;
                exit(-1);
            }

            problem.AddResidualBlock(
                reprojection_error, loss_function, kfx->ceres_pose_local_,
                kfx->ceres_extrinsics_, lm->ceres_pos_,
                intrinsics_map.at(kfx->id_), distortion_map.at(kfx->id_));
        }

    }
    
    Eigen::Matrix<precision_t,6,6> sqrt_info = Eigen::Matrix<precision_t,6,6>::Identity();
    sqrt_info.topLeftCorner<3,3>() *= 100.0;
    sqrt_info.bottomRightCorner<3,3>() *= 1e4;

    // Set Loop Edges

    if (1) {
      
      //CKF edges
      for (size_t i = 1; i < CKFs.size(); ++i) {
        
        KeyframePtr kf1 = CKF;
        KeyframePtr kf2 = CKFs[i];
        TransformType T_12 = kf1->GetPoseTsw() * kf2->GetPoseTws();

        Vector3Type t_12 = T_12.block<3,1>(0,3);
        QuaternionType q_12(T_12.block<3,3>(0,0));

        ceres::CostFunction* loop_edge = new robopt::posegraph::SixDofBetweenError(q_12, t_12, sqrt_info, robopt::defs::pose::PoseErrorType::kImu);
        problem.AddResidualBlock(loop_edge, loss_function, kf1->ceres_pose_local_, kf2->ceres_pose_local_, kf1->ceres_extrinsics_, kf2->ceres_extrinsics_);
      }

      // QKF edges
      for (size_t i = 1; i < QKFs.size(); ++i) {
        
        KeyframePtr kf1 = QKF;
        KeyframePtr kf2 = QKFs[i];
        TransformType T_12 = kf1->GetPoseTsw() * kf2->GetPoseTws();

        Vector3Type t_12 = T_12.block<3,1>(0,3);
        QuaternionType q_12(T_12.block<3,3>(0,0));

        ceres::CostFunction* loop_edge = new robopt::posegraph::SixDofBetweenError(q_12, t_12, sqrt_info, robopt::defs::pose::PoseErrorType::kImu);
        problem.AddResidualBlock(loop_edge, loss_function, kf1->ceres_pose_local_, kf2->ceres_pose_local_, kf1->ceres_extrinsics_, kf2->ceres_extrinsics_);
      }

    }

    // Solve
    ceres::Solver::Options solver_options;
    solver_options.linear_solver_type = ceres::SPARSE_SCHUR;
    solver_options.num_threads = covins_params::sys::threads_server;
    solver_options.num_linear_solver_threads = covins_params::sys::threads_server;
    solver_options.trust_region_strategy_type = ceres::DOGLEG;
    solver_options.max_num_iterations = 10000;
    ceres::Solver::Summary summary;
    ceres::Solve(solver_options, &problem, &summary);
    std::cout << summary.FullReport() << "\n";


    // std::cout << "Translation:" << CKF->ceres_pose_local_[4] << " | "
    //           << CKF->ceres_pose_local_[5] << " | " << CKF->ceres_pose_local_[6] << std::endl;
    TransformType T_s1s2_optim = Utils::Ceres2Transform(CKF->ceres_pose_local_);
    std::cout << "T_smatch_squery_LBA: " << std::endl
              << T_s1s2_optim.inverse() << std::endl;

    std::cout << "+++ LBA: End +++" << std::endl;

    std::cout << "+++ Starting: Covariance Estimation +++" << std::endl;

    // Get the covariance
    ceres::Problem::EvaluateOptions evalOpts;

    evalOpts.apply_loss_function = true;
    std::vector<double *> eval_blocks;

    // Add Poses for Candidates (3)
    for (size_t i = 0; i < CKFs.size(); ++i) {
      eval_blocks.push_back(CKFs[i]->ceres_pose_local_);
    }
    // Add Poses for Query without itself
    for (size_t i = 1; i < QKFs.size(); ++i) {
      eval_blocks.push_back(QKFs[i]->ceres_pose_local_);
    }

    // Add Landmarks
    for (size_t i = 0; i < lms.size(); ++i) {
      eval_blocks.push_back(lms[i]->ceres_pos_);
    }

    std::string ss;
    std::stringstream sss;
    ss = "/home/manthan/ws/covins_ws/src/covins/covins_backend/output/"
         "jacobians/results_Jacobian";
    sss << "/home/manthan/ws/covins_ws/src/covins/covins_backend/output/"
           "jacobians/";

    uint64_t t = std::chrono::duration_cast<std::chrono::milliseconds>(
                     std::chrono::system_clock::now().time_since_epoch())
                     .count();
    
    sss << t << ".csv";
    // std::cout << t << std::endl;
    ss += t;
    ss += ".csv";

    evalOpts.parameter_blocks = eval_blocks;
    ceres::CRSMatrix jacobianCRS;
    problem.Evaluate(evalOpts, NULL, NULL, NULL, &jacobianCRS);
    const Eigen::MatrixXd jacobianDense = transformJacobian(jacobianCRS);
    // std::cout << sss.str() << std::endl;
    writeToCSVfile(sss.str(), jacobianDense);

    cov_BA = (jacobianDense.transpose() * jacobianDense)
                              .completeOrthogonalDecomposition()
                              .pseudoInverse()
                              .block<6, 6>(0, 0);

    std::ofstream file_covBA("/home/manthan/ws/covins_ws/src/covins/"
                             "covins_backend/output/results_cov_BA.csv",
                             std::ios::app);

    const static Eigen::IOFormat csvf(Eigen::StreamPrecision, Eigen::DontAlignCols, ", ", ",");
    file_covBA << cov_BA.format(csvf);
    file_covBA << "\n";

    std::cout << cov_BA << std::endl;

    // ceres::Covariance::Options options;
    // options.algorithm_type = ceres::DENSE_SVD;
    // options.null_space_rank = -1;
    // // options.sparse_linear_algebra_library_type = ceres::SUITE_SPARSE;
    // options.num_threads = 6;
    // ceres::Covariance covariance(options);
    // std::vector<std::pair<const double *, const double *>> covariance_blocks;
    // covariance_blocks.push_back(
    //     std::make_pair(CKF->ceres_pose_local_, CKF->ceres_pose_local_));
    // covariance.Compute(covariance_blocks, &problem);
    // double covariance_xx[6 * 6];
    // covariance.GetCovarianceBlockInTangentSpace(CKF->ceres_pose_local_,
    //                               CKF->ceres_pose_local_, covariance_xx);
    // std::cout << covariance_xx << std::endl;

    // for (int i = 0; i < 36; ++i) {
    //   if (i % 6 == 0) {
    //     std::cout << std::endl;
    //   }
    //   std::cout << covariance_xx[i] << " ";
    // }
  
  }
}


auto Optimization::OptimizeRelativePose(KeyframePtr kf1, KeyframePtr kf2, LandmarkVector &matches1, TransformType &T12, const precision_t th2)->int {
    ceres::Problem::Options problem_options;
    problem_options.enable_fast_removal = true;
    ceres::Problem problem(problem_options);

    ceres::LossFunction *loss_function;
    loss_function = new ceres::CauchyLoss(1.0);
    ceres::LocalParameterization *local_pose_param = new robopt::local_param::PoseQuaternionLocalParameterization();

    // Transform the relative transformation into array format
    double ceresAB[robopt::defs::visual::kPoseBlockSize];
    Eigen::Quaterniond qAB(T12.block<3,3>(0,0));
    ceresAB[0] = qAB.x();
    ceresAB[1] = qAB.y();
    ceresAB[2] = qAB.z();
    ceresAB[3] = qAB.w();
    ceresAB[4] = T12(0,3);
    ceresAB[5] = T12(1,3);
    ceresAB[6] = T12(2,3);
    problem.AddParameterBlock(ceresAB, robopt::defs::visual::kPoseBlockSize, local_pose_param);

    // Get the poses (to transform the points)
    const Eigen::Matrix4d TcwA = (kf1->GetPoseTws()*kf1->GetStateExtrinsics()).inverse();
    const Eigen::Matrix4d TcwB = (kf2->GetPoseTws()*kf1->GetStateExtrinsics()).inverse();

    // Add the observation
    const int N = matches1.size();
    const LandmarkVector vpMapPointsA = kf1->GetLandmarks();
    int numCorrespondences = 0;
    std::vector<int> vIndex;
    vIndex.reserve(N);
    std::vector<ceres::ResidualBlockId> residIdsA, residIdsB;

    residIdsA.reserve(N);
    residIdsB.reserve(N);
    for (int i = 0; i < N; ++i) {
        if (!matches1[i]) continue;

        LandmarkPtr pMPA = vpMapPointsA[i];
        LandmarkPtr pMPB = matches1[i];

        const int iB = pMPB->GetFeatureIndex(kf2);
        if (pMPA && pMPB) {
            if (!pMPA->IsInvalid() && !pMPB->IsInvalid() && iB >= 0) {
                // Get the map point positions
                Eigen::Vector3d P3DAw = pMPA->GetWorldPos();
                Eigen::Vector3d P3DAc = TcwA.block<3,3>(0,0)*P3DAw + TcwA.block<3,1>(0,3);
                Eigen::Vector3d P3DBw = pMPB->GetWorldPos();
                Eigen::Vector3d P3DBc = TcwB.block<3,3>(0,0)*P3DBw + TcwB.block<3,1>(0,3);

                // Get the observation
                Eigen::Vector2d kpObsA = Utils::FromKeypointType(kf1->keypoints_distorted_[i]);
                const double obs_sigma_A = (kf1->keypoints_aors_[i][1] + 1) * 2.0;

                // Cam A
                ceres::CostFunction* reprojection_error_A;
                const aslam::Camera::Type camera_type_A = kf1->camera_->getType();
                const aslam::Distortion::Type distortion_type_A = kf1->camera_->getDistortion().getType();
                if (camera_type_A == aslam::Camera::Type::kPinhole) {
                    std::shared_ptr<aslam::PinholeCamera> camera_A = std::static_pointer_cast<aslam::PinholeCamera>(kf1->camera_);
                    switch (distortion_type_A) {
                        case aslam::Distortion::Type::kEquidistant :
                            reprojection_error_A = new robopt::reprojection::RelativeEuclideanReprError<aslam::PinholeCamera, aslam::EquidistantDistortion>(kpObsA, obs_sigma_A, camera_A.get(), P3DBc, robopt::defs::visual::RelativeProjectionType::kNormal);
                            break;
                        case aslam::Distortion::Type::kRadTan :
                            reprojection_error_A = new robopt::reprojection::RelativeEuclideanReprError<aslam::PinholeCamera, aslam::RadTanDistortion>(kpObsA, obs_sigma_A, camera_A.get(), P3DBc, robopt::defs::visual::RelativeProjectionType::kNormal);
                            break;
                        case aslam::Distortion::Type::kFisheye :
                            reprojection_error_A = new robopt::reprojection::RelativeEuclideanReprError<aslam::PinholeCamera, aslam::FisheyeDistortion>(kpObsA, obs_sigma_A, camera_A.get(), P3DBc, robopt::defs::visual::RelativeProjectionType::kNormal);
                            break;
                        default:
                            std::cout << COUTFATAL << "Unknown distortion type." << std::endl;
                            exit(-1);
                            break;
                    }
                } else if (camera_type_A == aslam::Camera::Type::kUnifiedProjection) {
                    std::shared_ptr<aslam::UnifiedProjectionCamera> camera = std::static_pointer_cast<aslam::UnifiedProjectionCamera>(kf1->camera_);
                    switch (distortion_type_A) {
                        case aslam::Distortion::Type::kEquidistant :
                            reprojection_error_A = new robopt::reprojection::RelativeEuclideanReprError<aslam::UnifiedProjectionCamera, aslam::EquidistantDistortion>(kpObsA, obs_sigma_A, camera.get(), P3DBc, robopt::defs::visual::RelativeProjectionType::kNormal);
                            break;
                        case aslam::Distortion::Type::kRadTan :
                            reprojection_error_A = new robopt::reprojection::RelativeEuclideanReprError<aslam::UnifiedProjectionCamera, aslam::RadTanDistortion>(kpObsA, obs_sigma_A, camera.get(), P3DBc, robopt::defs::visual::RelativeProjectionType::kNormal);
                            break;
                        case aslam::Distortion::Type::kFisheye :
                            reprojection_error_A = new robopt::reprojection::RelativeEuclideanReprError<aslam::UnifiedProjectionCamera, aslam::FisheyeDistortion>(kpObsA, obs_sigma_A, camera.get(), P3DBc, robopt::defs::visual::RelativeProjectionType::kNormal);
                            break;
                        default:
                            std::cout << COUTFATAL << "Unknown distortion type." << std::endl;
                            exit(-1);
                            break;

                    }
                } else {
                    std::cout << COUTFATAL << "Unknown projection type." << std::endl;
                    exit(-1);
                }

                // Cam B
                Eigen::Vector2d kpObsB = Utils::FromKeypointType(kf2->keypoints_distorted_[iB]);
                const double obs_sigma_B = (kf2->keypoints_aors_[iB][1] + 1) * 2.0;

                ceres::CostFunction* reprojection_error_B;
                const aslam::Camera::Type camera_type_B = kf2->camera_->getType();
                const aslam::Distortion::Type distortion_type_B = kf2->camera_->getDistortion().getType();
                if (camera_type_B == aslam::Camera::Type::kPinhole) {
                    std::shared_ptr<aslam::PinholeCamera> camera_B = std::static_pointer_cast<aslam::PinholeCamera>(kf2->camera_);
                    switch (distortion_type_B) {
                        case aslam::Distortion::Type::kEquidistant :
                            reprojection_error_B = new robopt::reprojection::RelativeEuclideanReprError<aslam::PinholeCamera, aslam::EquidistantDistortion>(kpObsB, obs_sigma_B, camera_B.get(), P3DAc, robopt::defs::visual::RelativeProjectionType::kInverse);
                            break;
                        case aslam::Distortion::Type::kRadTan :
                            reprojection_error_B = new robopt::reprojection::RelativeEuclideanReprError<aslam::PinholeCamera, aslam::RadTanDistortion>(kpObsB, obs_sigma_B, camera_B.get(), P3DAc, robopt::defs::visual::RelativeProjectionType::kInverse);
                            break;
                        case aslam::Distortion::Type::kFisheye :
                            reprojection_error_B = new robopt::reprojection::RelativeEuclideanReprError<aslam::PinholeCamera, aslam::FisheyeDistortion>(kpObsB, obs_sigma_B, camera_B.get(), P3DAc, robopt::defs::visual::RelativeProjectionType::kInverse);
                            break;
                        default:
                            std::cout << COUTFATAL << "Unknown distortion type." << std::endl;
                            exit(-1);
                            break;
                    }
                } else if (camera_type_B == aslam::Camera::Type::kUnifiedProjection) {
                    std::shared_ptr<aslam::UnifiedProjectionCamera> camera = std::static_pointer_cast<aslam::UnifiedProjectionCamera>(kf2->camera_);
                    switch (distortion_type_B) {
                        case aslam::Distortion::Type::kEquidistant :
                            reprojection_error_B = new robopt::reprojection::RelativeEuclideanReprError<aslam::UnifiedProjectionCamera, aslam::EquidistantDistortion>(kpObsB, obs_sigma_B, camera.get(), P3DAc, robopt::defs::visual::RelativeProjectionType::kInverse);
                            break;
                        case aslam::Distortion::Type::kRadTan :
                            reprojection_error_B = new robopt::reprojection::RelativeEuclideanReprError<aslam::UnifiedProjectionCamera, aslam::RadTanDistortion>(kpObsB, obs_sigma_B, camera.get(), P3DAc, robopt::defs::visual::RelativeProjectionType::kInverse);
                            break;
                        case aslam::Distortion::Type::kFisheye :
                            reprojection_error_B = new robopt::reprojection::RelativeEuclideanReprError<aslam::UnifiedProjectionCamera, aslam::FisheyeDistortion>(kpObsB, obs_sigma_B, camera.get(), P3DAc, robopt::defs::visual::RelativeProjectionType::kInverse);
                            break;
                        default:
                            std::cout << COUTFATAL << "Unknown distortion type." << std::endl;
                            exit(-1);
                            break;

                    }
                } else {
                    std::cout << COUTFATAL << "Unknown projection type." << std::endl;
                    exit(-1);
                }

                // Add observation factor
                ceres::ResidualBlockId tmpIdA =
                    problem.AddResidualBlock(reprojection_error_A, loss_function,
                                             ceresAB);
                residIdsA.push_back(tmpIdA);

                ceres::ResidualBlockId tmpIdB =
                    problem.AddResidualBlock(reprojection_error_B, loss_function,
                                             ceresAB);

                residIdsB.push_back(tmpIdB);
                vIndex.push_back(i);
                ++numCorrespondences;
            } else {
                continue;
            }
        } else {
            continue;
        }
    }

    // Solve
    ceres::Solver::Options solver_options;
    solver_options.linear_solver_type = ceres::SPARSE_SCHUR;
    solver_options.num_threads = covins_params::sys::threads_server;
    solver_options.num_linear_solver_threads = covins_params::sys::threads_server;
    solver_options.trust_region_strategy_type = ceres::DOGLEG;
    solver_options.max_num_iterations = 5;
    ceres::Solver::Summary summary;
    ceres::Solve(solver_options, &problem, &summary);
//    std::cout << summary.FullReport() << std::endl;

    // Check for outliers
    ceres::Problem::EvaluateOptions evalOpts;
    evalOpts.residual_blocks = residIdsA;
    double totalCost = 0.0;
    std::vector<double> residualsA;
    problem.Evaluate(evalOpts, &totalCost, &residualsA, NULL, NULL);
    evalOpts.residual_blocks = residIdsB;
    std::vector<double> residualsB;
    problem.Evaluate(evalOpts, &totalCost, &residualsB, NULL, NULL);
    size_t residInd = 0;
    size_t numBad = 0;
    for (size_t i = 0; i < residIdsA.size(); ++i) {
        Eigen::Vector2d residA(residualsA[residInd], residualsA[residInd + 1]);
        Eigen::Vector2d residB(residualsB[residInd], residualsB[residInd + 1]);
        residInd += 2;
        if (residA.norm() > covins_params::opt::th_outlier_align || residB.norm() > covins_params::opt::th_outlier_align) {
            problem.RemoveResidualBlock(residIdsA[i]);
            problem.RemoveResidualBlock(residIdsB[i]);
            matches1[i] = NULL;
            ++numBad;
        }
    }

    // Perform outlier-"free" optimization
    if (numCorrespondences - numBad < 12) {
        return 0;
    }
    solver_options.max_num_iterations = 5;
    ceres::Solve(solver_options, &problem, &summary);
  //  std::cout << summary.FullReport() << std::endl;

    // Recover the transformation
    T12 = Utils::Ceres2Transform(ceresAB);
    return numCorrespondences - numBad;
}

// auto Optimization::PoseGraphOptimization(MapPtr map, PoseMap corrected_poses)->void {

//     ceres::Problem::Options problem_options;
//     problem_options.enable_fast_removal = true;
//     ceres::Problem problem(problem_options);

//     ceres::LocalParameterization *local_pose_param = new robopt::local_param::PoseQuaternionLocalParameterization();

//     KeyframeVector keyframes = map->GetKeyframesVec();
//     LandmarkVector landmarks = map->GetLandmarksVec();

//     // Add Keyframes
//     for (size_t i = 0; i < keyframes.size(); ++i) {
//         KeyframePtr kf = keyframes[i];
//         if(kf->IsInvalid()) continue;

//         TransformType T_ws_init;
//         PoseMap::iterator mit = corrected_poses.find(kf->id_);
//         if(mit != corrected_poses.end()) T_ws_init = mit->second;
//         else T_ws_init = kf->GetPoseTws();

//         // Add parameter blocks
//         kf->UpdateCeresFromState(kf->ceres_pose_,kf->ceres_velocity_and_bias_,kf->ceres_extrinsics_);
//         QuaternionType q_ws_init(T_ws_init.block<3,3>(0,0));
//         kf->ceres_pose_[0] = q_ws_init.x();
//         kf->ceres_pose_[1] = q_ws_init.y();
//         kf->ceres_pose_[2] = q_ws_init.z();
//         kf->ceres_pose_[3] = q_ws_init.w();
//         kf->ceres_pose_[4] = T_ws_init(0,3);
//         kf->ceres_pose_[5] = T_ws_init(1,3);
//         kf->ceres_pose_[6] = T_ws_init(2,3);
//         problem.AddParameterBlock(kf->ceres_pose_, robopt::defs::pose::kPoseBlockSize, local_pose_param);
//         if(kf->id_.first == 0 && kf->id_.second == map->id_map_)
//             problem.SetParameterBlockConstant(kf->ceres_pose_);
//         problem.AddParameterBlock(kf->ceres_extrinsics_, robopt::defs::pose::kPoseBlockSize, local_pose_param);
//         problem.SetParameterBlockConstant(kf->ceres_extrinsics_);

//         if(kf->is_gba_optimized_ && covins_params::opt::pgo_fix_kfs_after_gba) {
//             if(kf->id_.first % 50 == 0) std::cout << COUTNOTICE << "Set GBA KFs constant" << std::endl;
//             problem.SetParameterBlockConstant(kf->ceres_pose_);
//         } else if(kf->is_loaded_ && covins_params::opt::pgo_fix_poses_loaded_maps) {
//             if(kf->id_.first % 50 == 0) std::cout << COUTNOTICE << "Set loaded KFs constant" << std::endl;
//             problem.SetParameterBlockConstant(kf->ceres_pose_);
//         }
//     }

//     Eigen::Matrix<precision_t,6,6> sqrt_info = Eigen::Matrix<precision_t,6,6>::Identity();
//     sqrt_info.topLeftCorner<3,3>() *= 100.0;
//     sqrt_info.bottomRightCorner<3,3>() *= 1e4;
//     std::set<std::pair<KeyframePtr,KeyframePtr>> inserted_edges;

//     // Current loop edges
//     {
//         Map::LoopVector loops = map->GetLoopConstraints();
//         auto i = loops[loops.size()-1];
//         KeyframePtr kf1 = i.kf1;
//         KeyframePtr kf2 = i.kf2;
//         TransformType T_12 = i.T_s1_s2;
//         Vector3Type t_12 = T_12.block<3,1>(0,3);
//         QuaternionType q_12(T_12.block<3,3>(0,0));
//         ceres::CostFunction* f = new robopt::posegraph::SixDofBetweenError(q_12, t_12, sqrt_info, robopt::defs::pose::PoseErrorType::kImu);
//         problem.AddResidualBlock(f, NULL, kf1->ceres_pose_, kf2->ceres_pose_, kf1->ceres_extrinsics_, kf2->ceres_extrinsics_);
//     }

//     // Set keyframe edges
//     for (size_t i = 0; i < keyframes.size(); ++i) {
//         KeyframePtr kf = keyframes[i];
//         if(kf->IsInvalid()) continue;

//         TransformType T_w_si = kf->GetPoseTws();

//         // Edge to successor
//         KeyframePtr succ = kf->GetSuccessor();
//         if(!succ) continue;
//         TransformType T_w_ssucc = succ->GetPoseTws();
//         TransformType T_si_ssucc = T_w_si.inverse() * T_w_ssucc;
//         Vector3Type t_si_ssucc = T_si_ssucc.block<3,1>(0,3);
//         QuaternionType q_si_ssucc(T_si_ssucc.block<3,3>(0,0));

//         std::pair<KeyframePtr,KeyframePtr> kf_pair = std::make_pair(kf,succ);
//         if(inserted_edges.count(kf_pair)) {
//             std::cout << COUTWARN << "KF edge already added" << std::endl;
//             continue;
//         } else {
//             inserted_edges.insert(kf_pair);
//         }

//         ceres::CostFunction* f = new robopt::posegraph::SixDofBetweenError(q_si_ssucc, t_si_ssucc, sqrt_info, robopt::defs::pose::PoseErrorType::kImu);
//         problem.AddResidualBlock(f, NULL, kf->ceres_pose_, succ->ceres_pose_, kf->ceres_extrinsics_, succ->ceres_extrinsics_);
//     }

//     // Solve
//     ceres::Solver::Options solver_options;
//     solver_options.linear_solver_type = ceres::SPARSE_SCHUR;
//     solver_options.num_threads = covins_params::sys::threads_server;
//     solver_options.num_linear_solver_threads = covins_params::sys::threads_server;
//     solver_options.trust_region_strategy_type = ceres::DOGLEG;
//     solver_options.max_num_iterations = covins_params::opt::pgo_iteration_limit;
//     ceres::Solver::Summary summary;
// //    std::cout << "--> Solve" << std::endl;
//     ceres::Solve(solver_options, &problem, &summary);

//     // Recover the optimized data

//     PoseMap non_corrected_poses;

//     // Keyframes
//     for (size_t i = 0; i < keyframes.size(); ++i) {
//         KeyframePtr kf = keyframes[i];
//         if(kf->IsInvalid()) {
//             continue;
//         }
//         TransformType T_ws_uncorrected = kf->GetPoseTws();
//         non_corrected_poses[kf->id_] = T_ws_uncorrected;
//         TransformType T_ws_corrected = Utils::Ceres2Transform(kf->ceres_pose_);
//         kf->UpdateFromCeres(kf->ceres_pose_,kf->ceres_velocity_and_bias_,kf->ceres_extrinsics_);
//         Vector3Type velocity_corrected = T_ws_corrected.block<3,3>(0,0) * T_ws_uncorrected.inverse().block<3,3>(0,0) * kf->GetStateVelocity();
//         kf->SetStateVelocity(velocity_corrected);

//         kf->SetPoseOptimized();
//     }

//     // Landmarks
//     int removed_lms = 0;
//     for(auto lm : landmarks) {
//         if(lm->IsInvalid()) {
//             continue;
//         }
//         KeyframePtr kf_ref = lm->GetReferenceKeyframe();
//         if(!kf_ref){
//             if(!lm->GetObservations().empty())
//             map->EraseLandmark(lm);
//             removed_lms++;
//             continue;
//         }
//         TransformType T_ws_uncorrected;
//         PoseMap::iterator mit = non_corrected_poses.find(kf_ref->id_);
//         if(mit != non_corrected_poses.end()) T_ws_uncorrected = mit->second;
//         else{
//             map->EraseLandmark(lm);
//             removed_lms++;
//             continue;
//         }
//         TransformType T_sw_uncorrected = T_ws_uncorrected.inverse();

//         Vector3Type pos_w_uncorrected = lm->GetWorldPos();
//         Vector3Type pos_s = T_sw_uncorrected.block<3,3>(0,0) * pos_w_uncorrected + T_sw_uncorrected.block<3,1>(0,3);

//         TransformType T_ws_corrected = kf_ref->GetPoseTws();
//         Vector3Type pos_w_corrected = T_ws_corrected.block<3,3>(0,0) * pos_s + T_ws_corrected.block<3,1>(0,3);
//         lm->SetWorldPos(pos_w_corrected);
//         lm->SetOptimized();
//     }

//     std::cout << "--> PGO removed " << removed_lms << " LMs" << std::endl;
// }


// 4 DOF PGO
auto Optimization::PoseGraphOptimization4DoF(MapPtr map, PoseMap corrected_poses)->void {
    std::cout << "+++ 4 DoF PGO: Start +++" << std::endl;

    ceres::Problem::Options problem_options;
    problem_options.enable_fast_removal = true;
    ceres::Problem problem(problem_options);

    ceres::LossFunction *loss_function;
    // loss_function = new ceres::CauchyLoss(1.0);
    loss_function = new ceres::HuberLoss(0.1);

    ceres::LocalParameterization *angle_local_parameterization = AngleLocalParameterization::Create();

    KeyframeVector keyframes = map->GetKeyframesVec();
    LandmarkVector landmarks = map->GetLandmarksVec();

    std::map<TypeDefs::idpair, QuaternionType> q_ws_map;
    // Add Keyframes
    for (size_t i = 0; i < keyframes.size(); ++i) {

        KeyframePtr kf = keyframes[i];
        if(kf->IsInvalid())
            continue;

        TransformType T_ws_init;
        T_ws_init = kf->GetPoseTws();

        // Add parameter blocks
        kf->UpdateCeresFromState(kf->ceres_pose_,kf->ceres_velocity_and_bias_,kf->ceres_extrinsics_);
        QuaternionType q_ws_init(T_ws_init.block<3,3>(0,0));
        q_ws_map[kf->id_] = q_ws_init;

        auto ypr = Utils::R2ypr(T_ws_init.block<3,3>(0,0));
        kf->ceres_pose_[1] = ypr.x();
        kf->ceres_pose_[2] = ypr.y();
        kf->ceres_pose_[3] = ypr.z();
        kf->ceres_pose_[4] = T_ws_init(0,3);
        kf->ceres_pose_[5] = T_ws_init(1,3);
        kf->ceres_pose_[6] = T_ws_init(2,3);

        problem.AddParameterBlock(&kf->ceres_pose_[1], 1, angle_local_parameterization);
        problem.AddParameterBlock(&kf->ceres_pose_[4], 3);

        if(kf->id_.first == 0 && kf->id_.second == map->id_map_) {
            problem.SetParameterBlockConstant(&kf->ceres_pose_[1]);
            problem.SetParameterBlockConstant(&kf->ceres_pose_[4]);
        }

        if(kf->is_gba_optimized_ && covins_params::opt::pgo_fix_kfs_after_gba) {
            if(kf->id_.first % 50 == 0)
                std::cout << COUTNOTICE << "Set GBA KFs constant" << std::endl;
            problem.SetParameterBlockConstant(&kf->ceres_pose_[1]);
            problem.SetParameterBlockConstant(&kf->ceres_pose_[4]);
        } else if(kf->is_loaded_ && covins_params::opt::pgo_fix_poses_loaded_maps) {
            if(kf->id_.first % 50 == 0)
                std::cout << COUTNOTICE << "Set loaded KFs constant" << std::endl;
            problem.SetParameterBlockConstant(&kf->ceres_pose_[1]);
            problem.SetParameterBlockConstant(&kf->ceres_pose_[4]);
        }

    }

    std::set<std::pair<KeyframePtr,KeyframePtr>> inserted_edges;

    // Set loop constraints
    Map::LoopVector loops = map->GetLoopConstraints();

    for(auto i : loops) {
        KeyframePtr kf1 = i.kf1;
        KeyframePtr kf2 = i.kf2;
        TransformType T_12 = i.T_s1_s2;
        double relative_yaw = i.relative_yaw_smatch_squery;
        double cov = i.cov_mat.block<3,3>(3,3).trace();
        double weight = 1;

        // if (cov < 0.2) {
        //   weight = 0.5;
        // } else if (cov < 0.5) {
        //   weight = 0.1;
        // } else
        //   weight = 0.01;

        Vector3Type t_12 = T_12.block<3,1>(0,3);
        auto euler_conncected = Utils::R2ypr(q_ws_map[kf1->id_].toRotationMatrix());


        ceres::CostFunction* cost_function = FourDOFWeightError::Create(t_12.x(), t_12.y(), t_12.z(),
                                                                        relative_yaw, euler_conncected.y(), euler_conncected.z(), weight);

        problem.AddResidualBlock(cost_function, loss_function,
                                 &kf1->ceres_pose_[1],
                                 &kf1->ceres_pose_[4],
                                 &kf2->ceres_pose_[1],
                                 &kf2->ceres_pose_[4]);

    }

    // Set keyframe edges
    for (size_t i = 0; i < keyframes.size(); ++i) {
        KeyframePtr kf = keyframes[i];
        if(kf->IsInvalid()) continue;

        TransformType T_w_si = kf->GetPoseTws();

        // Edge to successor
        KeyframePtr succ = kf->GetSuccessor();
        if(!succ) continue;
        
        TransformType T_w_ssucc = succ->GetPoseTws();
        TransformType T_si_ssucc = T_w_si.inverse() * T_w_ssucc;
        Vector3Type t_si_ssucc = T_si_ssucc.block<3,1>(0,3);
        QuaternionType q_si_ssucc(T_si_ssucc.block<3, 3>(0, 0));

        Vector3Type relative_t = t_si_ssucc;
        double relative_yaw = succ->ceres_pose_[1] - kf->ceres_pose_[1];
        QuaternionType q_ws = q_ws_map[kf->id_];
        Vector3Type euler_conncected = Utils::R2ypr(q_ws.toRotationMatrix());
        
        std::pair<KeyframePtr,KeyframePtr> kf_pair = std::make_pair(kf,succ);
        if(inserted_edges.count(kf_pair)) {
            std::cout << COUTWARN << "KF edge already added" << std::endl;
            continue;
        } else {
            inserted_edges.insert(kf_pair);
        }

        ceres::CostFunction *cost_function = FourDOFError::Create(
            relative_t.x(), relative_t.y(), relative_t.z(), relative_yaw,
            euler_conncected.y(), euler_conncected.z());
        
        problem.AddResidualBlock(cost_function, NULL, &kf->ceres_pose_[1],
                                 &kf->ceres_pose_[4], &succ->ceres_pose_[1],
                                 &succ->ceres_pose_[4]);
    }

    // Set Additonal Keyframe edges
    for (size_t i = 0; i < keyframes.size(); ++i) {
            KeyframePtr kf = keyframes[i];
            if(kf->IsInvalid()) continue;

            TransformType T_w_si = kf->GetPoseTws();

            KeyframeVector connections;
            KeyframePtr temp_kf = keyframes[i];
            // Use 4 Previous KFs for additional Edges as done in VINS Fusion
            for (int j = 1; j < 5; ++j) {
              if (int(kf->id_.first) - j > 0) {
                temp_kf = temp_kf->GetPredecessor();
                connections.push_back(temp_kf);

                }
            }

            for(auto kfc : connections) {

                TransformType T_w_sc = kfc->GetPoseTws();
                TransformType T_si_sc = T_w_si.inverse() * T_w_sc;
                Vector3Type t_si_sc = T_si_sc.block<3,1>(0,3);
                QuaternionType q_si_sc(T_si_sc.block<3,3>(0,0));

                Vector3Type relative_t = t_si_sc;
                double relative_yaw = kfc->ceres_pose_[1] - kf->ceres_pose_[1];
                QuaternionType q_ws = q_ws_map[kf->id_];
                Vector3Type euler_conncected = Utils::R2ypr(q_ws.toRotationMatrix());

                std::pair<KeyframePtr,KeyframePtr> kf_pair = std::make_pair(kf,kfc);
                if(inserted_edges.count(kf_pair)) {
                    continue;
                } else {
                    inserted_edges.insert(kf_pair);
                }

                ceres::CostFunction *cost_function = FourDOFError::Create(
                    relative_t.x(), relative_t.y(), relative_t.z(),
                    relative_yaw, euler_conncected.y(), euler_conncected.z());
        
                problem.AddResidualBlock(cost_function, NULL, &kf->ceres_pose_[1],
                                 &kf->ceres_pose_[4], &kfc->ceres_pose_[1],
                                 &kfc->ceres_pose_[4]);
            }
        }


    // Solve
    ceres::Solver::Options solver_options;
    solver_options.linear_solver_type = ceres::SPARSE_NORMAL_CHOLESKY;
    solver_options.num_threads = covins_params::sys::threads_server;
    solver_options.num_linear_solver_threads = covins_params::sys::threads_server;
    solver_options.max_num_iterations = covins_params::opt::pgo_iteration_limit;
    ceres::Solver::Summary summary;
    // std::cout << "--> Solve" << std::endl;
    ceres::Solve(solver_options, &problem, &summary);
    // std::cout << summary.FullReport() << std::endl;

    // Recover the optimized data
    PoseMap non_corrected_poses;

    // Keyframes
    for (size_t i = 0; i < keyframes.size(); ++i) {
        KeyframePtr kf = keyframes[i];
        if(kf->IsInvalid()) {
            std::cout << COUTWARN << kf << ": invalid" << std::endl;
            continue;
        }
        TransformType T_ws_uncorrected = kf->GetPoseTws();
        non_corrected_poses[kf->id_] = T_ws_uncorrected;

        QuaternionType q = eulerAnglesToQuat(kf->ceres_pose_[1], kf->ceres_pose_[2], kf->ceres_pose_[3]);
        kf->ceres_pose_[0] = q.x();
        kf->ceres_pose_[1] = q.y();
        kf->ceres_pose_[2] = q.z();
        kf->ceres_pose_[3] = q.w();

        TransformType T_ws_corrected = Utils::Ceres2Transform(kf->ceres_pose_);
        kf->UpdateFromCeres(kf->ceres_pose_,kf->ceres_velocity_and_bias_,kf->ceres_extrinsics_);
        Vector3Type velocity_corrected = T_ws_corrected.block<3,3>(0,0) * T_ws_uncorrected.inverse().block<3,3>(0,0) * kf->GetStateVelocity();
        kf->SetStateVelocity(velocity_corrected);
    }

    // Landmarks
    for(auto lm : landmarks) {
        if(lm->IsInvalid()) {
            std::cout << COUTWARN << lm << ": invalid" << std::endl;
            continue;
        }
        KeyframePtr kf_ref = lm->GetReferenceKeyframe();
        if(!kf_ref){
            std::cout << COUTWARN << lm << " has no ref-KF" << std::endl;
            continue;
        }
        TransformType T_ws_uncorrected;
        PoseMap::iterator mit = non_corrected_poses.find(kf_ref->id_);
        if(mit != non_corrected_poses.end()) T_ws_uncorrected = mit->second;
        else{
            std::cout << COUTERROR << "mit == non_corrected_poses.end()" << std::endl;
            exit(-1);
        }
        TransformType T_sw_uncorrected = T_ws_uncorrected.inverse();

        Vector3Type pos_w_uncorrected = lm->GetWorldPos();
        Vector3Type pos_s = T_sw_uncorrected.block<3,3>(0,0) * pos_w_uncorrected + T_sw_uncorrected.block<3,1>(0,3);

        TransformType T_ws_corrected = kf_ref->GetPoseTws();
        Vector3Type pos_w_corrected = T_ws_corrected.block<3,3>(0,0) * pos_s + T_ws_corrected.block<3,1>(0,3);
        lm->SetWorldPos(pos_w_corrected);
    }

    std::cout << "+++ 4 DoF PGO: End +++" << std::endl;
}

auto Optimization::PoseGraphOptimization(
    MapPtr map, PoseMap corrected_poses) -> void {
  
    std::cout << "+++ PGO: Start +++" << std::endl;

    ceres::Problem::Options problem_options;
    problem_options.enable_fast_removal = true;
    ceres::Problem problem(problem_options);

    ceres::LossFunction *loss_function;
    // loss_function = new ceres::CauchyLoss(1.0);
    loss_function = new ceres::HuberLoss(0.1);

    ceres::LocalParameterization *local_pose_param = new robopt::local_param::PoseQuaternionLocalParameterization();

    KeyframeVector keyframes = map->GetKeyframesVec();
    LandmarkVector landmarks = map->GetLandmarksVec();

    // Add Keyframes
    for (size_t i = 0; i < keyframes.size(); ++i) {
        KeyframePtr kf = keyframes[i];
        if(kf->IsInvalid()) continue;

        TransformType T_ws_init;
        PoseMap::iterator mit = corrected_poses.find(kf->id_);
        if(mit != corrected_poses.end()) T_ws_init = mit->second;
        else T_ws_init = kf->GetPoseTws();

        // Add parameter blocks
        kf->UpdateCeresFromState(kf->ceres_pose_,kf->ceres_velocity_and_bias_,kf->ceres_extrinsics_);
        QuaternionType q_ws_init(T_ws_init.block<3,3>(0,0));
        kf->ceres_pose_[0] = q_ws_init.x();
        kf->ceres_pose_[1] = q_ws_init.y();
        kf->ceres_pose_[2] = q_ws_init.z();
        kf->ceres_pose_[3] = q_ws_init.w();
        kf->ceres_pose_[4] = T_ws_init(0,3);
        kf->ceres_pose_[5] = T_ws_init(1,3);
        kf->ceres_pose_[6] = T_ws_init(2,3);
        problem.AddParameterBlock(kf->ceres_pose_, robopt::defs::pose::kPoseBlockSize, local_pose_param);
        if(kf->id_.first == 0 && kf->id_.second == map->id_map_)
            problem.SetParameterBlockConstant(kf->ceres_pose_);
        problem.AddParameterBlock(kf->ceres_extrinsics_, robopt::defs::pose::kPoseBlockSize, local_pose_param);
        problem.SetParameterBlockConstant(kf->ceres_extrinsics_);
    }

    // COV MAT for KF Edges
    // TypeDefs::Matrix6Type cov_mat_kf = TypeDefs::Matrix6Type::Identity();
    
    // cov_mat_kf(0, 0) = 0.00030785;
    // cov_mat_kf(1, 1) = 0.00018436;
    // cov_mat_kf(2, 2) = 0.00020151;
    // cov_mat_kf(3, 3) = 0.00209302;
    // cov_mat_kf(4, 4) = 0.00349413;
    // cov_mat_kf(5, 5) = 0.00305899;
    TypeDefs::Matrix6Type cov_mat_kf;

    cov_mat_kf << 2.51591680e-07, 1.34384891e-07, 3.16908987e-08,
        1.78271135e-07, 1.51987977e-07, 2.51930336e-07, 1.34384891e-07,
        1.13111621e-07, 1.98938689e-08, 1.24654191e-07, 6.00688406e-08,
        7.60118872e-08, 3.16908987e-08, 1.98938689e-08, 2.94108871e-08,
        3.45849866e-08, 2.78271812e-08, 2.26388210e-08, 1.78271135e-07,
        1.24654191e-07, 3.45849866e-08, 5.30247779e-06, 2.37292772e-06,
        8.45597210e-06, 1.51987977e-07, 6.00688406e-08, 2.78271812e-08,
        2.37292772e-06, 9.42999233e-06, 6.21689203e-06, 2.51930336e-07,
        7.60118872e-08, 2.26388210e-08, 8.45597210e-06, 6.21689203e-06,
        3.01405541e-05;

    // cov_mat_kf << 2.51591680e-05, 1.34384891e-05, 3.16908987e-06,
    //     1.78271135e-06, 1.51987977e-06, 2.51930336e-06, 1.34384891e-05,
    //     1.13111621e-05, 1.98938689e-06, 1.24654191e-06, 6.00688406e-07,
    //     7.60118872e-07, 3.16908987e-06, 1.98938689e-06, 2.94108871e-06,
    //     3.45849866e-07, 2.78271812e-07, 2.26388210e-07, 1.78271135e-06,
    //     1.24654191e-06, 3.45849866e-07, 5.30247779e-06, 2.37292772e-06,
    //     8.45597210e-06, 1.51987977e-06, 6.00688406e-07, 2.78271812e-07,
    //     2.37292772e-06, 9.42999233e-06, 6.21689203e-06, 2.51930336e-06,
    //     7.60118872e-07, 2.26388210e-07, 8.45597210e-06, 6.21689203e-06,
    //     3.01405541e-05;

    // cov_mat_kf *= covins_params::opt::wt_lp_r2;

    // Eigen::LLT<TypeDefs::Matrix6Type> lltofcov_mat(cov_mat_kf.inverse());
    // TypeDefs::Matrix6Type sqrt_info_f = lltofcov_mat.matrixL().transpose();
    // std::cout << "The Cov Mat: " << std::endl;
    // std::cout << cov_mat_kf << std::endl;
    // TypeDefs::Matrix6Type sqrt_info = TypeDefs::Matrix6Type::Identity();
    // sqrt_info = sqrt_info_f;

    // sqrt_info.diagonal() = sqrt_info_f.diagonal();

    // sqrt_info /= covins_params::opt::wt_lp_t2;

    
    Eigen::Matrix<precision_t,6,6> sqrt_info = Eigen::Matrix<precision_t,6,6>::Identity();
    sqrt_info.topLeftCorner<3,3>() *= covins_params::opt::wt_kf_r;
    sqrt_info.bottomRightCorner<3, 3>() *= covins_params::opt::wt_kf_t;

    Eigen::Matrix<precision_t, 6, 6> sqrt_info_l = Eigen::Matrix<precision_t, 6, 6>::Identity();
    // sqrt_info_l.topLeftCorner<3,3>() *=
    // covins_params::opt::wt_lp_r2; sqrt_info_l.bottomRightCorner<3, 3>() *=
    // covins_params::opt::wt_lp_t2; sqrt_info_l.topLeftCorner<3,3>() *=
    // covins_params::opt::wt_lp_r2; sqrt_info_l.bottomRightCorner<3,3>() *=
    // covins_params::opt::wt_lp_t2;
    
    // std::cout << "The LT Mat Adj KFs: " << std::endl;
    // std::cout << std::setprecision(4) << sqrt_info << std::endl;

    std::set<std::pair<KeyframePtr,KeyframePtr>> inserted_edges;

    // Set loop constraints
    Map::LoopVector loops = map->GetLoopConstraints();


    for (auto i : loops) {
      
        sqrt_info_l = Eigen::Matrix<precision_t, 6, 6>::Identity();
        KeyframePtr kf1 = i.kf1;
        KeyframePtr kf2 = i.kf2;
        TransformType T_12 = i.T_s1_s2;
        // For Covaraince Matrix Weighting
        // Eigen::LLT<TypeDefs::Matrix6Type> lltofA(i.cov_mat.inverse());
        // TypeDefs::Matrix6Type sqrt_info_l_f = lltofA.matrixL().transpose();

        // TypeDefs::Matrix6Type sqrt_info_l = TypeDefs::Matrix6Type::Identity();
        // // sqrt_info_l.diagonal() = sqrt_info_l_f.diagonal();
        // sqrt_info_l = sqrt_info_l_f;

        Vector3Type t_12 = T_12.block<3,1>(0,3);
        QuaternionType q_12(T_12.block<3, 3>(0, 0));

        double cov = i.cov_mat.block<3,3>(3,3).trace();
        double weight = covins_params::opt::wt_lp_t1;

         if (cov < covins_params::opt::cov_switch) {
           weight = covins_params::opt::wt_lp_r1;
         } else if (cov < covins_params::opt::cov_switch2) {
           weight = covins_params::opt::wt_lp_r2;
         } else
           weight = covins_params::opt::wt_lp_r3;

        sqrt_info_l *= weight;
        //  sqrt_info_l /= covins_params::opt::wt_lp_t3;

        // sqrt_info_l.topLeftCorner<3,3>() *= weight;
        // sqrt_info_l.bottomRightCorner<3, 3>() *= weight;

        // sqrt_info_l(3, 3) *= weight;
        // sqrt_info_l(4, 4) *= weight;
        // sqrt_info_l(5, 5) *= weight;

        // sqrt_info_l /= covins_params::opt::wt_lp_t2;

        // sqrt_info_l(0, 0) = 1.0;
        // sqrt_info_l(1, 1) = 1.0;
        // sqrt_info_l(2, 2) = 1.0;
        // // sqrt_info_l = TypeDefs::Matrix6Type::Identity();
        // // sqrt_info_l(3, 3) *= weight;
        // // sqrt_info_l(4, 4) *= weight;
        // // sqrt_info_l(5, 5) *= weight;
        // sqrt_info_l(3, 3) = min(0.5, sqrt_info_l(3, 3));
        // sqrt_info_l(4, 4) = min(0.5, sqrt_info_l(4, 4));
        // sqrt_info_l(5, 5) = min(0.5, sqrt_info_l(5, 5));
            
        
        // std::cout << "The LT Mat loop: " << std::endl;
        // std::cout << std::setprecision(4) << sqrt_info_l << std::endl;
         
        // std::cout << sqrt_info_l << std::endl;        
        ceres::CostFunction* f = new robopt::posegraph::SixDofBetweenError(q_12, t_12, sqrt_info_l, robopt::defs::pose::PoseErrorType::kImu);
        problem.AddResidualBlock(f,NULL, kf1->ceres_pose_, kf2->ceres_pose_, kf1->ceres_extrinsics_, kf2->ceres_extrinsics_);
    }


    // Set keyframe edges
    for (size_t i = 0; i < keyframes.size(); ++i) {
        KeyframePtr kf = keyframes[i];
        if(kf->IsInvalid()) continue;

        TransformType T_w_si = kf->GetPoseTws();
        // TransformType T_w_si = GetPoseTwsGT(kf, gt);
        // Edge to successor
        KeyframePtr succ = kf->GetSuccessor();
        if(!succ) continue;
        TransformType T_w_ssucc = succ->GetPoseTws();
        // TransformType T_w_ssucc = GetPoseTwsGT(succ, gt);
        TransformType T_si_ssucc = T_w_si.inverse() * T_w_ssucc;
        Vector3Type t_si_ssucc = T_si_ssucc.block<3,1>(0,3);
        QuaternionType q_si_ssucc(T_si_ssucc.block<3,3>(0,0));

        std::pair<KeyframePtr,KeyframePtr> kf_pair = std::make_pair(kf,succ);
        if(inserted_edges.count(kf_pair)) {
            std::cout << COUTWARN << "KF edge already added" << std::endl;
            continue;
        } else {
            inserted_edges.insert(kf_pair);
        }

        ceres::CostFunction* f = new robopt::posegraph::SixDofBetweenError(q_si_ssucc, t_si_ssucc, sqrt_info, robopt::defs::pose::PoseErrorType::kImu);
        problem.AddResidualBlock(f, /*lossFunction*/NULL, kf->ceres_pose_, succ->ceres_pose_, kf->ceres_extrinsics_, succ->ceres_extrinsics_);
    }

    // Add 4 Neighbors like VINS
    for (size_t i = 0; i < keyframes.size(); ++i) {
            KeyframePtr kf = keyframes[i];
            if(kf->IsInvalid()) continue;

            TransformType T_w_si = kf->GetPoseTws();

            KeyframeVector connections;
            KeyframePtr temp_kf = keyframes[i];
            // Use 4 Previous KFs for additional Edges as done in VINS Fusion

            for (int j = 1; j < 5; ++j) {
              if (int(kf->id_.first) - j > 0) {
                temp_kf = temp_kf->GetPredecessor();
                connections.push_back(temp_kf);

                }
            }
            
            // std::cout << "Connections Size" << connections.size() << std::endl;
            for(auto kfc : connections) {

                TransformType T_w_sc = kfc->GetPoseTws();
                TransformType T_si_sc = T_w_si.inverse() * T_w_sc;
                Vector3Type t_si_sc = T_si_sc.block<3,1>(0,3);
                QuaternionType q_si_sc(T_si_sc.block<3,3>(0,0));

                std::pair<KeyframePtr,KeyframePtr> kf_pair = std::make_pair(kf,kfc);
                if(inserted_edges.count(kf_pair)) {
                    continue;
                } else {
                    inserted_edges.insert(kf_pair);
                }

                ceres::CostFunction* f = new robopt::posegraph::SixDofBetweenError(q_si_sc, t_si_sc, sqrt_info, robopt::defs::pose::PoseErrorType::kImu);
                problem.AddResidualBlock(f, /*lossFunction*/NULL, kf->ceres_pose_, kfc->ceres_pose_, kf->ceres_extrinsics_, kfc->ceres_extrinsics_);
            }
    }
    


    // Solve
    ceres::Solver::Options solver_options;
    solver_options.linear_solver_type = ceres::SPARSE_SCHUR;
    solver_options.num_threads = covins_params::sys::threads_server;
    solver_options.num_linear_solver_threads = covins_params::sys::threads_server;
    solver_options.trust_region_strategy_type = ceres::DOGLEG;
    solver_options.max_num_iterations = covins_params::opt::pgo_iteration_limit;
    ceres::Solver::Summary summary;
    // std::cout << "--> Solve" << std::endl;
    ceres::Solve(solver_options, &problem, &summary);
    // std::cout << summary.FullReport() << std::endl;

    // Recover the optimized data
    PoseMap non_corrected_poses;

    // Keyframes
    for (size_t i = 0; i < keyframes.size(); ++i) {
        KeyframePtr kf = keyframes[i];
        if(kf->IsInvalid()) {
            std::cout << COUTWARN << kf << ": invalid" << std::endl;
            continue;
        }
        TransformType T_ws_uncorrected = kf->GetPoseTws();
        non_corrected_poses[kf->id_] = T_ws_uncorrected;
        TransformType T_ws_corrected = Utils::Ceres2Transform(kf->ceres_pose_);
        kf->UpdateFromCeres(kf->ceres_pose_,kf->ceres_velocity_and_bias_,kf->ceres_extrinsics_);
        Vector3Type velocity_corrected = T_ws_corrected.block<3,3>(0,0) * T_ws_uncorrected.inverse().block<3,3>(0,0) * kf->GetStateVelocity();
        kf->SetStateVelocity(velocity_corrected);
    }

    // Landmarks
    for(auto lm : landmarks) {
        if(lm->IsInvalid()) {
            std::cout << COUTWARN << lm << ": invalid" << std::endl;
            continue;
        }
        KeyframePtr kf_ref = lm->GetReferenceKeyframe();
        if(!kf_ref){
            std::cout << COUTWARN << lm << " has no ref-KF" << std::endl;
            continue;
        }
        TransformType T_ws_uncorrected;
        PoseMap::iterator mit = non_corrected_poses.find(kf_ref->id_);
        if(mit != non_corrected_poses.end()) T_ws_uncorrected = mit->second;
        else{
            std::cout << COUTERROR << "mit == non_corrected_poses.end()" << std::endl;
            exit(-1);
        }
        TransformType T_sw_uncorrected = T_ws_uncorrected.inverse();

        Vector3Type pos_w_uncorrected = lm->GetWorldPos();
        Vector3Type pos_s = T_sw_uncorrected.block<3,3>(0,0) * pos_w_uncorrected + T_sw_uncorrected.block<3,1>(0,3);

        TransformType T_ws_corrected = kf_ref->GetPoseTws();
        Vector3Type pos_w_corrected = T_ws_corrected.block<3,3>(0,0) * pos_s + T_ws_corrected.block<3,1>(0,3);
        lm->SetWorldPos(pos_w_corrected);
    }

    std::cout << "+++ PGO: End +++" << std::endl;
}


} //end ns
