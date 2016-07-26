/*
 *  Copyright (c) 2010 Sven Langkamp <sven.langkamp@gmail.com>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 */

#include "kis_brush_based_paintop_settings.h"

#include <kis_paint_action_type_option.h>
#include <kis_airbrush_option.h>
#include "kis_brush_based_paintop_options_widget.h"
#include <kis_boundary.h>
#include "kis_brush_server.h"
#include <QLineF>
#include "kis_signals_blocker.h"


KisBrushBasedPaintOpSettings::KisBrushBasedPaintOpSettings()
    : KisOutlineGenerationPolicy<KisPaintOpSettings>(KisCurrentOutlineFetcher::SIZE_OPTION |
            KisCurrentOutlineFetcher::ROTATION_OPTION |
            KisCurrentOutlineFetcher::MIRROR_OPTION)
{
}

bool KisBrushBasedPaintOpSettings::paintIncremental()
{
    if (hasProperty("PaintOpAction")) {
        return (enumPaintActionType)getInt("PaintOpAction", WASH) == BUILDUP;
    }
    return true;
}

bool KisBrushBasedPaintOpSettings::isAirbrushing() const
{
    return getBool(AIRBRUSH_ENABLED);
}


int KisBrushBasedPaintOpSettings::rate() const
{
    return getInt(AIRBRUSH_RATE);
}

KisPaintOpSettingsSP KisBrushBasedPaintOpSettings::clone() const
{
    KisPaintOpSettingsSP _settings = KisOutlineGenerationPolicy<KisPaintOpSettings>::clone();
    KisBrushBasedPaintOpSettings *settings =
        dynamic_cast<KisBrushBasedPaintOpSettings*>(_settings.data());
    settings->m_savedBrush = this->brush();
    return settings;
}

KisBrushSP KisBrushBasedPaintOpSettings::brush() const
{
    KisBrushBasedPaintopOptionWidget *widget = dynamic_cast<KisBrushBasedPaintopOptionWidget*>(optionsWidget());
    return widget ? widget->brush() : m_savedBrush;
}

QPainterPath KisBrushBasedPaintOpSettings::brushOutlineImpl(const KisPaintInformation &info,
                                                            OutlineMode mode,
                                                            qreal additionalScale,
                                                            bool forceOutline) const
{
    QPainterPath path;

    if (forceOutline || mode == CursorIsOutline || mode == CursorIsCircleOutline || mode == CursorTiltOutline) {
        KisBrushSP brush = this->brush();
        qreal finalScale = brush->scale() * additionalScale;

        QPainterPath realOutline = brush->outline();

        if (mode == CursorIsCircleOutline || mode == CursorTiltOutline ||
            (forceOutline && mode == CursorNoOutline)) {

            QPainterPath ellipse;
            ellipse.addEllipse(realOutline.boundingRect());
            realOutline = ellipse;
        }

        path = outlineFetcher()->fetchOutline(info, this, realOutline, finalScale, brush->angle());

        if (mode == CursorTiltOutline) {
            QPainterPath tiltLine = makeTiltIndicator(info,
                realOutline.boundingRect().center(),
                realOutline.boundingRect().width() * 0.5,
                3.0);
            path.addPath(outlineFetcher()->fetchOutline(info, this, tiltLine, finalScale, 0.0, true, realOutline.boundingRect().center().x(), realOutline.boundingRect().center().y()));
        }
    }

    return path;
}

QPainterPath KisBrushBasedPaintOpSettings::brushOutline(const KisPaintInformation &info, OutlineMode mode) const
{
    return brushOutlineImpl(info, mode, 1.0);
}

bool KisBrushBasedPaintOpSettings::isValid() const
{
    QString filename = getString("requiredBrushFile", "");
    if (!filename.isEmpty()) {
        KisBrushSP brush = KisBrushServer::instance()->brushServer()->resourceByFilename(filename);
        if (!brush) {
            return false;
        }
    }
    return true;
}

bool KisBrushBasedPaintOpSettings::isLoadable()
{
    return (KisBrushServer::instance()->brushServer()->resources().count() > 0);
}

void KisBrushBasedPaintOpSettings::setAngle(qreal value)
{
    KisBrushSP brush = this->brush();
    KIS_SAFE_ASSERT_RECOVER_RETURN(brush);

    brush->setAngle(value);

    KIS_ASSERT_RECOVER_RETURN(optionsWidget());
    optionsWidget()->writeConfigurationSafe(this);
}

qreal KisBrushBasedPaintOpSettings::angle() const
{
    KIS_ASSERT_RECOVER(optionsWidget()) { return 0.0; }
    KisSignalsBlocker b(optionsWidget());
    optionsWidget()->setConfigurationSafe(this);

    KisBrushSP brush = this->brush();
    KIS_SAFE_ASSERT_RECOVER(brush) { return 0.0; }

    return brush->angle();
}

#include <brushengine/kis_slider_based_paintop_property.h>
#include "kis_paintop_preset.h"
#include "kis_paintop_settings_update_proxy.h"


QList<KisUniformPaintOpPropertySP> KisBrushBasedPaintOpSettings::uniformProperties()
{
    QList<KisUniformPaintOpPropertySP> props =
        listWeakToStrong(m_uniformProperties);

    if (props.isEmpty()) {
        KisIntSliderBasedPaintOpPropertyCallback *prop =
            new KisIntSliderBasedPaintOpPropertyCallback(
                KisIntSliderBasedPaintOpPropertyCallback::Int,
                "angle",
                "Angle",
                this, 0);

            prop->setRange(0, 360);

            prop->setReadCallback(
                [](KisUniformPaintOpProperty *prop) {
                    KisBrushBasedPaintOpSettings *s =
                        dynamic_cast<KisBrushBasedPaintOpSettings*>(prop->settings().data());

                    const qreal angleResult = kisRadiansToDegrees(s->angle());
                    prop->setValue(angleResult);
                });
            prop->setWriteCallback(
                [](KisUniformPaintOpProperty *prop) {
                    KisBrushBasedPaintOpSettings *s =
                        dynamic_cast<KisBrushBasedPaintOpSettings*>(prop->settings().data());

                    s->setAngle(kisDegreesToRadians(prop->value().toReal()));
                });

            QObject::connect(preset()->updateProxy(), SIGNAL(sigSettingsChanged()), prop, SLOT(requestReadValue()));
            prop->requestReadValue();
            props << toQShared(prop);
    }

    return KisPaintOpSettings::uniformProperties() + props;
}
