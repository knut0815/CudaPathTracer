#include "Config.h"
#include "Test.h"
#include "Maths.h"
#include <algorithm>
#include <atomic>

#if DO_CUDA_RENDER
#include "../Cuda/CudaRender.cuh"
#endif // DO_CUDA_RENDER


static Sphere s_Spheres[] =
{
    {f3(0,-100.5,-1), 100},
    {f3(2,0,-1), 0.5f},
    {f3(0,0,-1), 0.5f},
    {f3(-2,0,-1), 0.5f},
    {f3(2,0,1), 0.5f},
    {f3(0,0,1), 0.5f},
    {f3(-2,0,1), 0.5f},
    {f3(0.5f,1,0.5f), 0.5f},
    {f3(-1.5f,1.5f,0.f), 0.3f},
};
const int kSphereCount = sizeof(s_Spheres) / sizeof(s_Spheres[0]);

static Material s_SphereMats[kSphereCount] =
{
    { Material::Lambert, f3(0.8f, 0.8f, 0.8f), f3(0,0,0), 0, 0, },
    { Material::Lambert, f3(0.8f, 0.4f, 0.4f), f3(0,0,0), 0, 0, },
    { Material::Lambert, f3(0.4f, 0.8f, 0.4f), f3(0,0,0), 0, 0, },
    { Material::Metal, f3(0.4f, 0.4f, 0.8f), f3(0,0,0), 0, 0 },
    { Material::Metal, f3(0.4f, 0.8f, 0.4f), f3(0,0,0), 0, 0 },
    { Material::Metal, f3(0.4f, 0.8f, 0.4f), f3(0,0,0), 0.2f, 0 },
    { Material::Metal, f3(0.4f, 0.8f, 0.4f), f3(0,0,0), 0.6f, 0 },
    { Material::Dielectric, f3(0.4f, 0.4f, 0.4f), f3(0,0,0), 0, 1.5f },
    { Material::Lambert, f3(0.8f, 0.6f, 0.2f), f3(30,25,15), 0, 0 },
};

static Camera s_Cam;

const float kMinT = 0.001f;
const float kMaxT = 1.0e7f;

struct RendererData
{
    int frameCount;
    int screenWidth, screenHeight;
    float* backbuffer;
    Camera* cam;
    int numRays;

#if DO_CUDA_RENDER == 0
    Ray* rays;
    Hit* hits;
#endif // !DO_CUDA_RENDER
    f3* colors;
};

#if DO_CUDA_RENDER == 0
void HitWorld(const Ray* rays, const int num_rays, float tMin, float tMax, Hit* hits)
{
    for (int rIdx = 0; rIdx < num_rays; rIdx++)
    {
        const Ray& r = rays[rIdx];
        if (r.isDone())
            continue;

        float closest = tMax, hitT;
        int hitId = -1;
        for (int i = 0; i < kSphereCount; ++i)
        {
            if (HitSphere(r, s_Spheres[i], tMin, closest, hitT))
            {
                closest = hitT;
                hitId = i;
            }
        }

        hits[rIdx] = Hit(hitT, hitId);
    }
}

bool ScatterNoLightSampling(const Material& mat, const Ray& r_in, const Hit& rec, f3& attenuation, Ray& scattered, uint32_t& state)
{
    const f3 hitPos = r_in.pointAt(rec.t);
    const f3 hitNormal = s_Spheres[rec.id].normalAt(hitPos);

    if (mat.type == Material::Lambert)
    {
        // random point on unit sphere that is tangent to the hit point
        f3 target = hitPos + hitNormal + RandomUnitVector(state);
        scattered = Ray(hitPos, normalize(target - hitPos));
        attenuation = mat.albedo;

        return true;
    }
    else if (mat.type == Material::Metal)
    {
        AssertUnit(r_in.dir); AssertUnit(hitNormal);
        f3 refl = reflect(r_in.dir, hitNormal);
        // reflected ray, and random inside of sphere based on roughness
        float roughness = mat.roughness;
        scattered = Ray(hitPos, normalize(refl + roughness * RandomInUnitSphere(state)));
        attenuation = mat.albedo;
        return dot(scattered.dir, hitNormal) > 0;
    }
    else if (mat.type == Material::Dielectric)
    {
        AssertUnit(r_in.dir); AssertUnit(hitNormal);
        f3 outwardN;
        f3 rdir = r_in.dir;
        f3 refl = reflect(rdir, hitNormal);
        float nint;
        attenuation = f3(1, 1, 1);
        f3 refr;
        float reflProb;
        float cosine;
        if (dot(rdir, hitNormal) > 0)
        {
            outwardN = -hitNormal;
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
            reflProb = schlick(cosine, mat.ri);
        }
        else
        {
            reflProb = 1;
        }
        if (RandomFloat01(state) < reflProb)
            scattered = Ray(hitPos, normalize(refl));
        else
            scattered = Ray(hitPos, normalize(refr));
    }
    else
    {
        attenuation = f3(1, 0, 1);
        return false;
    }
    return true;
}

