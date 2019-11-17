#pragma once


#include "tree.h"
#include "util.h"
#include <vector>
#include <iostream>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
#include <array>
#include <list>
#include <unordered_set>


using std::cout;
using std::endl;


//  Limited-growth qtrees
//  These models limit growth by prohibiting self-intersection


#pragma region SelfLimitingPolygonTree

class SelfLimitingPolygonTree : public qtree
{
protected:
    // --- settings ---

    int polygonSides = 5;
    int starAngle = 0;
    cv::Scalar rootNodeColor = cv::Scalar(0, 0, 1, 1);

    // controls size of intersection field--in pixels per model unit, independent of display resolution.
    int fieldResolution = 40;

    // minimum size (relative to rootNode) for new nodes to be considered viable
    float minimumScale = 0.01f; 

    std::vector<Matx44> colorTransformPalette;

    // --- model ---

    // intersection field
    cv::Mat1b m_field;
    Matx33 m_fieldTransform;
    // temp drawing layer, same size as field, for drawing individual nodes and checking for intersection
    mutable cv::Mat1b m_fieldLayer;
    mutable cv::Rect m_fieldLayerBoundingRect;

public:

    SelfLimitingPolygonTree() { }

    virtual void to_json(json &j) const override
    {
	    qtree::to_json(j);

        j["_class"] = "SelfLimitingPolygonTree";
        j["fieldResolution"] = fieldResolution;
        j["polygonSides"] = polygonSides;
        j["starAngle"] = starAngle;
        
        j["rootNode"] = json{
            { "color", util::toRgbHexString(rootNodeColor) }
        };
    }

    virtual void from_json(json const &j) override
    {
        qtree::from_json(j);

        fieldResolution = j.at("fieldResolution");
        polygonSides = (j.contains("polygonSides") ? j.at("polygonSides").get<int>() : 5);
        starAngle    = (j.contains("starAngle"   ) ? j.at("starAngle"   ).get<int>() : 0);
        
        if (j.contains("rootNode"))
        {
            ::from_json(j.at("rootNode").at("color"), rootNodeColor);
        }
    }

    virtual void setRandomSeed(int randomize) override
    {
        qtree::setRandomSeed(randomize);

        maxRadius = 10.0;
        polygonSides = 5;
        starAngle = 36;

        if (randomize)
        {
            maxRadius = 5.0 + r(40.0);
            polygonSides = (randomize % 6) + 3;
            starAngle = ((randomize % 12) < 6 ? 36 : 0);
            //gestationRandomness = r(200.0);
        }

        if (starAngle)
            util::polygon::createStar(polygon, polygonSides, starAngle);
        else
            util::polygon::createRegularPolygon(polygon, polygonSides);

        // create set of edge transforms to map edge 0 to all other edges,
        // child polygons will be potentially spawned whose 0 edge aligns with parent polygon's {i} edge
        transforms.clear();
        for (int i = 0; i < polygon.size(); ++i)
        {
            transforms.push_back(createEdgeTransform(i, 0));
        }

        randomizeTransforms(3);
    }

    //  Randomizes existing transforms: color, gestation
    virtual void randomizeTransforms(int flags) override
    {
        if (flags & 1)
        {
            colorTransformPalette.clear();
            // HLS space color transforms
            //colorTransformPalette.push_back(util::scaleAndTranslate(1.0, 0.99, 1.0, 180.0*r()*r()-90.0, 0.0, 0.0));    // darken and hue-shift
            //colorTransformPalette.push_back(util::colorSink( 20.0, 0.5, 1.0, 0.25));   // toward orange
            //colorTransformPalette.push_back(util::colorSink(200.0, 0.5, 1.0, 0.25));   // toward blue
            double lightness = 0.5;
            double sat = 1.0;
            colorTransformPalette.push_back(util::colorSink(r(720.0) - 360.0, 0.5 + r(0.5), sat, r(0.5)));
            colorTransformPalette.push_back(util::colorSink(r(720.0) - 360.0, 0.5 + r(0.5), sat, r(0.5)));
            colorTransformPalette.push_back(util::colorSink(r(720.0) - 360.0, 0.5 + r(0.5), sat, r(0.5)));
        }

        for (auto & t : transforms)
        {
            if (flags & 1)
            {
                int idx = r((int)colorTransformPalette.size());
                t.colorTransform = colorTransformPalette[idx];
            }

            if (flags & 2)
            {
                t.gestation = 1.0 + r(10.0);
            }
        }
    }

