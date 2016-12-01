/* This file is part of the KDE project
 * Copyright (C) 2002-2005,2007 Rob Buis <buis@kde.org>
 * Copyright (C) 2002-2004 Nicolas Goutte <nicolasg@snafu.de>
 * Copyright (C) 2005-2006 Tim Beaulen <tbscope@gmail.com>
 * Copyright (C) 2005-2009 Jan Hambrecht <jaham@gmx.net>
 * Copyright (C) 2005,2007 Thomas Zander <zander@kde.org>
 * Copyright (C) 2006-2007 Inge Wallin <inge@lysator.liu.se>
 * Copyright (C) 2007-2008,2010 Thorsten Zachmann <zachmann@kde.org>

 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public License
 * along with this library; see the file COPYING.LIB.  If not, write to
 * the Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */

#include "SvgParser.h"

#include <cmath>

#include <FlakeDebug.h>

#include <QColor>
#include <QPainter>

#include <KoShape.h>
#include <KoShapeRegistry.h>
#include <KoShapeFactoryBase.h>
#include <KoShapeGroup.h>
#include <KoPathShape.h>
#include <KoDocumentResourceManager.h>
#include <KoPathShapeLoader.h>
#include <commands/KoShapeGroupCommand.h>
#include <commands/KoShapeUngroupCommand.h>
#include <KoImageCollection.h>
#include <KoColorBackground.h>
#include <KoGradientBackground.h>
#include <KoPatternBackground.h>
#include <KoFilterEffectRegistry.h>
#include <KoFilterEffect.h>
#include "KoFilterEffectStack.h"
#include "KoFilterEffectLoadingContext.h"
#include <KoClipPath.h>
#include <KoClipMask.h>
#include <KoXmlNS.h>

#include "SvgUtil.h"
#include "SvgShape.h"
#include "SvgGraphicContext.h"
#include "SvgFilterHelper.h"
#include "SvgGradientHelper.h"
#include "SvgClipPathHelper.h"
#include "parsers/SvgTransformParser.h"
#include "kis_pointer_utils.h"
#include <KoVectorPatternBackground.h>

#include "kis_debug.h"


SvgParser::SvgParser(KoDocumentResourceManager *documentResourceManager)
    : m_context(documentResourceManager)
    , m_documentResourceManager(documentResourceManager)
{
}

SvgParser::~SvgParser()
{
}

void SvgParser::setXmlBaseDir(const QString &baseDir)
{
    m_context.setInitialXmlBaseDir(baseDir);
}

void SvgParser::setResolution(const QRectF boundsInPixels, qreal pixelsPerInch)
{
    KIS_ASSERT(!m_context.currentGC());
    m_context.pushGraphicsContext();
    m_context.currentGC()->pixelsPerInch = pixelsPerInch;

    const qreal scale = 72.0 / pixelsPerInch;
    const QTransform t = QTransform::fromScale(scale, scale);
    m_context.currentGC()->currentBoundingBox = boundsInPixels;
    m_context.currentGC()->matrix = t;
}

QList<KoShape*> SvgParser::shapes() const
{
    return m_shapes;
}

// Helper functions
// ---------------------------------------------------------------------------------------

SvgGradientHelper* SvgParser::findGradient(const QString &id)
{
    SvgGradientHelper *result = 0;

    // check if gradient was already parsed, and return it
    if (m_gradients.contains(id)) {
        result = &m_gradients[ id ];
    }

    // check if gradient was stored for later parsing
    if (!result && m_context.hasDefinition(id)) {
        const KoXmlElement &e = m_context.definition(id);
        if (e.tagName().contains("Gradient")) {
            result = parseGradient(m_context.definition(id));
        }
    }

    return result;
}

QSharedPointer<KoVectorPatternBackground> SvgParser::findPattern(const QString &id, const KoShape *shape)
{
    QSharedPointer<KoVectorPatternBackground> result;

    // check if gradient was stored for later parsing
    if (m_context.hasDefinition(id)) {
        const KoXmlElement &e = m_context.definition(id);
        if (e.tagName() == "pattern") {
            result = parsePattern(m_context.definition(id), shape);
        }
    }

    return result;
}

SvgFilterHelper* SvgParser::findFilter(const QString &id, const QString &href)
{
    // check if filter was already parsed, and return it
    if (m_filters.contains(id))
        return &m_filters[ id ];

    // check if filter was stored for later parsing
    if (!m_context.hasDefinition(id))
        return 0;

    const KoXmlElement &e = m_context.definition(id);
    if (e.childNodesCount() == 0) {
        QString mhref = e.attribute("xlink:href").mid(1);

        if (m_context.hasDefinition(mhref))
            return findFilter(mhref, id);
        else
            return 0;
    } else {
        // ok parse filter now
        if (! parseFilter(m_context.definition(id), m_context.definition(href)))
            return 0;
    }

    // return successfully parsed filter or 0
    QString n;
    if (href.isEmpty())
        n = id;
    else
        n = href;

    if (m_filters.contains(n))
        return &m_filters[ n ];
    else
        return 0;
}

SvgClipPathHelper* SvgParser::findClipPath(const QString &id)
{
    return m_clipPaths.contains(id) ? &m_clipPaths[id] : 0;
}

// Parsing functions
// ---------------------------------------------------------------------------------------

qreal SvgParser::parseUnit(const QString &unit, bool horiz, bool vert, const QRectF &bbox)
{
    return SvgUtil::parseUnit(m_context.currentGC(), unit, horiz, vert, bbox);
}

qreal SvgParser::parseUnitX(const QString &unit)
{
    return SvgUtil::parseUnitX(m_context.currentGC(), unit);
}

qreal SvgParser::parseUnitY(const QString &unit)
{
    return SvgUtil::parseUnitY(m_context.currentGC(), unit);
}

qreal SvgParser::parseUnitXY(const QString &unit)
{
    return SvgUtil::parseUnitXY(m_context.currentGC(), unit);
}

