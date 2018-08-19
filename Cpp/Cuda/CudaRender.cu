#include "CudaRender.cuh"
#include "../Source/Config.h"

__device__ float3 cRandomInUnitDisk(uint& state);

struct cHit
{
    __device__ cHit() {}
    __device__ cHit(float _t, float _id) :t(_t), id(_id) {}

    float t;
    int id;
};

struct cRay
{
    __device__ cRay() {}
    __device__ cRay(const float3& orig_, const float3& dir_) : orig(orig_), dir(dir_) {}

    __device__ float3 pointAt(float t) const { return orig + dir * t; }
    __device__ bool isDone() const { return dir.x == 0 && dir.y == 0 && dir.z == 0; }
    __device__ void setDone() { dir = make_float3(0); }

    float3 orig;
    float3 dir;
};

struct cSphere
{
    float3 center;
    float radius;
    float _not_used;

    __device__ float3 normalAt(const float3& pos) const { return (pos - center) / radius; }
};

struct cMaterial
{
    enum Type { Lambert, Metal, Dielectric };
    Type type;
    float3 albedo;
    float3 emissive;
    float roughness;
    float ri;
};

struct cSample
{
    float3 color;
    float3 attenuation;
};

struct cCamera
{
    __device__ cRay GetRay(float s, float t, uint32_t& state) const
    {
        float3 rd = lensRadius * cRandomInUnitDisk(state);
        float3 offset = u * rd.x + v * rd.y;
        return cRay(origin + offset, normalize(lowerLeftCorner + s * horizontal + t * vertical - origin - offset));
    }

    float3 origin;
    float3 lowerLeftCorner;
    float3 horizontal;
    float3 vertical;
    float3 u, v, w;
    float lensRadius;
};

struct DeviceData
{
    cRay* rays;
    cHit* hits;
    cSample* samples;
    cSphere* spheres;
    cMaterial* materials;
    cCamera* camera;
    uint numRays;
    uint spheresCount;
    uint frame;
    uint width;
    uint height;
};

DeviceData deviceData;

__device__ float sqLength(const float3& v)
{
    return v.x*v.x + v.y*v.y + v.z*v.z;
}

inline __device__ void AssertUnit(const float3& v)
{
    assert(fabsf(sqLength(v) - 1.0f) < 0.01f);
}

__device__ bool HitSphere(const cRay& r, const cSphere& s, float tMin, float tMax, float& outHitT)
{
    AssertUnit(r.dir);
    float3 oc = r.orig - s.center;
    float b = dot(oc, r.dir);
    float c = dot(oc, oc) - s.radius*s.radius;
    float discr = b * b - c;
    if (discr > 0)
    {
        float discrSq = sqrtf(discr);

        float t = (-b - discrSq);
        if (t < tMax && t > tMin)
        {
            outHitT = t;
            return true;
        }
        t = (-b + discrSq);
        if (t < tMax && t > tMin)
        {
            outHitT = t;
            return true;
        }
    }
    return false;
}

__global__ void HitWorldKernel(const DeviceData data, float tMin, float tMax)
{
    const int rIdx = blockIdx.x*blockDim.x + threadIdx.x;
    if (rIdx >= data.numRays)
        return;

    const cRay& r = data.rays[rIdx];
    if (r.isDone())
        return;

    int hitId = -1;
    float closest = tMax, hitT;
    for (int i = 0; i < data.spheresCount; ++i)
    {
        if (HitSphere(r, data.spheres[i], tMin, closest, hitT))
        {
            closest = hitT;
            hitId = i;
        }
    }

    data.hits[rIdx] = cHit(closest, hitId);
}

__device__ uint cXorShift32(uint& state)
{
    uint x = state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 15;
    state = x;
    return x;
}

__device__ float cRandomFloat01(uint& state)
{
    return (cXorShift32(state) & 0xFFFFFF) / 16777216.0f;
}

__device__ float3 cRandomInUnitDisk(uint& state)
{
    float3 p;
    do
    {
        p = make_float3(2 * cRandomFloat01(state) - 1, 2 * cRandomFloat01(state) - 1, 0);
    } while (dot(p, p) >= 1.0);
    return p;
}

float3 make_float3(const f3 f) {
    return make_float3(f.x, f.y, f.z);
}