    virtual void create() override
    {
        // initialize intersection field
        int size = (int)(0.5 + maxRadius * 2 * fieldResolution);
        m_field.create(size, size);
        m_field = 0;
        m_field.copyTo(m_fieldLayer);
        m_fieldTransform = util::transform3x3::getScaleTranslate((double)fieldResolution, maxRadius*fieldResolution, maxRadius*fieldResolution);

        // clear and initialize the queue with the seed

        qnode rootNode;
        createRootNode(rootNode);

        util::clear(nodeQueue);
        nodeQueue.push(rootNode);

        m_nodeList.clear();
    }

    virtual void createRootNode(qnode & rootNode)
    {
        rootNode.id = 0;
        rootNode.parentId = 0;
        rootNode.beginTime = 0;
        rootNode.generation = 0;
        rootNode.color = rootNodeColor;

        // center root node at origin
        auto centroid = util::polygon::centroid(polygon);
        rootNode.globalTransform = util::transform3x3::getScaleTranslate(1.0f, -centroid.x, -centroid.y);
    }

    virtual void beget(qnode const & parent, qtransform const & t, qnode & child) override
    {
        qtree::beget(parent, t, child);

        // apply affine transform in HLS space
        auto hls = util::cvtColor(parent.color, cv::ColorConversionCodes::COLOR_BGR2HLS);
        hls = t.colorTransform*hls;
        child.color = util::cvtColor(hls, cv::ColorConversionCodes::COLOR_HLS2BGR);
    }


    virtual bool isViable(qnode const &node) const override
    {
        if (!node) 
            return false;

        if (fabs(node.det()) < minimumScale*minimumScale)
            return false;

        if (!drawField(node))
            return false;   // out of image bounds

        thread_local cv::Mat andmat;
        cv::bitwise_and(m_field(m_fieldLayerBoundingRect), m_fieldLayer(m_fieldLayerBoundingRect), andmat);
        return(!cv::countNonZero(andmat));
    }

    //  draw node on field stage layer to prepare for collision detection
    //  full collision detection is not done here, but this function returns false if out-of-bounds or other
    //  quick detection means that this node is not viable.
    bool drawField(qnode const &node) const
    {
        vector<cv::Point2f> v;

        // first, transform node polygon to model coordinates
        cv::transform(polygon, v, node.globalTransform.get_minor<2, 3>(0, 0));

        // test each vertex against maxRadius
        for (auto const& p : v)
            if (p.dot(p) > maxRadius*maxRadius)
                return false;

        // transform model polygon to field coords
        Matx33 m = m_fieldTransform * node.globalTransform;
        cv::transform(polygon, v, m.get_minor<2, 3>(0, 0));
        // convert to int-coordinate struct for cv::polylines
        vector<vector<cv::Point> > pts(1);
        for (auto const& p : v)
            pts[0].push_back(p);

        // (double) check that coords are within field
        m_fieldLayerBoundingRect = cv::boundingRect(pts[0]);
        if ((cv::Rect(0, 0, m_field.cols, m_field.rows) & m_fieldLayerBoundingRect) != m_fieldLayerBoundingRect)
            return false;

        // clear a region of our scratch layer and draw node on it
        m_fieldLayer(m_fieldLayerBoundingRect) = 0;
        cv::fillPoly(m_fieldLayer, pts, cv::Scalar(255), cv::LineTypes::LINE_8);
        // reduce by drawing outline in black:
        // this is a bit of a hack to get around the problem of OpenCV always drawing a pixel-wide boundary even when only a fill is specified
        cv::polylines(m_fieldLayer, pts, true, cv::Scalar(0), 1, cv::LineTypes::LINE_8);
        // double-draw and soften the line--purely for aesthetics, since the field layer is exported as well
        cv::polylines(m_fieldLayer, pts, true, cv::Scalar(0), 1, cv::LineTypes::LINE_AA);

        return true;
    }

    void undrawNode(qnode &node)
    {
        drawField(node);
        cv::bitwise_not(m_fieldLayer(m_fieldLayerBoundingRect), m_fieldLayer(m_fieldLayerBoundingRect));
        cv::bitwise_and(m_field(m_fieldLayerBoundingRect), m_fieldLayer(m_fieldLayerBoundingRect), m_field(m_fieldLayerBoundingRect));
    }

    std::list<qnode> m_nodeList;

    virtual void addNode(qnode &currentNode) override
    {
        m_nodeList.push_back(currentNode);

        // update field image: composite new node
        cv::bitwise_or(m_field(m_fieldLayerBoundingRect), m_fieldLayer(m_fieldLayerBoundingRect), m_field(m_fieldLayerBoundingRect));
    }

    auto findNode(int id)
    {
        for (auto it = m_nodeList.begin(); it!=m_nodeList.end(); ++it)
        {
            if (it->id == id)
            {
                return it;
            }
        }
        return m_nodeList.end();
    }