SvgGradientHelper* SvgParser::parseGradient(const KoXmlElement &e)
{
    // IMPROVEMENTS:
    // - Store the parsed colorstops in some sort of a cache so they don't need to be parsed again.
    // - A gradient inherits attributes it does not have from the referencing gradient.
    // - Gradients with no color stops have no fill or stroke.
    // - Gradients with one color stop have a solid color.

    SvgGraphicsContext *gc = m_context.currentGC();
    if (!gc) return 0;

    SvgGradientHelper gradHelper;

    QString gradientId = e.attribute("id");
    if (gradientId.isEmpty()) return 0;

    // check if we have this gradient already parsed
    // copy existing gradient if it exists
    if (m_gradients.contains(gradientId)) {
        return &m_gradients[gradientId];
    }

    if (e.hasAttribute("xlink:href")) {
        // strip the '#' symbol
        QString href = e.attribute("xlink:href").mid(1);

        if (!href.isEmpty()) {
            // copy the referenced gradient if found
            SvgGradientHelper *pGrad = findGradient(href);
            if (pGrad) {
                gradHelper = *pGrad;
            }
        }
    }

    const QGradientStops defaultStops = gradHelper.gradient()->stops();

    if (e.attribute("gradientUnits") == "userSpaceOnUse") {
        gradHelper.setGradientUnits(SvgGradientHelper::UserSpaceOnUse);
    }

    m_context.pushGraphicsContext(e);
    uploadStyleToContext(e);

    if (e.tagName() == "linearGradient") {
        QLinearGradient *g = new QLinearGradient();
        if (gradHelper.gradientUnits() == SvgGradientHelper::ObjectBoundingBox) {
            g->setCoordinateMode(QGradient::ObjectBoundingMode);
            g->setStart(QPointF(SvgUtil::fromPercentage(e.attribute("x1", "0%")),
                                SvgUtil::fromPercentage(e.attribute("y1", "0%"))));
            g->setFinalStop(QPointF(SvgUtil::fromPercentage(e.attribute("x2", "100%")),
                                    SvgUtil::fromPercentage(e.attribute("y2", "0%"))));
        } else {
            g->setStart(QPointF(parseUnitX(e.attribute("x1")),
                                parseUnitY(e.attribute("y1"))));
            g->setFinalStop(QPointF(parseUnitX(e.attribute("x2")),
                                    parseUnitY(e.attribute("y2"))));
        }
        gradHelper.setGradient(g);

    } else if (e.tagName() == "radialGradient") {
        QRadialGradient *g = new QRadialGradient();
        if (gradHelper.gradientUnits() == SvgGradientHelper::ObjectBoundingBox) {
            g->setCoordinateMode(QGradient::ObjectBoundingMode);
            g->setCenter(QPointF(SvgUtil::fromPercentage(e.attribute("cx", "50%")),
                                 SvgUtil::fromPercentage(e.attribute("cy", "50%"))));
            g->setRadius(SvgUtil::fromPercentage(e.attribute("r", "50%")));
            g->setFocalPoint(QPointF(SvgUtil::fromPercentage(e.attribute("fx", "50%")),
                                     SvgUtil::fromPercentage(e.attribute("fy", "50%"))));
        } else {
            g->setCenter(QPointF(parseUnitX(e.attribute("cx")),
                                 parseUnitY(e.attribute("cy"))));
            g->setFocalPoint(QPointF(parseUnitX(e.attribute("fx")),
                                     parseUnitY(e.attribute("fy"))));
            g->setRadius(parseUnitXY(e.attribute("r")));
        }
        gradHelper.setGradient(g);
    } else {
        qDebug() << "WARNING: Failed to parse gradient with tag" << e.tagName();
    }

    // handle spread method
    QGradient::Spread spreadMethod = QGradient::PadSpread;
    QString spreadMethodStr = e.attribute("spreadMethod");
    if (!spreadMethodStr.isEmpty()) {
        if (spreadMethodStr == "reflect") {
            spreadMethod = QGradient::ReflectSpread;
        } else if (spreadMethodStr == "repeat") {
            spreadMethod = QGradient::RepeatSpread;
        }
    }

    gradHelper.setSpreadMode(spreadMethod);

    // Parse the color stops.
    m_context.styleParser().parseColorStops(gradHelper.gradient(), e, gc, defaultStops);

    if (e.hasAttribute("gradientTransform")) {
        SvgTransformParser p(e.attribute("gradientTransform"));
        if (p.isValid()) {
            gradHelper.setTransform(p.transform());
        }
    }

    m_context.popGraphicsContext();

    m_gradients.insert(gradientId, gradHelper);

    return &m_gradients[gradientId];
}

KoVectorPatternBackground::CoordinateSystem
parseUnits(const QString &value, KoVectorPatternBackground::CoordinateSystem defaultValue)
{
    KoVectorPatternBackground::CoordinateSystem result = defaultValue;

    if (value == "userSpaceOnUse") {
        result = KoVectorPatternBackground::UserSpaceOnUse;
    } else if (value == "objectBoundingBox") {
        result = KoVectorPatternBackground::ObjectBoundingBox;
    }

    return result;
}

inline QPointF bakeShapeOffset(const QTransform &patternTransform, const QPointF &shapeOffset)
{
    QTransform result =
            patternTransform *
            QTransform::fromTranslate(-shapeOffset.x(), -shapeOffset.y()) *
            patternTransform.inverted();
    KIS_ASSERT_RECOVER_NOOP(result.type() <= QTransform::TxTranslate);

    return QPointF(result.dx(), result.dy());
}

