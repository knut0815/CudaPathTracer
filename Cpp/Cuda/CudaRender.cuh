#pragma once

#include <helper_math.h>
#include <assert.h>

#include "../Source/Maths.h"
#include "device_launch_parameters.h"

void deviceInitData(const Camera* camera, const uint width, const uint height, const Sphere* spheres, const Material* materials, const int spheresCount, const int numRays);

void deviceStartFrame(const uint frame);
void deviceRenderFrame(const float tMin, const float tMax, const uint depth);
void deviceEndFrame(Sample* samples);

void deviceFreeData();
