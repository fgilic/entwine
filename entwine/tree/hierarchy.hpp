/******************************************************************************
* Copyright (c) 2016, Connor Manning (connor@hobu.co)
*
* Entwine -- Point cloud indexing
*
* Entwine is available under the terms of the LGPL2 license. See COPYING
* for specific license text and more information.
*
******************************************************************************/

#pragma once

#include <cstddef>
#include <deque>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include <entwine/third/json/json.hpp>
#include <entwine/types/bbox.hpp>
#include <entwine/types/metadata.hpp>
#include <entwine/types/point-pool.hpp>
#include <entwine/types/structure.hpp>
#include <entwine/util/spin-lock.hpp>

namespace entwine
{

class PointState;

class HierarchyCell
{
public:
    HierarchyCell() : m_val(0), m_spinner() { }
    HierarchyCell(uint64_t val) : m_val(val), m_spinner() { }

    HierarchyCell& operator=(const HierarchyCell& other)
    {
        m_val = other.val();
        return *this;
    }

    void count(int delta)
    {
        SpinGuard lock(m_spinner);
        m_val = static_cast<int>(m_val) + delta;
    }

    uint64_t val() const { return m_val; }

private:
    uint64_t m_val;
    SpinLock m_spinner;
};

using HierarchyTube = std::map<uint64_t, HierarchyCell>;

class HierarchyBlock
{
public:
    HierarchyBlock(const Id& id) : m_id(id) { }

    // Only count must be thread-safe.  Get/save are single-threaded.
    virtual void count(const Id& id, uint64_t tick, int delta) = 0;

    virtual uint64_t get(const Id& id, uint64_t tick) const = 0;
    virtual void save(const arbiter::Endpoint& ep, std::string pf = "") = 0;

    uint64_t get(const PointState& pointState) const;

protected:
    Id normalize(const Id& id) const { return id - m_id; }
    void push(std::vector<char>& data, uint64_t val)
    {
        data.insert(
                data.end(),
                reinterpret_cast<const char*>(&val),
                reinterpret_cast<const char*>(&val) + sizeof(val));
    }

    const Id m_id;
};

class ContiguousBlock : public HierarchyBlock
{
public:
    using HierarchyBlock::get;

    ContiguousBlock(const Id& id, std::size_t maxPoints)
        : HierarchyBlock(id)
        , m_tubes(maxPoints)
    { }

    ContiguousBlock(
            const Id& id,
            std::size_t maxPoints,
            const std::vector<char>& data)
        : HierarchyBlock(id)
        , m_tubes(maxPoints)
    {
        const char* pos(data.data());
        const char* end(data.data() + data.size());

        uint64_t tube, tick, cell;

        auto extract([&pos]()
        {
            const uint64_t v(*reinterpret_cast<const uint64_t*>(pos));
            pos += sizeof(uint64_t);
            return v;
        });

        while (pos < end)
        {
            tube = extract();
            tick = extract();
            cell = extract();

            m_tubes.at(tube)[tick] = cell;
        }
    }

    virtual void count(const Id& id, uint64_t tick, int delta) override
    {
        m_tubes.at(normalize(id).getSimple())[tick].count(delta);
    }

    virtual uint64_t get(const Id& id, uint64_t tick) const override
    {
        const HierarchyTube& tube(m_tubes.at(normalize(id).getSimple()));
        const auto it(tube.find(tick));
        if (it != tube.end()) return it->second.val();
        else return 0;
    }

    virtual void save(const arbiter::Endpoint& ep, std::string pf = "") override
    {
        std::vector<char> data;

        for (std::size_t tube(0); tube < m_tubes.size(); ++tube)
        {
            for (const auto& cell : m_tubes[tube])
            {
                push(data, tube);
                push(data, cell.first);
                push(data, cell.second.val());
            }
        }

        ep.put(m_id.str() + pf, data);
    }

private:
    std::vector<HierarchyTube> m_tubes;
};

class SparseBlock : public HierarchyBlock
{
public:
    using HierarchyBlock::get;

    virtual void count(const Id& id, uint64_t tick, int delta) override
    {
        SpinGuard lock(m_spinner);
        m_tubes[normalize(id)][tick].count(delta);
    }

    virtual uint64_t get(const Id& id, uint64_t tick) const override
    {
        const auto tubeIt(m_tubes.find(id));
        if (tubeIt != m_tubes.end())
        {
            const HierarchyTube& tube(tubeIt->second);
            const auto cellIt(tube.find(tick));
            if (cellIt != tube.end())
            {
                return cellIt->second.val();
            }
        }

        return 0;
    }

    virtual void save(const arbiter::Endpoint& ep, std::string pf = "") override
    {
        // TODO.
    }

private:
    SpinLock m_spinner;
    std::map<Id, HierarchyTube> m_tubes;
};

class HierarchyState;

class Hierarchy
{
public:
    Hierarchy(const Metadata& metadata)
        : m_bbox(metadata.bbox())
        , m_structure(metadata.hierarchyStructure())
        , m_base(0, m_structure.baseIndexSpan())
        , m_blocks()
    { }