QSharedPointer<KoVectorPatternBackground> SvgParser::parsePattern(const KoXmlElement &e, const KoShape *shape)
{
    /**
     * Unlike the gradient parsing function, this method is called every time we
     * *reference* the pattern, not when we define it. Therefore we can already
     * use the coordinate system of the destination.
     */

    QSharedPointer<KoVectorPatternBackground> pattHelper;

    SvgGraphicsContext *gc = m_context.currentGC();
    if (!gc) return pattHelper;

    const QString patternId = e.attribute("id");
    if (patternId.isEmpty()) return pattHelper;

    pattHelper = toQShared(new KoVectorPatternBackground);

    if (e.hasAttribute("xlink:href")) {
        // strip the '#' symbol
        QString href = e.attribute("xlink:href").mid(1);

        if (!href.isEmpty() &&href != patternId) {
            // copy the referenced pattern if found
            QSharedPointer<KoVectorPatternBackground> pPatt = findPattern(href, shape);
            if (pPatt) {
                pattHelper = pPatt;
            }
        }
    }

    pattHelper->setReferenceCoordinates(
                parseUnits(e.attribute("patternUnits"),
                           pattHelper->referenceCoordinates()));

    pattHelper->setContentCoordinates(
                parseUnits(e.attribute("patternContentUnits"),
                           pattHelper->contentCoordinates()));

    if (e.hasAttribute("patternTransform")) {
        SvgTransformParser p(e.attribute("patternTransform"));
        if (p.isValid()) {
            pattHelper->setPatternTransform(p.transform());
        }
    }

    if (pattHelper->referenceCoordinates() == KoVectorPatternBackground::ObjectBoundingBox) {
        QRectF referenceRect(
            SvgUtil::fromPercentage(e.attribute("x", "0%")),
            SvgUtil::fromPercentage(e.attribute("y", "0%")),
            SvgUtil::fromPercentage(e.attribute("width", "0%")), // 0% is according to SVG 1.1, don't ask me why!
            SvgUtil::fromPercentage(e.attribute("height", "0%"))); // 0% is according to SVG 1.1, don't ask me why!

        pattHelper->setReferenceRect(referenceRect);
    } else {
        QRectF referenceRect(
            parseUnitX(e.attribute("x", "0")),
            parseUnitY(e.attribute("y", "0")),
            parseUnitX(e.attribute("width", "0")), // 0 is according to SVG 1.1, don't ask me why!
            parseUnitY(e.attribute("height", "0"))); // 0 is according to SVG 1.1, don't ask me why!

        pattHelper->setReferenceRect(referenceRect);
    }

    m_context.pushGraphicsContext(e);
    gc = m_context.currentGC();

    /**
     * In Krita shapes X,Y coordinates are baked into the the shape global transform, but
     * the pattern should be painted in "user" coordinates. Therefore, we should handle
     * this offfset separately.
     *
     * TODO: Please also not that this offset is different from extraShapeOffset(),
     * because A.inverted() * B != A * B.inverted(). I'm not sure which variant is
     * correct (DK)
     */

   const QTransform dstShapeTransform = shape->absoluteTransformation(0);
   const QTransform shapeOffsetTransform = dstShapeTransform * gc->matrix.inverted();
   KIS_SAFE_ASSERT_RECOVER_NOOP(shapeOffsetTransform.type() <= QTransform::TxTranslate);
   const QPointF extraShapeOffset(shapeOffsetTransform.dx(), shapeOffsetTransform.dy());

   // start building shape tree from scratch
   gc->matrix = QTransform();

   const QRectF boundingRect = shape->outline().boundingRect()/*.translated(extraShapeOffset)*/;
   const QTransform relativeToShape(boundingRect.width(), 0, 0, boundingRect.height(),
                                    boundingRect.x(), boundingRect.y());



   // WARNING1: OBB and ViewBox transformations are *baked* into the pattern shapes!
   //          although we expect the pattern be reusable, but it is not so!
   // WARNING2: the pattern shapes are stored in *User* coordinate system, although
   //           the "official" content system might be either OBB or User. It means that
   //           this baked transform should be stripped before writing the shapes back
   //           into SVG
   if (e.hasAttribute("viewBox")) {
        gc->currentBoundingBox =
            pattHelper->referenceCoordinates() == KoVectorPatternBackground::ObjectBoundingBox ?
            relativeToShape.mapRect(pattHelper->referenceRect()) :
            pattHelper->referenceRect();

        applyViewBoxTransform(e);
        pattHelper->setContentCoordinates(pattHelper->referenceCoordinates());

    } else if (pattHelper->contentCoordinates() == KoVectorPatternBackground::ObjectBoundingBox) {
        gc->matrix = relativeToShape * gc->matrix;
    }

    // We do *not* apply patternTransform here! Here we only bake the untransformed
    // version of the shape. The transformed one will be done in the very end while rendering.

    QList<KoShape*> patternShapes = parseContainer(e);

    if (pattHelper->contentCoordinates() == KoVectorPatternBackground::UserSpaceOnUse) {
        // In Krita we normalize the shapes, bake this transform into the pattern shapes

        const QPointF offset = bakeShapeOffset(pattHelper->patternTransform(), extraShapeOffset);

        Q_FOREACH (KoShape *shape, patternShapes) {
            shape->applyAbsoluteTransformation(QTransform::fromTranslate(offset.x(), offset.y()));
        }
    }

    if (pattHelper->referenceCoordinates() == KoVectorPatternBackground::UserSpaceOnUse) {
        // In Krita we normalize the shapes, bake this transform into reference rect
        // NOTE: this is possible *only* when pattern transform is not perspective
        //       (which is always true for SVG)

        const QPointF offset = bakeShapeOffset(pattHelper->patternTransform(), extraShapeOffset);

        QRectF ref = pattHelper->referenceRect();
        ref.translate(offset);
        pattHelper->setReferenceRect(ref);
    }

    m_context.popGraphicsContext();
    gc = m_context.currentGC();

    if (!patternShapes.isEmpty()) {
        pattHelper->setShapes(patternShapes);
    }

    return pattHelper;
}

bool SvgParser::parseFilter(const KoXmlElement &e, const KoXmlElement &referencedBy)
{
    SvgFilterHelper filter;

    // Use the filter that is referencing, or if there isn't one, the original filter
    KoXmlElement b;
    if (!referencedBy.isNull())
        b = referencedBy;
    else
        b = e;

    // check if we are referencing another filter
    if (e.hasAttribute("xlink:href")) {
        QString href = e.attribute("xlink:href").mid(1);
        if (! href.isEmpty()) {
            // copy the referenced filter if found
            SvgFilterHelper *refFilter = findFilter(href);
            if (refFilter)
                filter = *refFilter;
        }
    } else {
        filter.setContent(b);
    }

    if (b.attribute("filterUnits") == "userSpaceOnUse")
        filter.setFilterUnits(SvgFilterHelper::UserSpaceOnUse);
    if (b.attribute("primitiveUnits") == "objectBoundingBox")
        filter.setPrimitiveUnits(SvgFilterHelper::ObjectBoundingBox);

    // parse filter region rectangle
    if (filter.filterUnits() == SvgFilterHelper::UserSpaceOnUse) {
        filter.setPosition(QPointF(parseUnitX(b.attribute("x")),
                                   parseUnitY(b.attribute("y"))));
        filter.setSize(QSizeF(parseUnitX(b.attribute("width")),
                              parseUnitY(b.attribute("height"))));
    } else {
        // x, y, width, height are in percentages of the object referencing the filter
        // so we just parse the percentages
        filter.setPosition(QPointF(SvgUtil::fromPercentage(b.attribute("x", "-0.1")),
                                   SvgUtil::fromPercentage(b.attribute("y", "-0.1"))));
        filter.setSize(QSizeF(SvgUtil::fromPercentage(b.attribute("width", "1.2")),
                              SvgUtil::fromPercentage(b.attribute("height", "1.2"))));
    }

    m_filters.insert(b.attribute("id"), filter);

    return true;
}

