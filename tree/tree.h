#pragma once

#include "util.h"
#include <opencv2/core/core.hpp>
#include <vector>
#include <queue>
#include <filesystem>
#include <random>
#include <nlohmann/json.hpp>

using json = nlohmann::basic_json<>;

namespace fs = std::filesystem;


class qcanvas;

using std::vector;
using std::priority_queue;

using cv::Point2f;
typedef cv::Matx<float, 3, 3> Matx33;
typedef cv::Matx<float, 4, 1> Matx41;
typedef cv::Matx<float, 4, 4> Matx44;


class qcanvas
{
public:
    Matx33 globalTransform;
    cv::Mat image;

    qcanvas() {
    }

    void create(cv::Mat im)
    {
        image = im;
    }

    // sets global transform map to map provided domain to image, centered, vertically flipped
    void setScaleToFit(cv::Rect_<float> const &rect, float buffer)
    {
        if (image.empty()) throw std::exception("Image is empty");

        globalTransform = util::transform3x3::centerAndFit(rect, cv::Rect_<float>(0.0f, 0.0f, (float)image.cols, (float)image.rows), buffer, true);
    }

};


class qtransform
{
public:
    std::string transformMatrixKey;
    Matx33 transformMatrix;
    Matx44 colorTransform;
    double gestation;

public:

    qtransform(Matx33 const &transformMatrix_ = Matx33::eye(), Matx44 const &colorTransform_ = Matx44::eye(), double gestation_ = 1.0)
    {
        transformMatrix = transformMatrix_;
        colorTransform = colorTransform_;
        gestation = gestation_;
    }

    template<typename _Tp>
    qtransform(_Tp m00, _Tp m01, _Tp mtx, _Tp m10, _Tp m11, _Tp mty, Matx44 const &colorTransform_)
    {
        transformMatrix = Matx33(m00, m01, mtx, m10, m11, mty, 0, 0, 1);
        colorTransform = colorTransform_;
        gestation = 1.0;
    }

    qtransform(double angle, double scale, cv::Point2f translate)
    {
        transformMatrix = util::transform3x3::getRotationMatrix2D(Point2f(), angle, scale, translate.x, translate.y);
        colorTransform = colorTransform.eye();
        colorTransform(2, 2) = 0.94f;
        colorTransform(1, 1) = 0.96f;
        gestation = 1.0;
    }
};

    // serialize vector of points

    template<typename _Tp>
    void to_json(json &j, std::vector<cv::Point_<_Tp> > const &polygon)
    {
        j = nlohmann::json::array();
        for (auto &pt : polygon)
        {
            j.push_back(pt.x);
            j.push_back(pt.y);
        }
    }

    template<typename _Tp>
    void from_json(json const &j, std::vector<cv::Point_<_Tp> > &polygon)
    {
        if (!j.is_array())
            throw(std::exception("Not JSON array type"));

        polygon.clear();
        polygon.reserve(j.size());

        for (int i = 0; i < j.size(); i += 2)
            polygon.push_back(cv::Point2f(j[i], j[i + 1]));
    }

    // serialize M x N matrix

    template<typename _Tp, int m, int n>
    void to_json(json &j, cv::Matx<_Tp, m, n> const &mat)
    {
        j = nlohmann::json::array();
        for (int r = 0; r < m; ++r)
            for (int c = 0; c < n; ++c)
                j[r].push_back((float)mat(r, c));
                //sz += std::to_string(mat(r, j)) + (j < n - 1 ? ", " : r < n - 1 ? ",\n  " : "\n  ]");
    }

    template<typename _Tp, int m, int n>
    void from_json(json const &j, cv::Matx<_Tp, m, n> &mat)
    {
        for (int r = 0; r < m; ++r)
            for (int c = 0; c < n; ++c)
                mat(r, c) = j[r][c];
        //sz += std::to_string(mat(r, j)) + (j < n - 1 ? ", " : r < n - 1 ? ",\n  " : "\n  ]");
    }

    template<typename _Tp, int m, int n>
    void from_json(json const &j, std::vector<cv::Matx<_Tp, m, n> > &mats)
    {
        mats.resize(j.size());
        for (int i = 0; i < j.size(); ++i)
            from_json(j[i], mats[i]);
    }


    inline void to_json(json &j, qtransform const &t)
    {
        j = json {
            { "gestation", t.gestation }
        };

        to_json(j["color"], t.colorTransform);

        if(t.transformMatrixKey.empty())
            to_json(j["transform"], t.transformMatrix);
        else
            j["transform"] = t.transformMatrixKey;
    }

    inline void to_json(json &j, std::vector<qtransform> const &transforms)
    {
        j = nlohmann::json::array();
        for (int i = 0; i < transforms.size(); ++i)
            to_json(j[i], transforms[i]);
    }

    inline void from_json(json const &j, qtransform &t)
    {
        t.gestation = j.at("gestation");
        from_json(j.at("color"),     t.colorTransform);
        from_json(j.at("transform"), t.transformMatrix);
    }

    inline void from_json(json const &j, std::vector<qtransform> &transforms)
    {
        transforms.resize(j.size());
        for (int i = 0; i < j.size(); ++i)
            from_json(j[i], transforms[i]);
    }


class qnode
{
public:
    double      beginTime;
    int         generation;
    Matx33      globalTransform;
    cv::Scalar  color = cv::Scalar(1, 0, 0, 1);

    qnode(double beginTime_ = 0)
    {
        beginTime = beginTime_;
        generation = 0;
        color = cv::Scalar(1, 1, 1, 1);
        globalTransform = globalTransform.eye();
    }

