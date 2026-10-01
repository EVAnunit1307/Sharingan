// Type declaration only. This target excludes the upstream OpenGL viewer.
// No graphics function is implemented or invoked by the replay executable.
#pragma once
using GLubyte = unsigned char;
namespace pangolin { struct OpenGlMatrix {}; }