bool SvgParser::parseClipPath(const KoXmlElement &e)
{
    SvgClipPathHelper clipPath;

    const QString id = e.attribute("id");
    if (id.isEmpty()) return false;

    clipPath.setClipPathUnits(
        e.attribute("clipPathUnits") == "objectBoundingBox" ?
            SvgClipPathHelper::ObjectBoundingBox :
            SvgClipPathHelper::UserSpaceOnUse);


    // ensure that the clip path is loaded in local coordinates system
    m_context.pushGraphicsContext(e);
    m_context.currentGC()->matrix = QTransform();

    KoShape *clipShape = parseGroup(e);

    m_context.popGraphicsContext();

    if (!clipShape) return false;

    clipPath.setShapes({clipShape});
    m_clipPaths.insert(id, clipPath);

    return true;
}

bool SvgParser::parseClipMask(const KoXmlElement &e)
{
    QSharedPointer<KoClipMask> clipMask(new KoClipMask);

    const QString id = e.attribute("id");
    if (id.isEmpty()) return false;

    clipMask->setCoordinates(
        e.attribute("maskUnits") == "userSpaceOnUse" ?
                    KoClipMask::UserSpaceOnUse :
                    KoClipMask::ObjectBoundingBox);

    clipMask->setContentCoordinates(
        e.attribute("maskContentUnits") == "objectBoundingBox" ?
                    KoClipMask::ObjectBoundingBox :
                    KoClipMask::UserSpaceOnUse);

    QRectF maskRect;

    if (clipMask->coordinates() == KoClipMask::ObjectBoundingBox) {
        maskRect.setRect(
            SvgUtil::fromPercentage(e.attribute("x", "-10%")),
            SvgUtil::fromPercentage(e.attribute("y", "-10%")),
            SvgUtil::fromPercentage(e.attribute("width", "120%")),
            SvgUtil::fromPercentage(e.attribute("height", "120%")));
    } else {
        maskRect.setRect(
            parseUnitX(e.attribute("x", "-10%")), // yes, percents are insane in this case,
            parseUnitY(e.attribute("y", "-10%")), // but this is what SVG 1.1 tells us...
            parseUnitX(e.attribute("width", "120%")),
            parseUnitY(e.attribute("height", "120%")));
    }

    clipMask->setMaskRect(maskRect);


    // ensure that the clip mask is loaded in local coordinates system
    m_context.pushGraphicsContext(e);
    m_context.currentGC()->matrix = QTransform();

    KoShape *clipShape = parseGroup(e);

    m_context.popGraphicsContext();

    if (!clipShape) return false;
    clipMask->setShapes({clipShape});

    m_clipMasks.insert(id, clipMask);
    return true;
}

void SvgParser::uploadStyleToContext(const KoXmlElement &e)
{
    SvgStyles styles = m_context.styleParser().collectStyles(e);
    m_context.styleParser().parseFont(styles);
    m_context.styleParser().parseStyle(styles);
}

void SvgParser::applyCurrentStyle(KoShape *shape, const QPointF &shapeToOriginalUserCoordinates)
{
    if (!shape) return;

    SvgGraphicsContext *gc = m_context.currentGC();
    KIS_ASSERT(gc);

    if (!dynamic_cast<KoShapeGroup*>(shape)) {
        applyFillStyle(shape);
        applyStrokeStyle(shape);
    }

    applyFilter(shape);
    applyClipping(shape, shapeToOriginalUserCoordinates);
    applyMaskClipping(shape, shapeToOriginalUserCoordinates);

    if (!gc->display || !gc->visible) {
        /**
         * WARNING: here is a small inconsistency with the standard:
         *          in the standard, 'display' is not inherited, but in
         *          flake it is!
         *
         * NOTE: though the standard says: "A value of 'display:none' indicates
         *       that the given element and ***its children*** shall not be
         *       rendered directly". Therefore, using setVisible(false) is fully
         *       legitimate here (DK 29.11.16).
         */
        shape->setVisible(false);
    }
    shape->setTransparency(1.0 - gc->opacity);
}

void SvgParser::applyStyle(KoShape *obj, const KoXmlElement &e, const QPointF &shapeToOriginalUserCoordinates)
{
    applyStyle(obj, m_context.styleParser().collectStyles(e), shapeToOriginalUserCoordinates);
}

void SvgParser::applyStyle(KoShape *obj, const SvgStyles &styles, const QPointF &shapeToOriginalUserCoordinates)
{
    SvgGraphicsContext *gc = m_context.currentGC();
    if (!gc)
        return;

    m_context.styleParser().parseStyle(styles);

    if (!obj)
        return;

    if (!dynamic_cast<KoShapeGroup*>(obj)) {
        applyFillStyle(obj);
        applyStrokeStyle(obj);
    }
    applyFilter(obj);
    applyClipping(obj, shapeToOriginalUserCoordinates);
    applyMaskClipping(obj, shapeToOriginalUserCoordinates);

    if (!gc->display || !gc->visible) {
        obj->setVisible(false);
    }
    obj->setTransparency(1.0 - gc->opacity);
}

QGradient* prepareGradientForShape(const SvgGradientHelper *gradient,
                                   const KoShape *shape,
                                   const SvgGraphicsContext *gc,
                                   QTransform *transform)
{
    QGradient *resultGradient = 0;
    KIS_ASSERT(transform);

    if (gradient->gradientUnits() == SvgGradientHelper::ObjectBoundingBox) {
        resultGradient = KoFlake::cloneGradient(gradient->gradient());
        *transform = gradient->transform();
    } else {
        if (gradient->gradient()->type() == QGradient::LinearGradient) {
            /**
             * Create a converted gradient that looks the same, but linked to the
             * bounding rect of the shape, so it would be transformed with the shape
             */

            const QRectF boundingRect = shape->outline().boundingRect();
            const QTransform relativeToShape(boundingRect.width(), 0, 0, boundingRect.height(),
                                             boundingRect.x(), boundingRect.y());

            const QTransform relativeToUser =
                    relativeToShape * shape->transformation() * gc->matrix.inverted();

            const QTransform userToRelative = relativeToUser.inverted();

            const QLinearGradient *o = static_cast<const QLinearGradient*>(gradient->gradient());
            QLinearGradient *g = new QLinearGradient();
            g->setStart(userToRelative.map(o->start()));
            g->setFinalStop(userToRelative.map(o->finalStop()));
            g->setCoordinateMode(QGradient::ObjectBoundingMode);
            g->setStops(o->stops());
            g->setSpread(o->spread());

            resultGradient = g;
            *transform = relativeToUser * gradient->transform() * userToRelative;

        } else if (gradient->gradient()->type() == QGradient::RadialGradient) {
            // For radial and conical gradients such conversion is not possible

            resultGradient = KoFlake::cloneGradient(gradient->gradient());
            *transform = gradient->transform() * gc->matrix * shape->transformation().inverted();
        }
    }

    return resultGradient;
}

