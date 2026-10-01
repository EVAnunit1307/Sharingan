// Headless adapter for the GPL-3.0 ORB-SLAM3 research build.
// Replaces display-only classes; the tracking/mapping implementations are upstream.
#include "System.h"
#include "Viewer.h"
#include "MapDrawer.h"
#include <chrono>
#include <stdexcept>
#include <thread>

namespace ORB_SLAM3 {
Atlas* System::ExportAtlas() const { return mpAtlas; }
Tracking* System::ExportTracker() const { return mpTracker; }
void System::WaitForExport() {
    // Upstream Shutdown currently requests termination without joining workers.
    // Export only once no local/loop/global optimizer can modify the map.
    while (!mpLocalMapper->isFinished() || !mpLoopCloser->isFinished() || mpLoopCloser->isRunningGBA())
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    if (mptLocalMapping->joinable()) mptLocalMapping->join();
    if (mptLoopClosing->joinable()) mptLoopClosing->join();
}
MapDrawer::MapDrawer(Atlas* a, const std::string&, Settings*) : mpAtlas(a) {}
void MapDrawer::SetCurrentCameraPose(const Sophus::SE3f&) {}
Viewer::Viewer(System*, FrameDrawer*, MapDrawer*, Tracking*, const std::string&, Settings*) {
    throw std::runtime_error("This build requires bUseViewer=false");
}
void Viewer::Run() { throw std::runtime_error("OpenGL viewer unavailable"); }
void Viewer::RequestFinish() {}
void Viewer::RequestStop() {}
bool Viewer::isFinished() { return true; }
bool Viewer::isStopped() { return true; }
bool Viewer::isStepByStep() { return false; }
void Viewer::Release() {}
}