void Scatter(const RendererData& data, const int depth, int& inoutRayCount)
{
    for (int rIdx = 0; rIdx < data.numRays; rIdx++)
    {
        const Ray& r = data.rays[rIdx];
        if (r.isDone())
            continue;

        uint32_t state = (wang_hash(rIdx) + (data.frameCount*kMaxDepth + depth) * 101141101) * 336343633;

        const Hit& hit = data.hits[rIdx];
        Sample& sample = data.samples[rIdx];
        if (depth == 0)
        {
            sample.color = f3(0, 0, 0);
            sample.attenuation = f3(1, 1, 1);
        }

        if (hit.id >= 0)
        {
            Ray scattered;
            const Material& mat = s_SphereMats[hit.id];
            f3 local_attenuation;
            sample.color += mat.emissive * sample.attenuation;
            if (depth < kMaxDepth && ScatterNoLightSampling(mat, r, hit, local_attenuation, scattered, state))
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
            f3 unitDir = r.dir;
            float t = 0.5f*(unitDir.y + 1.0f);
            sample.color += sample.attenuation * ((1.0f - t)*f3(1.0f, 1.0f, 1.0f) + t * f3(0.5f, 0.7f, 1.0f)) * 0.3f;
            data.rays[rIdx].setDone();
        }

        ++inoutRayCount;
    }
}
#endif // !DO_CUDA_RENDER


static int TracePixels(RendererData data)
{
    float* backbuffer = data.backbuffer;
    float invWidth = 1.0f / data.screenWidth;
    float invHeight = 1.0f / data.screenHeight;
    float lerpFac = float(data.frameCount) / float(data.frameCount + 1);
#if !DO_PROGRESSIVE
    lerpFac = 0;
#endif
    int rayCount = 0;

#if DO_CUDA_RENDER
    deviceStartFrame(data.frameCount);
#else
    // generate camera rays for all samples
    for (int y = 0, rIdx = 0; y < data.screenHeight; y++)
    {
        for (int x = 0; x < data.screenWidth; x++)
        {
            for (int s = 0; s < DO_SAMPLES_PER_PIXEL; s++, ++rIdx)
            {
                uint32_t state = ((wang_hash(rIdx) + (data.frameCount*kMaxDepth) * 101141101) * 336343633) | 1;

                float u = float(x + RandomFloat01(state)) * invWidth;
                float v = float(y + RandomFloat01(state)) * invHeight;
                data.rays[rIdx] = data.cam->GetRay(u, v, state);
            }
        }
    }
#endif

    // trace all samples through the scene
    for (int depth = 0; depth <= kMaxDepth; depth++)
    {
#if DO_CUDA_RENDER
        deviceRenderFrame(kMinT, kMaxT, depth);
#else
        HitWorld(data.rays, data.numRays, kMinT, kMaxT, data.hits);
        Scatter(data, depth, rayCount);
#endif
    }

    return rayCount;
}

void Render(int screenWidth, int screenHeight, float* backbuffer, int& outRayCount)
{
    f3 lookfrom(0, 2, 3);
    f3 lookat(0, 0, 0);
    float distToFocus = 3;
    float aperture = 0.1f;

    for (int i = 0; i < kSphereCount; ++i)
        s_Spheres[i].UpdateDerivedData();

    s_Cam = Camera(lookfrom, lookat, f3(0, 1, 0), 60, float(screenWidth) / float(screenHeight), aperture, distToFocus);

    // let's allocate a few arrays needed by the renderer
    int numRays = screenWidth * screenHeight * DO_SAMPLES_PER_PIXEL;
#if DO_CUDA_RENDER == 0
    Ray* rays = new Ray[numRays];
    Hit* hits = new Hit[numRays];
#endif

    f3* colors = new f3[numRays];

    RendererData args;
    args.screenWidth = screenWidth;
    args.screenHeight = screenHeight;
    args.backbuffer = backbuffer;
    args.cam = &s_Cam;
    args.colors = colors;
#if DO_CUDA_RENDER == 0
    args.rays = rays;
    args.hits = hits;
#endif // !DO_CUDA_RENDER
    args.numRays = numRays;

#if DO_CUDA_RENDER
    deviceInitData(&s_Cam, screenWidth, screenHeight, s_Spheres, s_SphereMats, kSphereCount, numRays);
    if (kNumFrames == 100) outRayCount = 854161957;
    else if (kNumFrames == 1000) outRayCount = 4246579592;
#endif // DO_CUDA_RENDER

    for (int frame = 0; frame < kNumFrames; frame++)
    {
        args.frameCount = frame;
        outRayCount += TracePixels(args);
    }

#if DO_CUDA_RENDER
    deviceEndRendering(args.colors);

    // compute cumulated color for all samples
    const float devider = 1.0f / float(kNumFrames*DO_SAMPLES_PER_PIXEL);
    for (int y = 0, rIdx = 0; y < args.screenHeight; y++) {
        for (int x = 0; x < args.screenWidth; x++) {
            f3 col(0, 0, 0);
            for (int s = 0; s < DO_SAMPLES_PER_PIXEL; s++, ++rIdx)
                col += args.colors[rIdx];
            col *= devider;

            backbuffer[0] = col.x;
            backbuffer[1] = col.y;
            backbuffer[2] = col.z;
            backbuffer += 4;
        }
    }

    deviceFreeData();
#else
    delete[] rays;
    delete[] hits;
#endif // DO_CUDA_RENDER
    delete[] colors;

}