void SvgParser::applyFillStyle(KoShape *shape)
{
    SvgGraphicsContext *gc = m_context.currentGC();
    if (! gc)
        return;

    if (gc->fillType == SvgGraphicsContext::None) {
        shape->setBackground(QSharedPointer<KoShapeBackground>(0));
    } else if (gc->fillType == SvgGraphicsContext::Solid) {
        shape->setBackground(QSharedPointer<KoColorBackground>(new KoColorBackground(gc->fillColor)));
    } else if (gc->fillType == SvgGraphicsContext::Complex) {
        // try to find referenced gradient
        SvgGradientHelper *gradient = findGradient(gc->fillId);
        if (gradient) {
            QTransform transform;
            QGradient *result = prepareGradientForShape(gradient, shape, gc, &transform);
            if (result) {
                QSharedPointer<KoGradientBackground> bg;
                bg = toQShared(new KoGradientBackground(result));
                bg->setTransform(transform);
                shape->setBackground(bg);
            }
        } else {
            QSharedPointer<KoVectorPatternBackground> pattern =
                findPattern(gc->fillId, shape);

            if (pattern) {
                shape->setBackground(pattern);
            } else {
                // no referenced fill found, use fallback color
                shape->setBackground(QSharedPointer<KoColorBackground>(new KoColorBackground(gc->fillColor)));
            }
        }
    }

    KoPathShape *path = dynamic_cast<KoPathShape*>(shape);
    if (path)
        path->setFillRule(gc->fillRule);
}

void applyDashes(const KoShapeStrokeSP srcStroke, KoShapeStrokeSP dstStroke)
{
    const double lineWidth = srcStroke->lineWidth();
    QVector<qreal> dashes = srcStroke->lineDashes();

    // apply line width to dashes and dash offset
    if (dashes.count() && lineWidth > 0.0) {
        const double dashOffset = srcStroke->dashOffset();
        QVector<qreal> dashes = srcStroke->lineDashes();

        for (int i = 0; i < dashes.count(); ++i) {
            dashes[i] /= lineWidth;
        }

        dstStroke->setLineStyle(Qt::CustomDashLine, dashes);
        dstStroke->setDashOffset(dashOffset / lineWidth);
    } else {
        dstStroke->setLineStyle(Qt::SolidLine, QVector<qreal>());
    }
}

void SvgParser::applyStrokeStyle(KoShape *shape)
{
    SvgGraphicsContext *gc = m_context.currentGC();
    if (! gc)
        return;

    if (gc->strokeType == SvgGraphicsContext::None) {
        shape->setStroke(KoShapeStrokeModelSP());
    } else if (gc->strokeType == SvgGraphicsContext::Solid) {
        KoShapeStrokeSP stroke(new KoShapeStroke(*gc->stroke));
        applyDashes(gc->stroke, stroke);
        shape->setStroke(stroke);
    } else if (gc->strokeType == SvgGraphicsContext::Complex) {
        // try to find referenced gradient
        SvgGradientHelper *gradient = findGradient(gc->strokeId);
        if (gradient) {
            QTransform transform;
            QGradient *result = prepareGradientForShape(gradient, shape, gc, &transform);
            if (result) {
                QBrush brush = *result;
                delete result;
                brush.setTransform(transform);

                KoShapeStrokeSP stroke(new KoShapeStroke(*gc->stroke));
                stroke->setLineBrush(brush);
                applyDashes(gc->stroke, stroke);
                shape->setStroke(stroke);
            }
        } else {
            // no referenced stroke found, use fallback color
            KoShapeStrokeSP stroke(new KoShapeStroke(*gc->stroke));
            applyDashes(gc->stroke, stroke);
            shape->setStroke(stroke);
        }
    }
}