    inline float det() const { return (globalTransform(0, 0) * globalTransform(1, 1) - globalTransform(0, 1) * globalTransform(1, 0)); }

    inline bool operator!() const { return !( fabs(det()) > 1e-5 ); }

    void getPolyPoints(std::vector<cv::Point2f> const &points, std::vector<cv::Point2f> &transformedPoints) const
    {
        cv::transform(points, transformedPoints, globalTransform.get_minor<2, 3>(0, 0));
    }

    struct EarliestFirst
    {
        bool operator()(qnode const& a, qnode const& b)
        {
            return (a.beginTime > b.beginTime);
        }
    };

    struct BiggestFirst
    {
        bool operator()(qnode const& a, qnode const& b)
        {
            return (b.det() > a.det());
        }
    };
};


//  This macro should be invoked for each qtree-extending class
//  It creates a global function pointer (which is never used)
//  and registers a constructor lambda for the given qtree-derived class
#define REGISTER_QTREE_TYPE(TYPE) \
    auto g_constructor_ ## TYPE = qtree::registerConstructor(#TYPE, [](){ return new TYPE(); });

class qtree
{
protected:
    static std::map<std::string, std::function<qtree*()> > factoryTable;

public:
    // settings
    double maxRadius = 100.0;
    int randomSeed = 0;

    // same polygon for all nodes
    std::vector<cv::Point2f> polygon;

    std::vector<qtransform> transforms;
    
    double gestationRandomness = 0.0;

    // draw settings
    cv::Scalar lineColor = cv::Scalar(0);
    int lineThickness = 1;

    // model
    std::mt19937 prng; //Standard mersenne_twister_engine with default seed
    std::priority_queue<qnode, std::deque<qnode>, qnode::EarliestFirst> nodeQueue;

public:
    qtree() {}

    virtual void setRandomSeed(int seed)
    {
        randomSeed = seed;
        prng.seed(seed);
    }

#pragma region Serialization

    //  Extending classes should override and invoke the base member as necessary
    virtual void to_json(json &j) const
    {
        //  extending classes need to override this value
        j["_class"] = "qtree";

        j["randomSeed"] = randomSeed;
        j["maxRadius"] = maxRadius;

        ::to_json(j["polygon"], polygon);
        for (auto &t : transforms)
        {
            json jt;
            ::to_json(jt, t);
            j["transforms"].push_back(jt);
        }

        j["gestationRandomness"] = gestationRandomness;

        j["drawSettings"] = json{
            { "lineColor", util::toRgbHexString(lineColor) },
            { "lineThickness", lineThickness }
        };
    }

    //  Extending classes should override and invoke the base member as necessary
    virtual void from_json(json const &j)
    {
        // using at() rather than [] so we get a proper exception on a missing key
        // (rather than assert/abort with NDEBUG)

        randomSeed = j.at("randomSeed");
        maxRadius  = j.at("maxRadius");
        ::from_json( j.at("polygon"), polygon );
        ::from_json( j.at("transforms"), transforms );

        gestationRandomness = (j.contains("gestationRandomness") ? j.at("gestationRandomness").get<double>() : 0.0);

        lineColor = util::fromRgbHexString(j.at("drawSettings").at("lineColor").get<std::string>().c_str());
        lineThickness = j.at("drawSettings").at("lineThickness");
    }

    //  Registers a typed constructor lambda fn
    static auto registerConstructor(std::string className, std::function<qtree*()> fn)
    {
        factoryTable[className] = fn;
        return fn;
    }

    //  Factory method to create instances of registered qtree-extending classes
    static qtree* createTreeFromJson(json const &j)
    {
        //  peek at the "_class" member of the json to see what class to instantiate

        if (!j.contains("_class"))
        {
            throw(std::exception("Invalid JSON or missing \"_class\" key."));
        }

        std::string className = j["_class"];
        if (factoryTable.find(className) == factoryTable.end())
        {
            std::string msg = std::string("Class not registered: '") + className + "'";
            throw(std::exception(msg.c_str()));
        }

        auto pfn = factoryTable.at(className);
        qtree* pPrototype = pfn();
        pPrototype->from_json(j);
        return pPrototype;
    }

    qtree* clone() const
    {
        json j;
        to_json(j);
        return qtree::createTreeFromJson(j);
    }

    virtual void create() = 0;

    // process the next node in the queue
    virtual bool process();

    // override to indicate that a child node should not be added
    virtual bool isViable(qnode const & node) const { return true; }

    // invoked when a viable node is pulled from the queue. override to update drawing, data structures, etc.
    virtual void addNode(qnode & node) { }

    // generate a child node from a parent
    virtual void beget(qnode const & parent, qtransform const & t, qnode & child);

    virtual cv::Rect_<float> getBoundingRect() const
    {
        float r = (float)maxRadius;
        return cv::Rect_<float>(-r, -r, 2 * r, 2 * r);
    }

    virtual void drawNode(qcanvas &canvas, qnode const &node);

    virtual void saveImage(fs::path imagePath) { };

    // util
    
#pragma region PRNG

    //  random double in [0, 1)
    inline double r()
    {
        std::uniform_real_distribution<double> dist{ 0.0, 1.0 };
        return dist(prng);
    }

    //  random double in [0, max)
    inline double r(double maxVal)
    {
        std::uniform_real_distribution<double> dist{ 0.0, maxVal };
        return dist(prng);
    }

    //  random int in [0, max)
    inline int r(int maxVal)
    {
        std::uniform_int_distribution<int> dist{ 0, maxVal-1 };
        return dist(prng);
    }

    inline cv::Scalar randomColor()
    {
        return util::hsv2bgr(r(360.0), 1.0, 0.5);
    }

#pragma endregion

};


