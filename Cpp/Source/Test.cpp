#include "Config.h"
#include "Test.h"
#include "Maths.h"
#include <algorithm>
#include <atomic>

static Sphere s_Spheres[] =
{
    {float3(0,-100.5,-1), 100},
    {float3(2,0,-1), 0.5f},
    {float3(0,0,-1), 0.5f},
    {float3(-2,0,-1), 0.5f},
    {float3(2,0,1), 0.5f},
    {float3(0,0,1), 0.5f},
    {float3(-2,0,1), 0.5f},
    {float3(0.5f,1,0.5f), 0.5f},
    {float3(-1.5f,1.5f,0.f), 0.3f},
};
const int kSphereCount = sizeof(s_Spheres) / sizeof(s_Spheres[0]);

struct Material
{
    enum Type { Lambert, Metal, Dielectric };
    Type type;
    float3 albedo;
    float3 emissive;
    float roughness;
    float ri;
};

static Material s_SphereMats[kSphereCount] =
{
    { Material::Lambert, float3(0.8f, 0.8f, 0.8f), float3(0,0,0), 0, 0, },
    { Material::Lambert, float3(0.8f, 0.4f, 0.4f), float3(0,0,0), 0, 0, },
    { Material::Lambert, float3(0.4f, 0.8f, 0.4f), float3(0,0,0), 0, 0, },
    { Material::Metal, float3(0.4f, 0.4f, 0.8f), float3(0,0,0), 0, 0 },
    { Material::Metal, float3(0.4f, 0.8f, 0.4f), float3(0,0,0), 0, 0 },
    { Material::Metal, float3(0.4f, 0.8f, 0.4f), float3(0,0,0), 0.2f, 0 },
    { Material::Metal, float3(0.4f, 0.8f, 0.4f), float3(0,0,0), 0.6f, 0 },
    { Material::Dielectric, float3(0.4f, 0.4f, 0.4f), float3(0,0,0), 0, 1.5f },
    { Material::Lambert, float3(0.8f, 0.6f, 0.2f), float3(30,25,15), 0, 0 },
};

static Camera s_Cam;

const float kMinT = 0.001f;
const float kMaxT = 1.0e7f;
const int kMaxDepth = 10;


void HitWorld(const Ray* rays, const int num_rays, float tMin, float tMax, Hit* hits)
{
    for (int rIdx = 0; rIdx < num_rays; rIdx++)
    {
        const Ray& r = rays[rIdx];
        if (r.done)
            continue;

        Hit tmpHit, outHit;
        float closest = tMax;
        for (int i = 0; i < kSphereCount; ++i)
        {
            if (HitSphere(r, s_Spheres[i], tMin, closest, tmpHit))
            {
                closest = tmpHit.t;
                outHit = tmpHit;
                outHit.id = i;
            }
        }

        hits[rIdx] = outHit;
    }
}

