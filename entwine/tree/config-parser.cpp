/******************************************************************************
* Copyright (c) 2016, Connor Manning (connor@hobu.co)
*
* Entwine -- Point cloud indexing
*
* Entwine is available under the terms of the LGPL2 license. See COPYING
* for specific license text and more information.
*
******************************************************************************/

#include <limits>
#include <numeric>

#include <entwine/tree/config-parser.hpp>

#include <entwine/third/arbiter/arbiter.hpp>
#include <entwine/third/json/json.hpp>
#include <entwine/tree/builder.hpp>
#include <entwine/tree/manifest.hpp>
#include <entwine/types/bbox.hpp>
#include <entwine/types/metadata.hpp>
#include <entwine/types/reprojection.hpp>
#include <entwine/types/schema.hpp>
#include <entwine/types/subset.hpp>
#include <entwine/util/inference.hpp>
#include <entwine/util/unique.hpp>

namespace entwine
{

namespace
{
    Json::Reader reader;

    std::unique_ptr<BBox> getBBox(const Json::Value& json)
    {
        std::unique_ptr<BBox> bbox;
        if (!json.empty()) bbox.reset(new BBox(json));
        return bbox;
    }

    std::unique_ptr<Reprojection> getReprojection(const Json::Value& json)
    {
        std::unique_ptr<Reprojection> reprojection;

        if (!json.empty())
        {
            reprojection.reset(new Reprojection(json));
        }

        return reprojection;
    }
}

std::unique_ptr<Builder> ConfigParser::getBuilder(
        Json::Value config,
        std::shared_ptr<arbiter::Arbiter> arbiter,
        std::unique_ptr<Manifest> manifest)
{
    const Json::Value& jsonInput(config["input"]);
    const Json::Value& jsonOutput(config["output"]);
    const Json::Value& jsonGeometry(config["geometry"]);
    Json::Value& jsonStructure(config["structure"]);

    // Build specifications and path info.
    const std::string outPath(jsonOutput["path"].asString());
    const std::string tmpPath(jsonOutput["tmp"].asString());
    const bool compress(jsonOutput["compress"].asUInt64());
    const bool force(jsonOutput["force"].asBool());

    // Indexing parameters.
    const bool trustHeaders(jsonInput["trustHeaders"].asBool());
    const std::size_t threads(jsonInput["threads"].asUInt64());

    if (!force)
    {
        auto builder = tryGetExisting(
                config,
                *arbiter,
                outPath,
                tmpPath,
                threads);

        if (builder) return builder;
    }

    // Geometry and spatial info.
    auto bboxConforming(getBBox(jsonGeometry["bbox"]));
    auto reprojection(getReprojection(jsonGeometry["reproject"]));
    Schema schema(jsonGeometry["schema"]);

    std::unique_ptr<Subset> subset;

    if (config.isMember("subset"))
    {
        BBox cube(*bboxConforming);
        if (!cube.isCubic()) cube.cubeify();

        subset = makeUnique<Subset>(cube, config["subset"]);

        const std::size_t configNullDepth(
                jsonStructure["nullDepth"].asUInt64());

        if (configNullDepth < subset->minimumNullDepth())
        {
            std::cout << "Bumping null depth to accomodate subset" << std::endl;
            jsonStructure["nullDepth"] =
                Json::UInt64(subset->minimumNullDepth());
        }
    }

    std::size_t numPointsHint(jsonStructure["numPointsHint"].asUInt64());
    if (!numPointsHint && manifest)
    {
        numPointsHint = std::accumulate(
                manifest->paths().begin(),
                manifest->paths().end(),
                std::size_t(0),
                [](std::size_t sum, const FileInfo& f)
                {
                    return sum + f.numPoints();
                });
    }

    if (manifest && (!bboxConforming || !schema.pointSize() || !numPointsHint))
    {
        std::cout << "Performing dataset inference..." << std::endl;
        Inference inference(
                *manifest,
                tmpPath,
                threads,
                true,
                reprojection.get(),
                trustHeaders,
                arbiter.get());

        inference.go();
        manifest.reset(new Manifest(inference.manifest()));

        if (!bboxConforming)
        {
            bboxConforming.reset(new BBox(inference.bbox()));
            std::cout << "Inferred: " << inference.bbox() << std::endl;
        }

        if (!schema.pointSize())
        {
            auto dims(inference.schema().dims());
            const std::size_t originSize([&manifest]()
            {
                if (manifest->size() <= std::numeric_limits<uint32_t>::max())
                    return 4;
                else
                    return 8;
            }());

            dims.emplace_back("Origin", "unsigned", originSize);

            schema = Schema(dims);
        }

        if (!numPointsHint) numPointsHint = inference.numPoints();
    }


    jsonStructure["numPointsHint"] = static_cast<Json::UInt64>(numPointsHint);
    Structure structure(jsonStructure);
    Structure hierarchyStructure(Hierarchy::structure(structure));

    const Metadata metadata(
            *bboxConforming,
            schema,
            structure,
            hierarchyStructure,
            *manifest,
            reprojection.get(),
            subset.get(),
            trustHeaders,
            compress);

    OuterScope outerScope;
    outerScope.setArbiter(arbiter);

    return makeUnique<Builder>(metadata, outPath, tmpPath, threads, outerScope);
}

std::unique_ptr<Builder> ConfigParser::tryGetExisting(
        const Json::Value& config,
        const arbiter::Arbiter& arbiter,
        const std::string& outPath,
        const std::string& tmpPath,
        const std::size_t numThreads)
{
    std::unique_ptr<Builder> builder;
    std::unique_ptr<std::size_t> subsetId;

    if (config.isMember("subset"))
    {
        subsetId = makeUnique<std::size_t>(config["subset"]["id"].asUInt64());
    }

    const std::string postfix(subsetId ? "-" + std::to_string(*subsetId) : "");

    if (arbiter.getEndpoint(outPath).tryGetSize("entwine" + postfix))
    {
        builder = makeUnique<Builder>(outPath, tmpPath, numThreads);
    }

    return builder;
}

std::unique_ptr<Manifest> ConfigParser::getManifest(
        const Json::Value& json,
        const arbiter::Arbiter& arbiter)
{
    std::unique_ptr<Manifest> manifest;

    const Json::Value& input(json["input"]);
    const Json::Value& jsonManifest(input["manifest"]);

    if (jsonManifest.isString() || jsonManifest.isArray())
    {
        // The input source is a path or array of paths.
        std::vector<std::string> paths;

        auto insert([&paths, &arbiter](std::string in)
        {
            std::vector<std::string> current(arbiter.resolve(in, true));
            paths.insert(paths.end(), current.begin(), current.end());
        });

        if (jsonManifest.isArray())
        {
            for (Json::ArrayIndex i(0); i < jsonManifest.size(); ++i)
            {
                insert(jsonManifest[i].asString());
            }
        }
        else
        {
            insert(jsonManifest.asString());
        }

        manifest.reset(new Manifest(paths));
    }
    else if (jsonManifest.isObject())
    {
        // The input source is a previously inferred manifest.
        manifest.reset(new Manifest(jsonManifest));
    }

    return manifest;
}

} // namespace entwine

