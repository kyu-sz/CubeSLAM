/**
* This file is part of ORB-SLAM2.
*
* Copyright (C) 2014-2016 Raúl Mur-Artal <raulmur at unizar dot es> (University of Zaragoza)
* For more information see <https://github.com/raulmur/ORB_SLAM2>
*
* ORB-SLAM2 is free software: you can redistribute it and/or modify
* it under the terms of the GNU General Public License as published by
* the Free Software Foundation, either version 3 of the License, or
* (at your option) any later version.
*
* ORB-SLAM2 is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
* GNU General Public License for more details.
*
* You should have received a copy of the GNU General Public License
* along with ORB-SLAM2. If not, see <http://www.gnu.org/licenses/>.
*/

#include "Optimizer.h"
#include "Converter.h"

#include "g2o/core/block_solver.h"
#include "g2o/core/optimization_algorithm_levenberg.h"
#include "g2o/solvers/eigen/linear_solver_eigen.h"
#include "g2o/types/sba/types_six_dof_expmap.h"
#include "g2o/core/robust_kernel_impl.h"
#include "g2o/solvers/dense/linear_solver_dense.h"
#include "g2o/types/sim3/types_seven_dof_expmap.h"
#include <g2o_Object.h>

#include <unordered_set>

using namespace cv;
using namespace std;