    Hierarchy(
            const Metadata& metadata,
            const arbiter::Endpoint& ep,
            std::string postfix = "")
        : m_bbox(metadata.bbox())
        , m_structure(metadata.hierarchyStructure())
        , m_base(0, m_structure.baseIndexSpan(), ep.getBinary("0" + postfix))
        , m_blocks()
    { }

    void count(const PointState& state, int delta);
    uint64_t get(const PointState& pointState) const;

    void save(const arbiter::Endpoint& ep, std::string postfix)
    {
        m_base.save(ep);
        for (auto& pair : m_blocks) pair.second->save(ep, postfix);
    }

    void awakenAll() { }
    void merge(const Hierarchy& other) { }

    Json::Value query(
            const BBox& qbox,
            std::size_t depthBegin,
            std::size_t depthEnd);

    static Structure structure(const Structure& treeStructure)
    {
        const std::size_t nullDepth(0);
        const std::size_t baseDepth(
                std::max<std::size_t>(treeStructure.baseDepthEnd(), 12));
        const std::size_t coldDepth(0);
        const std::size_t pointsPerChunk(treeStructure.basePointsPerChunk());
        const std::size_t dimensions(treeStructure.dimensions());
        const std::size_t numPointsHint(treeStructure.numPointsHint());
        const bool tubular(treeStructure.tubular());
        const bool dynamicChunks(true);
        const bool prefixIds(false);
        const std::size_t sparseDepth(
                treeStructure.sparseDepthBegin() - startDepth());

        return Structure(
                nullDepth,
                baseDepth,
                coldDepth,
                pointsPerChunk,
                dimensions,
                numPointsHint,
                tubular,
                dynamicChunks,
                prefixIds,
                sparseDepth);
    }

    static constexpr std::size_t startDepth() { return 6; }

private:
    class Query
    {
    public:
        Query(const BBox& bbox, std::size_t depthBegin, std::size_t depthEnd)
            : m_bbox(bbox), m_depthBegin(depthBegin), m_depthEnd(depthEnd)
        { }

        const BBox& bbox() const { return m_bbox; }
        std::size_t depthBegin() const { return m_depthBegin; }
        std::size_t depthEnd() const { return m_depthEnd; }

    private:
        const BBox m_bbox;
        const std::size_t m_depthBegin;
        const std::size_t m_depthEnd;
    };

    void traverse(
            Json::Value& json,
            const Query& query,
            const PointState& pointState,
            std::deque<Dir>& lag);

    void accumulate(
            Json::Value& json,
            const Query& query,
            const PointState& pointState,
            std::deque<Dir>& lag,
            uint64_t inc);

    const BBox& m_bbox;
    const Structure& m_structure;

    ContiguousBlock m_base;
    std::map<Id, std::unique_ptr<HierarchyBlock>> m_blocks;
};
















































class Node
{
public:
    typedef splicer::ObjectPool<Node> NodePool;
    typedef NodePool::UniqueNodeType PooledNode;

    typedef std::map<Id, Node*> NodeMap;
    typedef std::set<Id> NodeSet;
    typedef std::map<Dir, PooledNode> Children;

    struct AnchoredNode
    {
        AnchoredNode() : node(nullptr), isAnchor(false) { }
        explicit AnchoredNode(Node* node) : node(node), isAnchor(false) { }

        Node* node;
        bool isAnchor;
    };

    typedef std::map<Id, AnchoredNode> AnchoredMap;

    Node() : m_count(0), m_children() { }

    Node(
            NodePool& nodePool,
            const char*& pos,
            std::size_t step,
            NodeMap& edges,
            Id id = 0,
            std::size_t depth = 0);

    void assign(
            NodePool& nodePool,
            const char*& pos,
            std::size_t step,
            NodeMap& edges,
            const Id& id,
            std::size_t depth = 0);

    Node& next(Dir dir, NodePool& nodePool)
    {
        if (Node* node = maybeNext(dir)) return *node;
        else
        {
            auto result(
                    m_children.emplace(
                        std::make_pair(dir, nodePool.acquireOne())));

            return *result.first->second;
        }
    }

    Node* maybeNext(Dir dir)
    {
        auto it(m_children.find(dir));

        if (it != m_children.end()) return &*it->second;
        else return nullptr;
    }

    void increment() { ++m_count; }
    void incrementBy(std::size_t n) { m_count += n; }

    std::size_t count() const { return m_count; }

    void merge(Node& other);
    void insertInto(Json::Value& json) const;

    NodeSet insertInto(
            const arbiter::Endpoint& ep,
            std::string postfix,
            std::size_t step);

    const Children& children() const { return m_children; }

private:
    Children& children() { return m_children; }