__device__ float3 cRandomUnitVector(uint& state)
{
    float z = cRandomFloat01(state) * 2.0f - 1.0f;
    float a = cRandomFloat01(state) * 2.0f * kPI;
    float r = sqrtf(1.0f - z * z);
    float x = r * cosf(a);
    float y = r * sinf(a);
    return make_float3(x, y, z);
}

__device__ float3 cRandomInUnitSphere(uint& state)
{
    float3 p;
    do {
        p = make_float3(2*cRandomFloat01(state) - 1, 2*cRandomFloat01(state) - 1, 2*cRandomFloat01(state) - 1);
    } while (sqLength(p) >= 1.0);
    return p;
}

/*
* based off http://www.reedbeta.com/blog/quick-and-easy-gpu-random-numbers-in-d3d11/
*/
__device__ uint cWang_hash(uint seed)
{
    seed = (seed ^ 61) ^ (seed >> 16);
    seed *= 9;
    seed = seed ^ (seed >> 4);
    seed *= 0x27d4eb2d;
    seed = seed ^ (seed >> 15);
    return seed;
}

__device__ bool refract(const float3& v, const float3& n, float nint, float3& outRefracted)
{
    AssertUnit(v);
    float dt = dot(v, n);
    float discr = 1.0f - nint * nint*(1 - dt * dt);
    if (discr > 0)
    {
        outRefracted = nint * (v - n * dt) - n * sqrtf(discr);
        return true;
    }
    return false;
}

__device__ float cSchlick(float cosine, float ri)
{
    float r0 = (1 - ri) / (1 + ri);
    r0 = r0 * r0;
    return r0 + (1 - r0)*powf(1 - cosine, 5);
}

__device__ bool ScatterNoLightSampling(const DeviceData& data, const cMaterial& mat, const cRay& r_in, const cHit& rec, float3& attenuation, cRay& scattered, uint& state)
{
    const float3 hitPos = r_in.pointAt(rec.t);
    const float3 hitNormal = data.spheres[rec.id].normalAt(hitPos);

    if (mat.type == cMaterial::Lambert)
    {
        // random point on unit sphere that is tangent to the hit point
        float3 target = hitPos + hitNormal + cRandomUnitVector(state);
        scattered = cRay(hitPos, normalize(target - hitPos));
        attenuation = mat.albedo;

        return true;
    }
    else if (mat.type == cMaterial::Metal)
    {
        AssertUnit(r_in.dir); AssertUnit(hitNormal);
        float3 refl = reflect(r_in.dir, hitNormal);
        // reflected ray, and random inside of sphere based on roughness
        float roughness = mat.roughness;
        scattered = cRay(hitPos, normalize(refl + roughness * cRandomInUnitSphere(state)));
        attenuation = mat.albedo;
        return dot(scattered.dir, hitNormal) > 0;
    }
    else if (mat.type == cMaterial::Dielectric)
    {
        AssertUnit(r_in.dir); AssertUnit(hitNormal);
        float3 outwardN;
        float3 rdir = r_in.dir;
        float3 refl = reflect(rdir, hitNormal);
        float nint;
        attenuation = make_float3(1, 1, 1);
        float3 refr;
        float reflProb;
        float cosine;
        if (dot(rdir, hitNormal) > 0)
        {
            outwardN = -1*hitNormal;
            nint = mat.ri;
            cosine = mat.ri * dot(rdir, hitNormal);
        }
        else
        {
            outwardN = hitNormal;
            nint = 1.0f / mat.ri;
            cosine = -dot(rdir, hitNormal);
        }
        if (refract(rdir, outwardN, nint, refr))
        {
            reflProb = cSchlick(cosine, mat.ri);
        }
        else
        {
            reflProb = 1;
        }
        if (cRandomFloat01(state) < reflProb)
            scattered = cRay(hitPos, normalize(refl));
        else
            scattered = cRay(hitPos, normalize(refr));
    }
    else
    {
        attenuation = make_float3(1, 0, 1);
        return false;
    }
    return true;
}