    virtual int removeNode(int id) override
    {
        auto it = m_nodeList.begin();
        if(id>=0)
            it = findNode(id);

        if (it == m_nodeList.end())
        {
            // not found
            return 0;
        }
        undrawNode(*it);
        it = m_nodeList.erase(it);
        cout << " -- removing " << (it->id) << " from " << (it->parentId) << " remaining: " << m_nodeList.size() << endl;
        return 1;
        /*
        m_markedForDeletion.clear();
        m_markedForDeletion.insert(id);

        markDescendantsForDeletion();

        int removed = 0;
        for (auto it = m_nodeList.begin(); it != m_nodeList.end(); )
        {
            if (m_markedForDeletion.find(it->id) != m_markedForDeletion.end())
            {
                cout << " -- removing " << (it->id) << " from " << (it->parentId) << endl;
                undrawNode(*it);

                it = m_nodeList.erase(it);
                ++removed;
            }
            else
            {
                ++it;
            }
        }

        m_markedForDeletion.clear();

        //sprout();

        return removed;//*/
    }

    virtual void regrowAll() override
    {
        for (auto & currentNode : m_nodeList)
        {
            // !TODO! this is duplicated code
            // create a child node for each available transform.
            // all child nodes are added to the queue, even if not viable.
            for (auto const &t : transforms)
            {
                qnode child;
                beget(currentNode, t, child);
                nodeQueue.push(child);
            }
        }
    }

    //  Node draw function for tree of nodes with all the same polygon
    virtual void redrawAll(qcanvas &canvas) override
    {
        canvas.image = 0;
        for (auto & node : m_nodeList)
        {
            drawNode(canvas, node);
        }
    }

};

REGISTER_QTREE_TYPE(SelfLimitingPolygonTree);

#pragma endregion

#pragma region ScaledPolygonTree

class ScaledPolygonTree : public SelfLimitingPolygonTree
{
    double m_ratio;
    bool m_ambidextrous;

public:
    virtual void setRandomSeed(int randomize) override
    {
        SelfLimitingPolygonTree::setRandomSeed(randomize);

        fieldResolution = 100.0;
        maxRadius = 4.0;
        
        // ratio: child size / parent size
        std::array<float, 5> ratioPresets = { {
            (sqrt(5.0f) - 1.0f) / 2.0f,        // phi
            0.5f,
            1.0f / 3.0f,
            1.0f / sqrt(2.0f),
            (sqrt(3.0f) - 1.0f) / 2.0f
        } };

        m_ratio = ratioPresets[randomize%ratioPresets.size()];
        m_ambidextrous = r(2);// (randomize % 2);

        // override edge transforms
        transforms.clear();
        for (int i = 0; i < polygon.size(); ++i)
        {
            transforms.push_back(createEdgeTransform(i, polygon.size() - 1, false, 0.0f, m_ratio));

            if (m_ambidextrous)
                transforms.push_back(createEdgeTransform(i, polygon.size() - 1, true, 1.0f - m_ratio, 1.0f));

        }

        randomizeTransforms(3);
    }

    virtual void to_json(json &j) const override
    {
	    SelfLimitingPolygonTree::to_json(j);

        j["_class"] = "ScaledPolygonTree";
        j["ratio"] = m_ratio;
        j["ambidextrous"] = m_ambidextrous;
    }

    virtual void from_json(json const &j) override
    {
        SelfLimitingPolygonTree::from_json(j);

        m_ratio = j["ratio"];
        m_ambidextrous = j["ambidextrous"];
    }

    virtual void create() override
    {
        SelfLimitingPolygonTree::create();


    }

};

REGISTER_QTREE_TYPE(ScaledPolygonTree);

#pragma endregion

#pragma region TrapezoidTree

class TrapezoidTree : public SelfLimitingPolygonTree
{
public:
    virtual void setRandomSeed(int randomize) override
    {
        SelfLimitingPolygonTree::setRandomSeed(randomize);

        fieldResolution = 200;
        maxRadius = 10;
        gestationRandomness = 10;
        rootNodeColor = cv::Scalar(0.2, 0.5, 0, 1);
    }