    AnchoredMap insertSlice(
            NodeSet& anchors,
            const AnchoredMap& slice,
            const arbiter::Endpoint& ep,
            std::string postfix,
            std::size_t step);

    void insertData(
            std::vector<char>& data,
            AnchoredMap& nextSlice,
            const Id& id,
            std::size_t step,
            std::size_t depth = 0);

    bool insertBinary(std::vector<char>& s) const;

    uint64_t m_count;
    Children m_children;
};

inline bool operator==(const Node& lhs, const Node& rhs)
{
    if (lhs.count() == rhs.count())
    {
        const auto& lhsChildren(lhs.children());
        const auto& rhsChildren(rhs.children());

        if (lhsChildren.size() == rhsChildren.size())
        {
            for (const auto& c : lhsChildren)
            {
                if (
                        !rhsChildren.count(c.first) ||
                        !(*c.second == *rhsChildren.at(c.first)))
                {
                    return false;
                }
            }

            return true;
        }
    }

    return false;
}

class OHierarchy
{
public:
    OHierarchy(const BBox& bbox, Node::NodePool& nodePool)
        : m_bbox(bbox)
        , m_nodePool(nodePool)
        , m_depthBegin(defaultDepthBegin)
        , m_step(defaultStep)
        , m_root()
        , m_edges()
        , m_anchors()
        , m_mutex()
        , m_endpoint()
        , m_postfix()
    { }

    OHierarchy(
            const BBox& bbox,
            Node::NodePool& nodePool,
            const Json::Value& json,
            const arbiter::Endpoint& ep,
            std::string postfix = "");

    Node& root() { return m_root; }

    Json::Value toJson(const arbiter::Endpoint& ep, std::string postfix);

    Json::Value query(
            BBox qbox,
            std::size_t depthBegin,
            std::size_t depthEnd);

    void merge(OHierarchy& other)
    {
        m_root.merge(other.root());
        m_anchors.insert(other.m_anchors.begin(), other.m_anchors.end());
    }

    std::size_t depthBegin() const { return m_depthBegin; }
    std::size_t step() const { return m_step; }
    const BBox& bbox() const { return m_bbox; }

    void awakenAll()
    {
        for (const auto& a : m_anchors) awaken(a);
        m_anchors.clear();
    }

    void setStep(std::size_t set) { m_step = set; }

    static const std::size_t defaultDepthBegin = 6;
    static const std::size_t defaultStep = 8;
    static const std::size_t defaultChunkBytes = 1 << 20;   // 1 MB.

    static Id climb(const Id& id, Dir dir)
    {
        return (id << 3) + 1 + toIntegral(dir);
    }

    Node::NodePool& nodePool() { return m_nodePool; }

private:
    OHierarchy(const OHierarchy& other) = delete;
    OHierarchy& operator=(const OHierarchy& other) = delete;

    void traverse(
            Node& out,
            std::deque<Dir>& lag,
            Node& cur,
            const BBox& cbox,
            const BBox& qbox,
            std::size_t depth,
            std::size_t depthBegin,
            std::size_t depthEnd,
            Id id = 0);

    void accumulate(
            Node& node,
            std::deque<Dir>& lag,
            Node& cur,
            std::size_t depth,
            std::size_t depthEnd,
            const Id& id);

    void awaken(const Id& id, const Node* node = nullptr);

    const BBox& m_bbox;
    Node::NodePool& m_nodePool;

    const std::size_t m_depthBegin;
    std::size_t m_step;

    Node m_root;
    Node::NodeMap m_edges;
    Node::NodeSet m_anchors;
    Node::NodeSet m_awoken;

    mutable std::mutex m_mutex;
    std::unique_ptr<arbiter::Endpoint> m_endpoint;
    std::string m_postfix;
};

class HierarchyClimber
{
public:
    HierarchyClimber(OHierarchy& hierarchy, std::size_t dimensions)
        : m_hierarchy(hierarchy)
        , m_bbox(hierarchy.bbox())
        , m_depthBegin(hierarchy.depthBegin())
        , m_depth(m_depthBegin)
        , m_step(hierarchy.step())
        , m_node(&hierarchy.root())
    { }

    void reset()
    {
        m_bbox = m_hierarchy.bbox();
        m_depth = m_depthBegin;
        m_node = &m_hierarchy.root();
    }

    void magnify(const Point& point)
    {
        const Dir dir(getDirection(point, m_bbox.mid()));
        m_bbox.go(dir);
        m_node = &m_node->next(dir, m_hierarchy.nodePool());
    }

    void count() { m_node->increment(); }
    std::size_t depthBegin() const { return m_depthBegin; }

private:
    OHierarchy& m_hierarchy;
    BBox m_bbox;

    const std::size_t m_depthBegin;

    std::size_t m_depth;
    std::size_t m_step;

    Node* m_node;
};

} // namespace entwine