static bool ScatterNoLightSampling(const Material& mat, const Ray& r_in, const Hit& rec, float3& attenuation, Ray& scattered, uint32_t& state)
{
    if (mat.type == Material::Lambert)
    {
        // random point on unit sphere that is tangent to the hit point
        float3 target = rec.pos + rec.normal + RandomUnitVector(state);
        scattered = Ray(rec.pos, normalize(target - rec.pos));
        attenuation = mat.albedo;

        return true;
    }
    else if (mat.type == Material::Metal)
    {
        AssertUnit(r_in.dir); AssertUnit(rec.normal);
        float3 refl = reflect(r_in.dir, rec.normal);
        // reflected ray, and random inside of sphere based on roughness
        float roughness = mat.roughness;
#if DO_MITSUBA_COMPARE
        roughness = 0; // until we get better BRDF for metals
#endif
        scattered = Ray(rec.pos, normalize(refl + roughness * RandomInUnitSphere(state)));
        attenuation = mat.albedo;
        return dot(scattered.dir, rec.normal) > 0;
    }
    else if (mat.type == Material::Dielectric)
    {
        AssertUnit(r_in.dir); AssertUnit(rec.normal);
        float3 outwardN;
        float3 rdir = r_in.dir;
        float3 refl = reflect(rdir, rec.normal);
        float nint;
        attenuation = float3(1, 1, 1);
        float3 refr;
        float reflProb;
        float cosine;
        if (dot(rdir, rec.normal) > 0)
        {
            outwardN = -rec.normal;
            nint = mat.ri;
            cosine = mat.ri * dot(rdir, rec.normal);
        }
        else
        {
            outwardN = rec.normal;
            nint = 1.0f / mat.ri;
            cosine = -dot(rdir, rec.normal);
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
            scattered = Ray(rec.pos, normalize(refl));
        else
            scattered = Ray(rec.pos, normalize(refr));
    }
    else
    {
        attenuation = float3(1, 0, 1);
        return false;
    }
    return true;
}

static void TraceIterative(Ray* rays, Sample* samples, Hit* hits, const int numRays, int& inoutRayCount, uint32_t& state)
{
    for (int rIdx = 0; rIdx < numRays; rIdx++)
    {
        Sample& sample = samples[rIdx];
        sample.color = float3(0, 0, 0);
        sample.attenuation = float3(1, 1, 1);
    }

    for (int depth = 0; depth <= kMaxDepth; depth++)
    {
        HitWorld(rays, numRays, kMinT, kMaxT, hits);
        for (int rIdx = 0; rIdx < numRays; rIdx++)
        {
            const Ray& r = rays[rIdx];
            if (r.done)
                continue;

            const Hit& rec = hits[rIdx];
            Sample& sample = samples[rIdx];

            ++inoutRayCount;
            if (rec.id >= 0)
            {
                Ray scattered;
                const Material& mat = s_SphereMats[rec.id];
                float3 local_attenuation;
                sample.color += mat.emissive * sample.attenuation;
                if (depth < kMaxDepth && ScatterNoLightSampling(mat, r, rec, local_attenuation, scattered, state))
                {
                    sample.attenuation *= local_attenuation;
                    rays[rIdx] = scattered;
                }
                else
                {
                    rays[rIdx].done = true;
                }
            }
            else
            {
                // sky
#if DO_MITSUBA_COMPARE
                sample.color += sample.attenuation * float3(0.15f, 0.21f, 0.3f); // easier compare with Mitsuba's constant environment light
#else
                float3 unitDir = r.dir;
                float t = 0.5f*(unitDir.y + 1.0f);
                sample.color += sample.attenuation * ((1.0f - t)*float3(1.0f, 1.0f, 1.0f) + t * float3(0.5f, 0.7f, 1.0f)) * 0.3f;
                rays[rIdx].done = true;
#endif
            }
        }
    }
}

struct RendererData
{
    int frameCount;
    int screenWidth, screenHeight;
    float* backbuffer;
    Camera* cam;
    int numRays;
    Ray* rays;
    Hit* hits;
    Sample* samples;
};

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
    uint32_t state = (data.frameCount * 26699) | 1;


    // generate camera rays for all samples
    for (int y = 0, rIdx = 0; y < data.screenHeight; y++)
    {
        for (int x = 0; x < data.screenWidth; x++)
        {
            for (int s = 0; s < DO_SAMPLES_PER_PIXEL; s++, ++rIdx)
            {
                float u = float(x + RandomFloat01(state)) * invWidth;
                float v = float(y + RandomFloat01(state)) * invHeight;
                data.rays[rIdx] = data.cam->GetRay(u, v, state);
            }
        }
    }

    // trace all samples through the scene
    TraceIterative(data.rays, data.samples, data.hits, data.numRays, rayCount, state);

    // compute cumulated color for all samples
    for (int y = 0, rIdx = 0; y < data.screenHeight; y++)
    {
        for (int x = 0; x < data.screenWidth; x++)
        {
            float3 col(0, 0, 0);
            for (int s = 0; s < DO_SAMPLES_PER_PIXEL; s++, ++rIdx)
            {
                col += data.samples[rIdx].color;
            }
            col *= 1.0f / float(DO_SAMPLES_PER_PIXEL);

            float3 prev(backbuffer[0], backbuffer[1], backbuffer[2]);
            col = prev * lerpFac + col * (1 - lerpFac);
            backbuffer[0] = col.x;
            backbuffer[1] = col.y;
            backbuffer[2] = col.z;
            backbuffer += 4;
        }
    }

    return rayCount;
}

void Render(int screenWidth, int screenHeight, float* backbuffer, int& outRayCount)
{
    float3 lookfrom(0, 2, 3);
    float3 lookat(0, 0, 0);
    float distToFocus = 3;
#if DO_MITSUBA_COMPARE
    float aperture = 0.0f;
#else
    float aperture = 0.1f;
#endif

    for (int i = 0; i < kSphereCount; ++i)
        s_Spheres[i].UpdateDerivedData();

    s_Cam = Camera(lookfrom, lookat, float3(0, 1, 0), 60, float(screenWidth) / float(screenHeight), aperture, distToFocus);

    // let's allocate a few arrays needed by the renderer
    int numRays = screenWidth * screenHeight * DO_SAMPLES_PER_PIXEL;
    Ray* rays = new Ray[numRays];
    Sample* samples = new Sample[numRays];
    Hit* hits = new Hit[numRays];

    RendererData args;
    args.screenWidth = screenWidth;
    args.screenHeight = screenHeight;
    args.backbuffer = backbuffer;
    args.cam = &s_Cam;
    args.rays = rays;
    args.samples = samples;
    args.hits = hits;
    args.numRays = numRays;

    for (int frame = 0; frame < kNumFrames; frame++)
    {
        args.frameCount = frame;
        outRayCount += TracePixels(args);
    }

    delete[] rays;
    delete[] samples;
    delete[] hits;
}