__global__ void ScatterKernel(const DeviceData data, const uint depth)
{
    const int rIdx = blockIdx.x*blockDim.x + threadIdx.x;
    if (rIdx >= data.numRays)
        return;

    const cRay& r = data.rays[rIdx];
    if (r.isDone())
        return;

    uint state = (cWang_hash(rIdx) + (data.frame*kMaxDepth + depth) * 101141101) * 336343633;

    const cHit& hit = data.hits[rIdx];
    cSample& sample = data.samples[rIdx];
    if (depth == 0)
    {
        sample.color = make_float3(0);
        sample.attenuation = make_float3(1);
    }

    if (hit.id >= 0)
    {
        cRay scattered;
        const cMaterial& mat = data.materials[hit.id];
        float3 local_attenuation;
        sample.color += mat.emissive * sample.attenuation;
        if (depth < kMaxDepth && ScatterNoLightSampling(data, mat, r, hit, local_attenuation, scattered, state))
        {
            sample.attenuation *= local_attenuation;
            data.rays[rIdx] = scattered;
        }
        else
        {
            data.rays[rIdx].setDone();
        }
    }
    else
    {
        // sky
        float3 unitDir = r.dir;
        float t = 0.5f*(unitDir.y + 1.0f);
        sample.color += sample.attenuation * ((1.0f - t)*make_float3(1) + t * make_float3(0.5f, 0.7f, 1.0f)) * 0.3f;
        data.rays[rIdx].setDone();
    }
}

__global__ void generateRays(const DeviceData data)
{
    const int rIdx = blockIdx.x*blockDim.x + threadIdx.x;
    if (rIdx >= data.numRays)
        return;

    const uint w = data.width*DO_SAMPLES_PER_PIXEL;
    const uint y = rIdx / w;
    const uint x = (rIdx % w) / DO_SAMPLES_PER_PIXEL;
    uint state = ((cWang_hash(rIdx) + (data.frame*kMaxDepth) * 101141101) * 336343633) | 1;

    float u = float(x + cRandomFloat01(state)) / data.width;
    float v = float(y + cRandomFloat01(state)) / data.height;
    data.rays[rIdx] = data.camera->GetRay(u, v, state);

}

void deviceInitData(const Camera* camera, const uint width, const uint height, const Sphere* spheres, const Material* materials, const int spheresCount, const int numRays)
{
    deviceData.numRays = numRays;
    deviceData.spheresCount = spheresCount;
    deviceData.width = width;
    deviceData.height = height;

    // allocate device memory
    cudaMalloc((void**)&deviceData.spheres, spheresCount * sizeof(cSphere));
    cudaMalloc((void**)&deviceData.materials, spheresCount * sizeof(cMaterial));
    cudaMalloc((void**)&deviceData.rays, numRays * sizeof(cRay));
    cudaMalloc((void**)&deviceData.hits, numRays * sizeof(cHit));
    cudaMalloc((void**)&deviceData.samples, numRays * sizeof(cSample));
    cudaMalloc((void**)&deviceData.camera, sizeof(cCamera));

    // copy spheres and materials to device
    cudaMemcpy(deviceData.spheres, spheres, spheresCount * sizeof(cSphere), cudaMemcpyHostToDevice);
    cudaMemcpy(deviceData.materials, materials, spheresCount * sizeof(cMaterial), cudaMemcpyHostToDevice);

    cudaMemcpy(deviceData.camera, camera, sizeof(cCamera), cudaMemcpyHostToDevice);
}

void deviceStartFrame(const uint frame) {
    deviceData.frame = frame;

    // call kernel
    const int threadsPerBlock = 1024;
    const int blocksPerGrid = ceilf((float)deviceData.numRays / threadsPerBlock);
    generateRays <<<blocksPerGrid, threadsPerBlock >>> (deviceData);
}

void deviceRenderFrame(const float tMin, const float tMax, const uint depth)
{
    // call kernel
    const int threadsPerBlock = 1024;
    const int blocksPerGrid = ceilf((float)deviceData.numRays / threadsPerBlock);

    HitWorldKernel <<<blocksPerGrid, threadsPerBlock >> > (deviceData, tMin, tMax);
    ScatterKernel <<<blocksPerGrid, threadsPerBlock >> > (deviceData, depth);
}

void deviceEndFrame(Sample* samples)
{
    // copy samples to host
    cudaMemcpy(samples, deviceData.samples, deviceData.numRays * sizeof(cSample), cudaMemcpyDeviceToHost);
}

void deviceFreeData()
{
    cudaFree(deviceData.spheres);
    cudaFree(deviceData.rays);
    cudaFree(deviceData.hits);
    cudaFree(deviceData.samples);
    cudaFree(deviceData.camera);
}
