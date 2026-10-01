// Research adapter around ORB-SLAM3 (GPL-3.0); no flight control.
#include "System.h"
#include "Tracking.h"
#include "Map.h"
#include <opencv2/imgcodecs.hpp>
#include <chrono>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <set>
#include <sstream>
#include <thread>

using namespace ORB_SLAM3;
using Clock = std::chrono::steady_clock;

int main(int argc, char** argv) {
    if (argc != 6) {
        std::cerr << "orb_replay vocabulary settings frame-list session output\n";
        return 2;
    }
    cv::setNumThreads(2);
    std::ifstream input(argv[3]);
    std::vector<double> times;
    std::vector<std::string> images;
    double t; std::string name;
    while (input >> t >> name) { times.push_back(t); images.push_back(name); }
    if (times.empty()) return 3;
    const std::string output(argv[5]);
    std::ofstream states(output + "/tracking.csv");
    states << "index,timestamp,state,map_id,landmarks,processing_ms,features\n" << std::setprecision(12);
    System slam(argv[1], argv[2], System::MONOCULAR, false);
    const auto start = Clock::now();
    for (size_t i = 0; i < times.size(); ++i) {
        const auto image = cv::imread(std::string(argv[4]) + "/" + images[i], cv::IMREAD_COLOR);
        if (image.empty()) { std::cerr << "Image decode failed: " << images[i] << '\n'; return 4; }
        const auto before = Clock::now();
        slam.TrackMonocular(image, times[i], {}, images[i]);
        const auto ms = std::chrono::duration<double, std::milli>(Clock::now()-before).count();
        int landmarks = 0;
        for (auto* p : slam.GetTrackedMapPoints()) if (p && !p->isBad() && p->Observations() > 0) ++landmarks;
        auto* map = slam.ExportAtlas()->GetCurrentMap();
        states << i << ',' << times[i] << ',' << slam.GetTrackingState() << ','
               << (map ? static_cast<long>(map->GetId()) : -1) << ',' << landmarks << ',' << ms
               << ',' << slam.GetTrackedKeyPointsUn().size() << '\n';
        states.flush();
        if (i+1 < times.size())
            std::this_thread::sleep_until(start + std::chrono::duration_cast<Clock::duration>(
                std::chrono::duration<double>(times[i+1]-times[0])));
    }
    slam.Shutdown();
    slam.WaitForExport();
    std::ofstream points(output + "/points.csv");
    points << "map_id,point_id,x,y,z,observations\n" << std::setprecision(9);
    std::set<Map*> retained;
    for (auto* map : slam.ExportAtlas()->GetAllMaps()) {
        retained.insert(map);
        for (auto* point : map->GetAllMapPoints()) {
            if (!point || point->isBad()) continue;
            const auto p = point->GetWorldPos();
            if (p.allFinite()) points << map->GetId() << ',' << point->mnId << ',' << p.x() << ','
                << p.y() << ',' << p.z() << ',' << point->Observations() << '\n';
        }
    }
    // Recompute frame poses through their final optimized reference keyframes.
    // Each map retains its own frame and scale. Never concatenate map origins.
    auto* tracker = slam.ExportTracker();
    auto reference = tracker->mlpReferences.begin();
    auto timestamp = tracker->mlFrameTimes.begin();
    auto lost = tracker->mlbLost.begin();
    std::ofstream poses(output + "/poses.csv");
    poses << "timestamp,map_id,x,y,z,qx,qy,qz,qw\n" << std::setprecision(12);
    for (auto relative = tracker->mlRelativeFramePoses.begin(); relative != tracker->mlRelativeFramePoses.end();
         ++relative, ++reference, ++timestamp, ++lost) {
        if (*lost || !*reference) continue;
        auto* keyframe = *reference;
        Sophus::SE3f parent;
        std::set<KeyFrame*> visited;
        while (keyframe && keyframe->isBad() && visited.insert(keyframe).second) {
            parent = parent * keyframe->mTcp;
            keyframe = keyframe->GetParent();
        }
        if (!keyframe || keyframe->isBad() || !retained.count(keyframe->GetMap())) continue;
        const auto Twc = (*relative * parent * keyframe->GetPose()).inverse();
        const auto p = Twc.translation();
        const auto q = Twc.unit_quaternion();
        if (!p.allFinite() || !q.coeffs().allFinite()) continue;
        poses << *timestamp << ',' << keyframe->GetMap()->GetId() << ',' << p.x() << ',' << p.y() << ',' << p.z()
              << ',' << q.x() << ',' << q.y() << ',' << q.z() << ',' << q.w() << '\n';
    }
    return 0;
}
