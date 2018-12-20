#pragma once

#include <helper_math.h>
#include <assert.h>

#include "../Source/Maths.h"
#include "device_launch_parameters.h"

void deviceInitData(const Camera* camera, const uint width, const uint height, const uint threadsPerPixel, const uint threadsPerBlock, const Sphere* spheres, const Material* materials, const int spheresCount, const int numRays, const uint maxDepth);

void deviceRenderFrame(const uint frame);
void deviceEndRendering(f3* colors, unsigned long long& rayCount);

void deviceFreeData();