    virtual void create() override
    {
        SelfLimitingPolygonTree::create();

        // override polygon
        polygon = { { { -0.5f, -0.5f}, {0.5f, -0.5f}, {0.4f, 0.4f}, {-0.5f, 0.45f} } };

        //auto m = util::transform3x3::getMirroredEdgeMap(cv::Point2f( 0.0f,0.0f ), cv::Point2f( 1.0f,0.0f ), cv::Point2f( 0.0f,0.0f ), cv::Point2f( 1.0f,0.0f ));

        int steps = 24;
        float angle = 6.283f / steps;
        float r0 = 0.5f;
        float r1 = 1.0f;
        float growthFactor = pow(r1 / r0, 2.0f / steps);
        polygon = { { {r0,0}, {r1,0}, {r1*growthFactor*cos(angle),r1*growthFactor*sin(angle)}, {r0*growthFactor*cos(angle),r0*growthFactor*sin(angle)} } };

        // override edge transforms
        transforms.clear();
        //for (int i = 1; i < polygon.size(); ++i)
        //{
        //    transforms.push_back(
        //        qtransform(
        //            util::transform3x3::getMirroredEdgeMap(polygon[0], polygon[1], polygon[i], polygon[(i+1)%polygon.size()]),
        //            util::colorSink(randomColor(), 0.5f))
        //    );
        //}

        transforms.push_back(
            qtransform(
                util::transform3x3::getMirroredEdgeMap(polygon[0], polygon[1], polygon[1], polygon[2]),
                util::colorSink(randomColor(), 0.5))
        );
        transforms.push_back(
            qtransform(
                util::transform3x3::getEdgeMap(polygon[0], polygon[1], polygon[3], polygon[2]),
                util::colorSink(randomColor(), 0.5))
        );
        transforms.push_back(
            qtransform(
                util::transform3x3::getMirroredEdgeMap(polygon[0], polygon[1], polygon[3], polygon[0]),
                util::colorSink(randomColor(), 0.5))
        );

        //transforms[0].gestation = 1111.1;
        transforms[0].gestation = r(10.0);
        transforms[1].gestation = r(10.0);
        transforms[2].gestation = r(10.0);

        //transforms[0].colorTransform = util::colorSink(1.0f,1.0f,1.0f, 0.5f);
        //transforms[0].colorTransform = util::colorSink(0.0f,0.5f,0.0f, 0.8f);
        //transforms[1].colorTransform = util::colorSink(0.5f,1.0f,1.0f, 0.3f);
        //transforms[2].colorTransform = util::colorSink(0.9f,0.5f,0.0f, 0.8f);

    }

};

REGISTER_QTREE_TYPE(TrapezoidTree);

#pragma endregion

#pragma region ThornTree

//  Tesselations based on the "Versatile" thorn-shaped 9-sided polygon
//  described by Penrose, Grunwald, ...
class ThornTree : public SelfLimitingPolygonTree
{
public:
    virtual void setRandomSeed(int randomize) override
    {
        SelfLimitingPolygonTree::setRandomSeed(randomize);

        fieldResolution = 20;
        maxRadius = 50;
        gestationRandomness = 0;

        rootNodeColor = cv::Scalar(1.0, 1.0, 0.0, 1);

        // override polygon
        polygon.clear();
        cv::Point2f pt(0, 0);
        polygon.push_back(pt);
        polygon.push_back(pt += util::polygon::headingStep(  0.0f));
        polygon.push_back(pt += util::polygon::headingStep(120.0f));
        polygon.push_back(pt += util::polygon::headingStep(105.0f));
        polygon.push_back(pt += util::polygon::headingStep( 90.0f));
        polygon.push_back(pt += util::polygon::headingStep( 75.0f));
        polygon.push_back(pt += util::polygon::headingStep(240.0f));
        polygon.push_back(pt += util::polygon::headingStep(255.0f));
        polygon.push_back(pt += util::polygon::headingStep(270.0f));

        // override edge transforms
        transforms.clear();
        //auto ct1 = util::colorSink(util::hsv2bgr( 10.0, 1.0, 0.75), 0.3);
        //auto ct2 = util::colorSink(util::hsv2bgr(200.0, 1.0, 0.75), 0.3);

        for (int i = 0; i < polygon.size(); ++i)
        {
            for (int j = 0; j < polygon.size(); ++j)
            {
                if (r(20) == 0)
                    transforms.push_back(createEdgeTransform(i, j));

                if (r(20) == 0)
                    transforms.push_back(createEdgeTransform(i, j, true));
            }
        }

        randomizeTransforms(3);
    }

    virtual void to_json(json &j) const override
    {
	    SelfLimitingPolygonTree::to_json(j);
		
        j["_class"] = "ThornTree";
    }

    virtual void from_json(json const &j) override
    {
        SelfLimitingPolygonTree::from_json(j);
    }

    virtual void create() override
    {
        SelfLimitingPolygonTree::create();

    }

    // overriding to save intersection field mask as well
    virtual void saveImage(fs::path imagePath) override
    {
        // save the intersection field mask
        imagePath = imagePath.replace_extension("mask.png");
        cv::imwrite(imagePath.string(), m_field);
    }

};

REGISTER_QTREE_TYPE(ThornTree);

#pragma endregion