void SvgParser::applyFilter(KoShape *shape)
{
    SvgGraphicsContext *gc = m_context.currentGC();
    if (! gc)
        return;

    if (gc->filterId.isEmpty())
        return;

    SvgFilterHelper *filter = findFilter(gc->filterId);
    if (! filter)
        return;

    KoXmlElement content = filter->content();

    // parse filter region
    QRectF bound(shape->position(), shape->size());
    // work on bounding box without viewbox tranformation applied
    // so user space coordinates of bounding box and filter region match up
    bound = gc->viewboxTransform.inverted().mapRect(bound);

    QRectF filterRegion(filter->position(bound), filter->size(bound));

    // convert filter region to boundingbox units
    QRectF objectFilterRegion;
    objectFilterRegion.setTopLeft(SvgUtil::userSpaceToObject(filterRegion.topLeft(), bound));
    objectFilterRegion.setSize(SvgUtil::userSpaceToObject(filterRegion.size(), bound));

    KoFilterEffectLoadingContext context(m_context.xmlBaseDir());
    context.setShapeBoundingBox(bound);
    // enable units conversion
    context.enableFilterUnitsConversion(filter->filterUnits() == SvgFilterHelper::UserSpaceOnUse);
    context.enableFilterPrimitiveUnitsConversion(filter->primitiveUnits() == SvgFilterHelper::UserSpaceOnUse);

    KoFilterEffectRegistry *registry = KoFilterEffectRegistry::instance();

    KoFilterEffectStack *filterStack = 0;

    QSet<QString> stdInputs;
    stdInputs << "SourceGraphic" << "SourceAlpha";
    stdInputs << "BackgroundImage" << "BackgroundAlpha";
    stdInputs << "FillPaint" << "StrokePaint";

    QMap<QString, KoFilterEffect*> inputs;

    // create the filter effects and add them to the shape
    for (KoXmlNode n = content.firstChild(); !n.isNull(); n = n.nextSibling()) {
        KoXmlElement primitive = n.toElement();
        KoFilterEffect *filterEffect = registry->createFilterEffectFromXml(primitive, context);
        if (!filterEffect) {
            debugFlake << "filter effect" << primitive.tagName() << "is not implemented yet";
            continue;
        }

        const QString input = primitive.attribute("in");
        if (!input.isEmpty()) {
            filterEffect->setInput(0, input);
        }
        const QString output = primitive.attribute("result");
        if (!output.isEmpty()) {
            filterEffect->setOutput(output);
        }

        QRectF subRegion;
        // parse subregion
        if (filter->primitiveUnits() == SvgFilterHelper::UserSpaceOnUse) {
            const QString xa = primitive.attribute("x");
            const QString ya = primitive.attribute("y");
            const QString wa = primitive.attribute("width");
            const QString ha = primitive.attribute("height");

            if (xa.isEmpty() || ya.isEmpty() || wa.isEmpty() || ha.isEmpty()) {
                bool hasStdInput = false;
                bool isFirstEffect = filterStack == 0;
                // check if one of the inputs is a standard input
                Q_FOREACH (const QString &input, filterEffect->inputs()) {
                    if ((isFirstEffect && input.isEmpty()) || stdInputs.contains(input)) {
                        hasStdInput = true;
                        break;
                    }
                }
                if (hasStdInput || primitive.tagName() == "feImage") {
                    // default to 0%, 0%, 100%, 100%
                    subRegion.setTopLeft(QPointF(0, 0));
                    subRegion.setSize(QSizeF(1, 1));
                } else {
                    // defaults to bounding rect of all referenced nodes
                    Q_FOREACH (const QString &input, filterEffect->inputs()) {
                        if (!inputs.contains(input))
                            continue;

                        KoFilterEffect *inputFilter = inputs[input];
                        if (inputFilter)
                            subRegion |= inputFilter->filterRect();
                    }
                }
            } else {
                const qreal x = parseUnitX(xa);
                const qreal y = parseUnitY(ya);
                const qreal w = parseUnitX(wa);
                const qreal h = parseUnitY(ha);
                subRegion.setTopLeft(SvgUtil::userSpaceToObject(QPointF(x, y), bound));
                subRegion.setSize(SvgUtil::userSpaceToObject(QSizeF(w, h), bound));
            }
        } else {
            // x, y, width, height are in percentages of the object referencing the filter
            // so we just parse the percentages
            const qreal x = SvgUtil::fromPercentage(primitive.attribute("x", "0"));
            const qreal y = SvgUtil::fromPercentage(primitive.attribute("y", "0"));
            const qreal w = SvgUtil::fromPercentage(primitive.attribute("width", "1"));
            const qreal h = SvgUtil::fromPercentage(primitive.attribute("height", "1"));
            subRegion = QRectF(QPointF(x, y), QSizeF(w, h));
        }

        filterEffect->setFilterRect(subRegion);

        if (!filterStack)
            filterStack = new KoFilterEffectStack();

        filterStack->appendFilterEffect(filterEffect);
        inputs[filterEffect->output()] = filterEffect;
    }
    if (filterStack) {
        filterStack->setClipRect(objectFilterRegion);
        shape->setFilterEffectStack(filterStack);
    }
}

void SvgParser::applyClipping(KoShape *shape, const QPointF &shapeToOriginalUserCoordinates)
{
    SvgGraphicsContext *gc = m_context.currentGC();
    if (! gc)
        return;

    if (gc->clipPathId.isEmpty())
        return;

    SvgClipPathHelper *clipPath = findClipPath(gc->clipPathId);
    if (!clipPath || clipPath->isEmpty())
        return;

    QList<KoShape*> shapes;

    Q_FOREACH (KoShape *item, clipPath->shapes()) {
        KoShape *clonedShape = item->cloneShape();
        KIS_ASSERT_RECOVER(clonedShape) { continue; }

        shapes.append(clonedShape);
    }

    if (!shapeToOriginalUserCoordinates.isNull()) {
        const QTransform t =
            QTransform::fromTranslate(shapeToOriginalUserCoordinates.x(),
                                      shapeToOriginalUserCoordinates.y());

        Q_FOREACH(KoShape *s, shapes) {
            s->applyAbsoluteTransformation(t);
        }
    }

    KoClipPath *clipPathObject = new KoClipPath(shapes,
                                                clipPath->clipPathUnits() == SvgClipPathHelper::ObjectBoundingBox ?
                                                KoClipPath::ObjectBoundingBox : KoClipPath::UserSpaceOnUse);
    shape->setClipPath(clipPathObject);
}

void SvgParser::applyMaskClipping(KoShape *shape, const QPointF &shapeToOriginalUserCoordinates)
{
    SvgGraphicsContext *gc = m_context.currentGC();
    if (!gc)
        return;

    if (gc->clipMaskId.isEmpty())
        return;


    QSharedPointer<KoClipMask> originalClipMask = m_clipMasks.value(gc->clipMaskId);
    if (!originalClipMask || originalClipMask->isEmpty()) return;

    KoClipMask *clipMask = originalClipMask->clone();

    clipMask->setExtraShapeOffset(shapeToOriginalUserCoordinates);

    shape->setClipMask(clipMask);
}

KoShape* SvgParser::parseUse(const KoXmlElement &e)
{
    KoShape *result = 0;

    QString id = e.attribute("xlink:href");
    if (!id.isEmpty()) {

        QString key = id.mid(1);
        if (m_context.hasDefinition(key)) {

            SvgGraphicsContext *gc = m_context.pushGraphicsContext(e);

            // TODO: parse 'width' and 'hight' as well
            gc->matrix.translate(parseUnitX(e.attribute("x", "0")), parseUnitY(e.attribute("y", "0")));

            const KoXmlElement &referencedElement = m_context.definition(key);
            result = parseGroup(e, referencedElement);

            m_context.popGraphicsContext();
        }
    }

    return result;
}

void SvgParser::addToGroup(QList<KoShape*> shapes, KoShapeGroup *group)
{
    m_shapes += shapes;

    if (!group || shapes.isEmpty())
        return;

    // not clipped, but inherit transform
    KoShapeGroupCommand cmd(group, shapes, false, true, false);
    cmd.redo();
}