namespace ORB_SLAM2
{

void Optimizer::LocalBundleAdjustment(KeyFrame* pKF, bool* pbStopFlag, Map* pMap)
{
    // Local KeyFrames: First Breath Search from Current Keyframe
    list<KeyFrame*> lLocalKeyFrames;

    lLocalKeyFrames.push_back(pKF);
    pKF->mnBALocalForKF = pKF->mnId;

    const vector<KeyFrame*> vNeighKFs = pKF->GetVectorCovisibleKeyFrames();
    for (auto pKFi : vNeighKFs) {
        pKFi->mnBALocalForKF = pKF->mnId;
        if (!pKFi->isBad())
            lLocalKeyFrames.push_back(pKFi);
    }

    // Local MapPoints seen in Local KeyFrames
    list<MapPoint*> lLocalMapPoints;
    for (auto& lLocalKeyFrame : lLocalKeyFrames) {
        vector<MapPoint*> vpMPs = lLocalKeyFrame->GetMapPointMatches();
        for (auto pMP : vpMPs) {
            if (pMP)
                if (!pMP->isBad())
                    if (pMP->mnBALocalForKF != pKF->mnId) {
                        lLocalMapPoints.push_back(pMP);
                        pMP->mnBALocalForKF = pKF->mnId;
                    }
        }
    }

    // Fixed Keyframes. Keyframes that see Local MapPoints but that are not Local Keyframes
    list<KeyFrame*> lFixedCameras;
    for (auto& lLocalMapPoint : lLocalMapPoints) {
        map<KeyFrame*, size_t> observations = lLocalMapPoint->GetObservations();
        for (auto& observation : observations) {
            KeyFrame* pKFi = observation.first;

            if (pKFi->mnBALocalForKF != pKF->mnId && pKFi->mnBAFixedForKF != pKF->mnId) {
                pKFi->mnBAFixedForKF = pKF->mnId;
                if (!pKFi->isBad())
                    lFixedCameras.push_back(pKFi);
            }
        }
    }

    // Setup optimizer
    g2o::SparseOptimizer optimizer;

    auto linearSolver = g2o::make_unique<g2o::LinearSolverEigen<g2o::BlockSolver_6_3::PoseMatrixType>>();

    auto solver_ptr = g2o::make_unique<g2o::BlockSolver_6_3>(std::move(linearSolver));

    g2o::OptimizationAlgorithmLevenberg* solver = new g2o::OptimizationAlgorithmLevenberg(std::move(solver_ptr));
    optimizer.setAlgorithm(solver);

    if (pbStopFlag)
        optimizer.setForceStopFlag(pbStopFlag);

    unsigned long maxKFId = 0, maxLandmarkId = 0;

    // Set Local KeyFrame vertices
    unordered_set<shared_ptr<Landmark>> lLocalLandmarks;
    for (auto pKFi : lLocalKeyFrames) {
        auto landmarks = pKFi->GetLandmarks();
        for (const auto& pLandmark : landmarks) {
            lLocalLandmarks.insert(pLandmark);
            pKFi->landmarkMeasurements[pLandmark->mnLandmarkId] =
                    pLandmark->GetCuboid()->transformTo(pKFi->cam_pose_Twc);
        }
        auto* vSE3 = new g2o::VertexSE3Expmap();
        vSE3->setEstimate(Converter::toSE3Quat(pKFi->GetPose()));
        vSE3->setId(pKFi->mnId);
        vSE3->setFixed(pKFi->mnId == 0);
        optimizer.addVertex(vSE3);
        if (pKFi->mnId > maxKFId)
            maxKFId = pKFi->mnId;
    }

    // Set Fixed KeyFrame vertices
    for (auto pKFi : lFixedCameras) {
        auto* vSE3 = new g2o::VertexSE3Expmap();
        vSE3->setEstimate(Converter::toSE3Quat(pKFi->GetPose()));
        vSE3->setId(pKFi->mnId);
        vSE3->setFixed(true);
        optimizer.addVertex(vSE3);
        if (pKFi->mnId > maxKFId)
            maxKFId = pKFi->mnId;
    }

    for (const auto& pLandmark : lLocalLandmarks) {
        auto pInitCuboidGlobalPose = pLandmark->GetCuboid();
        auto* vCuboid = new g2o::VertexCuboid();
        vCuboid->setEstimate(*pInitCuboidGlobalPose);
        vCuboid->setId(maxKFId + 1 + pLandmark->mnLandmarkId);
        vCuboid->setFixed(false);
        optimizer.addVertex(vCuboid);
        if (pLandmark->mnLandmarkId > maxLandmarkId)
            maxLandmarkId = pLandmark->mnLandmarkId;
    }

    // Set MapPoint vertices
    const int nExpectedSize = (lLocalKeyFrames.size() + lFixedCameras.size()) * lLocalMapPoints.size();

    vector<g2o::EdgeSE3ProjectXYZ*> vpEdgesMono;
    vpEdgesMono.reserve(nExpectedSize);

    vector<KeyFrame*> vpEdgeKFMono;
    vpEdgeKFMono.reserve(nExpectedSize);

    vector<MapPoint*> vpMapPointEdgeMono;
    vpMapPointEdgeMono.reserve(nExpectedSize);

    vector<g2o::EdgeStereoSE3ProjectXYZ*> vpEdgesStereo;
    vpEdgesStereo.reserve(nExpectedSize);

    vector<KeyFrame*> vpEdgeKFStereo;
    vpEdgeKFStereo.reserve(nExpectedSize);

    vector<MapPoint*> vpMapPointEdgeStereo;
    vpMapPointEdgeStereo.reserve(nExpectedSize);

    const float thHuberMono = sqrtf(5.991f);
    const float thHuberStereo = sqrtf(7.815f);

    for (auto pMP : lLocalMapPoints) {
        auto* vPoint = new g2o::VertexSBAPointXYZ();
        vPoint->setEstimate(Converter::toVector3d(pMP->GetWorldPos()));
        int id = pMP->mnId + maxKFId + maxLandmarkId + 2;
        vPoint->setId(id);
        vPoint->setMarginalized(true);
        optimizer.addVertex(vPoint);

        const auto observations = pMP->GetObservations();

        //Set edges
        for (auto observation : observations) {
            KeyFrame* pKFi = observation.first;

            if (!pKFi->isBad()) {
                const cv::KeyPoint& kpUn = pKFi->mvKeysUn[observation.second];

                // Monocular observation
                if (pKFi->mvuRight[observation.second] < 0) {
                    Eigen::Matrix<double, 2, 1> obs;
                    obs << kpUn.pt.x, kpUn.pt.y;

                    auto* e = new g2o::EdgeSE3ProjectXYZ();

                    e->setVertex(0, optimizer.vertex(id));
                    e->setVertex(1, optimizer.vertex(pKFi->mnId));
                    e->setMeasurement(obs);
                    const float& invSigma2 = pKFi->mvInvLevelSigma2[kpUn.octave];
                    e->setInformation(Eigen::Matrix2d::Identity() * invSigma2);

                    auto* rk = new g2o::RobustKernelHuber;
                    e->setRobustKernel(rk);
                    rk->setDelta(thHuberMono);

                    e->fx = pKFi->fx;
                    e->fy = pKFi->fy;
                    e->cx = pKFi->cx;
                    e->cy = pKFi->cy;

                    optimizer.addEdge(e);
                    vpEdgesMono.push_back(e);
                    vpEdgeKFMono.push_back(pKFi);
                    vpMapPointEdgeMono.push_back(pMP);

                    // add g2o camera-object measurement edges, if there is
                    auto landmarks = pKFi->GetLandmarks();
                    for (const auto& pLandmark : landmarks) {
                        auto* cuboid = new g2o::EdgeSE3Cuboid();
                        cuboid->setVertex(0, optimizer.vertex(pKFi->mnId));
                        cuboid->setVertex(1, optimizer.vertex(maxKFId + 1 + pLandmark->mnLandmarkId));
                        cuboid->setMeasurement(pKFi->landmarkMeasurements[pLandmark->mnLandmarkId]);
                        Eigen::Vector9d inv_sigma;
                        inv_sigma << 1, 1, 1, 1, 1, 1, 1, 1, 1;
                        inv_sigma = inv_sigma * 2.0 * pLandmark->mQuality;
                        Eigen::Matrix9d info = inv_sigma.cwiseProduct(inv_sigma).asDiagonal();
                        cuboid->setInformation(info);
                        optimizer.addEdge(cuboid);
                    }
                }
                else // Stereo observation
                {
                    Eigen::Matrix<double, 3, 1> obs;
                    const float kp_ur = pKFi->mvuRight[observation.second];
                    obs << kpUn.pt.x, kpUn.pt.y, kp_ur;

                    auto* e = new g2o::EdgeStereoSE3ProjectXYZ();

                    e->setVertex(0, optimizer.vertex(id));
                    e->setVertex(1, optimizer.vertex(pKFi->mnId));
                    e->setMeasurement(obs);
                    const float& invSigma2 = pKFi->mvInvLevelSigma2[kpUn.octave];
                    Eigen::Matrix3d Info = Eigen::Matrix3d::Identity() * invSigma2;
                    e->setInformation(Info);

                    auto* rk = new g2o::RobustKernelHuber;
                    e->setRobustKernel(rk);
                    rk->setDelta(thHuberStereo);

                    e->fx = pKFi->fx;
                    e->fy = pKFi->fy;
                    e->cx = pKFi->cx;
                    e->cy = pKFi->cy;
                    e->bf = pKFi->mbf;

                    optimizer.addEdge(e);
                    vpEdgesStereo.push_back(e);
                    vpEdgeKFStereo.push_back(pKFi);
                    vpMapPointEdgeStereo.push_back(pMP);
                }
            }
        }
    }

    if (pbStopFlag)
        if (*pbStopFlag)
            return;

    optimizer.initializeOptimization();
    optimizer.optimize(5);

    bool bDoMore = true;

    if (pbStopFlag)
        if (*pbStopFlag)
            bDoMore = false;

    if (bDoMore) {

        // Check inlier observations
        for (size_t i = 0, iend = vpEdgesMono.size(); i < iend; i++) {
            g2o::EdgeSE3ProjectXYZ* e = vpEdgesMono[i];
            MapPoint* pMP = vpMapPointEdgeMono[i];

            if (pMP->isBad())
                continue;

            if (e->chi2() > 5.991 || !e->isDepthPositive()) {
                e->setLevel(1);
            }

            e->setRobustKernel(nullptr);
        }

        for (size_t i = 0, iend = vpEdgesStereo.size(); i < iend; i++) {
            g2o::EdgeStereoSE3ProjectXYZ* e = vpEdgesStereo[i];
            MapPoint* pMP = vpMapPointEdgeStereo[i];

            if (pMP->isBad())
                continue;

            if (e->chi2() > 7.815 || !e->isDepthPositive()) {
                e->setLevel(1);
            }

            e->setRobustKernel(0);
        }

        // Optimize again without the outliers

        optimizer.initializeOptimization(0);
        optimizer.optimize(10);

    }

    vector<pair<KeyFrame*, MapPoint*> > vToErase;
    vToErase.reserve(vpEdgesMono.size() + vpEdgesStereo.size());

    // Check inlier observations
    for (size_t i = 0; i < vpEdgesMono.size(); i++) {
        g2o::EdgeSE3ProjectXYZ* e = vpEdgesMono[i];
        MapPoint* pMP = vpMapPointEdgeMono[i];

        if (pMP->isBad())
            continue;

        if (e->chi2() > 5.991 || !e->isDepthPositive()) {
            KeyFrame* pKFi = vpEdgeKFMono[i];
            vToErase.emplace_back(pKFi, pMP);
        }
    }

    for (size_t i = 0; i < vpEdgesStereo.size(); i++) {
        g2o::EdgeStereoSE3ProjectXYZ* e = vpEdgesStereo[i];
        MapPoint* pMP = vpMapPointEdgeStereo[i];

        if (pMP->isBad())
            continue;

        if (e->chi2() > 7.815 || !e->isDepthPositive()) {
            KeyFrame* pKFi = vpEdgeKFStereo[i];
            vToErase.emplace_back(pKFi, pMP);
        }
    }

    // Get Map Mutex
    unique_lock<mutex> lock(pMap->mMutexMapUpdate);

    if (!vToErase.empty()) {
        for (auto& i : vToErase) {
            KeyFrame* pKFi = i.first;
            MapPoint* pMPi = i.second;
            pKFi->EraseMapPointMatch(pMPi);
            pMPi->EraseObservation(pKFi);
        }
    }

    // Recover optimized data

    //Keyframes
    for (auto pLocalKF : lLocalKeyFrames) {
        auto* vSE3 = dynamic_cast<g2o::VertexSE3Expmap*>(optimizer.vertex(pLocalKF->mnId));
        const g2o::SE3Quat& SE3quat = vSE3->estimate();
        pLocalKF->SetPose(Converter::toCvMat(SE3quat));
    }

    //Landmarks
    for (const auto& pLandmark : lLocalLandmarks) {
        auto* vCube = dynamic_cast<g2o::VertexCuboid*>(optimizer.vertex(maxKFId + 1 + pLandmark->mnLandmarkId));
        const g2o::Cuboid& cuboid = vCube->estimate();
        pLandmark->SetPoseAndDimension(cuboid);
    }

    //Points
    for (auto pMP : lLocalMapPoints) {
        auto* vPoint = dynamic_cast<g2o::VertexSBAPointXYZ*>(optimizer.vertex(pMP->mnId + maxKFId + 1));
        pMP->SetWorldPos(Converter::toCvMat(vPoint->estimate()));
        pMP->UpdateNormalAndDepth();
    }
}

}