QList<KoShape*> SvgParser::parseSvg(const KoXmlElement &e, QSizeF *fragmentSize)
{
    // check if we are the root svg element
    const bool isRootSvg = m_context.isRootContext();

    // parse 'transform' field if preset
    SvgGraphicsContext *gc = m_context.pushGraphicsContext(e);

    applyStyle(0, e, QPointF());

    const QString w = e.attribute("width");
    const QString h = e.attribute("height");
    const qreal width = w.isEmpty() ? 666.0 : parseUnitX(w);
    const qreal height = h.isEmpty() ? 555.0 : parseUnitY(h);

    QSizeF svgFragmentSize(QSizeF(width, height));

    if (fragmentSize) {
        *fragmentSize = svgFragmentSize;
    }

    gc->currentBoundingBox = QRectF(QPointF(0, 0), svgFragmentSize);

    if (!isRootSvg) {
        // x and y attribute has no meaning for outermost svg elements
        const qreal x = parseUnit(e.attribute("x", "0"));
        const qreal y = parseUnit(e.attribute("y", "0"));

        QTransform move = QTransform::fromTranslate(x, y);
        gc->matrix = move * gc->matrix;
    }

    applyViewBoxTransform(e);

    QList<KoShape*> shapes;

    // SVG 1.1: skip the rendering of the element if it has null viewBox
    if (gc->currentBoundingBox.isValid()) {
        shapes = parseContainer(e);
    }

    m_context.popGraphicsContext();

    return shapes;
}

void SvgParser::applyViewBoxTransform(const KoXmlElement &element)
{
    SvgGraphicsContext *gc = m_context.currentGC();

    QRectF viewRect = gc->currentBoundingBox;
    QTransform viewTransform;

    if (SvgUtil::parseViewBox(gc, element, gc->currentBoundingBox,
                              &viewRect, &viewTransform)) {

        gc->matrix = viewTransform * gc->matrix;
        gc->currentBoundingBox = viewRect;
    }
}

void SvgParser::setFileFetcher(SvgParser::FileFetcherFunc func)
{
    m_context.setFileFetcher(func);
}

inline QPointF extraShapeOffset(const KoShape *shape, const QTransform coordinateSystemOnLoading)
{
    const QTransform shapeToOriginalUserCoordinates =
        shape->absoluteTransformation(0).inverted() *
        coordinateSystemOnLoading;

    KIS_SAFE_ASSERT_RECOVER_NOOP(shapeToOriginalUserCoordinates.type() <= QTransform::TxTranslate);
    return QPointF(shapeToOriginalUserCoordinates.dx(), shapeToOriginalUserCoordinates.dy());
}

KoShape* SvgParser::parseGroup(const KoXmlElement &b, const KoXmlElement &overrideChildrenFrom)
{
    m_context.pushGraphicsContext(b);

    KoShapeGroup *group = new KoShapeGroup();
    group->setZIndex(m_context.nextZIndex());

    // groups should also have their own coordinate system!
    group->applyAbsoluteTransformation(m_context.currentGC()->matrix);
    const QPointF extraOffset = extraShapeOffset(group, m_context.currentGC()->matrix);

    uploadStyleToContext(b);

    QList<KoShape*> childShapes;

    if (!overrideChildrenFrom.isNull()) {
        // we upload styles from both: <use> and <defs>
        uploadStyleToContext(overrideChildrenFrom);
        childShapes = parseSingleElement(overrideChildrenFrom);
    } else {
        childShapes = parseContainer(b);
    }

    // handle id
    applyId(b.attribute("id"), group);

    addToGroup(childShapes, group);

    applyCurrentStyle(group, extraOffset); // apply style to this group after size is set

    m_context.popGraphicsContext();

    return group;
}

QList<KoShape*> SvgParser::parseContainer(const KoXmlElement &e)
{
    QList<KoShape*> shapes;

    // are we parsing a switch container
    bool isSwitch = e.tagName() == "switch";

    for (KoXmlNode n = e.firstChild(); !n.isNull(); n = n.nextSibling()) {
        KoXmlElement b = n.toElement();
        if (b.isNull())
            continue;

        if (isSwitch) {
            // if we are parsing a switch check the requiredFeatures, requiredExtensions
            // and systemLanguage attributes
            // TODO: evaluate feature list
            if (b.hasAttribute("requiredFeatures")) {
                continue;
            }
            if (b.hasAttribute("requiredExtensions")) {
                // we do not support any extensions
                continue;
            }
            if (b.hasAttribute("systemLanguage")) {
                // not implemeted yet
            }
        }

        QList<KoShape*> currentShapes = parseSingleElement(b);
        shapes.append(currentShapes);

        // if we are parsing a switch, stop after the first supported element
        if (isSwitch && !currentShapes.isEmpty())
            break;
    }

    return shapes;
}

QList<KoShape*> SvgParser::parseSingleElement(const KoXmlElement &b)
{
    QList<KoShape*> shapes;

    // save definition for later instantiation with 'use'
    m_context.addDefinition(b);

    if (b.tagName() == "svg") {
        shapes += parseSvg(b);
    } else if (b.tagName() == "g" || b.tagName() == "a" || b.tagName() == "symbol") {
        // treat svg link <a> as group so we don't miss its child elements
        shapes += parseGroup(b);
    } else if (b.tagName() == "switch") {
        m_context.pushGraphicsContext(b);
        shapes += parseContainer(b);
        m_context.popGraphicsContext();
    } else if (b.tagName() == "defs") {
        /**
         * WARNING: 'defs' are basically 'display:none' style, therefore they should not play
         *          any role in shapes outline calculation. But setVisible(false) shapes do!
         *          Should be fixed in the future!
         */
        KoShape *defsShape = parseGroup(b);
        defsShape->setVisible(false);
        shapes += defsShape;
    } else if (b.tagName() == "linearGradient" || b.tagName() == "radialGradient") {
        parseGradient(b);
    } else if (b.tagName() == "pattern") {
    } else if (b.tagName() == "filter") {
        parseFilter(b);
    } else if (b.tagName() == "clipPath") {
        parseClipPath(b);
    } else if (b.tagName() == "mask") {
        parseClipMask(b);
    } else if (b.tagName() == "style") {
        m_context.addStyleSheet(b);
    } else if (b.tagName() == "rect" ||
               b.tagName() == "ellipse" ||
               b.tagName() == "circle" ||
               b.tagName() == "line" ||
               b.tagName() == "polyline" ||
               b.tagName() == "polygon" ||
               b.tagName() == "path" ||
               b.tagName() == "image" ||
               b.tagName() == "text") {
        KoShape *shape = createObjectDirect(b);
        if (shape)
            shapes.append(shape);
    } else if (b.tagName() == "use") {
        shapes += parseUse(b);
    } else if (b.tagName() == "color-profile") {
        m_context.parseProfile(b);
    } else {
        // this is an unknown element, so try to load it anyway
        // there might be a shape that handles that element
        KoShape *shape = createObject(b);
        if (shape) {
            shapes.append(shape);
        }
    }

    return shapes;
}

// Creating functions
// ---------------------------------------------------------------------------------------

KoShape * SvgParser::createPath(const KoXmlElement &element)
{
    KoShape *obj = 0;
    if (element.tagName() == "line") {
        KoPathShape *path = static_cast<KoPathShape*>(createShape(KoPathShapeId));
        if (path) {
            double x1 = element.attribute("x1").isEmpty() ? 0.0 : parseUnitX(element.attribute("x1"));
            double y1 = element.attribute("y1").isEmpty() ? 0.0 : parseUnitY(element.attribute("y1"));
            double x2 = element.attribute("x2").isEmpty() ? 0.0 : parseUnitX(element.attribute("x2"));
            double y2 = element.attribute("y2").isEmpty() ? 0.0 : parseUnitY(element.attribute("y2"));
            path->clear();
            path->moveTo(QPointF(x1, y1));
            path->lineTo(QPointF(x2, y2));
            path->normalize();
            obj = path;
        }
    } else if (element.tagName() == "polyline" || element.tagName() == "polygon") {
        KoPathShape *path = static_cast<KoPathShape*>(createShape(KoPathShapeId));
        if (path) {
            path->clear();

            bool bFirst = true;
            QString points = element.attribute("points").simplified();
            points.replace(',', ' ');
            points.remove('\r');
            points.remove('\n');
            QStringList pointList = points.split(' ', QString::SkipEmptyParts);
            for (QStringList::Iterator it = pointList.begin(); it != pointList.end(); ++it) {
                QPointF point;
                point.setX(SvgUtil::fromUserSpace((*it).toDouble()));
                ++it;
                if (it == pointList.end())
                    break;
                point.setY(SvgUtil::fromUserSpace((*it).toDouble()));
                if (bFirst) {
                    path->moveTo(point);
                    bFirst = false;
                } else
                    path->lineTo(point);
            }
            if (element.tagName() == "polygon")
                path->close();

            path->setPosition(path->normalize());

            obj = path;
        }
    } else if (element.tagName() == "path") {
        KoPathShape *path = static_cast<KoPathShape*>(createShape(KoPathShapeId));
        if (path) {
            path->clear();

            KoPathShapeLoader loader(path);
            loader.parseSvg(element.attribute("d"), true);
            path->setPosition(path->normalize());

            QPointF newPosition = QPointF(SvgUtil::fromUserSpace(path->position().x()),
                                          SvgUtil::fromUserSpace(path->position().y()));
            QSizeF newSize = QSizeF(SvgUtil::fromUserSpace(path->size().width()),
                                    SvgUtil::fromUserSpace(path->size().height()));

            path->setSize(newSize);
            path->setPosition(newPosition);

            obj = path;
        }
    }

    return obj;
}

KoShape * SvgParser::createObjectDirect(const KoXmlElement &b)
{
    m_context.pushGraphicsContext(b);
    uploadStyleToContext(b);

    KoShape *obj = createShapeFromElement(b, m_context);
    if (obj) {
        obj->applyAbsoluteTransformation(m_context.currentGC()->matrix);
        const QPointF extraOffset = extraShapeOffset(obj, m_context.currentGC()->matrix);

        applyCurrentStyle(obj, extraOffset);

        // handle id
        applyId(b.attribute("id"), obj);
        obj->setZIndex(m_context.nextZIndex());
    }

    m_context.popGraphicsContext();

    return obj;
}

KoShape * SvgParser::createObject(const KoXmlElement &b, const SvgStyles &style)
{
    m_context.pushGraphicsContext(b);

    KoShape *obj = createShapeFromElement(b, m_context);
    if (obj) {
        obj->applyAbsoluteTransformation(m_context.currentGC()->matrix);
        const QPointF extraOffset = extraShapeOffset(obj, m_context.currentGC()->matrix);

        SvgStyles objStyle = style.isEmpty() ? m_context.styleParser().collectStyles(b) : style;
        m_context.styleParser().parseFont(objStyle);
        applyStyle(obj, objStyle, extraOffset);

        // handle id
        applyId(b.attribute("id"), obj);
        obj->setZIndex(m_context.nextZIndex());
    }

    m_context.popGraphicsContext();

    return obj;
}

KoShape * SvgParser::createShapeFromElement(const KoXmlElement &element, SvgLoadingContext &context)
{
    KoShape *object = 0;

    QList<KoShapeFactoryBase*> factories = KoShapeRegistry::instance()->factoriesForElement(KoXmlNS::svg, element.tagName());
    foreach (KoShapeFactoryBase *f, factories) {
        KoShape *shape = f->createDefaultShape(m_documentResourceManager);
        if (!shape)
            continue;

        SvgShape *svgShape = dynamic_cast<SvgShape*>(shape);
        if (!svgShape) {
            delete shape;
            continue;
        }

        // reset transformation that might come from the default shape
        shape->setTransformation(QTransform());

        // reset border
        KoShapeStrokeModelSP oldStroke = shape->stroke();
        shape->setStroke(KoShapeStrokeModelSP());

        // reset fill
        shape->setBackground(QSharedPointer<KoShapeBackground>(0));

        if (!svgShape->loadSvg(element, context)) {
            delete shape;
            continue;
        }

        object = shape;
        break;
    }

    if (!object) {
        object = createPath(element);
    }

    return object;
}

KoShape * SvgParser::createShape(const QString &shapeID)
{
    KoShapeFactoryBase *factory = KoShapeRegistry::instance()->get(shapeID);
    if (!factory) {
        debugFlake << "Could not find factory for shape id" << shapeID;
        return 0;
    }

    KoShape *shape = factory->createDefaultShape(m_documentResourceManager);
    if (!shape) {
        debugFlake << "Could not create Default shape for shape id" << shapeID;
        return 0;
    }
    if (shape->shapeId().isEmpty())
        shape->setShapeId(factory->id());

    // reset tranformation that might come from the default shape
    shape->setTransformation(QTransform());

    // reset border
    KoShapeStrokeModelSP oldStroke = shape->stroke();
    shape->setStroke(KoShapeStrokeModelSP());

    // reset fill
    shape->setBackground(QSharedPointer<KoShapeBackground>(0));

    return shape;
}

void SvgParser::applyId(const QString &id, KoShape *shape)
{
    if (id.isEmpty())
        return;

    shape->setName(id);
    m_context.registerShape(id, shape);
}
